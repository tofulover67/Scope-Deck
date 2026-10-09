// ScopeTransmit - Premiere Pro's counterpart to ScopeTap.
//
// A Transmit plugin: Premiere's interface for external video outputs (the one
// AJA/Blackmagic cards use), which the user turns on once in Preferences >
// Playback > Video Device. Premiere then pushes every frame the Program or
// Source Monitor shows - playing, scrubbing or parked - with nothing to add
// to the timeline. Video only: no audio, no clock, so it never takes over
// playback from whatever device already drives it.
//
// Frames go through the same ScopeEngine into a block with the same layout as
// ScopeTap's, under its own name (kShmNamePremiere), so the Scope Deck app
// reads Premiere exactly as it reads Resolve and offers the two as separate
// inputs.
//
// Threading: every SDK call stays on the thread Premiere calls us on.
// PushVideo converts the frame into a buffer of ours and disposes the PPix at
// once; a worker thread does the reduction and the publish. Premiere is never
// held up by the scopes, and when they fall behind, the newest frame replaces
// the one still waiting rather than a queue building up.

#include "ScopeControl.h"
#include "ScopeCore.h"
#include "ScopeShm.h"

#include <PrSDKEntry.h>   // DllExport, PREMPLUGENTRY
#include <PrSDKTransmit.h>
#include <PrSDKPPixSuite.h>
#include <PrSDKSequenceInfoSuite.h>
#include <PrSDKTimeSuite.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

using Clock = std::chrono::steady_clock;

double MillisSince(Clock::time_point p_Start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - p_Start).count();
}

// ---------------------------------------------------------------------------
// Log - %TEMP%\scope_transmit.log, the same habit as ScopeTap's own log
// ---------------------------------------------------------------------------

std::mutex g_LogMutex;

void Log(const char* p_Format, ...)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    static FILE* file = nullptr;
    if (!file)
    {
        const char* tmp = std::getenv("TEMP");
        const std::string path = std::string(tmp ? tmp : ".") + "\\scope_transmit.log";
        file = std::fopen(path.c_str(), "a");
        if (!file) return;
    }

#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(file, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#endif
    va_list args;
    va_start(args, p_Format);
    std::vfprintf(file, p_Format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

// ---------------------------------------------------------------------------
// The scope worker - one per process, shared by every instance
// ---------------------------------------------------------------------------
//
// One, because Premiere makes an instance per monitor (Program and Source)
// and the app reads one block: whichever monitor is pushing frames is the one
// the scopes show, the way a hardware output follows whatever the user is
// looking at.

struct Frame
{
    std::vector<float> rgba;     // float RGBA, row 0 = bottom, as ScopeEngine reads
    int      width  = 0;
    int      height = 0;
    double   timelineTime = 0.0; // frames from the start of the sequence
    uint32_t instanceId = 0;

    // How Premiere shows that position - see ScopeControl.h. tcFps 0 when
    // the instance has no sequence behind it (a Source Monitor clip).
    uint32_t tcFps = 0;
    bool     tcDropFrame = false;
    int32_t  tcStartFrame = 0;
};

class ScopeWorker
{
public:
    // Shutdown() stops the thread in the normal course. This only runs if the
    // module is torn down without it, during DLL unload, where joining would
    // deadlock on the loader lock - and a still-joinable std::thread would
    // call std::terminate and take Premiere down with it.
    ~ScopeWorker()
    {
        if (m_Thread.joinable()) m_Thread.detach();
    }

    // Hands out a buffer to fill. Never blocks on the reduction.
    std::unique_ptr<Frame> Acquire()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty())
        {
            std::unique_ptr<Frame> f = std::move(m_Free.back());
            m_Free.pop_back();
            return f;
        }
        return std::make_unique<Frame>();
    }

    // Queues a filled buffer, replacing one still waiting - that one is
    // skipped, which is the right call for a scope: it should show now.
    void Submit(std::unique_ptr<Frame> p_Frame)
    {
        EnsureRunning();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Pending)
            {
                m_Free.push_back(std::move(m_Pending));
                ++m_Skipped;
            }
            m_Pending = std::move(p_Frame);
            ++m_Submitted;
        }
        m_Wake.notify_one();
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stopping = true;
        }
        m_Wake.notify_one();
        if (m_Thread.joinable()) m_Thread.join();
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopping = false;
        m_Pending.reset();
        m_Free.clear();
    }

    void Counters(uint64_t& p_Submitted, uint64_t& p_Published, uint64_t& p_Skipped, double& p_LastBinMs)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        p_Submitted = m_Submitted;
        p_Published = m_Published;
        p_Skipped   = m_Skipped;
        p_LastBinMs = m_LastBinMs;
    }

