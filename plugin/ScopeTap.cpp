// ScopeTap - the OFX half of Scope Deck.
//
// Passes the image through untouched, reduces it to scope bins in-process, and
// publishes those bins to shared memory. Deliberately thin: all the analysis lives
// in core/ScopeCore.cpp and all the transport in core/ScopeShm.cpp, neither of
// which includes an OFX header. That keeps the code that could take Resolve down
// with it as small as possible, and lets the scope maths be tested without Resolve.
//
// CPU-only by design: no setSupportsCudaRender / OpenCL / Metal, so the host hands
// us host-memory float RGBA with no vendor-specific code, and this file compiles
// unchanged on macOS.

#include "ScopeTap.h"

#include "ScopeCore.h"
#include "ScopeShm.h"

#ifdef SCOPE_TAP_CUDA
#include "ScopeCuda.h"
#endif

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#define kPluginName "Scope Tap"
#define kPluginGrouping "Scope Deck"
#define kPluginDescription \
    "Publishes scope data for the Scope Deck app. Passes the image through unchanged."
#define kPluginIdentifier "group.zen.ScopeDeck.ScopeTap"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 4

// A scope reads the whole frame, so tiling must be off or we would be handed
// fragments instead of frames.
#define kSupportsTiles false
#define kSupportsMultiResolution false
#define kSupportsMultipleClipPARs false

#define kParamRowStep "rowStep"
#define kParamEnabled "publish"
#define kParamSpace   "colorSpace"
#define kParamPublishVideo "publishVideo"
#define kParamPreviewScale "previewScale"
#define kParamGpuAcceleration "gpuAcceleration"
#define kParamOpenApp      "openApp"

////////////////////////////////////////////////////////////////////////////////
// Logging

namespace
{

std::mutex g_logMutex;
std::atomic<unsigned long long> g_renderCount(0);
std::atomic<unsigned int> g_nextInstanceId(1);

std::string LogPath()
{
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = ".";
    return std::string(tmp) + "\\scope_tap.log";
#else
    return "/tmp/scope_tap.log";
#endif
}

void Log(const char* p_Fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    static FILE* file = nullptr;
    if (!file)
    {
        file = std::fopen(LogPath().c_str(), "a");
        if (!file) return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);

    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif

    std::fprintf(file, "%02d:%02d:%02d.%03d  ", tm.tm_hour, tm.tm_min, tm.tm_sec, millis);

    va_list args;
    va_start(args, p_Fmt);
    std::vfprintf(file, p_Fmt, args);
    va_end(args);

    std::fprintf(file, "\n");
    std::fflush(file);
}

double MillisSince(const std::chrono::steady_clock::time_point& p_Start)
{
    const auto delta = std::chrono::steady_clock::now() - p_Start;
    return std::chrono::duration<double, std::milli>(delta).count();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class ScopeTapPlugin : public OFX::ImageEffect
{
public:
    explicit ScopeTapPlugin(OfxImageEffectHandle p_Handle);

    virtual void render(const OFX::RenderArguments& p_Args);
    virtual bool isIdentity(const OFX::IsIdentityArguments& p_Args,
                            OFX::Clip*& p_IdentityClip, double& p_IdentityTime);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args,
                              const std::string& p_ParamName);

private:
    // Not owned.
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;

    OFX::IntParam*     m_RowStep;
    OFX::BooleanParam* m_Enabled;
    OFX::ChoiceParam*  m_Space;
    OFX::BooleanParam* m_PublishVideo;
    OFX::ChoiceParam*  m_PreviewScale;

    OFX::BooleanParam* m_GpuAcceleration;

    scopedeck::ScopeEngine    m_Engine;
    scopedeck::ScopeResult    m_Result;
    scopedeck::ScopePublisher m_Publisher;

#ifdef SCOPE_TAP_CUDA
    scopedeck::GpuTap  m_GpuTap;
    bool               m_GpuTapTried;

    scopedeck::CpuFallbackTap m_CpuFallback;
    bool               m_CpuFallbackTried;
#endif

    unsigned int m_InstanceId;
};

ScopeTapPlugin::ScopeTapPlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
    , m_InstanceId(g_nextInstanceId++)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_RowStep = fetchIntParam(kParamRowStep);
    m_Enabled = fetchBooleanParam(kParamEnabled);
    m_Space   = fetchChoiceParam(kParamSpace);
    m_PublishVideo = fetchBooleanParam(kParamPublishVideo);
    m_PreviewScale = fetchChoiceParam(kParamPreviewScale);
    m_GpuAcceleration = fetchBooleanParam(kParamGpuAcceleration);
#ifdef SCOPE_TAP_CUDA
    m_GpuTapTried = false;
    m_CpuFallbackTried = false;
#endif

