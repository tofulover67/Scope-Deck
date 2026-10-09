// Publish-contention check: several publishers on one block, one real reader.
//
// Regression test for the 2026-10-08 bug where every ScopePublisher took its
// ticket from the block's committed write counter with nothing reserved. Two
// Scope Tap instances on the CPU path - or a CPU instance beside the GPU hub -
// could pick the same slot at once: the two seqlock increments took the slot
// even -> odd -> even while both were still writing, the reader accepted the
// mix as a settled frame, and out-of-order commits could move the write
// counter backwards. Each publisher also numbered frames from zero on its own,
// so panel caches keyed on frameIndex kept another instance's result.
//
//   1  Four publishers, each its own ScopePublisher object, publishing as fast
//      as they can - Scope Tap's CPU path with four instances.
//   2  Two of those beside an asynchronous publisher that reserves, opens,
//      fills the slot by hand and commits a few milliseconds later - the GPU
//      hub's shape, whose copies land long after the render call returned.
//
// Every publish is tagged: its timelineTime, every cell of every array, the
// extremes, the probe and every preview byte carry the same tag, so any torn
// mix of two writes shows up as a cell that disagrees with its own frame. The
// reader is the app's real ScopeReader.
//
// Publishes into a scratch block (ScopeDeck.Contention.v1), never the live
// ScopeDeck.v1. No GPU needed; release.ps1 runs it before packaging.
//
// Exit 0 = pass, 1 = a torn frame, a backwards counter or a reused index,
// 2 = could not run or checked too few frames to mean anything.
//
// SCOPE_CONTENTION_BASELINE builds scenario 1 only, against the publisher API
// as it was before the fix, so the test can be shown to catch the bug.

#include "ScopeReader.h"
#include "ScopeShm.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace scopedeck;

