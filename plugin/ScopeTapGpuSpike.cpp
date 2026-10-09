// Scope Tap GPU Spike - a throwaway measurement plugin, not a scope.
//
// Deliberately a SEPARATE plugin from ScopeTap rather than a mode inside it.
// ScopeTap's whole design premise is that the code which could take Resolve
// down with it stays small (see the header of plugin/ScopeTap.cpp); the point
// of this spike is to find out whether that premise can safely be relaxed, and
// finding that out must not put the working tap at risk. Install both, put this
// one on a test node, uninstall it when the questions are answered.
//
// It publishes nothing the app can see - its publish leg writes into a scratch
// mapping of its own, never the live block. It answers seven questions that
// cannot be answered by reasoning about the OFX spec, only by running on real
// hardware inside Resolve:
//
//   1. Does the host actually set CudaEnabled, and is kOfxImagePropData then a
//      genuine device pointer? (cudaPointerGetAttributes, logged per render.)
//   2. Is a CUDA stream supplied, i.e. does declaring CudaStreamSupported buy
//      anything on this Resolve version?
//   3. What does the mandatory passthrough cost on-card, against the measured
//      10.5ms/frame it costs as a host memcpy at UHD?
//   4. What does scope-shaped scatter cost - including a deterministic worst
//      case, not just whatever footage happens to be on the timeline?
//   5. Can the preview leg live on the GPU (downscale on card, small D2H)
//      instead of reading back a 127MB float frame?
//   6. Will CUDA page-lock a shared-memory view, making the publish leg a
//      direct async D2H into the slot? (Yes - 0.217 ms for 11.7 MB.)
//   7. Can the render thread wait for that copy before flipping the seqlock?
//      (No - 9.57 ms, nearly all of it queue latency behind Resolve's own
//      work. The publish handshake belongs on its own thread.)
//
// The Work parameter is sized to the three-way playback comparison: run each
// setting against Resolve's own dropped-frame counter, on cached playback,
// uncached playback with a heavy grade, and scrubbing.

#include "ScopeTapGpuSpike.h"

#include "spike/ScopeGpuSpike.h"

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

#define kPluginName "Scope Tap GPU Spike"
#define kPluginGrouping "Scope Deck"
// A plain constant, deliberately not a line-continued #define. This is the
// most-edited line in the file - version notes, scope changes - and
// backslash continuation is the most fragile construct available: drop one
// backslash while editing prose and the build breaks somewhere unrelated,
// which is exactly what happened on 2026-09-21. A string literal cannot lose
// a backslash it does not have.
static const char* const kPluginDescription =
    "Measurement spike. Passes the image through and times GPU work against it. "
    "Publishes nothing the app reads - not a scope. Remove once the GPU "
    "questions are answered.";
#define kPluginIdentifier "group.zen.ScopeDeck.ScopeTapGpuSpike"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 1

// Same reasoning as the real tap: a scope reads whole frames.
#define kSupportsTiles false
#define kSupportsMultiResolution false
#define kSupportsMultipleClipPARs false

#define kParamWork           "spikeWork"
#define kParamScatterContent "scatterContent"
#define kParamPreviewScale   "spikePreviewScale"
#define kParamAtomics        "atomicMode"
#define kParamPublish        "publishMode"
#define kParamLogEvery       "logEvery"

////////////////////////////////////////////////////////////////////////////////
// Logging - its own file, so spike noise never mixes with the real tap's log.

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
    return std::string(tmp) + "\\scope_tap_gpu_spike.log";
#else
    return "/tmp/scope_tap_gpu_spike.log";
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

// cudaMemoryType, spelled out so the log is readable without the CUDA headers.
const char* PointerKindName(int p_Kind)
{
    switch (p_Kind)
    {
        case 0:  return "unregistered";   // cudaMemoryTypeUnregistered - PLAIN HOST MEMORY
        case 1:  return "host";           // cudaMemoryTypeHost - pinned host
        case 2:  return "device";         // cudaMemoryTypeDevice - what we want
        case 3:  return "managed";        // cudaMemoryTypeManaged
        default: return "query-failed";
    }
}

const char* WorkName(int p_Work)
{
    switch (p_Work)
    {
        case 0:  return "passthrough";
        case 1:  return "scatter";
        case 2:  return "scatter+preview";
        case 3:  return "scatter+preview+publish";
        default: return "?";
    }
}

const char* ContentName(int p_Content)
{
    switch (p_Content)
    {
        case 0:  return "image";
        case 1:  return "worstcase";
        case 2:  return "uniform";
        default: return "?";
    }
}