    const bool started = m_Publisher.Start();

    Log("---- ScopeTap v%d.%d instance %u created  shm=%s  threads=%d  slot=%llu bytes ----",
        kPluginVersionMajor, kPluginVersionMinor, m_InstanceId,
        started ? "ok" : "FAILED", m_Engine.ThreadCount(),
        static_cast<unsigned long long>(scopedeck::kSlotSize));
}

bool ScopeTapPlugin::isIdentity(const OFX::IsIdentityArguments& /*p_Args*/,
                                OFX::Clip*& /*p_IdentityClip*/,
                                double& /*p_IdentityTime*/)
{
    // Never report identity. A pass-through that claims to be a no-op gets skipped
    // by the host entirely, and the tap would never see a frame.
    return false;
}

#ifdef _WIN32
// Quote a single argument for CreateProcess's lpCommandLine using the escaping
// rules CommandLineToArgvW expects, so a path containing quotes or backslashes
// can't terminate the argument early or inject additional arguments.
static std::string QuoteWindowsArg(const std::string& arg)
{
    std::string result = "\"";
    for (auto it = arg.begin(); ; ++it)
    {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == '\\')
        {
            ++backslashes;
            ++it;
        }
        if (it == arg.end())
        {
            result.append(backslashes * 2, '\\');
            break;
        }
        else if (*it == '"')
        {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
        }
        else
        {
            result.append(backslashes, '\\');
            result.push_back(*it);
        }
    }
    result.push_back('"');
    return result;
}
#endif

