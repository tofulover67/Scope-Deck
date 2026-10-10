// CUDA scope reduction, behind a plain C++ interface so nothing above this line
// includes a CUDA header.
//
// Two callers, deliberately in this order:
//
//   1. tools/ScopeConformance.cpp, which drives it with host-memory frames and
//      diffs it against the CPU reduction bit-for-bit. That is how correctness
//      gets settled - on this machine, in seconds, with no Resolve in the loop.
//   2. Later, ScopeTap, where the frame is already device memory and the
//      results go straight out over PCIe into the shared-memory slot. That path
//      does no upload and no download of bins to host vectors at all.
//
// Analyse() below serves the first caller: it uploads a host frame and reads
// everything back into a ScopeResult, which is pure overhead for the tap and
// exactly what a conformance test needs. The device-resident entry point is a
// separate function added when the tap is wired up; both drive the same kernels,
// so passing conformance means the tap's maths is the maths that was tested.
//
// Every numeric decision here exists to match core/ScopeCore.cpp bit-for-bit,
// not to be independently reasonable. See the notes in ScopeCuda.cu - and the
// harness header - for the three places where the obvious GPU formulation
// silently disagrees with the CPU one.

#pragma once

#include "ScopeCore.h"
#include "ScopeGpuTypes.h"

namespace scopedeck
{

// Device time for one Analyse, split so a benchmark can talk about the kernels
// without the upload drowning them: at UHD the H2D is ~127 MB and costs an
// order of magnitude more than the reduction it feeds. The tap never pays that
// upload - its frame is already on the card - so kernel time is the number that
// transfers to the shipping path, and the only one worth quoting from here.
struct CudaReduceTiming
{
    double msUpload = 0.0;
    double msKernels = 0.0;
    bool   valid = false;
};

class CudaReducer
{
public:
    CudaReducer();
    ~CudaReducer();

    CudaReducer(const CudaReducer&) = delete;
    CudaReducer& operator=(const CudaReducer&) = delete;

    // False when no CUDA device is usable. Never throws; Error() says why.
    bool Available() const;
    const char* Error() const;

    // Human-readable device description, for test output.
    const char* DeviceDescription() const;

