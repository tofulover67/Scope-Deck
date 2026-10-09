// Publish-race check: does every published slot carry its own frame's scopes?
//
// Regression test for the 2026-10-08 bug where GpuTap had one set of device
// result buffers per instance. A frame's publish copies read those buffers on
// publishStream while the NEXT frame's memsets and kernels were already
// writing them on the host's stream, so a slot could carry another frame's
// waveform, extrema and probe under its own timestamp. Measured before the fix:
// 192-195 of 300 slot reads wrong in every run of scenario C, on an RTX 5080.
//
// scope_conformance_cuda cannot see this: it drives CudaReducer, which is
// synchronous and never overlaps frames. This drives the real
// GpuTap::RenderFrame, through the real publish hub, and reads the slots back.
//
//   A  One host stream. The publish stream is held, so frame N's copy-out is
//      still pending when frame N+1's kernels run - deterministic.
//   B  Two host streams. Stream 1 is held while a second frame runs on
//      stream 2 - deterministic, the multi-stream form of the same hazard.
//   C  300 alternating white/black frames back to back on one stream, nothing
//      held - the realistic form (an export, a cache render, a fast scrub).
//
// White and black frames make the check exact: the probe and max of every
// slot must be exactly 1.0 or 0.0 according to its own timelineTime.
//
// It publishes into a scratch block (ScopeDeck.PublishRace.v1), never the live
// ScopeDeck.v1, so it is safe with Resolve and the app running. ScopeCuda.cu is
// compiled into this file rather than linked, because the hub it needs to
// point at that block is internal to it.
//
// Exit 0 = pass, 1 = a slot carried the wrong frame, 2 = could not run.

#include "ScopeCuda.cu"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace scopedeck
{
struct GpuTapTestAccess
{
    static cudaStream_t PublishStream(GpuTap& p_Tap) { return p_Tap.m_Impl->publishStream; }
};
}

using namespace scopedeck;

