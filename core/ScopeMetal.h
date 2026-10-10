// Metal scope reduction, behind a plain C++ interface so nothing above this line
// includes a Metal header - the macOS counterpart of ScopeCuda.h, with the same
// two callers in the same order:
//
//   1. tools/ScopeConformance.cpp (scope_conformance_metal), which drives
//      MetalReducer with host-memory frames and diffs it against the CPU
//      reduction bit-for-bit. Correctness is settled there, in seconds, with no
//      Resolve in the loop.
//   2. plugin/ScopeTap.cpp, where the frame arrives as the id<MTLBuffer> Resolve
//      rendered into, and MetalTap reduces it on the host's own command queue.
//
// Both drive the same kernels (one MSL source, compiled at run time by the Metal
// framework - no Xcode toolchain is needed, only the Command Line Tools), so
// passing conformance means the tap's maths is the maths that was tested.
//
// What is the same as CUDA, and what is not:
//
//   - The three bit-exactness traps (column table, Cb/Cr reciprocals, FMA
//     contraction) apply unchanged. Contraction is off through
//     `#pragma METAL fp contract(off)` plus fastMathEnabled = NO, and
//     scope_conformance's own detector would catch a build where it was not.
//   - Apple silicon has unified memory. There is no page-locking and no D2H
//     over PCIe: the result buffers are MTLStorageModeShared, and the publish
//     leg is a memcpy from them into the shared-memory slot on the hub's worker
//     thread, after the command buffer completes. (The mapping itself could be
//     wrapped as an MTLBuffer and written by the kernels directly - measured to
//     work on this platform - and that is the obvious next step if the memcpy
//     ever shows up in the numbers.)
//   - The render thread encodes and commits one command buffer on the host's
//     queue and returns. Kernels read Resolve's source buffer, which is only
//     valid during render(), so they must be queued on the host's queue, in
//     order with the work that produced it. The seqlock is closed by the hub's
//     worker once the command buffer reports completion - the render thread
//     never waits, for the reasons GPU_PORT_HANDOFF.md §3 measured.
//   - Warp aggregation becomes SIMD-group aggregation: there is no
//     __match_any_sync, so the kernel loops over the distinct addresses in the
//     SIMD group with simd_ballot/simd_shuffle, which costs one iteration per
//     distinct address - nothing on flat content, where it matters. The
//     waveform's four increments per pixel go to a threadgroup-memory copy of
//     the one column the threadgroup owns and are flushed once, which removes
//     them from the device-atomic budget entirely.
//
// Every numeric decision here exists to match core/ScopeCore.cpp bit-for-bit,
// not to be independently reasonable.

#pragma once

#include "ScopeCore.h"
#include "ScopeGpuTypes.h"

namespace scopedeck
{

// Device time for one Analyse, split the way CudaReduceTiming is: the upload
// (a host memcpy into a shared buffer here) is pure conformance overhead the
// tap never pays, so kernel time is the only number that transfers.
struct MetalReduceTiming
{
    double msUpload = 0.0;
    double msKernels = 0.0;
    bool   valid = false;
};

class MetalReducer
{
public:
    MetalReducer();
    ~MetalReducer();

    MetalReducer(const MetalReducer&) = delete;
    MetalReducer& operator=(const MetalReducer&) = delete;

    // False when no Metal device is usable or the kernels did not compile.
    // Never throws; Error() says why.
    bool Available() const;
    const char* Error() const;

    // Human-readable device description, for test output.
    const char* DeviceDescription() const;

    // Reduces one host-memory frame, filling p_Out exactly as
    // ScopeEngine::Analyse would. Returns false and sets Error() on any Metal
    // failure - a conformance harness must be able to tell "wrong answer" from
    // "did not run", and so must a tap deciding whether to fall back.
    bool Analyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out,
                 MetalReduceTiming* p_Timing = nullptr);

private:
    struct Impl;
    Impl* m_Impl;
};