void ScopeTapPlugin::changedParam(const OFX::InstanceChangedArgs& /*p_Args*/,
                                  const std::string& p_ParamName)
{
    if (p_ParamName != kParamOpenApp) return;

    Log("inst=%u  Open Scope Deck button pressed", m_InstanceId);

    // Find scopedeck.exe (the native C++ app - see "Scope Deck C++" in the
    // parent tree; this old Python app and its plugin are the legacy half).
    // Unlike the old scope_deck.py, it needs no Python interpreter to run,
    // so this launches it directly rather than shelling out through one.
    // SCOPE_DECK_APP still overrides the path outright - handy at dev time
    // to point straight at a build output directory (e.g.
    // ".../Scope Deck C++/build/scopedeck.exe") without touching the
    // installed default below.
    std::string appPath;
    const char* envPath = std::getenv("SCOPE_DECK_APP");
    if (envPath && envPath[0])
    {
        appPath = envPath;
    }

#ifdef _WIN32
    // Only honoured when it names an .exe that exists. A leftover value from
    // the old Python app (...\app\scope_deck.py) is what actually turned up on
    // a dev machine, and CreateProcess cannot run a .py - it failed with
    // error 193 on every press while the installed app sat unused.
    if (!appPath.empty())
    {
        const bool isExe = appPath.size() > 4 &&
                           _stricmp(appPath.c_str() + appPath.size() - 4, ".exe") == 0;
        if (!isExe || GetFileAttributesA(appPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log("inst=%u  Ignoring SCOPE_DECK_APP=%s (not an existing .exe)", m_InstanceId, appPath.c_str());
            appPath.clear();
        }
    }

    if (appPath.empty())
    {
        // Where the installer puts it (installer/ScopeDeck.iss fixes the
        // install folder so this path always holds). Only the app's settings
        // live in %LOCALAPPDATA%\ScopeDeck - it has to write those, and a
        // normal user cannot write under Program Files.
        const char* programFiles = std::getenv("ProgramFiles");
        appPath = std::string(programFiles && programFiles[0] ? programFiles : "C:\\Program Files")
                + "\\Scope Deck\\scopedeck.exe";
    }

    // scopedeck.exe is launched by its own path now, not resolved via PATH
    // the way "python" was - lpApplicationName is that exact path, and
    // lpCommandLine is just its own quoted argv[0], no interpreter/script
    // split to build. The working directory is set to the exe's own folder
    // (its own timecode helper script sits there) rather than left as
    // Resolve's, which a launch-time cwd should never be assumed to be.
    std::string cmdLine = QuoteWindowsArg(appPath);
    const size_t lastSlash = appPath.find_last_of('\\');
    const std::string appDir = (lastSlash == std::string::npos) ? std::string() : appPath.substr(0, lastSlash);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(
        appPath.c_str(),
        &cmdLine[0],              // mutable buffer, as CreateProcessA requires
        nullptr, nullptr, FALSE,
        0,                        // a GUI app (no console to inherit or spawn)
        nullptr,
        appDir.empty() ? nullptr : appDir.c_str(),
        &si, &pi);
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        Log("inst=%u  Failed to launch Scope Deck app (path=%s), error=%lu",
            m_InstanceId, appPath.c_str(), GetLastError());
    }
#else
    // Unverified: the new C++ app has only been built and run on Windows so
    // far (see MAC_PORTING.md) - this mirrors the Windows logic as best as
    // can be done without a macOS build to check it against.
    if (appPath.empty())
    {
        const char* home = std::getenv("HOME");
        if (home)
            appPath = std::string(home) + "/Library/Application Support/ScopeDeck/scopedeck";
    }

    // Double-fork so the app is reparented to init instead of becoming our
    // zombie, without ever handing appPath to a shell. No interpreter
    // needed - execv the resolved binary directly.
    pid_t pid = fork();
    if (pid == 0)
    {
        const char* argv[] = { appPath.c_str(), nullptr };
        pid_t grandchild = -1;
        posix_spawn(&grandchild, appPath.c_str(), nullptr, nullptr, const_cast<char* const*>(argv), environ);
        _exit(0);
    }
    else if (pid > 0)
    {
        waitpid(pid, nullptr, 0);
    }
    else
    {
        Log("inst=%u  Failed to fork for Scope Deck app launch", m_InstanceId);
    }
#endif
}

void ScopeTapPlugin::render(const OFX::RenderArguments& p_Args)
{
    const auto tEnter = std::chrono::steady_clock::now();
    const unsigned long long callNo = ++g_renderCount;

    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));

    // Timed separately from the passthrough copy below, and deliberately so:
    // until this split existed the log's "copy" number spanned both, and
    // fetchImage is not a cheap accessor. On a GPU-rendered timeline it is
    // where the host materialises the frame in system memory for a CPU-only
    // plugin - the readback off the card - so attributing it to our memcpy
    // sent a threading change after work that was never ours.
    const auto tFetched = std::chrono::steady_clock::now();
    const double msFetch = MillisSince(tEnter);

    if (!dst.get() || !src.get())
    {
        Log("#%llu  inst=%u  FETCH FAILED  t=%.1f", callNo, m_InstanceId, p_Args.time);
        return;
    }

    if ((dst->getPixelDepth() != OFX::eBitDepthFloat) ||
        (dst->getPixelComponents() != OFX::ePixelComponentRGBA))
    {
        Log("#%llu  inst=%u  UNSUPPORTED FORMAT", callNo, m_InstanceId);
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        return;
    }

    const OfxRectI& rw = p_Args.renderWindow;
    const int width  = rw.x2 - rw.x1;
    const int height = rw.y2 - rw.y1;
    if ((width <= 0) || (height <= 0)) return;