private:
    void EnsureRunning()
    {
        if (m_Thread.joinable()) return;
        m_Thread = std::thread([this] { Run(); });
    }

    void Run()
    {
        scopedeck::ScopePublisher publisher;
        const bool started = publisher.Start(scopedeck::kShmNamePremiere);
        Log("worker started, shm=%s, engine threads=%d", started ? "ok" : "FAILED", m_Engine.ThreadCount());

        scopedeck::ScopeParams params;   // Rec.709: what Premiere hands over by default
        scopedeck::ScopeResult result;

        // Preview Scale and Row Step, set in the app's Input menu - see
        // ScopeControl.h. Absent until the app has run; defaults until then.
        scopedeck::ControlChannel control;
        auto lastControlAttempt = Clock::now() - std::chrono::seconds(10);
        uint32_t loggedScale = 0xFFFFFFFFu, loggedStep = 0xFFFFFFFFu;

        // The last frame measured, kept so a settings change can be shown on
        // it straight away. Premiere sends nothing while the playhead is
        // parked, and waiting for the next frame meant a change made while
        // parked - the usual way to try one - showed nothing at all.
        std::unique_ptr<Frame> last;
        uint32_t lastScale = 0xFFFFFFFFu, lastStep = 0xFFFFFFFFu;   // what `last` was measured with

        for (;;)
        {
            std::unique_ptr<Frame> frame;
            {
                // Timed, not indefinite: the settings are polled here too, so
                // they are noticed - and the block attached to - with no
                // frames flowing.
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Wake.wait_for(lock, std::chrono::milliseconds(250),
                                [this] { return m_Stopping || m_Pending; });
                if (m_Stopping) break;
                frame = std::move(m_Pending);   // null on a timed wake
            }

            const auto t0 = Clock::now();

            if (!control.Block() && MillisSince(lastControlAttempt) >= 1000.0)
            {
                lastControlAttempt = Clock::now();
                control.Open(scopedeck::kControlNamePremiere);
            }

            uint32_t scalePct = scopedeck::kPreviewScaleAuto;
            uint32_t rowStep  = 1;
            if (scopedeck::ControlBlock* block = control.Block())
            {
                scalePct = block->previewScalePct.load(std::memory_order_relaxed);
                rowStep  = block->rowStep.load(std::memory_order_relaxed);
            }
            if (scalePct != scopedeck::kPreviewScaleAuto) scalePct = std::clamp(scalePct, 25u, 100u);
            rowStep = std::clamp(rowStep, 1u, 16u);
            if (scalePct != loggedScale || rowStep != loggedStep)
            {
                char scaleText[16] = "auto";
                if (scalePct != scopedeck::kPreviewScaleAuto)
                    std::snprintf(scaleText, sizeof(scaleText), "%u%%", scalePct);
                Log("settings: preview scale %s, row step %u%s", scaleText, rowStep,
                    control.Block() ? "" : " (defaults - app not seen yet)");
                loggedScale = scalePct;
                loggedStep  = rowStep;
            }
            params.rowStep = static_cast<int>(rowStep);

            if (!frame)
            {
                // A timed wake: re-measure the parked frame, but only if the
                // settings moved since it was measured.
                if (!last || (scalePct == lastScale && rowStep == lastStep)) continue;
                frame = std::move(last);
            }

            scopedeck::FrameView view;
            view.pixels   = frame->rgba.data();
            view.width    = frame->width;
            view.height   = frame->height;
            view.rowBytes = frame->width * 4 * static_cast<int>(sizeof(float));

            m_Engine.Analyse(view, params, result);

            // The preview is for looking at, not measuring. Auto caps it at
            // 1920 wide, since a full UHD preview was measured costing the tap
            // ~18 ms a frame for nothing the app can show.
            const float previewScale = (scalePct == scopedeck::kPreviewScaleAuto)
                ? std::min(1.0f, 1920.0f / static_cast<float>(frame->width))
                : static_cast<float>(scalePct) / 100.0f;
            m_Engine.BuildPreview(view, previewScale, result.preview);

            // The timecode format first, so it is in place by the time the
            // app reads the frame it belongs to.
            if (scopedeck::ControlBlock* block = control.Block())
            {
                block->tcDropFrame.store(frame->tcDropFrame ? 1u : 0u, std::memory_order_relaxed);
                block->tcStartFrame.store(frame->tcStartFrame, std::memory_order_relaxed);
                block->tcFps.store(frame->tcFps, std::memory_order_release);
            }

            if (!publisher.IsRunning()) publisher.Start(scopedeck::kShmNamePremiere);
            publisher.Publish(result, frame->timelineTime,
                              static_cast<uint32_t>(frame->width), static_cast<uint32_t>(frame->height),
                              frame->instanceId);

            const double binMs = MillisSince(t0);
            lastScale = scalePct;
            lastStep  = rowStep;

            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_Published;
            m_LastBinMs = binMs;
            if (last) m_Free.push_back(std::move(last));
            last = std::move(frame);
        }

        publisher.Stop();
        Log("worker stopped");
    }

    std::mutex              m_Mutex;
    std::condition_variable m_Wake;
    std::thread             m_Thread;
    bool                    m_Stopping = false;

    std::unique_ptr<Frame>              m_Pending;
    std::vector<std::unique_ptr<Frame>> m_Free;

    scopedeck::ScopeEngine m_Engine;   // touched by the worker thread only

    uint64_t m_Submitted = 0;
    uint64_t m_Published = 0;
    uint64_t m_Skipped   = 0;
    double   m_LastBinMs = 0.0;
};