    // Reduces one host-memory frame, filling p_Out exactly as
    // ScopeEngine::Analyse would. Returns false and sets Error() on any CUDA
    // failure - a conformance harness must be able to tell "wrong answer" from
    // "did not run", and so must a tap deciding whether to fall back.
    // p_Timing is optional and costs a pair of CUDA events when asked for.
    bool Analyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out,
                 CudaReduceTiming* p_Timing = nullptr);

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace scopedeck

// -----------------------------------------------------------------------------
// GpuTap - the shipping path.
//
// Everything above serves conformance. This is what runs inside Resolve: the
// frame is already device memory, the bins never reach host memory as a
// ScopeResult at all, and the results land in the shared-memory slot by DMA.
//
// Shape, all of it measured rather than chosen (see GPU_PORT_HANDOFF.md §3):
//
//   - Kernels run on the HOST's stream. They read Resolve's source buffer,
//     which is only guaranteed valid during render(), and Resolve will not wait
//     on a stream it does not know about.
//   - The publish D2H runs on our OWN stream, ordered after the kernels by an
//     event. It reads only our buffers, so it may safely outlive render(), and
//     keeping it off Resolve's stream means Resolve is not serialised behind
//     0.2 ms of transfer.
//   - The slot is page-locked with cudaHostRegister, which was measured to work
//     on a mapped file view: 11.7 MB in 0.217 ms at 54 GB/s, against 10.2 ms of
//     blocked render thread for the pageable alternative.
//   - A background thread waits for the copies and closes the seqlock. The
//     render thread never waits: measured at 9.57 ms to wait for 1.0 ms of
//     work, because synchronising also waits for everything Resolve queued
//     ahead of us. Publishing one frame late was rejected separately - with the
//     playhead parked there is no next frame to carry the deferred publish, so
//     the scope would sit one edit behind forever.
//   - The mapping, its page-lock, the ticket counter, the in-flight ring and
//     that thread are ONE set per process, however many instances Resolve
//     makes. Per-instance ticket allocation was a check-then-act across
//     instances: two could claim the same ticket, and because OpenSlot and
//     CommitSlot are plain seq+1 increments, two interleaved opens take the
//     sequence even -> odd -> even, advertising the slot as readable while two
//     DMAs are still landing in it. Async publishing made that window wide.
//     Instances still own their own device buffers and publish stream, because
//     they really do reduce different frames at the same time.
//
// One instance owns one of these. If anything fails it reports false and the
// caller falls back to the CPU path; nothing here may take Resolve down.
// -----------------------------------------------------------------------------

namespace scopedeck { class ScopePublisher; }

namespace scopedeck
{

// GpuTapArgs and GpuTapStats live in ScopeGpuTypes.h, shared with the Metal
// backend so plugin/ScopeTap.cpp drives either through one set of names.

// When the host hands over device memory but the GPU reduction is not being
// used, the tap still has to honour the passthrough - and a host memcpy of a
// device pointer faults. This does it on-card instead.
bool CudaPassthrough(void* p_Dst, size_t p_DstRowBytes,
                     const void* p_Src, size_t p_SrcRowBytes,
                     int p_Width, int p_Height, void* p_Stream);

// Copies a device frame into host memory and WAITS for it, so the CPU reduction
// can read it. Used only when the host handed over device memory and the user
// turned GPU Acceleration off - the deliberately slow path, where blocking is
// the point rather than a bug. Expect roughly a 10 ms transfer at UHD on top of
// whatever the CPU reduction then costs, plus however much of Resolve's own
// queued work sits ahead of us on this stream.
// p_MsCopy, when given, receives the DEVICE time of the transfer alone. The
// difference between it and the caller's wall clock is time spent waiting for
// the stream to drain - i.e. for work Resolve queued ahead of us - and the two
// have completely different fixes. Section 3 of the handoff was caught
// attributing one to the other once already.
bool CudaDownload(void* p_DstHost, size_t p_DstRowBytes,
                  const void* p_SrcDevice, size_t p_SrcRowBytes,
                  int p_Width, int p_Height, void* p_Stream,
                  double* p_MsCopy = nullptr);

// Page-locked host memory, for anyone staging a frame back to the CPU.
// Returns null if the driver refuses; callers fall back to ordinary memory
// rather than giving up, because slow scopes beat no scopes.
void* CudaAllocHost(size_t p_Bytes);
void  CudaFreeHost(void* p_Ptr);

// -----------------------------------------------------------------------------
// CpuFallbackTap - what "GPU Acceleration off" runs on a CUDA host.
//
// The frame is on the card, so a CPU reading of it needs a readback. The only
// real question is whether Resolve's render thread has to WAIT for that, and
// the answer is no - measured, after getting it wrong twice:
//
//   download 28.49 ms p50, of which the transfer itself is 5.51 ms and 22.51 ms
//   is waiting for the stream to drain - i.e. for work Resolve queued ahead of
//   us. Page-locking the destination changed nothing, because the wait was
//   never about transfer bandwidth. §3 of the handoff had already established
//   this for the publish leg (a pinned copy blocks 9.57 ms too) and it applies
//   here unchanged.
//
// So this does what the publish leg does: the render thread enqueues the D2H
// and returns, and a worker thread picks the frame up once it has landed,
// reduces it on the CPU and publishes. The render thread's cost becomes the
// enqueue alone.
//
// Reading Resolve's source buffer from a copy that outlives render() is the
// same bet the GPU path already makes with its kernels - the copy is ordered
// on the host's stream, so it runs before anything Resolve queues afterwards.
//
// One staging buffer, and a frame arriving while the worker is still busy is
// skipped rather than queued. A scope is a monitor: a dropped scope frame is
// cheaper than a stalled render, and cheaper than showing a stale one late.
class CpuFallbackTap
{
public:
    CpuFallbackTap();
    ~CpuFallbackTap();

    CpuFallbackTap(const CpuFallbackTap&) = delete;
    CpuFallbackTap& operator=(const CpuFallbackTap&) = delete;

    struct SubmitArgs
    {
        const void* srcDevice = nullptr;
        size_t      srcRowBytes = 0;
        int         width = 0;
        int         height = 0;
        void*       stream = nullptr;

        ScopeParams params;
        double      timelineTime = 0.0;
        uint32_t    instanceId = 0;

        bool        publishPreview = true;
        float       previewScale = 0.5f;
    };

    struct FallbackStats
    {
        double   msEnqueue = 0.0;       // what the render thread actually spent
        double   msCopy = 0.0;          // device transfer of the last frame done
        double   msBin = 0.0;           // CPU reduction of the last frame done
        uint64_t published = 0;
        uint64_t skipped = 0;           // arrived while the worker was busy
        bool     pinned = false;
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

class GpuTap
{
public:
    GpuTap();
    ~GpuTap();

    GpuTap(const GpuTap&) = delete;
    GpuTap& operator=(const GpuTap&) = delete;

    // Brings up this instance's device buffers and joins the process-wide
    // publish hub, which owns the mapping, its page-lock, the ticket counter,
    // the in-flight ring and the thread that closes seqlocks. The first
    // instance starts the hub; the last one out tears it down. False means the
    // GPU path is unavailable and the caller should stay on the CPU.
    bool Start();
    void Stop();
    bool IsRunning() const;

    // Passthrough, reduce, preview and publish - all enqueued, none waited on.
    // Returns false on any CUDA failure, with Error() set; the caller then
    // falls back for that frame.
    bool RenderFrame(const GpuTapArgs& p_Args);

    const char* Error() const;
    void Stats(GpuTapStats& p_Out) const;

private:
    // tools/ScopePublishRace.cu holds this instance's publish stream to force
    // the copy-out/next-frame overlap deterministically. Declaration only;
    // nothing in the shipping build defines or uses it.
    friend struct GpuTapTestAccess;

    struct Impl;
    Impl* m_Impl;
};

} // namespace scopedeck
