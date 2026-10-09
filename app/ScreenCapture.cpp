#include "ScreenCapture.h"

#include "ScopeCore.h"
#include "ScopeReader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <timeapi.h>
#endif

#ifdef __APPLE__
#include "mac/ScreenCaptureMac.h"
#endif

namespace scopedeck
{

using Clock = std::chrono::steady_clock;

static double MillisBetween(Clock::time_point p_A, Clock::time_point p_B)
{
    return std::chrono::duration<double, std::milli>(p_B - p_A).count();
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Region picker
// ---------------------------------------------------------------------------
//
// A plain Win32 popup rather than an ImGui window: it has to cover every
// monitor at once, above everything, including Scope Deck's own windows -
// the same job the old app's RegionPickerOverlay did with a frameless Qt
// widget. The background is a screenshot taken before the overlay appears,
// dimmed everywhere except inside the selection, like a snipping tool.

namespace
{

struct PickerState
{
    int vx = 0, vy = 0, vw = 0, vh = 0;   // virtual desktop, physical pixels

    HDC     brightDC  = nullptr;  HBITMAP brightBmp = nullptr;  HGDIOBJ brightOld = nullptr;
    HDC     dimDC     = nullptr;  HBITMAP dimBmp    = nullptr;  HGDIOBJ dimOld    = nullptr;
    HDC     backDC    = nullptr;  HBITMAP backBmp   = nullptr;  HGDIOBJ backOld   = nullptr;
    HFONT   font      = nullptr;

    bool  dragging = false;
    bool  done     = false;
    bool  accepted = false;
    POINT start{};
    POINT current{};
};

PickerState* g_Picker = nullptr;   // one modal picker at a time

// Label and border sit just outside the selection, so every invalidation
// grows the selection by this much to repaint them too.
constexpr int kPickerMargin = 48;

RECT PickerSelection(const PickerState& p_State)
{
    RECT r;
    r.left   = std::min(p_State.start.x, p_State.current.x);
    r.top    = std::min(p_State.start.y, p_State.current.y);
    r.right  = std::max(p_State.start.x, p_State.current.x) + 1;
    r.bottom = std::max(p_State.start.y, p_State.current.y) + 1;
    return r;
}

RECT Grown(RECT p_Rect, int p_By)
{
    InflateRect(&p_Rect, p_By, p_By);
    return p_Rect;
}

void DrawLabel(HDC p_DC, const wchar_t* p_Text, int p_X, int p_Y)
{
    SetBkMode(p_DC, OPAQUE);
    SetBkColor(p_DC, RGB(20, 20, 20));
    SetTextColor(p_DC, RGB(240, 240, 240));
    TextOutW(p_DC, p_X, p_Y, p_Text, static_cast<int>(wcslen(p_Text)));
}

void PaintPicker(PickerState& p_State, HDC p_Target, const RECT& p_Dirty)
{
    const int dx = p_Dirty.left, dy = p_Dirty.top;
    const int dw = p_Dirty.right - p_Dirty.left, dh = p_Dirty.bottom - p_Dirty.top;
    if (dw <= 0 || dh <= 0) return;

    // Composed off-screen and copied once, so the dim-then-bright layering
    // never reaches the screen half-drawn.
    HDC back = p_State.backDC;
    BitBlt(back, dx, dy, dw, dh, p_State.dimDC, dx, dy, SRCCOPY);

    HGDIOBJ oldFont = SelectObject(back, p_State.font);

    if (p_State.dragging)
    {
        const RECT sel = PickerSelection(p_State);
        BitBlt(back, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top,
               p_State.brightDC, sel.left, sel.top, SRCCOPY);

        HBRUSH border = CreateSolidBrush(RGB(255, 196, 0));
        RECT outer = Grown(sel, 1);
        FrameRect(back, &outer, border);
        RECT outer2 = Grown(sel, 2);
        FrameRect(back, &outer2, border);
        DeleteObject(border);

        wchar_t size[64];
        std::swprintf(size, 64, L" %d x %d ", static_cast<int>(sel.right - sel.left),
                      static_cast<int>(sel.bottom - sel.top));
        const int labelY = (sel.bottom + 26 < p_State.vh) ? sel.bottom + 6 : sel.top - 26;
        DrawLabel(back, size, sel.left, std::max(labelY, 0));
    }

    // Instructions on the primary monitor, whose top-left is the virtual
    // desktop's (0,0) - not necessarily the overlay's own origin.
    DrawLabel(back, L"  Drag to select a capture region  -  Esc or right-click to cancel  ",
              -p_State.vx + 24, -p_State.vy + 24);

    SelectObject(back, oldFont);
    BitBlt(p_Target, dx, dy, dw, dh, back, dx, dy, SRCCOPY);
}

LRESULT CALLBACK PickerProc(HWND p_Wnd, UINT p_Msg, WPARAM p_W, LPARAM p_L)
{
    PickerState* s = g_Picker;
    if (!s) return DefWindowProcW(p_Wnd, p_Msg, p_W, p_L);

    const auto mousePoint = [&]() {
        POINT p{ GET_X_LPARAM(p_L), GET_Y_LPARAM(p_L) };
        p.x = std::clamp<LONG>(p.x, 0, s->vw - 1);
        p.y = std::clamp<LONG>(p.y, 0, s->vh - 1);
        return p;
    };

    switch (p_Msg)
    {
        case WM_ERASEBKGND:
            return 1;   // PaintPicker covers every pixel it is asked for

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(p_Wnd, &ps);
            PaintPicker(*s, dc, ps.rcPaint);
            EndPaint(p_Wnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            s->dragging = true;
            s->start = s->current = mousePoint();
            SetCapture(p_Wnd);
            {
                const RECT r = Grown(PickerSelection(*s), kPickerMargin);
                InvalidateRect(p_Wnd, &r, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (s->dragging)
            {
                RECT dirty = Grown(PickerSelection(*s), kPickerMargin);
                s->current = mousePoint();
                const RECT now = Grown(PickerSelection(*s), kPickerMargin);
                UnionRect(&dirty, &dirty, &now);
                InvalidateRect(p_Wnd, &dirty, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (s->dragging)
            {
                s->current = mousePoint();
                ReleaseCapture();
                const RECT sel = PickerSelection(*s);
                if ((sel.right - sel.left) >= 8 && (sel.bottom - sel.top) >= 8)
                {
                    s->accepted = true;
                    s->done = true;
                }
                else
                {
                    // A click, not a drag - nothing to capture yet, keep waiting.
                    const RECT r = Grown(sel, kPickerMargin);
                    s->dragging = false;
                    InvalidateRect(p_Wnd, &r, FALSE);
                }
            }
            return 0;

        case WM_RBUTTONDOWN:
            s->done = true;
            return 0;

        case WM_KEYDOWN:
            if (p_W == VK_ESCAPE) s->done = true;
            return 0;

        case WM_ACTIVATE:
            // Alt-Tab away cancels rather than leaving a topmost overlay
            // sitting over a window the user just switched to.
            if (LOWORD(p_W) == WA_INACTIVE) s->done = true;
            return 0;

        case WM_CLOSE:
            s->done = true;
            return 0;
    }
    return DefWindowProcW(p_Wnd, p_Msg, p_W, p_L);
}

bool MakeDib(HDC p_Screen, int p_W, int p_H, HDC& p_DC, HBITMAP& p_Bmp, HGDIOBJ& p_Old, void** p_Bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = p_W;
    bi.bmiHeader.biHeight      = -p_H;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    p_Bmp = CreateDIBSection(p_Screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!p_Bmp || !bits) return false;
    p_DC  = CreateCompatibleDC(p_Screen);
    p_Old = SelectObject(p_DC, p_Bmp);
    if (p_Bits) *p_Bits = bits;
    return true;
}

void FreeDib(HDC& p_DC, HBITMAP& p_Bmp, HGDIOBJ p_Old)
{
    if (p_DC)  { SelectObject(p_DC, p_Old); DeleteDC(p_DC); p_DC = nullptr; }
    if (p_Bmp) { DeleteObject(p_Bmp); p_Bmp = nullptr; }
}

} // namespace

bool PickScreenRegion(ScreenRegion& p_Out)
{
    if (g_Picker) return false;

    PickerState state;
    state.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    state.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    state.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    state.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (state.vw <= 0 || state.vh <= 0) return false;

    HDC screen = GetDC(nullptr);
    void* brightBits = nullptr;
    void* dimBits    = nullptr;
    const bool ok =
        MakeDib(screen, state.vw, state.vh, state.brightDC, state.brightBmp, state.brightOld, &brightBits) &&
        MakeDib(screen, state.vw, state.vh, state.dimDC,    state.dimBmp,    state.dimOld,    &dimBits) &&
        MakeDib(screen, state.vw, state.vh, state.backDC,   state.backBmp,   state.backOld,   nullptr);

    bool picked = false;
    if (ok)
    {
        // The screenshot the user picks against. Taken before the overlay
        // exists, so the overlay never appears in its own background.
        BitBlt(state.brightDC, 0, 0, state.vw, state.vh, screen, state.vx, state.vy, SRCCOPY);
        GdiFlush();

        const size_t count = static_cast<size_t>(state.vw) * state.vh * 4;
        const uint8_t* src = static_cast<const uint8_t*>(brightBits);
        uint8_t* dst = static_cast<uint8_t*>(dimBits);
        for (size_t i = 0; i < count; ++i)
            dst[i] = static_cast<uint8_t>((src[i] * 102u) >> 8);   // ~40%

        state.font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        static const wchar_t* kClass = L"ScopeDeckRegionPicker";
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = PickerProc;
            wc.hInstance     = instance;
            wc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
            wc.lpszClassName = kClass;
            registered = RegisterClassExW(&wc) != 0;
        }

        g_Picker = &state;
        HWND wnd = registered
            ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Scope Deck - Select Region",
                              WS_POPUP, state.vx, state.vy, state.vw, state.vh,
                              nullptr, nullptr, instance, nullptr)
            : nullptr;

        if (wnd)
        {
            ShowWindow(wnd, SW_SHOW);
            SetForegroundWindow(wnd);
            SetFocus(wnd);

            // Its own loop, like a modal dialog. It still dispatches messages
            // for the app's GLFW windows (same thread); they just queue input
            // until the main loop runs again.
            MSG msg;
            while (!state.done)
            {
                const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                if (got == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }   // app closing
                if (got < 0) break;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            DestroyWindow(wnd);

            if (state.accepted)
            {
                const RECT sel = PickerSelection(state);
                p_Out.x      = sel.left + state.vx;
                p_Out.y      = sel.top  + state.vy;
                p_Out.width  = sel.right - sel.left;
                p_Out.height = sel.bottom - sel.top;
                picked = true;
            }
        }
        g_Picker = nullptr;
    }

    if (state.font) DeleteObject(state.font);
    FreeDib(state.backDC,   state.backBmp,   state.backOld);
    FreeDib(state.dimDC,    state.dimBmp,    state.dimOld);
    FreeDib(state.brightDC, state.brightBmp, state.brightOld);
    ReleaseDC(nullptr, screen);
    return picked;
}

#elif defined(__APPLE__)

// An overlay per display, in app/mac/ScreenCaptureMac.mm.
bool PickScreenRegion(ScreenRegion& p_Out) { return mac::PickScreenRegion(p_Out); }

#else

bool PickScreenRegion(ScreenRegion& /*p_Out*/) { return false; }

#endif

// ---------------------------------------------------------------------------
// From captured pixels to a ScopeFrame
// ---------------------------------------------------------------------------
//
// Every platform's grab ends in the same place - a BGRA8 picture, top row
// first - and from there to the frame the panels read is one shared path, so
// the platforms can't drift apart in what the scopes see.

namespace
{

class CaptureAnalyser
{
public:
    CaptureAnalyser()
    {
        for (int i = 0; i < 256; ++i) m_Lut[i] = static_cast<float>(i) / 255.0f;
    }

    // p_Stride is the bytes from one row to the next. p_FrameNo is this
    // capture session's count, which stands in for timeline time. p_Frame's
    // vectors are swapped with the engine's, not copied: the engine gets last
    // frame's storage back, which Analyse reassigns anyway.
    void Analyse(const uint8_t* p_BGRA, int p_Width, int p_Height, size_t p_Stride,
                 uint64_t p_FrameNo, ScopeFrame& p_Frame)
    {
        const int w = p_Width;
        const int h = p_Height;
        m_RGBA.resize(static_cast<size_t>(w) * h * 4);

        // BGRA8 top-down -> float RGBA, which is what ScopeEngine reads (the
        // tap hands it Resolve's float frames).
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = p_BGRA + static_cast<size_t>(y) * p_Stride;
            float* dst = m_RGBA.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x, src += 4, dst += 4)
            {
                dst[0] = m_Lut[src[2]];
                dst[1] = m_Lut[src[1]];
                dst[2] = m_Lut[src[0]];
                dst[3] = 1.0f;
            }
        }

        // ScopeEngine's row 0 is the bottom of the picture (OFX's convention -
        // BuildPreview flips on output). The capture is top-down, so the view
        // starts at the last row and walks upwards.
        FrameView view;
        view.pixels   = m_RGBA.data() + static_cast<size_t>(h - 1) * w * 4;
        view.width    = w;
        view.height   = h;
        view.rowBytes = -static_cast<int>(static_cast<size_t>(w) * 4 * sizeof(float));

        m_Engine.Analyse(view, m_Params, m_Result);
        m_Engine.BuildPreview(view, 1.0f, m_Result.preview);

        // Exactly the fields ScopePublisher::Publish writes and
        // ScopeReader::ReadSlot copies back out, without the round trip.
        ScopeFrame& frame = p_Frame;
        frame.timelineTime  = static_cast<double>(p_FrameNo);
        frame.frameIndex    = NextFrameIdentity();
        frame.width         = static_cast<uint32_t>(w);
        frame.height        = static_cast<uint32_t>(h);
        frame.instanceId    = 0;
        frame.pixelsSampled = static_cast<uint32_t>(std::min<uint64_t>(m_Result.pixelsSampled, 0xFFFFFFFFull));
        frame.binMillis     = m_Result.millis;
        frame.colorSpace    = m_Result.colorSpace;
        frame.lumaCoeff[0]  = m_Result.luma.r;
        frame.lumaCoeff[1]  = m_Result.luma.g;
        frame.lumaCoeff[2]  = m_Result.luma.b;
        for (int c = 0; c < 3; ++c)
        {
            frame.minRGB[c]   = m_Result.minRGB[c];
            frame.maxRGB[c]   = m_Result.maxRGB[c];
            frame.probeRGB[c] = m_Result.probeRGB[c];
        }
        frame.previewWidth  = m_Result.preview.width;
        frame.previewHeight = m_Result.preview.height;
        frame.waveform.swap(m_Result.waveform);
        frame.histogram.swap(m_Result.histogram);
        frame.vectorscope.swap(m_Result.vectorscope);
        frame.twinPeaks.swap(m_Result.twinPeaks);
        frame.waveformTrace.swap(m_Result.waveformTrace);
        frame.preview.swap(m_Result.preview.rgb);
    }

private:
    float              m_Lut[256];
    std::vector<float> m_RGBA;
    ScopeEngine        m_Engine;
    ScopeParams        m_Params;   // Rec.709, every row - screen content is display sRGB
    ScopeResult        m_Result;
};

} // namespace

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

// frameIndex for captured frames comes from NextFrameIdentity(), the same
// counter ScopeReader stamps tap frames with. Panels cache derived work keyed
// on frameIndex, so a capture numbering 1, 2, 3... would collide with a tap
// frame or an earlier capture session and show a cached result for a different
// picture; one shared counter makes every frame distinct by construction.

struct ScreenCaptureSource::Impl
{
    ScreenRegion       region;
#ifdef __APPLE__
    uint32_t           displayId = 0;   // the display region lies on
#endif
    std::thread        worker;
    std::atomic<bool>  stop{ false };

    mutable std::mutex mutex;          // guards everything below
    ScopeFrame         latest;
    bool               fresh = false;
    ScreenCaptureStats stats;

    void Run();
    void SetError(const char* p_Message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats.error = p_Message;
    }
};

#ifdef _WIN32

void ScreenCaptureSource::Impl::Run()
{
    const int w = region.width;
    const int h = region.height;

    HDC screen = GetDC(nullptr);
    HDC mem = nullptr;  HBITMAP dib = nullptr;  HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    if (!MakeDib(screen, w, h, mem, dib, oldBmp, &bits))
    {
        FreeDib(mem, dib, oldBmp);
        ReleaseDC(nullptr, screen);
        SetError("Couldn't allocate a buffer for the capture region.");
        return;
    }

    // Default timer granularity is ~15.6 ms, which would cap a 60 Hz loop at
    // barely 30. Raised for this thread's lifetime only.
    timeBeginPeriod(1);

    CaptureAnalyser analyser;
    ScopeFrame      frame;

    const auto period = std::chrono::microseconds(16667);   // 60 Hz
    auto next = Clock::now();
    uint64_t frameNo = 0;

    auto fpsWindowStart = Clock::now();
    uint64_t fpsWindowFrames = 0;
    double fps = 0.0;

    while (!stop.load(std::memory_order_relaxed))
    {
        const auto t0 = Clock::now();
        const BOOL grabbed = BitBlt(mem, 0, 0, w, h, screen, region.x, region.y, SRCCOPY);
        GdiFlush();
        const auto t1 = Clock::now();

        if (!grabbed)
        {
            SetError("Can't read the screen right now - it may be locked, or a UAC prompt is showing.");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            next = Clock::now();
            continue;
        }

        // A DIB section's rows are DWORD-aligned, which for 32-bit pixels
        // means exactly w * 4 bytes apart.
        ++frameNo;
        analyser.Analyse(static_cast<const uint8_t*>(bits), w, h, static_cast<size_t>(w) * 4,
                         frameNo, frame);
        const auto t2 = Clock::now();

        ++fpsWindowFrames;
        const double windowMs = MillisBetween(fpsWindowStart, t2);
        if (windowMs >= 500.0)
        {
            fps = fpsWindowFrames * 1000.0 / windowMs;
            fpsWindowStart = t2;
            fpsWindowFrames = 0;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            std::swap(latest, frame);
            fresh = true;
            stats.fps      = fps;
            stats.msGrab   = MillisBetween(t0, t1);
            stats.msScopes = MillisBetween(t1, t2);
            stats.frames   = frameNo;
            stats.error.clear();
        }

        next += period;
        const auto now = Clock::now();
        if (next < now) next = now;   // running behind: don't try to catch up
        std::this_thread::sleep_until(next);
    }

    timeEndPeriod(1);
    FreeDib(mem, dib, oldBmp);
    ReleaseDC(nullptr, screen);
}

#elif defined(__APPLE__)

// ScreenCaptureKit pushes frames rather than being polled: the stream's
// callback leaves the newest one for this thread and returns, and the
// conversion and scopes run here, as on Windows. There is no 60 Hz timer -
// the stream is capped at 60 fps itself, and sends nothing at all while the
// region's content is still, so the fps reading falls to 0 over a paused
// picture rather than re-analysing an unchanged one.
void ScreenCaptureSource::Impl::Run()
{
    mac::ScreenStream stream;
    std::string error;
    if (!stream.Start(displayId, region, stop, error))
    {
        if (!error.empty()) SetError(error.c_str());
        return;
    }

    CaptureAnalyser analyser;
    ScopeFrame      frame;
    uint64_t        frameNo = 0;

    auto fpsWindowStart = Clock::now();
    uint64_t fpsWindowFrames = 0;
    double fps = 0.0;

    while (!stop.load(std::memory_order_relaxed))
    {
        // Short waits, so Stop() is never held up by a still screen.
        mac::ScreenFrame pixels;
        const mac::ScreenStream::Wait got = stream.WaitFrame(100, pixels);
        const auto t1 = Clock::now();

        if (got == mac::ScreenStream::Wait::Failed)
        {
            SetError(stream.Error().c_str());
            break;
        }

        const double msGrab = pixels.msGrab;   // Release clears it
        if (got == mac::ScreenStream::Wait::Frame)
        {
            ++frameNo;
            analyser.Analyse(pixels.bgra, pixels.width, pixels.height, pixels.stride, frameNo, frame);
            stream.Release(pixels);
            ++fpsWindowFrames;
        }
        const auto t2 = Clock::now();

        const double windowMs = MillisBetween(fpsWindowStart, t2);
        if (windowMs >= 500.0)
        {
            fps = fpsWindowFrames * 1000.0 / windowMs;
            fpsWindowStart = t2;
            fpsWindowFrames = 0;
        }

        std::lock_guard<std::mutex> lock(mutex);
        stats.fps = fps;
        if (got != mac::ScreenStream::Wait::Frame) continue;

        std::swap(latest, frame);
        fresh = true;
        stats.msGrab   = msGrab;
        stats.msScopes = MillisBetween(t1, t2);
        stats.frames   = frameNo;
        stats.error.clear();
    }

    stream.Stop();
}

#else

void ScreenCaptureSource::Impl::Run() {}

#endif

ScreenCaptureSource::ScreenCaptureSource() : m_Impl(new Impl) {}

ScreenCaptureSource::~ScreenCaptureSource()
{
    Stop();
    delete m_Impl;
}

bool ScreenCaptureSource::Start(const ScreenRegion& p_Region)
{
    Stop();
#if defined(_WIN32) || defined(__APPLE__)
    if (!p_Region.IsValid()) return false;

#ifdef __APPLE__
    // Here rather than on the worker so Region() is the region actually
    // captured from the moment Start() returns.
    ScreenRegion fitted;
    uint32_t displayId = 0;
    if (!mac::FitRegionToDisplay(p_Region, fitted, displayId)) return false;
    m_Impl->region = fitted;
    m_Impl->displayId = displayId;
#else
    m_Impl->region = p_Region;
#endif
    m_Impl->stop = false;
    {
        std::lock_guard<std::mutex> lock(m_Impl->mutex);
        m_Impl->fresh = false;
        m_Impl->stats = ScreenCaptureStats{};
    }
    m_Impl->worker = std::thread([impl = m_Impl] { impl->Run(); });
    return true;
#else
    (void)p_Region;
    return false;
#endif
}

void ScreenCaptureSource::Stop()
{
    m_Impl->stop = true;
    if (m_Impl->worker.joinable()) m_Impl->worker.join();
}

bool ScreenCaptureSource::IsRunning() const { return m_Impl->worker.joinable(); }

const ScreenRegion& ScreenCaptureSource::Region() const { return m_Impl->region; }

bool ScreenCaptureSource::TakeLatest(ScopeFrame& p_Out)
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    if (!m_Impl->fresh) return false;
    std::swap(p_Out, m_Impl->latest);
    m_Impl->fresh = false;
    return true;
}

void ScreenCaptureSource::Stats(ScreenCaptureStats& p_Out) const
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    p_Out = m_Impl->stats;
}

} // namespace scopedeck