ScopeWorker g_Worker;

// ---------------------------------------------------------------------------
// Plugin and instance state
// ---------------------------------------------------------------------------

struct PluginData
{
    SPBasicSuite*           sp   = nullptr;
    PrSDKPPixSuite*         ppix = nullptr;
    PrSDKTimeSuite*         time = nullptr;
    PrSDKSequenceInfoSuite* seq  = nullptr;   // start timecode and drop-frame
    PrTime                  ticksPerSecond = 0;
};

struct InstanceData
{
    PluginData* plugin = nullptr;
    csSDK_int32 id = 0;
    PrTime      frameDuration = 0;   // ticks per frame
    double      lastTimelineTime = 0.0;

    uint64_t    pushed = 0;
    double      convertMs = 0.0;
    Clock::time_point lastReport = Clock::now();
    PrPixelFormat lastFormat = PrPixelFormat_Invalid;
    int         lastWidth = 0, lastHeight = 0;

    PrTimelineID timeline = 0;         // 0: not a sequence (e.g. a Source Monitor clip)
    uint32_t    tcFps = 0;             // nominal timecode rate, 0 = no timecode
    bool        tcDropFrame = false;
    int32_t     tcStartFrame = 0;
    bool        tcLogged = false;
};

// Premiere's two uncompressed BGRA layouts, into ScopeEngine's float RGBA.
// Rows keep their order: Premiere's uncompressed formats already start at the
// lower-left corner, which is the orientation ScopeEngine expects.
bool ConvertToRgba(const char* p_Pixels, csSDK_int32 p_RowBytes, int p_W, int p_H,
                   PrPixelFormat p_Format, std::vector<float>& p_Out)
{
    p_Out.resize(static_cast<size_t>(p_W) * p_H * 4);
    float* dst = p_Out.data();

    if (p_Format == PrPixelFormat_BGRA_4444_32f)
    {
        for (int y = 0; y < p_H; ++y)
        {
            const float* src = reinterpret_cast<const float*>(p_Pixels + static_cast<ptrdiff_t>(y) * p_RowBytes);
            for (int x = 0; x < p_W; ++x, src += 4, dst += 4)
            {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        return true;
    }

    if (p_Format == PrPixelFormat_BGRA_4444_8u)
    {
        static float lut[256];
        static bool  lutReady = false;
        if (!lutReady)
        {
            for (int i = 0; i < 256; ++i) lut[i] = static_cast<float>(i) / 255.0f;
            lutReady = true;
        }
        for (int y = 0; y < p_H; ++y)
        {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(p_Pixels + static_cast<ptrdiff_t>(y) * p_RowBytes);
            for (int x = 0; x < p_W; ++x, src += 4, dst += 4)
            {
                dst[0] = lut[src[2]];
                dst[1] = lut[src[1]];
                dst[2] = lut[src[0]];
                dst[3] = lut[src[3]];
            }
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// tmModule
// ---------------------------------------------------------------------------

tmResult Startup(tmStdParms* ioStdParms, tmPluginInfo* outPluginInfo)
{
    // The persistent identity Premiere stores the Playback preference under.
    // Never change it, or every user's "Scope Deck" device setting is lost.
    std::strncpy(outPluginInfo->outIdentifier.mGUID, "5C0D3C4E-7A11-4E5B-9C3D-5C0DEDEC0001",
                 sizeof(outPluginInfo->outIdentifier.mGUID) - 1);
    outPluginInfo->outPriority            = 0;
    outPluginInfo->outAudioAvailable      = kPrFalse;
    outPluginInfo->outAudioDefaultEnabled = kPrFalse;
    outPluginInfo->outClockAvailable      = kPrFalse;   // never drives playback
    outPluginInfo->outVideoAvailable      = kPrTrue;
    outPluginInfo->outVideoDefaultEnabled = kPrFalse;   // the user opts in, in Preferences > Playback
    outPluginInfo->outHideInUI            = kPrFalse;
    outPluginInfo->outHasSetup            = kPrFalse;
    outPluginInfo->outInterfaceVersion    = tmInterfaceVersion;
    outPluginInfo->outPushAudioAvailable  = kPrFalse;
    outPluginInfo->outHasStreaming        = kPrFalse;

    const wchar_t* name = L"Scope Deck";
    size_t i = 0;
    for (; name[i] && i < 255; ++i) outPluginInfo->outDisplayName[i] = static_cast<prUTF16Char>(name[i]);
    outPluginInfo->outDisplayName[i] = 0;

    PluginData* data = new PluginData;
    data->sp = ioStdParms->piSuites->utilFuncs->getSPBasicSuite();
    if (data->sp)
    {
        data->sp->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                               const_cast<const void**>(reinterpret_cast<void**>(&data->ppix)));
        data->sp->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                               const_cast<const void**>(reinterpret_cast<void**>(&data->time)));
        data->sp->AcquireSuite(kPrSDKSequenceInfoSuite, kPrSDKSequenceInfoSuiteVersion,
                               const_cast<const void**>(reinterpret_cast<void**>(&data->seq)));
    }
    if (data->time) data->time->GetTicksPerSecond(&data->ticksPerSecond);
    ioStdParms->ioPrivatePluginData = data;

    Log("---- ScopeTransmit started (interface v%d)  ppix=%s time=%s seq=%s ticks/s=%lld ----",
        tmInterfaceVersion, data->ppix ? "ok" : "MISSING", data->time ? "ok" : "MISSING",
        data->seq ? "ok" : "MISSING", static_cast<long long>(data->ticksPerSecond));
    return tmResult_Success;
}

tmResult Shutdown(tmStdParms* ioStdParms)
{
    g_Worker.Stop();

    PluginData* data = static_cast<PluginData*>(ioStdParms->ioPrivatePluginData);
    if (data)
    {
        if (data->sp)
        {
            if (data->ppix) data->sp->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
            if (data->time) data->sp->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
            if (data->seq)  data->sp->ReleaseSuite(kPrSDKSequenceInfoSuite, kPrSDKSequenceInfoSuiteVersion);
        }
        delete data;
    }
    ioStdParms->ioPrivatePluginData = nullptr;
    Log("---- ScopeTransmit shut down ----");
    return tmResult_Success;
}

tmResult CreateInstance(const tmStdParms* inStdParms, tmInstance* ioInstance)
{
    InstanceData* inst = new InstanceData;
    inst->plugin = static_cast<PluginData*>(inStdParms->ioPrivatePluginData);
    inst->id = ioInstance->inInstanceID;
    inst->frameDuration = ioInstance->inVideoFrameRate;
    inst->timeline = ioInstance->inTimelineID;
    ioInstance->ioPrivateInstanceData = inst;

    const double fps = (inst->frameDuration > 0 && inst->plugin && inst->plugin->ticksPerSecond > 0)
        ? static_cast<double>(inst->plugin->ticksPerSecond) / static_cast<double>(inst->frameDuration) : 0.0;
    Log("instance %d created: %dx%d  %.3f fps  video=%d timeline=%lld play=%d",
        inst->id, ioInstance->inVideoWidth, ioInstance->inVideoHeight, fps,
        static_cast<int>(ioInstance->inHasVideo), static_cast<long long>(ioInstance->inTimelineID),
        static_cast<int>(ioInstance->inPlayID));
    return tmResult_Success;
}

tmResult DisposeInstance(const tmStdParms* /*inStdParms*/, tmInstance* ioInstance)
{
    InstanceData* inst = static_cast<InstanceData*>(ioInstance->ioPrivateInstanceData);
    if (inst) Log("instance %d disposed after %llu frames", inst->id, static_cast<unsigned long long>(inst->pushed));
    delete inst;
    ioInstance->ioPrivateInstanceData = nullptr;
    return tmResult_Success;
}

tmResult QueryVideoMode(const tmStdParms* /*inStdParms*/, const tmInstance* /*inInstance*/,
                        csSDK_int32 inQueryIterationIndex, tmVideoMode* outVideoMode)
{
    if (inQueryIterationIndex > 0) return tmResult_ErrorUnsupported;

    // Any size: at a fractional playback resolution Premiere then sends the
    // smaller frame rather than scaling it back up, which is cheaper to scope
    // and no less true. 32-bit float BGRA, so super-whites and sub-blacks
    // survive - the thing a scope is read to find.
    outVideoMode->outWidth       = 0;
    outVideoMode->outHeight      = 0;
    outVideoMode->outPARNum      = 0;
    outVideoMode->outPARDen      = 0;
    outVideoMode->outFieldType   = prFieldsAny;
    outVideoMode->outPixelFormat = PrPixelFormat_BGRA_4444_32f;
    outVideoMode->outLatency     = 0;
    return tmResult_Success;
}

tmResult ActivateDeactivate(const tmStdParms* /*inStdParms*/, const tmInstance* inInstance,
                            PrActivationEvent inActivationEvent, prBool inAudioActive, prBool inVideoActive)
{
    Log("instance %d activate event=%d audio=%d video=%d", inInstance->inInstanceID,
        static_cast<int>(inActivationEvent), static_cast<int>(inAudioActive), static_cast<int>(inVideoActive));
    return tmResult_Success;
}

tmResult PushVideo(const tmStdParms* /*inStdParms*/, const tmInstance* inInstance, const tmPushVideo* inPushVideo)
{
    InstanceData* inst = static_cast<InstanceData*>(inInstance->ioPrivateInstanceData);
    PluginData* plugin = inst ? inst->plugin : nullptr;
    PrSDKPPixSuite* ppix = plugin ? plugin->ppix : nullptr;

    if (!ppix || !inPushVideo || inPushVideo->inFrameCount == 0)
        return tmResult_Success;

    const auto t0 = Clock::now();
    PPixHand frame = inPushVideo->inFrames[0].inFrame;

    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    prRect bounds{};
    PrPixelFormat format = PrPixelFormat_Invalid;
    ppix->GetPixels(frame, PrPPixBufferAccess_ReadOnly, &pixels);
    ppix->GetRowBytes(frame, &rowBytes);
    ppix->GetBounds(frame, &bounds);
    ppix->GetPixelFormat(frame, &format);

    const int w = std::abs(bounds.right - bounds.left);
    const int h = std::abs(bounds.bottom - bounds.top);

    if (inPushVideo->inTime >= 0 && inst->frameDuration > 0)
        inst->lastTimelineTime = static_cast<double>(inPushVideo->inTime) / static_cast<double>(inst->frameDuration);

    // The timecode format, re-read every frame - two suite calls - so a change
    // to the sequence's start time or drop-frame setting shows up straight
    // away. The position itself is inTime above, which counts from the start
    // of the sequence; Premiere displays it offset by the zero point.
    if (inst->timeline != 0 && plugin->seq && inst->frameDuration > 0 && plugin->ticksPerSecond > 0)
    {
        PrTime zeroPoint = 0;
        prBool drop = kPrFalse;
        plugin->seq->GetZeroPoint(inst->timeline, &zeroPoint);
        plugin->seq->GetTimecodeDropFrame(inst->timeline, &drop);

        const uint32_t fps = static_cast<uint32_t>(std::llround(
            static_cast<double>(plugin->ticksPerSecond) / static_cast<double>(inst->frameDuration)));
        const int32_t start = static_cast<int32_t>(std::llround(
            static_cast<double>(zeroPoint) / static_cast<double>(inst->frameDuration)));

        if (!inst->tcLogged || fps != inst->tcFps || start != inst->tcStartFrame || (drop != 0) != inst->tcDropFrame)
        {
            Log("instance %d timecode: %u fps %s, sequence starts at frame %d (zero point %lld ticks), first push t=%lld ticks",
                inst->id, fps, drop ? "drop-frame" : "non-drop", start, static_cast<long long>(zeroPoint),
                static_cast<long long>(inPushVideo->inTime));
            inst->tcLogged = true;
        }
        inst->tcFps        = fps;
        inst->tcDropFrame  = drop != 0;
        inst->tcStartFrame = start;
    }

    bool converted = false;
    std::unique_ptr<Frame> out;
    if (pixels && w > 0 && h > 0)
    {
        out = g_Worker.Acquire();
        converted = ConvertToRgba(pixels, rowBytes, w, h, format, out->rgba);
        out->width = w;
        out->height = h;
        out->timelineTime = inst->lastTimelineTime;
        out->instanceId = 1000u + static_cast<uint32_t>(inst->id);
        out->tcFps        = inst->tcFps;
        out->tcDropFrame  = inst->tcDropFrame;
        out->tcStartFrame = inst->tcStartFrame;
    }

    // Ours to dispose, every one of them - including any extra labelled
    // streams, which this plugin never asks for but must not leak.
    for (csSDK_size_t i = 0; i < inPushVideo->inFrameCount; ++i)
        ppix->Dispose(inPushVideo->inFrames[i].inFrame);

    if (format != inst->lastFormat || w != inst->lastWidth || h != inst->lastHeight)
    {
        const uint32_t f = static_cast<uint32_t>(format);
        Log("instance %d frames now %dx%d  format '%c%c%c%c' (0x%08x)  rowBytes=%d  %s",
            inst->id, w, h, char(f & 0xFF), char((f >> 8) & 0xFF), char((f >> 16) & 0xFF), char(f >> 24),
            f, static_cast<int>(rowBytes), converted ? "" : "UNSUPPORTED - not scoped");
        inst->lastFormat = format;
        inst->lastWidth = w;
        inst->lastHeight = h;
    }

    if (converted) g_Worker.Submit(std::move(out));

    ++inst->pushed;
    inst->convertMs = MillisSince(t0);

    // A line every two seconds while frames flow - enough to read the rate
    // and cost off, not one per frame.
    if (MillisSince(inst->lastReport) >= 2000.0)
    {
        inst->lastReport = Clock::now();
        uint64_t submitted = 0, published = 0, skipped = 0;
        double binMs = 0.0;
        g_Worker.Counters(submitted, published, skipped, binMs);
        Log("instance %d  t=%.1f  mode=%d  pushed=%llu  push=%.2fms  |  worker submitted=%llu published=%llu skipped=%llu scopes=%.2fms",
            inst->id, inst->lastTimelineTime, static_cast<int>(inPushVideo->inPlayMode),
            static_cast<unsigned long long>(inst->pushed), inst->convertMs,
            static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(published),
            static_cast<unsigned long long>(skipped), binMs);
    }
    return tmResult_Success;
}

} // namespace

extern "C" DllExport PREMPLUGENTRY xTransmitEntry(csSDK_int32 /*inInterfaceVersion*/, prBool inLoadModule,
                                                  piSuitesPtr /*piSuites*/, tmModule* outModule)
{
    if (inLoadModule)
    {
        // Everything not filled in stays 0, which the SDK defines as "not
        // supported": no audio, no clock, no setup dialog, no streaming.
        std::memset(outModule, 0, sizeof(*outModule));
        outModule->Startup            = Startup;
        outModule->Shutdown           = Shutdown;
        outModule->CreateInstance     = CreateInstance;
        outModule->DisposeInstance    = DisposeInstance;
        outModule->QueryVideoMode     = QueryVideoMode;
        outModule->ActivateDeactivate = ActivateDeactivate;
        outModule->PushVideo          = PushVideo;
    }
    return tmResult_Success;
}