#ifdef SCOPE_TAP_CUDA
    // The GPU path. Taken only when the host actually handed over device memory
    // this render - keyed off isEnabledCudaRender per call, never off the device
    // name, so an OpenCL or CPU-mode host falls straight through to the host
    // code below with no vendor sniffing.
    if (p_Args.isEnabledCudaRender)
    {
        const bool wantGpu = m_GpuAcceleration->getValueAtTime(p_Args.time);
        const bool thumbnail = (p_Args.renderScale.x < 0.999) || (p_Args.renderScale.y < 0.999);
        const bool publishing = m_Enabled->getValueAtTime(p_Args.time) && !thumbnail;

        if (wantGpu && publishing)
        {
            if (!m_Publisher.IsRunning()) m_Publisher.Start();

            // Started once per instance. A failure is remembered rather than
            // retried every frame - a driver that refuses to page-lock the
            // block will refuse again 24 times a second.
            if (!m_GpuTapTried)
            {
                m_GpuTapTried = true;
                if (!m_GpuTap.Start())
                {
                    Log("#%llu  inst=%u  GPU path unavailable, staying on CPU: %s",
                        callNo, m_InstanceId, m_GpuTap.Error());
                }
                else
                {
                    scopedeck::GpuTapStats started;
                    m_GpuTap.Stats(started);
                    Log("#%llu  inst=%u  GPU path started: %s",
                        callNo, m_InstanceId, started.note);
                }
            }

            if (m_GpuTap.IsRunning())
            {
                scopedeck::GpuTapArgs args;
                args.srcDevice    = src->getPixelData();
                args.dstDevice    = dst->getPixelData();
                args.srcRowBytes  = static_cast<size_t>(src->getRowBytes());
                args.dstRowBytes  = static_cast<size_t>(dst->getRowBytes());
                args.width        = width;
                args.height       = height;
                args.stream       = p_Args.pCudaStream;
                args.timelineTime = p_Args.time;
                args.instanceId   = m_InstanceId;

                args.params.rowStep = m_RowStep->getValueAtTime(p_Args.time);
                if (args.params.rowStep < 1) args.params.rowStep = 1;
                int gpuSpace = 0;
                m_Space->getValueAtTime(p_Args.time, gpuSpace);
                args.params.colorSpace = static_cast<uint32_t>(gpuSpace);
                args.params.luma = scopedeck::LumaWeightsFor(args.params.colorSpace);

                args.publishPreview = m_PublishVideo->getValueAtTime(p_Args.time);
                int gpuScaleIndex = 2;
                m_PreviewScale->getValueAtTime(p_Args.time, gpuScaleIndex);
                static const float kGpuScales[] = { 1.0f, 0.75f, 0.5f, 0.25f };
                args.previewScale =
                    kGpuScales[(gpuScaleIndex >= 0 && gpuScaleIndex < 4) ? gpuScaleIndex : 2];

                if (m_GpuTap.RenderFrame(args))
                {
                    scopedeck::GpuTapStats stats;
                    m_GpuTap.Stats(stats);
                    // dev= is the device cost of an EARLIER frame, not this one
                    // - pricing this one would mean waiting for it. It is the
                    // number that says what the GPU tap costs; enqueue= and
                    // total= are render-thread wall time, which is what decides
                    // whether Resolve drops frames. Both are logged because
                    // they answer different questions and this project has
                    // twice been caught quoting one for the other.
                    char dev[96] = "dev=-";
                    if (stats.deviceTimingValid)
                        std::snprintf(dev, sizeof(dev),
                                      "dev=%.3fms (kern=%.3f pub=%.3f)",
                                      stats.msDeviceTotal, stats.msDeviceKernels,
                                      stats.msDevicePublish);

                    Log("#%llu  inst=%u  t=%.1f  %dx%d  step=%d  %s  GPU  "
                        "enqueue=%.3fms total=%.2fms  %s  published=%llu skipped=%llu  pinned=%d",
                        callNo, m_InstanceId, p_Args.time, width, height,
                        args.params.rowStep,
                        scopedeck::ColorSpaceName(args.params.colorSpace),
                        stats.msEnqueue, MillisSince(tEnter), dev,
                        static_cast<unsigned long long>(stats.published),
                        static_cast<unsigned long long>(stats.skipped),
                        stats.slotPinned ? 1 : 0);
                    return;
                }

                // One failed frame: honour the passthrough below and say so.
                // The next frame tries again, so a persistent fault shows up as
                // a run of these rather than as a silent black output.
                Log("#%llu  inst=%u  GPU frame failed (%s) - passthrough only",
                    callNo, m_InstanceId, m_GpuTap.Error());
            }
        }

        // Device memory with the GPU reduction not in use: the image must still
        // pass through, and a host memcpy of a device pointer would fault.
        scopedeck::CudaPassthrough(dst->getPixelData(), static_cast<size_t>(dst->getRowBytes()),
                                   src->getPixelData(), static_cast<size_t>(src->getRowBytes()),
                                   width, height, p_Args.pCudaStream);

        // GPU Acceleration turned off on a CUDA host. This used to publish
        // nothing at all - the image passed through and the app simply stopped
        // updating, which is not what a box labelled "GPU Acceleration" should
        // do when you untick it. It now means what it says: the reduction moves
        // to the CPU.
        //
        // The frame is on the card, so that needs a readback - but the render
        // thread does not wait for it. Waiting was measured at 28.5 ms a frame,
        // of which only 5.5 ms was the transfer and 22.5 ms was queue latency,
        // and it was slow enough to visibly hold Resolve up. The submit below
        // enqueues and returns, exactly as the GPU publish leg does; a worker
        // thread reduces and publishes once the copy lands.
        if (publishing && !wantGpu)
        {
            if (!m_CpuFallback.IsRunning() && !m_CpuFallbackTried)
            {
                m_CpuFallbackTried = true;
                if (!m_CpuFallback.Start())
                    Log("#%llu  inst=%u  CPU fallback unavailable (%s)",
                        callNo, m_InstanceId, m_CpuFallback.Error());
            }

            if (m_CpuFallback.IsRunning())
            {
                scopedeck::CpuFallbackTap::SubmitArgs sub;
                sub.srcDevice    = src->getPixelData();
                sub.srcRowBytes  = static_cast<size_t>(src->getRowBytes());
                sub.width        = width;
                sub.height       = height;
                sub.stream       = p_Args.pCudaStream;
                sub.timelineTime = p_Args.time;
                sub.instanceId   = m_InstanceId;

                m_RowStep->getValueAtTime(p_Args.time, sub.params.rowStep);
                if (sub.params.rowStep < 1) sub.params.rowStep = 1;
                int space = 0;
                m_Space->getValueAtTime(p_Args.time, space);
                sub.params.colorSpace = static_cast<uint32_t>(space);
                sub.params.luma = scopedeck::LumaWeightsFor(sub.params.colorSpace);

                sub.publishPreview = m_PublishVideo->getValueAtTime(p_Args.time);
                int scaleIndex = 0;
                m_PreviewScale->getValueAtTime(p_Args.time, scaleIndex);
                static const float kScales[] = { 1.0f, 0.75f, 0.5f, 0.25f };
                sub.previewScale = kScales[(scaleIndex >= 0 && scaleIndex < 4) ? scaleIndex : 3];

                const bool took = m_CpuFallback.Submit(sub);

                scopedeck::CpuFallbackTap::FallbackStats st;
                m_CpuFallback.Stats(st);
                Log("#%llu  inst=%u  t=%.1f  %dx%d  step=%d  %s  CPU-on-GPU-host  "
                    "enqueue=%.3fms total=%.2fms  %s  copy=%.2fms bin=%.2fms  "
                    "published=%llu skipped=%llu  pinned=%d",
                    callNo, m_InstanceId, p_Args.time, width, height, sub.params.rowStep,
                    scopedeck::ColorSpaceName(sub.params.colorSpace),
                    st.msEnqueue, MillisSince(tEnter), took ? "queued" : "busy",
                    st.msCopy, st.msBin,
                    static_cast<unsigned long long>(st.published),
                    static_cast<unsigned long long>(st.skipped),
                    st.pinned ? 1 : 0);
            }
        }
        return;
    }
