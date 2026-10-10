// Publish-race check for the Metal tap: does every published slot carry its
// own frame's scopes, and does the whole path - MetalTap::RenderFrame, the
// publish hub, the worker's memcpy, the seqlock - hold up with frames arriving
// faster than they settle, and from more than one instance at once?
//
// The macOS counterpart of tools/ScopePublishRace.cu. scope_conformance_metal
// cannot see any of this: it drives MetalReducer, which is synchronous and
// never overlaps frames. This drives the real MetalTap through the real hub
// and reads the slots back.
//
//   C  300 alternating white/black frames back to back on one queue - the
//      realistic form (an export, a cache render, a fast scrub). A slot whose
//      result set was reused before the worker had copied it out would carry
//      the wrong frame's probe and maximum under its own timestamp.
//   D  Two instances interleaving on one queue, as Resolve's node graph
//      makes them - the ticket and ring reservation have to hold across
//      instances, never two frames in one slot.
//
// White and black frames make the check exact: the probe and max of every
// settled slot must be exactly 1.0 or 0.0 according to its own timelineTime.
// The waveform's pixel count and the preview are checked too, since the
// memcpy is the one leg the conformance harness never exercises.
//
// It publishes into a scratch block (ScopeDeck.PublishRace.v1), never the
// live ScopeDeck.v1, so it is safe with Resolve and the app running.
// ScopeMetal.mm is compiled into this file rather than linked, because the hub
// it needs to point at that block is internal to it.
//
// Exit 0 = pass, 1 = a slot carried the wrong frame, 2 = could not run.

#include "ScopeMetal.mm"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace scopedeck;

