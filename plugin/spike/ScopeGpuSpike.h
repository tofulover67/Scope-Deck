// CUDA side of the GPU spike, behind a plain C++ interface so the OFX plugin
// itself never includes a CUDA header and can be compiled by MSVC alone.
//
// This is a measurement instrument, not a scope. Nothing it computes reaches
// the Scope Deck app - the bins it fills are written and thrown away, and the
// publish leg copies them into a scratch mapping of its own that no reader
// ever opens. Its only job is to answer, on real hardware inside Resolve:
//
//   1. Does the OFX CUDA contract work here at all - are the image pointers
//      genuinely device memory, is a stream supplied, is the passthrough clean?
//   2. What does scope-shaped scatter actually cost, including its worst case?
//   3. Can the preview leg survive on the GPU path (downscale on card, small
//      D2H) without a full-frame readback?
//   4. Will CUDA page-lock a shared-memory view, so the publish leg can be a
//      direct async D2H into the slot rather than a staged copy? (Yes,
//      measured 2026-09-21 - 0.217 ms for 11.7 MB.)
//   5. Can the render thread wait for that copy before advertising the slot?
//      (No: 9.57 ms of waiting for 1.0 ms of work, because synchronising also
//      waits for everything Resolve queued ahead of us. kPublishPinnedSync
//      exists to prove that, not to be used.)
//
// Everything is allocated once per instance and reused - see the VRAM note in
// the plan; a per-frame allocation would show up as Resolve's "GPU memory full"
// long before it showed up as a slow frame.

#pragma once

#include <cstddef>
#include <cstdint>

namespace scopespike
{

// Mirrors the real wire format (core/ScopeTypes.h) so the atomic contention
// pattern this measures is the one the real kernel would hit. Kept as its own
// constants rather than including ScopeTypes.h: the spike must not grow a
// dependency on the format it is only pretending to fill.
constexpr uint32_t kWaveformColumns = 512;
constexpr uint32_t kWaveformLevels  = 256;
constexpr uint32_t kPlaneCount      = 4;
constexpr uint32_t kVectorscopeSize = 256;
constexpr uint32_t kBandCount       = 3;

constexpr float kBinRangeLow  = -0.080f;
constexpr float kBinRangeHigh =  1.080f;
constexpr float kChromaRangeLow  = -0.75f;
constexpr float kChromaRangeHigh =  0.75f;

enum ScatterContent : int
{
    kScatterFromImage = 0,   // realistic: bins driven by actual pixel values
    kScatterWorstCase = 1,   // every pixel into one cell - maximum atomic serialisation
    kScatterUniform   = 2,   // hashed across all cells - minimum contention
};

// Whether the scatter kernel collapses same-address increments within a warp
// before touching global memory. A toggle rather than a replacement, because the
// only interesting number is the difference between the two on identical frames.
enum AtomicMode : int
{
    kAtomicsPlain      = 0,   // one global atomicAdd per pixel per plane
    kAtomicsAggregated = 1,   // __match_any_sync, one atomic per unique address per warp
};

enum SpikeWork : int
{
    kWorkPassthroughOnly = 0,
    kWorkPlusScatter     = 1,
    kWorkPlusPreview     = 2,
    kWorkPlusPublish     = 3,
};

// Byte sizes of the real wire format's arrays (core/ScopeTypes.h v10), mirrored
// here for the same reason the bin dimensions above are: the spike must not grow
// a dependency on the format it is only pretending to fill. These are not
// approximations - header 128 + bins + preview canvas sums to 30,392,448, which
// is exactly the slot size a live ScopeTap logs at startup.
constexpr size_t kWaveformBytes    = 512u * 256u * 4u * sizeof(unsigned int);   // 2,097,152
constexpr size_t kHistogramBytes   = 256u * 4u * sizeof(unsigned int);          //     4,096
constexpr size_t kVectorscopeBytes = 256u * 256u * 3u * sizeof(unsigned int);   //   786,432
constexpr size_t kTwinPeaksBytes   = 256u * 256u * 2u * sizeof(unsigned int);   //   524,288
constexpr size_t kTraceBytes       = 256u * 512u * 4u * sizeof(float);          // 2,097,152
constexpr size_t kBinBytesTotal    = kWaveformBytes + kHistogramBytes +
                                     kVectorscopeBytes + kTwinPeaksBytes + kTraceBytes;

constexpr size_t kSlotHeaderBytes    = 128;
constexpr size_t kPreviewCanvasBytes = 3840u * 2160u * 3u;                      // 24,883,200
constexpr size_t kSlotBytes          = kSlotHeaderBytes + kBinBytesTotal + kPreviewCanvasBytes;

// The mirror above is only worth anything if it still matches. 30,392,448 is the
// slot size a live ScopeTap prints in its own startup line, so this ties the
// spike's copy to an observed fact rather than to arithmetic done once.
static_assert(kSlotBytes == 30392448u,
              "slot size no longer matches the wire format ScopeTap logs - re-mirror it");

// Where the publish copies land. The real tap publishes into a shared-memory
// slot, so the destination is a mapped file view, not ordinary heap - and
// whether CUDA will pin a mapped view is exactly the open question.
enum PublishMode : int
{
    kPublishOff      = 0,
    kPublishPageable = 1,   // plain D2H into the mapped view; CUDA stages it and
                            // the host thread blocks for the duration
    kPublishPinned   = 2,   // cudaHostRegister the view first, then async D2H.
                            // If registration fails this reports it and falls
                            // back to pageable rather than pretending.
    kPublishPinnedSync = 3, // pinned, then WAIT on the render thread for the
                            // copy to land - what a tap must do before it can
                            // advertise the slot through the seqlock, unless a
                            // separate thread does the waiting instead.
};

struct SpikeArgs
{
    const void* srcDevice;      // kOfxImagePropData of the source clip
    void*       dstDevice;      // ... of the output clip
    size_t      srcRowBytes;
    size_t      dstRowBytes;
    int         width;
    int         height;

