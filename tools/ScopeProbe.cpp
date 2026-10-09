// Headless reader check: what does the app see in shared memory right now?
//
// A console target rather than a flag on the app, because the app is built as a
// GUI subsystem executable and has nowhere to print. This exists for the question
// that comes up every time a scope looks wrong - "is the tap publishing at all,
// and is it the version this build reads" - which is otherwise only answerable by
// reading a status line inside the very UI that is misbehaving.
//
// Prints the layout this build expects alongside what is actually mapped, so a
// mismatch shows both numbers rather than just failing.

#include "ScopeImages.h"
#include "ScopeReader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace scopedeck;

namespace
{

// Colorize check for the vectorscope's density trace.
//
// Runs on a synthetic frame rather than the live one, because whether Colorize
// "works" cannot be judged from typical footage: a near-neutral frame puts every
// sample within a few bins of the centre, where every hue legitimately resolves to
// white. Planting one count at a known Cb/Cr and reading the colour back is the
// only way to answer it.
int CheckVectorscopeColorize()
{
    ScopeFrame frame;
    frame.pixelsSampled   = 1;
    frame.chromaRangeLow  = kChromaRangeLow;
    frame.chromaRangeHigh = kChromaRangeHigh;

    const LumaWeights w = LumaWeightsFor(kSpaceRec709);
    frame.lumaCoeff[0] = w.r;
    frame.lumaCoeff[1] = w.g;
    frame.lumaCoeff[2] = w.b;

    frame.vectorscope.assign(size_t(kVectorscopeTotalCells), 0u);

    // Bin indices for pure red, pure blue and neutral, derived through the same
    // Cb/Cr maths the tap uses rather than hardcoded.
    struct Probe { const char* name; float r, g, b; };
    const Probe probes[3] = {
        { "red",     1.0f, 0.0f, 0.0f },
        { "blue",    0.0f, 0.0f, 1.0f },
        { "neutral", 0.5f, 0.5f, 0.5f },
    };

    const float span = kChromaRangeHigh - kChromaRangeLow;
    int failures = 0;

    std::printf("\nVectorscope Colorize (synthetic bins):\n");

    for (const Probe& pr : probes)
    {
        float cb = 0.0f, cr = 0.0f;
        CbCrOfRgb(pr.r, pr.g, pr.b, frame.lumaCoeff, cb, cr);

        const int cbBin = int((cb - kChromaRangeLow) / span * float(kVectorscopeSize));
        const int crBin = int((cr - kChromaRangeLow) / span * float(kVectorscopeSize));
        if (cbBin < 0 || cbBin >= int(kVectorscopeSize) ||
            crBin < 0 || crBin >= int(kVectorscopeSize))
        {
            std::printf("  %-8s : bin out of range <-- FAIL\n", pr.name);
            ++failures;
            continue;
        }

        std::fill(frame.vectorscope.begin(), frame.vectorscope.end(), 0u);
        frame.vectorscope[VectorscopeIndex(kBandMid, uint32_t(cbBin), uint32_t(crBin))] = 100u;

        Image img;
        BuildVectorscopeImage(frame, 1u << kBandMid, 8.0f, /*colorize=*/true, 1.0f, img);

        const int y = int(kVectorscopeSize) - 1 - crBin;
        const uint8_t* px = &img.pixels[(size_t(y) * kVectorscopeSize + cbBin) * 4];

        // The check is which channel dominates, not an exact triple: the trace
        // curve and the brightest-channel normalisation both scale the values, and
        // pinning those would test the tuning rather than the colour.
        const char* verdict = "OK";
        if (std::strcmp(pr.name, "red") == 0 && !(px[0] > px[1] + 40 && px[0] > px[2] + 40))
        { verdict = "<-- FAIL (expected red dominant)"; ++failures; }
        if (std::strcmp(pr.name, "blue") == 0 && !(px[2] > px[0] + 40 && px[2] > px[1] + 40))
        { verdict = "<-- FAIL (expected blue dominant)"; ++failures; }
        if (std::strcmp(pr.name, "neutral") == 0 &&
            !(std::abs(int(px[0]) - int(px[1])) < 12 && std::abs(int(px[1]) - int(px[2])) < 12))
        { verdict = "<-- FAIL (expected near-grey)"; ++failures; }

        std::printf("  %-8s : bin (%3d,%3d)  rgb %3u %3u %3u  %s\n",
                    pr.name, cbBin, crBin, px[0], px[1], px[2], verdict);
    }

    if (failures == 0)
        std::printf("  neutral reading white is correct - a desaturated frame\n"
                    "  sits at the centre, where every hue resolves to grey.\n");

    return failures;
}

const char* StatusName(ReaderStatus p_Status)
{
    switch (p_Status)
    {
        case ReaderStatus::Closed:          return "Closed (no block - no tap has run)";
        case ReaderStatus::NoPublisher:     return "NoPublisher (block exists, nothing published yet)";
        case ReaderStatus::VersionMismatch: return "VersionMismatch";
        case ReaderStatus::Stale:           return "Stale (no new frames)";
        case ReaderStatus::Live:            return "Live";
    }
    return "?";
}

} // namespace