const char* PublishName(int p_Mode)
{
    switch (p_Mode)
    {
        case 0:  return "off";
        case 1:  return "pageable";
        case 2:  return "pinned";
        case 3:  return "pinned+sync";
        default: return "?";
    }
}

// What the driver said about pinning the mapped slot view - the one thing this
// leg exists to find out.
const char* RegisteredName(int p_State)
{
    switch (p_State)
    {
        case 1:  return "pinned";
        case 0:  return "pageable";
        default: return "REGISTER-FAILED";
    }
}

const char* AtomicsName(int p_Mode)
{
    return p_Mode == 1 ? "aggregated" : "plain";
}

const char* BitDepthName(OFX::BitDepthEnum p_Depth)
{
    switch (p_Depth)
    {
        case OFX::eBitDepthUByte:  return "ubyte";
        case OFX::eBitDepthUShort: return "ushort";
        case OFX::eBitDepthHalf:   return "half";
        case OFX::eBitDepthFloat:  return "float";
        case OFX::eBitDepthNone:   return "none";
        default:                   return "custom/unknown";
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class SpikePlugin : public OFX::ImageEffect
{
public:
    explicit SpikePlugin(OfxImageEffectHandle p_Handle);

    virtual void render(const OFX::RenderArguments& p_Args);
    virtual bool isIdentity(const OFX::IsIdentityArguments& p_Args,
                            OFX::Clip*& p_IdentityClip, double& p_IdentityTime);

private:
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;

    OFX::ChoiceParam* m_Work;
    OFX::ChoiceParam* m_ScatterContent;
    OFX::ChoiceParam* m_PreviewScale;
    OFX::ChoiceParam* m_Atomics;
    OFX::ChoiceParam* m_Publish;
    OFX::IntParam*    m_LogEvery;

    scopespike::SpikeContext m_Spike;
    unsigned int m_InstanceId;
};

SpikePlugin::SpikePlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
    , m_InstanceId(g_nextInstanceId++)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_Work           = fetchChoiceParam(kParamWork);
    m_ScatterContent = fetchChoiceParam(kParamScatterContent);
    m_PreviewScale   = fetchChoiceParam(kParamPreviewScale);
    m_Atomics        = fetchChoiceParam(kParamAtomics);
    m_Publish        = fetchChoiceParam(kParamPublish);
    m_LogEvery       = fetchIntParam(kParamLogEvery);

    Log("---- GPU spike v%d.%d instance %u created  cuda=%s ----",
        kPluginVersionMajor, kPluginVersionMinor, m_InstanceId,
        scopespike::SpikeContext::DeviceDescription());
}

bool SpikePlugin::isIdentity(const OFX::IsIdentityArguments& /*p_Args*/,
                             OFX::Clip*& /*p_IdentityClip*/, double& /*p_IdentityTime*/)
{
    // Same reason as the real tap: an identity node gets skipped and never sees
    // a frame, which would make the spike measure nothing at all.
    return false;
}

void SpikePlugin::render(const OFX::RenderArguments& p_Args)
{
    const auto tEnter = std::chrono::steady_clock::now();
    const unsigned long long callNo = ++g_renderCount;

    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));
    const double msFetch = MillisSince(tEnter);

    if (!dst.get() || !src.get())
    {
        Log("#%llu  inst=%u  FETCH FAILED  t=%.1f", callNo, m_InstanceId, p_Args.time);
        return;
    }

    const OfxRectI& rw = p_Args.renderWindow;
    const int width  = rw.x2 - rw.x1;
    const int height = rw.y2 - rw.y1;
    if (width <= 0 || height <= 0) return;

    int logEvery = 1;
    m_LogEvery->getValueAtTime(p_Args.time, logEvery);
    if (logEvery < 1) logEvery = 1;
    const bool shouldLog = (callNo % static_cast<unsigned long long>(logEvery)) == 0;

    int work = 0, content = 0, scaleIndex = 2, atomics = 0, publish = 0;
    m_Work->getValueAtTime(p_Args.time, work);
    m_ScatterContent->getValueAtTime(p_Args.time, content);
    m_Atomics->getValueAtTime(p_Args.time, atomics);
    m_Publish->getValueAtTime(p_Args.time, publish);
    m_PreviewScale->getValueAtTime(p_Args.time, scaleIndex);
    static const float kScales[] = { 1.0f, 0.5f, 0.25f };
    const float previewScale = kScales[(scaleIndex >= 0 && scaleIndex < 3) ? scaleIndex : 2];

    const OfxRectI srcBounds = src->getBounds();
    const OfxRectI dstBounds = dst->getBounds();
    const size_t srcRowBytes = static_cast<size_t>(src->getRowBytes());
    const size_t dstRowBytes = static_cast<size_t>(dst->getRowBytes());

    // Base pointers are for the images' own bounds origin; the render window may
    // in principle be a subrect (it should not be, with tiles off - logged below
    // so a surprise is visible rather than silently mis-addressed).
    char* srcBase = static_cast<char*>(const_cast<void*>(src->getPixelData()));
    char* dstBase = static_cast<char*>(dst->getPixelData());
    const size_t pixelBytes = 4 * sizeof(float);

    char* srcOrigin = srcBase + static_cast<size_t>(rw.y1 - srcBounds.y1) * srcRowBytes
                              + static_cast<size_t>(rw.x1 - srcBounds.x1) * pixelBytes;
    char* dstOrigin = dstBase + static_cast<size_t>(rw.y1 - dstBounds.y1) * dstRowBytes
                              + static_cast<size_t>(rw.x1 - dstBounds.x1) * pixelBytes;

    scopespike::SpikeTimings timings;
    std::memset(&timings, 0, sizeof(timings));
    bool usedGpu = false;

    if (p_Args.isEnabledCudaRender)
    {
        scopespike::SpikeArgs args;
        args.srcDevice      = srcOrigin;
        args.dstDevice      = dstOrigin;
        args.srcRowBytes    = srcRowBytes;
        args.dstRowBytes    = dstRowBytes;
        args.width          = width;
        args.height         = height;
        args.stream         = p_Args.pCudaStream;
        args.work           = work;
        args.scatterContent = content;
        args.atomicMode     = atomics;
        args.publishMode    = publish;
        args.previewScale   = (work >= scopespike::kWorkPlusPreview) ? previewScale : 0.0f;

        usedGpu = m_Spike.Run(args, timings);

        if (!usedGpu)
        {
            // The pointers are device memory, so there is no host fallback to
            // take here - a memcpy would fault. Fail loudly and let Resolve show
            // an error rather than write garbage into the output buffer.
            Log("#%llu  inst=%u  GPU FAILED: %s", callNo, m_InstanceId, timings.error);
            OFX::throwSuiteStatusException(kOfxStatFailed);
            return;
        }
    }
    else
    {
        // CPU path: Fusion page, GPU processing disabled in prefs, or a host
        // that declined. Plain host passthrough, matching the real tap.
        const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
        for (int y = 0; y < height; ++y)
            std::memcpy(dstOrigin + static_cast<size_t>(y) * dstRowBytes,
                        srcOrigin + static_cast<size_t>(y) * srcRowBytes, rowBytes);
    }

    if (!shouldLog) return;

    Log("#%llu  inst=%u  t=%.1f  %dx%d  %s  "
        "work=%s content=%s atomics=%s publish=%s(%s %zuKB wait=%.3fms)  "
        "cuda=%d ocl=%d metal=%d stream=%s draft=%d interactive=%d  "
        "depth=%s comps=%d srcRB=%zu dstRB=%zu  "
        "rw=[%d,%d,%d,%d] srcB=[%d,%d,%d,%d] dstB=[%d,%d,%d,%d] scale=%.3f  "
        "ptr src=%s(dev %d) dst=%s(dev %d)  "
        "fetch=%.2fms host=%.2fms | gpu copy=%.3f scatter=%.3f preview=%.3f publish=%.3f total=%.3f  "
        "vram free=%zuMB/%zuMB",
        callNo, m_InstanceId, p_Args.time, width, height,
        usedGpu ? "GPU" : "cpu",
        WorkName(work), ContentName(content), AtomicsName(atomics),
        PublishName(publish), RegisteredName(timings.publishRegistered),
        timings.publishBytes / 1024, timings.msPublishWait,
        p_Args.isEnabledCudaRender ? 1 : 0,
        p_Args.isEnabledOpenCLRender ? 1 : 0,
        p_Args.isEnabledMetalRender ? 1 : 0,
        p_Args.pCudaStream ? "yes" : "NO",
        p_Args.renderQualityDraft ? 1 : 0,
        p_Args.interactiveRenderStatus ? 1 : 0,
        BitDepthName(src->getPixelDepth()),
        static_cast<int>(src->getPixelComponents()),
        srcRowBytes, dstRowBytes,
        rw.x1, rw.y1, rw.x2, rw.y2,
        srcBounds.x1, srcBounds.y1, srcBounds.x2, srcBounds.y2,
        dstBounds.x1, dstBounds.y1, dstBounds.x2, dstBounds.y2,
        p_Args.renderScale.x,
        PointerKindName(timings.pointerKindSrc), timings.deviceSrc,
        PointerKindName(timings.pointerKindDst), timings.deviceDst,
        msFetch, MillisSince(tEnter),
        timings.msPassthrough, timings.msScatter, timings.msPreview, timings.msPublish,
        timings.msTotalGpu,
        timings.vramFreeBytes / (1024 * 1024), timings.vramTotalBytes / (1024 * 1024));
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