// Named, not anonymous: ScopeCuda.cu's kernels already sit in an anonymous
// namespace, and nvcc's generated stubs reject __global__ functions from a
// second anonymous namespace in the same file as an ambiguous symbol.
namespace publishrace
{

const char* const kScratchBlock = "ScopeDeck.PublishRace.v1";
const int kWidth = 1920, kHeight = 1080;
int g_Fail = 0;

// Waits on a host-visible flag, which is how a stream is held open on purpose.
__global__ void HoldUntil(volatile int* p_Flag) { while (*p_Flag == 0) {} }

__global__ void FillFlat(float* p_Px, size_t p_Count, float p_V)
{
    const size_t i = blockIdx.x * size_t(blockDim.x) + threadIdx.x;
    if (i >= p_Count) return;
    p_Px[i * 4 + 0] = p_V; p_Px[i * 4 + 1] = p_V; p_Px[i * 4 + 2] = p_V; p_Px[i * 4 + 3] = 1.0f;
}

struct Frame { float* src = nullptr; float* dst = nullptr; };

bool MakeFrame(float p_V, Frame& p_Out)
{
    const size_t n = size_t(kWidth) * kHeight;
    if (cudaMalloc(&p_Out.src, n * 4 * sizeof(float)) != cudaSuccess) return false;
    if (cudaMalloc(&p_Out.dst, n * 4 * sizeof(float)) != cudaSuccess) return false;
    FillFlat<<<unsigned((n + 255) / 256), 256>>>(p_Out.src, n, p_V);
    return cudaDeviceSynchronize() == cudaSuccess;
}

GpuTapArgs Args(const Frame& p_Frame, cudaStream_t p_Stream, double p_Time)
{
    GpuTapArgs a;
    a.srcDevice = p_Frame.src;
    a.dstDevice = p_Frame.dst;
    a.srcRowBytes = a.dstRowBytes = size_t(kWidth) * 4 * sizeof(float);
    a.width = kWidth;
    a.height = kHeight;
    a.stream = p_Stream;
    a.params.rowStep = 1;
    a.params.luma = LumaWeightsFor(0);
    a.timelineTime = p_Time;
    a.instanceId = 1;
    a.publishPreview = true;
    a.previewScale = 0.5f;
    return a;
}

// Every frame handed to the hub ends up either published or skipped.
uint64_t Settled()
{
    std::lock_guard<std::mutex> lock(Hub().mutex);
    return Hub().published + Hub().skipped;
}

bool WaitSettled(uint64_t p_Target)
{
    for (int i = 0; i < 2000; ++i)
    {
        if (Settled() >= p_Target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("  timed out waiting for the publisher\n");
    ++g_Fail;
    return false;
}

const SlotHeader* SlotAt(uint32_t p_Index)
{
    const char* base = static_cast<const char*>(Hub().publisher.BlockData());
    return reinterpret_cast<const SlotHeader*>(base + sizeof(ShmHeader) + size_t(p_Index) * kSlotSize);
}

void Expect(double p_Time, float p_Want, const char* p_Label)
{
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        const SlotHeader* sh = SlotAt(i);
        if (sh->timelineTime != p_Time || (sh->sequence & 1u)) continue;
        const bool ok = sh->probeRGB[0] == p_Want && sh->maxRGB[0] == p_Want;
        std::printf("  %-28s t=%-4.0f probe=%.1f max=%.1f  %s\n", p_Label, p_Time,
                    sh->probeRGB[0], sh->maxRGB[0], ok ? "ok" : "WRONG FRAME");
        if (!ok) ++g_Fail;
        return;
    }
    std::printf("  %-28s t=%-4.0f no settled slot carries this frame\n", p_Label, p_Time);
    ++g_Fail;
}

} // namespace publishrace

using namespace publishrace;

int main()
{
    std::printf("Scope Deck publish-race check (GpuTap::RenderFrame, scratch block %s)\n\n", kScratchBlock);

    // Started before the tap, so HubAcquire finds it running and keeps it.
    if (!Hub().publisher.Start(kScratchBlock)) { std::printf("could not create the scratch block\n"); return 2; }

    GpuTap tap;
    if (!tap.Start()) { std::printf("GpuTap::Start failed: %s\n", tap.Error()); return 2; }

    Frame white, black;
    if (!MakeFrame(1.0f, white) || !MakeFrame(0.0f, black)) { std::printf("could not allocate test frames\n"); return 2; }

    int* hostFlag = nullptr;
    int* deviceFlag = nullptr;
    if (cudaHostAlloc(&hostFlag, sizeof(int), cudaHostAllocMapped) != cudaSuccess ||
        cudaHostGetDevicePointer(&deviceFlag, hostFlag, 0) != cudaSuccess)
    {
        std::printf("could not allocate the hold flag\n");
        return 2;
    }
    volatile int* hold = hostFlag;

    cudaStream_t s1 = nullptr, s2 = nullptr;
    cudaStreamCreate(&s1);
    cudaStreamCreate(&s2);

    // One ordinary frame first, so table upload and preview allocation are done.
    uint64_t handed = Settled();
    if (!tap.RenderFrame(Args(white, s1, 1))) { std::printf("RenderFrame failed: %s\n", tap.Error()); return 2; }
    cudaStreamSynchronize(s1);
    WaitSettled(++handed);

    std::printf("A. one host stream, frame N's copy-out held until frame N+1 has run\n");
    *hold = 0;
    HoldUntil<<<1, 1, 0, GpuTapTestAccess::PublishStream(tap)>>>(deviceFlag);
    tap.RenderFrame(Args(white, s1, 10));
    tap.RenderFrame(Args(black, s1, 20));
    cudaStreamSynchronize(s1);
    *hold = 1;
    handed += 2;
    WaitSettled(handed);
    Expect(10, 1.0f, "frame N   (white)");
    Expect(20, 0.0f, "frame N+1 (black)");

    std::printf("B. two host streams, stream 1 held while stream 2 renders\n");
    *hold = 0;
    HoldUntil<<<1, 1, 0, s1>>>(deviceFlag);
    tap.RenderFrame(Args(white, s1, 30));
    tap.RenderFrame(Args(black, s2, 40));
    cudaStreamSynchronize(s2);
    *hold = 1;
    cudaStreamSynchronize(s1);
    handed += 2;
    WaitSettled(handed);
    Expect(30, 1.0f, "white on stream 1 (held)");
    Expect(40, 0.0f, "black on stream 2");

    std::printf("C. 300 alternating frames back to back on one stream, nothing held\n");
    int wrong = 0, checked = 0;
    for (int i = 0; i < 300; ++i)
    {
        tap.RenderFrame(Args((i & 1) == 0 ? white : black, s1, 100 + i));
        if (i % 3 != 2) continue;

        // Let this batch settle, then every settled slot must match its own
        // timestamp: even offsets were white, odd were black.
        cudaStreamSynchronize(s1);
        WaitSettled(handed + uint64_t(i + 1));
        for (uint32_t s = 0; s < kSlotCount; ++s)
        {
            const SlotHeader* sh = SlotAt(s);
            if (sh->timelineTime < 100 || (sh->sequence & 1u)) continue;
            const float want = (int(sh->timelineTime - 100) & 1) == 0 ? 1.0f : 0.0f;
            ++checked;
            if (sh->probeRGB[0] != want || sh->maxRGB[0] != want) ++wrong;
        }
    }
    std::printf("  %d of %d slot reads carried another frame's data  %s\n", wrong, checked, wrong ? "WRONG" : "ok");
    g_Fail += wrong;

    tap.Stop();
    std::printf("\n%s (%d failures)\n", g_Fail ? "FAIL" : "PASS", g_Fail);
    return g_Fail ? 1 : 0;
}