// When the host hands over Metal buffers but the GPU reduction is not being
// used, the tap still has to honour the passthrough - and a host memcpy of an
// id<MTLBuffer> cast to a pointer is a crash. This does it with a blit on the
// host's queue, in order with everything Resolve queued before it.
bool MetalPassthrough(void* p_Dst, size_t p_DstRowBytes,
                      const void* p_Src, size_t p_SrcRowBytes,
                      int p_Width, int p_Height, void* p_CommandQueue);

// -----------------------------------------------------------------------------
// MetalFallbackTap - what "GPU Acceleration off" runs on a Metal host.
//
// Same shape as CpuFallbackTap: the render thread enqueues a blit of the frame
// into a staging buffer and returns; a worker waits for the copy, reduces on
// the CPU and publishes. The reasons are the same too - a wait on the render
// thread includes everything Resolve queued ahead of us. On unified memory the
// staging buffer is readable by the CPU as soon as the blit completes, with no
// second transfer.
//
// One staging buffer, and a frame arriving while the worker is still busy is
// skipped rather than queued. A scope is a monitor: a dropped scope frame is
// cheaper than a stalled render, and cheaper than showing a stale one late.
class MetalFallbackTap
{
public:
    MetalFallbackTap();
    ~MetalFallbackTap();

    MetalFallbackTap(const MetalFallbackTap&) = delete;
    MetalFallbackTap& operator=(const MetalFallbackTap&) = delete;

    struct SubmitArgs
    {
        const void* srcDevice = nullptr;   // id<MTLBuffer>
        size_t      srcRowBytes = 0;
        int         width = 0;
        int         height = 0;
        void*       stream = nullptr;      // id<MTLCommandQueue>

        ScopeParams params;
        double      timelineTime = 0.0;
        uint32_t    instanceId = 0;

        bool        publishPreview = true;
        float       previewScale = 0.5f;
    };

    struct FallbackStats
    {
        double   msEnqueue = 0.0;       // what the render thread actually spent
        double   msCopy = 0.0;          // device time of the last blit done
        double   msBin = 0.0;           // CPU reduction of the last frame done
        uint64_t published = 0;
        uint64_t skipped = 0;           // arrived while the worker was busy
        bool     pinned = false;        // always true: unified memory
        bool     busy = false;
    };

    bool Start();
    void Stop();
    bool IsRunning() const;

    // Enqueues and returns. False means nothing was submitted - either a
    // failure (Error() says what) or the worker was still busy, which is not an
    // error and shows up as a skip.
    bool Submit(const SubmitArgs& p_Args);

    const char* Error() const;
    void Stats(FallbackStats& p_Out) const;

private:
    struct Impl;
    Impl* m_Impl;
};

// -----------------------------------------------------------------------------
// MetalTap - the shipping path. See the header comment for the shape; see
// ScopeCuda.h's GpuTap for the measured reasoning it inherits.
//
// One instance owns one of these. If anything fails it reports false and the
// caller falls back to the CPU path; nothing here may take Resolve down.
class MetalTap
{
public:
    MetalTap();
    ~MetalTap();

    MetalTap(const MetalTap&) = delete;
    MetalTap& operator=(const MetalTap&) = delete;

    // Brings up this instance's result buffers and joins the process-wide
    // publish hub, which owns the mapping, the ticket counter, the in-flight
    // ring and the thread that closes seqlocks. The first instance starts the
    // hub; the last one out tears it down. False means the GPU path is
    // unavailable and the caller should stay on the CPU.
    bool Start();
    void Stop();
    bool IsRunning() const;

    // Passthrough, reduce, preview and publish - all encoded and committed,
    // none waited on. Returns false on any Metal failure, with Error() set;
    // the caller then falls back for that frame.
    bool RenderFrame(const GpuTapArgs& p_Args);

    const char* Error() const;
    void Stats(GpuTapStats& p_Out) const;

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace scopedeck