SpikeFactory::SpikeFactory()
    : OFX::PluginFactoryHelper<SpikeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void SpikeFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
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
    p_Desc.setNoSpatialAwareness(false);

    // The whole reason this plugin exists. CPU render stays supported (it is the
    // default and the Fusion-page path), and nothing is vendor-sniffed: render()
    // keys off p_Args.isEnabledCudaRender per call, never off the device name.
    p_Desc.setSupportsCudaRender(true);
    p_Desc.setSupportsCudaStream(true);
}

void SpikeFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
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

    // These three settings are the (a)/(b)/(c) of the playback comparison.
    ChoiceParamDescriptor* work = p_Desc.defineChoiceParam(kParamWork);
    work->setLabels("Work", "Work", "Work");
    work->setHint("How much GPU work to enqueue per frame. Run each against "
                  "Resolve's dropped-frame counter.");
    work->appendOption("Passthrough only");
    work->appendOption("Passthrough + scatter");
    work->appendOption("Passthrough + scatter + preview");
    work->appendOption("Passthrough + scatter + preview + publish");
    work->setDefault(0);
    page->addChild(*work);

    ChoiceParamDescriptor* content = p_Desc.defineChoiceParam(kParamScatterContent);
    content->setLabels("Scatter Content", "Scatter Content", "Scatter Content");
    content->setHint("What the histogram increments target. 'Worst case' sends every "
                     "pixel to one cell, bounding atomic contention deterministically "
                     "instead of hoping the timeline happens to contain a crushed frame.");
    content->appendOption("From image");
    content->appendOption("Worst case (one bin)");
    content->appendOption("Uniform (spread)");
    content->setDefault(0);
    page->addChild(*content);

    ChoiceParamDescriptor* scale = p_Desc.defineChoiceParam(kParamPreviewScale);
    scale->setLabels("Preview Scale", "Preview Scale", "Preview Scale");
    scale->setHint("Resolution of the on-card preview downscale and its D2H copy.");
    scale->appendOption("100%");
    scale->appendOption("50%");
    scale->appendOption("25%");
    scale->setDefault(2);
    page->addChild(*scale);

    ChoiceParamDescriptor* atomics = p_Desc.defineChoiceParam(kParamAtomics);
    atomics->setLabels("Atomics", "Atomics", "Atomics");
    atomics->setHint("How histogram increments reach global memory. 'Warp-aggregated' "
                     "collapses same-cell increments within a warp into one atomic, "
                     "which is the standard fix for the contention the worst case "
                     "measures. Needs sm_70+; older cards silently use the plain path.");
    atomics->appendOption("Plain");
    atomics->appendOption("Warp-aggregated");
    atomics->setDefault(0);
    page->addChild(*atomics);

    ChoiceParamDescriptor* publish = p_Desc.defineChoiceParam(kParamPublish);
    publish->setLabels("Publish", "Publish", "Publish");
    publish->setHint("Only active at Work = '...+ publish'. Copies a full wire-format "
                     "slot's worth of bins (5.25 MB) plus the preview from device to "
                     "host each frame, into a real shared-memory view - the one leg of "
                     "the GPU path that still has to cross into system memory. "
                     "'Pageable' is the plain copy, which CUDA stages and which blocks "
                     "the calling thread, so its true cost lands in the log's host= "
                     "column. 'Pinned' asks the driver to page-lock the mapped view "
                     "first; if it refuses, the log says REGISTER-FAILED rather than "
                     "quietly measuring something else.");
    publish->appendOption("Off");
    publish->appendOption("Pageable");
    publish->appendOption("Pinned (host-registered)");
    publish->appendOption("Pinned + sync (seqlock handshake)");
    publish->setDefault(0);
    page->addChild(*publish);

    IntParamDescriptor* logEvery = p_Desc.defineIntParam(kParamLogEvery);
    logEvery->setLabels("Log Every N", "Log Every N", "Log Every N");
    logEvery->setHint("Write one log line per N renders. 1 while proving the contract; "
                      "turn up for long playback runs so logging is not itself the cost.");
    logEvery->setDefault(1);
    logEvery->setRange(1, 1000);
    logEvery->setDisplayRange(1, 100);
    page->addChild(*logEvery);
}

ImageEffect* SpikeFactory::createInstance(OfxImageEffectHandle p_Handle, ContextEnum /*p_Context*/)
{
    return new SpikePlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static SpikeFactory spike;
    p_FactoryArray.push_back(&spike);
}