int main(int, char**)
{
    std::printf("Scope Deck reader probe\n");
    std::printf("-----------------------\n\n");

    std::printf("This build expects:\n");
    std::printf("  wire version   : %u\n",   kVersion);
    std::printf("  ShmHeader      : %zu bytes\n", sizeof(ShmHeader));
    std::printf("  SlotHeader     : %zu bytes\n", sizeof(SlotHeader));
    std::printf("  slot size      : %llu bytes\n", (unsigned long long)kSlotSize);
    std::printf("  slots          : %u\n",   kSlotCount);
    std::printf("  total          : %llu bytes (%.1f MB)\n\n",
                (unsigned long long)kTotalShmSize, double(kTotalShmSize) / (1024.0 * 1024.0));

    const int colorizeFailures = CheckVectorscopeColorize();

    ScopeReader reader;
    reader.SetWantWaveformTrace(true);   // so the trace check below has data
    if (!reader.Open())
    {
        std::printf("Status: %s\n", StatusName(reader.Status()));
        std::printf("\nNo shared memory block named ScopeDeck.v1 in this session.\n");
        std::printf("Start Resolve and add Scope Tap to a node on the Color page.\n");
        return colorizeFailures > 0 ? 4 : 1;
    }

    ScopeFrame frame;
    const bool got = reader.ReadLatest(frame);

    std::printf("Status: %s\n", StatusName(reader.Status()));

    if (reader.Status() == ReaderStatus::VersionMismatch)
    {
        std::printf("\n  shared memory : v%u\n", reader.ShmVersion());
        std::printf("  this build    : v%u\n", ScopeReader::ExpectedVersion());
        std::printf("\nThe ScopeTap.ofx installed in\n");
        std::printf("  C:\\Program Files\\Common Files\\OFX\\Plugins\n");
        std::printf("is not the one this app was built against. Rebuild and redeploy it\n");
        std::printf("(Resolve must be closed - OFX plugins are scanned only at startup).\n");
        return 2;
    }

    if (!got)
    {
        std::printf("\nNo frame read. That is normal when the playhead is parked -\n");
        std::printf("render() is silent while nothing changes.\n");
        return 0;
    }

    std::printf("\nFrame:\n");
    std::printf("  size          : %u x %u\n", frame.width, frame.height);
    std::printf("  color space   : %s  (luma %.4f %.4f %.4f)\n",
                frame.SpaceName(), frame.lumaCoeff[0], frame.lumaCoeff[1], frame.lumaCoeff[2]);
    std::printf("  publish index : %llu   instance %u\n",
                (unsigned long long)frame.publishIndex, frame.instanceId);
    std::printf("  pixelsSampled : %u\n", frame.pixelsSampled);
    std::printf("  bin cost      : %.3f ms\n", frame.binMillis);
    std::printf("  min RGB       : %.5f %.5f %.5f\n",
                frame.minRGB[0], frame.minRGB[1], frame.minRGB[2]);
    std::printf("  max RGB       : %.5f %.5f %.5f\n",
                frame.maxRGB[0], frame.maxRGB[1], frame.maxRGB[2]);
    std::printf("  probe RGB     : %.5f %.5f %.5f\n",
                frame.probeRGB[0], frame.probeRGB[1], frame.probeRGB[2]);
    std::printf("  preview       : %u x %u\n", frame.previewWidth, frame.previewHeight);

    // Bin accounting: every waveform plane should total the same sample count, and
    // that count should match pixelsSampled. A plane that disagrees means the
    // reader's indexing is wrong even though the image still looks plausible -
    // which is exactly the failure that is invisible on screen.
    std::printf("\nBin accounting (each plane should total pixelsSampled):\n");
    for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
    {
        unsigned long long total = 0;
        for (uint32_t col = 0; col < kWaveformColumns; ++col)
            for (uint32_t level = 0; level < kWaveformLevels; ++level)
                total += frame.waveform[WaveformIndex(plane, col, level)];

        const char* names[] = { "R", "G", "B", "Y" };
        std::printf("  waveform %-2s   : %llu %s\n", names[plane], total,
                    (total == frame.pixelsSampled) ? "OK" : "<-- MISMATCH");
    }

    for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
    {
        unsigned long long total = 0;
        for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
            total += frame.histogram[HistogramIndex(plane, bin)];

        const char* names[] = { "R", "G", "B", "Y" };
        std::printf("  histogram %-2s  : %llu %s\n", names[plane], total,
                    (total == frame.pixelsSampled) ? "OK" : "<-- MISMATCH");
    }

    unsigned long long vec = 0;
    for (uint64_t i = 0; i < kVectorscopeTotalCells; ++i) vec += frame.vectorscope[i];
    std::printf("  vectorscope   : %llu %s\n", vec,
                (vec == frame.pixelsSampled) ? "OK" : "<-- MISMATCH");

    // Enhanced Render's own input. Unlike the arrays above these are raw float
    // values, not counts, so there is no total to check against pixelsSampled -
    // what matters is whether the tap is filling it at all, and whether the range
    // looks like a real signal rather than zeroes.
    std::printf("\nWaveform trace (Enhanced Render input):\n");
    if (frame.waveformTrace.size() < kWaveformTraceCells)
    {
        std::printf("  NOT PUBLISHED - %zu floats, expected %llu\n",
                    frame.waveformTrace.size(), (unsigned long long)kWaveformTraceCells);
        return 3;
    }

    const char* names[] = { "R", "G", "B", "Y" };
    for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
    {
        float lo = 1e30f, hi = -1e30f;
        double mean = 0.0;
        unsigned long long nonZero = 0;

        for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
            for (uint32_t col = 0; col < kWaveformColumns; ++col)
            {
                const float v = frame.waveformTrace[WaveformTraceIndex(row, col, plane)];
                lo = (v < lo) ? v : lo;
                hi = (v > hi) ? v : hi;
                mean += v;
                if (v != 0.0f) ++nonZero;
            }

        mean /= double(kWaveformTraceRows) * kWaveformColumns;
        std::printf("  plane %-2s      : min %.5f  max %.5f  mean %.5f  non-zero %llu/%llu\n",
                    names[plane], lo, hi, mean, nonZero,
                    (unsigned long long)(uint64_t(kWaveformTraceRows) * kWaveformColumns));
    }

    return 0;
}