#endif


    // The image must pass through whatever else happens, including on the
    // thumbnail renders we decline to analyse.
    //
    // Single-threaded on purpose, and measured rather than assumed. At UHD this
    // moves 127 MB in and 127 MB out and costs ~10.5 ms, which looks like one
    // core saturating but is not: a standalone benchmark of exactly this copy on
    // this machine runs 14.1 ms on one thread and only 8.0 ms on twelve, so the
    // limit is DRAM bandwidth, not cores. Splitting it into row bands across
    // ScopeEngine::ThreadCount() threads was tried here and measured over 1169
    // UHD frames inside Resolve: 10.4 ms -> 10.5 ms, i.e. nothing, because
    // Resolve's own render threads are already competing for the same memory
    // controller. The threads were pure cost in the process that can least
    // afford surprises.
    //
    // The floor here is therefore ~10 ms of the tap's ~26 ms at UHD, and no
    // CPU-side change moves it - only not having the frame in system memory at
    // all would (see the GPU-backend discussion).
    const size_t rowBytes = static_cast<size_t>(width) * 4 * sizeof(float);
    for (int y = rw.y1; y < rw.y2; ++y)
    {
        const float* srcRow = static_cast<const float*>(src->getPixelAddress(rw.x1, y));
        float* dstRow = static_cast<float*>(dst->getPixelAddress(rw.x1, y));
        if (srcRow && dstRow) std::memcpy(dstRow, srcRow, rowBytes);
    }

    const double msCopy = MillisSince(tFetched);

    // Resolve renders clip thumbnails through the node graph at a fraction of
    // full scale. Publishing those would make the scope flash thumbnail data.
    const bool isThumbnail = (p_Args.renderScale.x < 0.999) || (p_Args.renderScale.y < 0.999);
    if (isThumbnail) return;

    if (!m_Enabled->getValueAtTime(p_Args.time)) return;

    if (!m_Publisher.IsRunning() && !m_Publisher.Start())
    {
        Log("#%llu  inst=%u  shared memory unavailable", callNo, m_InstanceId);
        return;
    }

    scopedeck::ScopeParams params;
    params.rowStep = m_RowStep->getValueAtTime(p_Args.time);
    if (params.rowStep < 1) params.rowStep = 1;

    // The color space only affects the luma plane - it is the one quantity that is
    // a per-pixel combination of R, G and B and so cannot be reinterpreted later.
    int space = 0;
    m_Space->getValueAtTime(p_Args.time, space);
    params.colorSpace = static_cast<uint32_t>(space);
    params.luma = scopedeck::LumaWeightsFor(params.colorSpace);

    scopedeck::FrameView view;
    view.pixels   = static_cast<const float*>(src->getPixelAddress(rw.x1, rw.y1));
    view.width    = width;
    view.height   = height;
    view.rowBytes = src->getRowBytes();

    if (!view.pixels) return;

    m_Engine.Analyse(view, params, m_Result);

    if (m_PublishVideo->getValueAtTime(p_Args.time))
    {
        int scaleIndex = 0;   // 100%, matching the descriptor's default
        m_PreviewScale->getValueAtTime(p_Args.time, scaleIndex);
        static const float kScales[] = { 1.0f, 0.75f, 0.5f, 0.25f };
        const float previewScale = kScales[(scaleIndex >= 0 && scaleIndex < 4) ? scaleIndex : 3];

        m_Engine.BuildPreview(view, previewScale, m_Result.preview);
    }
    else
    {
        // 0x0 tells the reader there is nothing new to show this frame, rather
        // than re-publishing a stale image every render while the toggle is off.
        m_Result.preview.width = 0;
        m_Result.preview.height = 0;
    }

    m_Publisher.Publish(m_Result, p_Args.time,
                        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        m_InstanceId);

    Log("#%llu  inst=%u  t=%.1f  %dx%d  step=%d  %s  fetch=%.2fms copy=%.2fms bin=%.2fms total=%.2fms  "
        "sampled=%llu  min=(%.4f,%.4f,%.4f) max=(%.4f,%.4f,%.4f)  probe=(%.4f,%.4f,%.4f)",
        callNo, m_InstanceId, p_Args.time, width, height, params.rowStep,
        scopedeck::ColorSpaceName(params.colorSpace),
        msFetch, msCopy, m_Result.millis, MillisSince(tEnter),
        static_cast<unsigned long long>(m_Result.pixelsSampled),
        m_Result.minRGB[0], m_Result.minRGB[1], m_Result.minRGB[2],
        m_Result.maxRGB[0], m_Result.maxRGB[1], m_Result.maxRGB[2],
        m_Result.probeRGB[0], m_Result.probeRGB[1], m_Result.probeRGB[2]);
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