    void*       stream;         // cudaStream_t from the host, or null
    int         work;           // SpikeWork
    int         scatterContent; // ScatterContent
    int         atomicMode;     // AtomicMode
    int         publishMode;    // PublishMode
    float       previewScale;   // 0.25 = quarter resolution
};

// Filled in by Run(). Stage timings come from CUDA events recorded on the host
// stream and read back one frame late, so nothing here forces a synchronise -
// the OFX header is explicit that a plugin given a stream must never call
// cudaDeviceSynchronize(). A value of -1 means "not measured yet".
struct SpikeTimings
{
    float msPassthrough;
    float msScatter;
    float msPreview;
    float msPublish;            // device-side cost of the D2H publish copies
    float msTotalGpu;
    double msHostEnqueue;       // wall time spent in Run() on the CPU

    // Publish leg state. A pageable destination makes the D2H synchronous, so
    // its true cost shows up in msHostEnqueue rather than in msPublish - both
    // numbers are needed to read this leg honestly.
    size_t publishBytes;        // actually copied this frame
    double msPublishWait;       // render-thread wall time spent waiting for the
                                // copy to land; 0 unless kPublishPinnedSync
    int    publishRegistered;   // 1 pinned, 0 pageable, -1 registration failed
    char   publishNote[128];    // why registration failed, when it did

    int    pointerKindSrc;      // cudaMemoryType, or -1 if the query failed
    int    pointerKindDst;
    int    deviceSrc;
    int    deviceDst;

    size_t vramFreeBytes;
    size_t vramTotalBytes;

    bool   ok;
    char   error[256];
};

// One of these per plugin instance. Allocates on first Run() and holds until
// destroyed, so steady-state cost is what gets measured rather than allocation.
class SpikeContext
{
public:
    SpikeContext();
    ~SpikeContext();

    SpikeContext(const SpikeContext&) = delete;
    SpikeContext& operator=(const SpikeContext&) = delete;

    // Never throws and never returns a CUDA error to the host: on failure it
    // reports ok=false with a message and the caller falls back to the CPU
    // passthrough. A spike that can take Resolve down has failed at its job.
    bool Run(const SpikeArgs& p_Args, SpikeTimings& p_Out);

    // Human-readable device description, logged once at instance creation.
    static const char* DeviceDescription();

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace scopespike