namespace publishrace
{

const char* const kScratchBlock = "ScopeDeck.PublishRace.v1";
const int kWidth = 1920, kHeight = 1080;
int g_Fail = 0;

struct Frame { id<MTLBuffer> src; id<MTLBuffer> dst; float value = 0.0f; };

bool MakeFrame(id<MTLDevice> p_Dev, float p_V, Frame& p_Out)
{
    const size_t n = size_t(kWidth) * kHeight;
    p_Out.src = [p_Dev newBufferWithLength:n * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    p_Out.dst = [p_Dev newBufferWithLength:n * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    if (!p_Out.src || !p_Out.dst) return false;
    float* px = static_cast<float*>(p_Out.src.contents);
    for (size_t i = 0; i < n; ++i) { px[i * 4 + 0] = p_V; px[i * 4 + 1] = p_V; px[i * 4 + 2] = p_V; px[i * 4 + 3] = 1.0f; }
    p_Out.value = p_V;
    return true;
}

GpuTapArgs Args(const Frame& p_Frame, id<MTLCommandQueue> p_Queue, double p_Time, uint32_t p_Instance)
{
    GpuTapArgs a;
    a.srcDevice = (__bridge const void*)p_Frame.src;
    a.dstDevice = (__bridge void*)p_Frame.dst;
    a.srcRowBytes = a.dstRowBytes = size_t(kWidth) * 4 * sizeof(float);
    a.width = kWidth;
    a.height = kHeight;
    a.stream = (__bridge void*)p_Queue;
    a.params.rowStep = 1;
    a.params.luma = LumaWeightsFor(0);
    a.timelineTime = p_Time;
    a.instanceId = p_Instance;
    a.publishPreview = true;
    a.previewScale = 0.5f;
    return a;
}

// Every frame handed to the hub ends up published, skipped or faulted.
uint64_t Settled()
{
    std::lock_guard<std::mutex> lock(Hub().mutex);
    return Hub().published + Hub().skipped + Hub().faulted;
}

bool WaitSettled(uint64_t p_Target)
{
    for (int i = 0; i < 5000; ++i)
    {
        if (Settled() >= p_Target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("  timed out waiting for the publisher\n");
    ++g_Fail;
    return false;
}

const char* SlotBase(uint32_t p_Index)
{
    const char* base = static_cast<const char*>(Hub().publisher.BlockData());
    return base + sizeof(ShmHeader) + size_t(p_Index) * kSlotSize;
}

// A settled slot's contents against the frame its timestamp names.
bool SlotCarries(uint32_t p_Index, float p_Want, const char** p_Why)
{
    const char* slot = SlotBase(p_Index);
    const SlotHeader* sh = reinterpret_cast<const SlotHeader*>(slot);
    if (sh->probeRGB[0] != p_Want || sh->maxRGB[0] != p_Want) { *p_Why = "probe/max"; return false; }
    if (sh->pixelsSampled != uint32_t(kWidth) * uint32_t(kHeight)) { *p_Why = "pixelsSampled"; return false; }

    // Every pixel landed in exactly one waveform bin per plane.
    const uint32_t* hist = reinterpret_cast<const uint32_t*>(slot + kHistogramOffset);
    uint64_t total = 0;
    for (uint32_t b = 0; b < kHistogramBins; ++b) total += hist[HistogramIndex(kPlaneR, b)];
    if (total != uint64_t(kWidth) * kHeight) { *p_Why = "histogram total"; return false; }

    // The preview is the same flat value, 8-bit.
    if (sh->previewWidth == 0 || sh->previewHeight == 0) { *p_Why = "no preview"; return false; }
    const uint8_t* pv = reinterpret_cast<const uint8_t*>(slot + kPreviewOffset);
    const uint8_t want8 = p_Want >= 1.0f ? 255 : 0;
    const size_t last = size_t(sh->previewWidth) * sh->previewHeight * 3 - 1;
    if (pv[0] != want8 || pv[last] != want8) { *p_Why = "preview"; return false; }
    return true;
}

} // namespace publishrace

using namespace publishrace;

int main()
{
    @autoreleasepool
    {
        std::printf("Scope Deck publish-race check (MetalTap::RenderFrame, scratch block %s)\n\n", kScratchBlock);

        // Started before the tap, so HubAcquire finds it running and keeps it.
        if (!Hub().publisher.Start(kScratchBlock)) { std::printf("could not create the scratch block\n"); return 2; }

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { std::printf("no Metal device\n"); return 2; }
        id<MTLCommandQueue> queue = [dev newCommandQueue];

        MetalTap tap;
        if (!tap.Start()) { std::printf("MetalTap::Start failed: %s\n", tap.Error()); return 2; }

        Frame white, black;
        if (!MakeFrame(dev, 1.0f, white) || !MakeFrame(dev, 0.0f, black)) { std::printf("could not allocate test frames\n"); return 2; }

        // One ordinary frame first, so the table and preview allocations are done.
        uint64_t handed = Settled();
        if (!tap.RenderFrame(Args(white, queue, 1, 1))) { std::printf("RenderFrame failed: %s\n", tap.Error()); return 2; }
        WaitSettled(++handed);
        {
            const char* why = "";
            bool found = false;
            for (uint32_t s = 0; s < kSlotCount; ++s)
            {
                const SlotHeader* sh = reinterpret_cast<const SlotHeader*>(SlotBase(s));
                if (sh->timelineTime != 1 || (sh->sequence & 1u)) continue;
                found = true;
                const bool ok = SlotCarries(s, 1.0f, &why);
                std::printf("  warm-up frame: %s%s%s\n", ok ? "ok" : "WRONG (", ok ? "" : why, ok ? "" : ")");
                if (!ok) ++g_Fail;
            }
            if (!found) { std::printf("  warm-up frame: no settled slot carries it\n"); ++g_Fail; }
        }

        std::printf("C. 300 alternating frames back to back on one queue\n");
        int wrong = 0, checked = 0;
        for (int i = 0; i < 300; ++i)
        {
            tap.RenderFrame(Args((i & 1) == 0 ? white : black, queue, 100 + i, 1));
            if (i % 3 != 2) continue;

            // Let this batch settle, then every settled slot must match its own
            // timestamp: even offsets were white, odd were black.
            WaitSettled(handed + uint64_t(i + 1));
            for (uint32_t s = 0; s < kSlotCount; ++s)
            {
                const SlotHeader* sh = reinterpret_cast<const SlotHeader*>(SlotBase(s));
                if (sh->timelineTime < 100 || (sh->sequence & 1u)) continue;
                const float want = (int(sh->timelineTime - 100) & 1) == 0 ? 1.0f : 0.0f;
                ++checked;
                const char* why = "";
                if (!SlotCarries(s, want, &why)) ++wrong;
            }
        }
        handed += 300;
        {
            std::lock_guard<std::mutex> lock(Hub().mutex);
            std::printf("  %d of %d slot reads carried another frame's data  %s   (published %llu, skipped %llu, faulted %llu; last frame dev=%.3fms pub=%.3fms)\n",
                        wrong, checked, wrong ? "WRONG" : "ok",
                        (unsigned long long)Hub().published, (unsigned long long)Hub().skipped, (unsigned long long)Hub().faulted,
                        Hub().msDeviceKernels, Hub().msDevicePublish);
        }
        g_Fail += wrong;

        std::printf("D. two instances interleaving on one queue, 200 frames\n");
        MetalTap tap2;
        if (!tap2.Start()) { std::printf("second MetalTap::Start failed: %s\n", tap2.Error()); return 2; }
        wrong = 0; checked = 0;
        for (int i = 0; i < 200; ++i)
        {
            // Instance 1 publishes white at even times, instance 2 black at odd.
            if ((i & 1) == 0) tap.RenderFrame(Args(white, queue, 1000 + i, 1));
            else              tap2.RenderFrame(Args(black, queue, 1000 + i, 2));
            if (i % 3 != 2) continue;
            WaitSettled(handed + uint64_t(i + 1));
            for (uint32_t s = 0; s < kSlotCount; ++s)
            {
                const SlotHeader* sh = reinterpret_cast<const SlotHeader*>(SlotBase(s));
                if (sh->timelineTime < 1000 || (sh->sequence & 1u)) continue;
                const int offset = int(sh->timelineTime - 1000);
                const float want = (offset & 1) == 0 ? 1.0f : 0.0f;
                const uint32_t wantInstance = (offset & 1) == 0 ? 1u : 2u;
                ++checked;
                const char* why = "";
                if (sh->instanceId != wantInstance || !SlotCarries(s, want, &why)) ++wrong;
            }
        }
        std::printf("  %d of %d slot reads carried another frame's data  %s\n", wrong, checked, wrong ? "WRONG" : "ok");
        g_Fail += wrong;

        tap2.Stop();
        tap.Stop();
        std::printf("\n%s (%d failures)\n", g_Fail ? "FAIL" : "PASS", g_Fail);
    }
    return g_Fail ? 1 : 0;
}