ScopeTapFactory::ScopeTapFactory()
    : OFX::PluginFactoryHelper<ScopeTapFactory>(
          kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void ScopeTapFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);

#ifdef SCOPE_TAP_CUDA
    // Declaring these is what gets us device pointers instead of host memory -
    // the whole point of the port, since the ~10 ms host passthrough is DRAM
    // bandwidth-bound and no CPU-side change moves it (GPU_PORT_HANDOFF.md §3).
    // The plugin still keys off isEnabledCudaRender per render, so a host in
    // OpenCL or CPU mode gets the unchanged host path.
    //
    // SCOPE_TAP_NO_CUDA is the kill switch: set it, restart Resolve, and the
    // plugin declares no GPU support at all, behaving exactly as it did before
    // the port. Deliberately an environment variable rather than a parameter,
    // because these flags are read once when the plugin is described - long
    // before any instance or saved project exists - and because the situation
    // it exists for is "the GPU path is breaking Resolve", which is not a
    // moment to be editing node settings.
    {
        const char* off = std::getenv("SCOPE_TAP_NO_CUDA");
        const bool disabled = off && off[0] && !(off[0] == '0' && !off[1]);
        if (!disabled)
        {
            p_Desc.setSupportsCudaRender(true);
            p_Desc.setSupportsCudaStream(true);
        }
    }