namespace
{

const char* const kBlock = "ScopeDeck.Contention.v1";
const uint32_t kPreviewW = 16, kPreviewH = 9;
const auto kRunTime = std::chrono::milliseconds(2000);

// Between publishes. Flat out, four publishers keep all three slots mid-write
// permanently and a correct reader never finds one settled; real taps publish
// at frame rate. ~200 a second each is still far denser than any timeline, and
// with four of them overlapping writes are constant.
const auto kPublishGap = std::chrono::milliseconds(5);

// Tags stay below 2^24 so they survive a round trip through float exactly.
uint32_t Tag(int p_Publisher, uint32_t p_Count) { return uint32_t(p_Publisher + 1) * 1000000u + p_Count; }

void FillResult(uint32_t p_Tag, ScopeResult& p_Out)
{
    p_Out.waveform.assign(kWaveformCells, p_Tag);
    p_Out.histogram.assign(kHistogramCells, p_Tag);
    p_Out.vectorscope.assign(kVectorscopeTotalCells, p_Tag);
    p_Out.twinPeaks.assign(kTwinPeaksTotalCells, p_Tag);
    p_Out.waveformTrace.assign(kWaveformTraceCells, float(p_Tag));
    for (int c = 0; c < 3; ++c)
        p_Out.minRGB[c] = p_Out.maxRGB[c] = p_Out.probeRGB[c] = float(p_Tag);
    p_Out.pixelsSampled = p_Tag;
    p_Out.preview.width = kPreviewW;
    p_Out.preview.height = kPreviewH;
    p_Out.preview.rgb.assign(size_t(kPreviewW) * kPreviewH * 3, uint8_t(p_Tag & 0xFF));
}

struct Tally
{
    std::atomic<uint64_t> accepted{ 0 }, torn{ 0 }, backwards{ 0 }, reusedIndex{ 0 }, staleIdentity{ 0 };
};

template <typename T>
bool AllEqual(const std::vector<T>& p_Values, T p_Want)
{
    for (const T& v : p_Values) if (v != p_Want) return false;
    return true;
}

// The real reader, checking every frame it accepts against its own tag.
void ReaderLoop(const std::atomic<bool>& p_Stop, Tally& p_Tally)
{
    ScopeReader reader;
    while (!reader.Open(kBlock) && !p_Stop) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reader.SetWantWaveformTrace(true);

    ScopeFrame frame;
    uint64_t lastIdentity = 0;
    std::map<uint64_t, uint32_t> tagOfIndex;   // publishIndex -> the tag it carried
    int reported = 0;

    while (!p_Stop)
    {
        // Yield rather than spin: CI runners have two to four cores, and a
        // hot loop here starves the very publishers it is waiting on.
        if (!reader.ReadLatest(frame)) { std::this_thread::yield(); continue; }
        ++p_Tally.accepted;

        const uint32_t tag = uint32_t(frame.timelineTime);
        const float ftag = float(tag);
        const bool ok =
            AllEqual(frame.waveform, tag) && AllEqual(frame.histogram, tag) &&
            AllEqual(frame.vectorscope, tag) && AllEqual(frame.twinPeaks, tag) &&
            AllEqual(frame.waveformTrace, ftag) &&
            frame.minRGB[0] == ftag && frame.maxRGB[2] == ftag && frame.probeRGB[1] == ftag &&
            frame.pixelsSampled == tag &&
            frame.previewWidth == kPreviewW && frame.previewHeight == kPreviewH &&
            AllEqual(frame.preview, uint8_t(tag & 0xFF));
        if (!ok)
        {
            ++p_Tally.torn;
            if (reported++ < 3)
                std::printf("    torn frame: tag %u, but waveform[0]=%u probe=%.0f preview[0]=%u\n", tag,
                            frame.waveform.empty() ? 0u : frame.waveform[0], frame.probeRGB[1],
                            frame.preview.empty() ? 0u : unsigned(frame.preview[0]));
        }

#ifndef SCOPE_CONTENTION_BASELINE
        // Finding #7: one publish index per frame across all publishers, and a
        // fresh app-side identity on every accepted read.
        auto found = tagOfIndex.find(frame.publishIndex);
        if (found == tagOfIndex.end()) tagOfIndex[frame.publishIndex] = tag;
        else if (found->second != tag) ++p_Tally.reusedIndex;
        if (frame.frameIndex <= lastIdentity) ++p_Tally.staleIdentity;
        lastIdentity = frame.frameIndex;
#else
        (void)lastIdentity; (void)tagOfIndex;
#endif
    }
}

// Watches the block's write counter for any step backwards.
void CounterWatch(const std::atomic<bool>& p_Stop, const ScopePublisher& p_Any, Tally& p_Tally)
{
    const volatile uint64_t* counter =
        &static_cast<const ShmHeader*>(p_Any.BlockData())->writeCounter;
    uint64_t last = *counter;
    while (!p_Stop)
    {
        const uint64_t now = *counter;
        if (now < last) ++p_Tally.backwards;
        last = now;
        std::this_thread::yield();
    }
}

void SyncPublisher(int p_Id, const std::atomic<bool>& p_Stop)
{
    ScopePublisher publisher;              // its own object, as each Scope Tap instance has
    if (!publisher.Start(kBlock)) return;
    ScopeResult result;
    for (uint32_t n = 0; !p_Stop; ++n)
    {
        const uint32_t tag = Tag(p_Id, n);
        FillResult(tag, result);
        publisher.Publish(result, double(tag), 1000 + p_Id, 500, uint32_t(p_Id));
        std::this_thread::sleep_for(kPublishGap);
    }
}

#ifndef SCOPE_CONTENTION_BASELINE
// The GPU hub's shape: reserve, open, fill the slot directly, commit later.
void AsyncPublisher(int p_Id, const std::atomic<bool>& p_Stop)
{
    ScopePublisher publisher;
    if (!publisher.Start(kBlock)) return;
    LumaWeights luma = LumaWeightsFor(0);
    for (uint32_t n = 0; !p_Stop; ++n)
    {
        uint64_t ticket = 0;
        if (!publisher.ReserveTicket(ticket)) { std::this_thread::yield(); continue; }

        const uint32_t tag = Tag(p_Id, n);
        publisher.OpenSlot(ticket, double(tag), 1000 + p_Id, 500, uint32_t(p_Id), tag, 0, luma,
                           kPreviewW, kPreviewH, 0.0);
        char* slot = static_cast<char*>(publisher.SlotAddress(ticket));
        auto fillU32 = [&](uint64_t p_Offset, uint64_t p_Count) {
            uint32_t* p = reinterpret_cast<uint32_t*>(slot + p_Offset);
            for (uint64_t i = 0; i < p_Count; ++i) p[i] = tag;
        };
        fillU32(kWaveformOffset, kWaveformCells);
        fillU32(kHistogramOffset, kHistogramCells);
        fillU32(kVectorscopeOffset, kVectorscopeTotalCells);
        fillU32(kTwinPeaksOffset, kTwinPeaksTotalCells);
        float* trace = reinterpret_cast<float*>(slot + kWaveformTraceOffset);
        for (uint64_t i = 0; i < kWaveformTraceCells; ++i) trace[i] = float(tag);
        float* minMax = reinterpret_cast<float*>(slot + ScopePublisher::MinMaxOffset());
        float* probe  = reinterpret_cast<float*>(slot + ScopePublisher::ProbeOffset());
        for (int c = 0; c < 6; ++c) minMax[c] = float(tag);
        for (int c = 0; c < 3; ++c) probe[c] = float(tag);
        std::memset(slot + kPreviewOffset, int(tag & 0xFF), size_t(kPreviewW) * kPreviewH * 3);

        // The copy "landing" late, as a DMA does, while the others publish.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        publisher.CommitSlot(ticket);
        std::this_thread::sleep_for(kPublishGap);
    }
}
#endif

// 0 = pass, 1 = a real failure, 2 = could not run or too few frames to judge.
int RunScenario(const char* p_Name, int p_Sync, bool p_WithAsync)
{
    std::printf("%s\n", p_Name);

    ScopePublisher owner;                  // keeps the block alive and is watched
    if (!owner.Start(kBlock)) { std::printf("  could not create the scratch block\n"); return 2; }

    Tally tally;
    std::atomic<bool> stop{ false };
    std::vector<std::thread> threads;
    threads.emplace_back(ReaderLoop, std::cref(stop), std::ref(tally));
    threads.emplace_back(CounterWatch, std::cref(stop), std::cref(owner), std::ref(tally));
    for (int i = 0; i < p_Sync; ++i) threads.emplace_back(SyncPublisher, i, std::cref(stop));
#ifndef SCOPE_CONTENTION_BASELINE
    if (p_WithAsync) threads.emplace_back(AsyncPublisher, p_Sync, std::cref(stop));
#else
    (void)p_WithAsync;
#endif

    std::this_thread::sleep_for(kRunTime);
    stop = true;
    for (std::thread& t : threads) t.join();

    std::printf("  frames checked %llu: torn %llu, write counter went backwards %llu",
                (unsigned long long)tally.accepted.load(), (unsigned long long)tally.torn.load(),
                (unsigned long long)tally.backwards.load());
#ifndef SCOPE_CONTENTION_BASELINE
    std::printf(", publish index reused %llu, identity not fresh %llu",
                (unsigned long long)tally.reusedIndex.load(), (unsigned long long)tally.staleIdentity.load());
#endif
    std::printf("\n");

    const bool ok = tally.torn == 0 && tally.backwards == 0 && tally.reusedIndex == 0 && tally.staleIdentity == 0;
    if (!ok) { std::printf("  FAIL\n"); return 1; }
    if (tally.accepted < 50)
    {
        std::printf("  too few frames checked to mean anything\n");
        return 2;
    }
    std::printf("  ok\n");
    return 0;
}

} // namespace

int main()
{
    std::printf("Scope Deck publish-contention check (scratch block %s)\n\n", kBlock);
    int worst = RunScenario("1. four independent publishers (CPU path, four instances)", 4, false);
#ifndef SCOPE_CONTENTION_BASELINE
    const int second = RunScenario("2. two synchronous publishers beside an asynchronous one (CPU beside GPU)", 2, true);
    if (second == 1 || (second == 2 && worst == 0)) worst = second;
#endif
    std::printf("\n%s\n", worst == 0 ? "PASS" : (worst == 1 ? "FAIL" : "COULD NOT JUDGE"));
    return worst;
}