#else
    // No GPU render flags: declining them is what gets us host-memory float RGBA
    // with no vendor code.
#endif

    // Must stay false. The sample sets it true so it can be collapsed into a LUT;
    // a tap reads the entire frame and would be baked away.
    p_Desc.setNoSpatialAwareness(false);
}

void ScopeTapFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc,
                                        OFX::ContextEnum /*p_Context*/)
{
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    BooleanParamDescriptor* enabled = p_Desc.defineBooleanParam(kParamEnabled);
    enabled->setLabels("Publish", "Publish", "Publish");
    enabled->setHint("Send scope data to the Scope Deck app.");
    enabled->setDefault(true);
    page->addChild(*enabled);

    ChoiceParamDescriptor* space = p_Desc.defineChoiceParam(kParamSpace);
    space->setLabels("Color Space", "Color Space", "Color Space");
    space->setHint("Color space at this point in the node graph. Sets the luma "
                   "coefficients for the Y waveform. Placed after a CST to Rec.709, "
                   "leave this on Rec.709.");
    for (uint32_t i = 0; i < scopedeck::kSpaceCount; ++i)
        space->appendOption(scopedeck::ColorSpaceName(i));
    space->setDefault(scopedeck::kSpaceRec709);
    page->addChild(*space);

    IntParamDescriptor* rowStep = p_Desc.defineIntParam(kParamRowStep);
    rowStep->setLabels("Row Step", "Row Step", "Row Step");
    rowStep->setHint("Analyse every Nth row. 1 reads every pixel; higher is faster "
                     "but coarser.");
    rowStep->setDefault(1);
    rowStep->setRange(1, 16);
    rowStep->setDisplayRange(1, 8);
    page->addChild(*rowStep);

    // Placeholder only - not read anywhere in render() yet, and Scope Deck's
    // own Preferences already carries a disabled "Scope Source (GPU
    // Acceleration)" control waiting on the same thing. Exists here now so
    // it's visible in Resolve's Inspector while that GPU measuring path
    // itself is designed; wiring it up (here and in the app) is follow-up
    // work, not part of adding the control.
    BooleanParamDescriptor* gpuAcceleration = p_Desc.defineBooleanParam(kParamGpuAcceleration);
    gpuAcceleration->setLabels("GPU Acceleration", "GPU Acceleration", "GPU Acceleration");
    gpuAcceleration->setHint("Measure on the GPU instead of the CPU. On a CUDA host this "
                             "removes the frame readback entirely - the scope data never "
                             "enters system memory except as the finished bins. Ignored when "
                             "the host is not using CUDA, where the CPU path runs regardless. "
                             "Turning it off measures on the CPU instead, which on a CUDA host "
                             "also has to copy the frame back from the card first and is "
                             "markedly slower - expect dropped frames on a graded UHD "
                             "timeline.");
    gpuAcceleration->setDefault(true);
    page->addChild(*gpuAcceleration);

    BooleanParamDescriptor* publishVideo = p_Desc.defineBooleanParam(kParamPublishVideo);
    publishVideo->setLabels("Publish Video", "Publish Video", "Publish Video");
    publishVideo->setHint("Send a downscaled live preview of the frame to the "
                          "Scope Deck app. Adds render cost on top of the scope "
                          "bins; turn off if not needed.");
    publishVideo->setDefault(true);
    page->addChild(*publishVideo);

    ChoiceParamDescriptor* previewScale = p_Desc.defineChoiceParam(kParamPreviewScale);
    previewScale->setLabels("Preview Scale", "Preview Scale", "Preview Scale");
    previewScale->setHint("Resolution of the live preview, relative to the source "
                         "frame. Higher costs more render time and shared-memory "
                         "bandwidth per frame - 100% at UHD is real work, not free.");
    previewScale->appendOption("100%");
    previewScale->appendOption("75%");
    previewScale->appendOption("50%");
    previewScale->appendOption("25%");
    // 50%, not 100%. Measured on a UHD timeline: preview+publish costs ~21.7ms
    // per frame at 100% and ~3.3ms at 25%, against a whole-tap total of ~26.5ms -
    // so the old default was spending the larger part of the tap's entire budget
    // on a viewfinder, and a UHD project blew a 24fps frame budget before Resolve
    // did anything else. 50% halves the linear resolution for roughly a quarter of
    // the cost (~7ms) and is still 1920x1080 from a UHD source, which is far more
    // than the app's preview panel resolves. 25% is there for anyone who wants the
    // cost floor. Only new instances pick this up; saved projects keep their
    // stored value, which is the right behaviour for a setting someone may have
    // chosen deliberately.
    previewScale->setDefault(2);
    page->addChild(*previewScale);

    PushButtonParamDescriptor* openApp = p_Desc.definePushButtonParam(kParamOpenApp);
    openApp->setLabels("Open Scope Deck", "Open Scope Deck", "Open Scope Deck");
    openApp->setHint("Launch the Scope Deck application. Set the SCOPE_DECK_APP "
                     "environment variable to override the path to scopedeck.exe.");
    page->addChild(*openApp);
}

ImageEffect* ScopeTapFactory::createInstance(OfxImageEffectHandle p_Handle,
                                             ContextEnum /*p_Context*/)
{
    return new ScopeTapPlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static ScopeTapFactory scopeTap;
    p_FactoryArray.push_back(&scopeTap);
}
