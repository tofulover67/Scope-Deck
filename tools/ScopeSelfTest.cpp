// Drives scopecore without Resolve.
//
// Two jobs:
//   1. Print the exact wire-format sizes and offsets, so app/scope_format.py can be
//      checked against the compiler rather than against hand arithmetic.
//   2. Publish synthetic frames into shared memory on a timer, so the Scope Deck app
//      can be developed and debugged with no plugin installed and Resolve closed.
//
// Usage:
//   scope_selftest                 print layout, run one benchmark frame, exit
//   scope_selftest --publish       keep publishing animated frames until Ctrl+C
//   scope_selftest --publish --hd  publish at 1920x1080 instead of UHD
//   scope_selftest --publish --small       publish at 64x64, for exact bin checks
//   scope_selftest --publish --name <name> publish to a scratch block instead of
//                                  the shared one, so this can run while a real
//                                  tap and app are live on the usual block
//   scope_selftest --publish --frames N    stop after N frames instead of Ctrl+C

#include "ScopeCore.h"
#include "ScopeShm.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace scopedeck;

namespace
{

// A gradient with a moving bright band, so the waveform has obvious structure and
// visible motion. The band pushes past 1.0 to exercise the above-range bins.
void FillSynthetic(std::vector<float>& p_Pixels, int p_Width, int p_Height, double p_Phase)
{
    p_Pixels.resize(static_cast<size_t>(p_Width) * p_Height * 4);

    const double bandCentre = 0.5 + 0.35 * std::sin(p_Phase);

    for (int y = 0; y < p_Height; ++y)
    {
        float* row = p_Pixels.data() + static_cast<size_t>(y) * p_Width * 4;
        const double v = static_cast<double>(y) / p_Height;

        for (int x = 0; x < p_Width; ++x)
        {
            const double u = static_cast<double>(x) / p_Width;

            // Horizontal ramp in R, vertical in G, a diagonal in B: three clearly
            // different traces so channel mix-ups are obvious at a glance.
            double r = u;
            double g = v;
            double b = 0.5 * (u + v);

            const double d = std::fabs(u - bandCentre);
            if (d < 0.03)
            {
                const double boost = 1.35 * (1.0 - d / 0.03);
                r += boost; g += boost; b += boost;
            }

            row[x * 4 + 0] = static_cast<float>(r);
            row[x * 4 + 1] = static_cast<float>(g);
            row[x * 4 + 2] = static_cast<float>(b);
            row[x * 4 + 3] = 1.0f;
        }
    }
}

void PrintLayout()
{
    std::printf("Scope Deck wire format v%u\n", kVersion);
    std::printf("---------------------------------------------\n");
    std::printf("sizeof(ShmHeader)   = %zu\n", sizeof(ShmHeader));
    std::printf("sizeof(SlotHeader)  = %zu\n", sizeof(SlotHeader));
    std::printf("waveform  %u cols x %u levels x %u planes = %llu cells\n",
                kWaveformColumns, kWaveformLevels, kPlaneCount,
                static_cast<unsigned long long>(kWaveformCells));
    std::printf("histogram %u bins x %u planes = %llu cells\n",
                kHistogramBins, kPlaneCount,
                static_cast<unsigned long long>(kHistogramCells));
    std::printf("vectorscope %u x %u x %u bands = %llu cells\n",
                kVectorscopeSize, kVectorscopeSize, kBandCount,
                static_cast<unsigned long long>(kVectorscopeTotalCells));
    std::printf("twin peaks  %u x %u x %u diamonds = %llu cells\n",
                kTwinPeaksSize, kTwinPeaksSize, kDiamondCount,
                static_cast<unsigned long long>(kTwinPeaksTotalCells));
    std::printf("waveform trace %u rows x %u cols x %u planes = %llu cells (floats, not counts)\n",
                kWaveformTraceRows, kWaveformColumns, kPlaneCount,
                static_cast<unsigned long long>(kWaveformTraceCells));
    std::printf("preview canvas      = %u x %u x 3 = %llu bytes\n",
                kPreviewCanvasWidth, kPreviewCanvasHeight,
                static_cast<unsigned long long>(kPreviewCanvasBytes));
    std::printf("kWaveformOffset     = %llu\n", static_cast<unsigned long long>(kWaveformOffset));
    std::printf("kHistogramOffset    = %llu\n", static_cast<unsigned long long>(kHistogramOffset));
    std::printf("kVectorscopeOffset  = %llu\n", static_cast<unsigned long long>(kVectorscopeOffset));
    std::printf("kTwinPeaksOffset    = %llu\n", static_cast<unsigned long long>(kTwinPeaksOffset));
    std::printf("kWaveformTraceOffset = %llu\n", static_cast<unsigned long long>(kWaveformTraceOffset));
    std::printf("kPreviewOffset      = %llu\n", static_cast<unsigned long long>(kPreviewOffset));
    std::printf("kSlotSize           = %llu\n", static_cast<unsigned long long>(kSlotSize));
    std::printf("kSlotCount          = %u\n", kSlotCount);
    std::printf("kTotalShmSize       = %llu\n", static_cast<unsigned long long>(kTotalShmSize));
    std::printf("bin range           = [%.4f .. %.4f]\n", kBinRangeLow, kBinRangeHigh);
    std::printf("chroma range        = [%.4f .. %.4f]\n", kChromaRangeLow, kChromaRangeHigh);
    std::printf("twin peaks diff range = [%.4f .. %.4f]\n",
                kTwinPeaksDiffRangeLow, kTwinPeaksDiffRangeHigh);
    std::printf("vectorscope bands   = Low < %.2f <= Mid < %.2f <= High\n",
                kVectorscopeLowMax, kVectorscopeHighMin);
    std::printf("---------------------------------------------\n\n");
}

} // namespace

int main(int argc, char** argv)
{
    bool publish = false;
    int width = 3840, height = 2160;
    std::string shmName = kShmName;
    int frameLimit = 0;   // 0 = run until Ctrl+C

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--publish") publish = true;
        else if (arg == "--hd") { width = 1920; height = 1080; }
        else if (arg == "--small") { width = 64; height = 64; }
        else if (arg == "--name" && i + 1 < argc) shmName = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frameLimit = std::atoi(argv[++i]);
    }

    PrintLayout();

    ScopeEngine engine;
    ScopeResult result;
    ScopeParams params;
    std::vector<float> pixels;

    std::printf("Engine threads: %d\n", engine.ThreadCount());
    std::printf("Frame: %dx%d (%.1f MB as float RGBA)\n\n",
                width, height,
                static_cast<double>(width) * height * 16.0 / (1024.0 * 1024.0));

    FrameView view;
    view.width    = width;
    view.height   = height;
    view.rowBytes = width * 4 * static_cast<int>(sizeof(float));

    if (!publish)
    {
        FillSynthetic(pixels, width, height, 0.0);
        view.pixels = pixels.data();

        // First pass warms the allocations; report the steady-state cost.
        params.rowStep = 1;
        engine.Analyse(view, params, result);

        // Sweep threads against row step. Each thread carries a private bin buffer
        // that is zeroed and merged every frame, so the two axes trade off against
        // each other and the best combination is not obvious a priori.
        std::printf("              rowStep=1   rowStep=2   rowStep=4\n");
        for (int threads : { 1, 2, 4, 6, 8, 12, 16 })
        {
            engine.SetThreadCount(threads);
            std::printf("threads=%-3d ", threads);

            for (int step : { 1, 2, 4 })
            {
                params.rowStep = step;
                engine.Analyse(view, params, result);   // warm
                double best = 1e9;
                for (int rep = 0; rep < 5; ++rep)
                {
                    engine.Analyse(view, params, result);
                    if (result.millis < best) best = result.millis;
                }
                std::printf("%9.2f   ", best);
            }
            std::printf("\n");
        }
        std::printf("\n");

        // Re-run at full sampling so the checks below test every pixel, not
        // whatever the sweep happened to leave behind.
        engine.SetThreadCount(8);
        params.rowStep = 1;
        engine.Analyse(view, params, result);

        const uint64_t expected = static_cast<uint64_t>(width) * height;
        std::printf("full sampling: %llu px in %.2f ms  (expected %llu) %s\n",
                    static_cast<unsigned long long>(result.pixelsSampled),
                    result.millis,
                    static_cast<unsigned long long>(expected),
                    result.pixelsSampled == expected ? "ok" : "MISMATCH - work is being skipped");

        std::printf("luma: %s (%.4f, %.4f, %.4f)\n",
                    ColorSpaceName(result.colorSpace),
                    result.luma.r, result.luma.g, result.luma.b);
        std::printf("probe (centre px): (%.4f, %.4f, %.4f)\n",
                    result.probeRGB[0], result.probeRGB[1], result.probeRGB[2]);

        std::printf("\nmin RGB = (%.4f, %.4f, %.4f)\nmax RGB = (%.4f, %.4f, %.4f)\n",
                    result.minRGB[0], result.minRGB[1], result.minRGB[2],
                    result.maxRGB[0], result.maxRGB[1], result.maxRGB[2]);

        // The synthetic band exceeds 1.0, so the top bin must be occupied.
        uint64_t topBin = 0;
        for (uint32_t col = 0; col < kWaveformColumns; ++col)
            topBin += result.waveform[WaveformIndex(kPlaneR, col, kWaveformLevels - 1)];
        std::printf("\ntop waveform bin population (R): %llu %s\n",
                    static_cast<unsigned long long>(topBin),
                    topBin > 0 ? "- above-range values reached the bins"
                               : "- UNEXPECTED, should be > 0");

        // Every plane's bins must total exactly the pixels sampled. This is the
        // check that the histogram-derived-from-waveform shortcut is sound, and
        // that no sample was dropped or double counted.
        bool consistent = true;
        for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
        {
            uint64_t waveTotal = 0, histTotal = 0;
            for (uint32_t col = 0; col < kWaveformColumns; ++col)
                for (uint32_t level = 0; level < kWaveformLevels; ++level)
                    waveTotal += result.waveform[WaveformIndex(plane, col, level)];
            for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
                histTotal += result.histogram[HistogramIndex(plane, bin)];

            const bool ok = (waveTotal == result.pixelsSampled) && (histTotal == waveTotal);
            if (!ok) consistent = false;

            std::printf("plane %u: waveform total %llu, histogram total %llu  %s\n",
                        plane,
                        static_cast<unsigned long long>(waveTotal),
                        static_cast<unsigned long long>(histTotal),
                        ok ? "ok" : "MISMATCH");
        }

        uint64_t vecTotal = 0;
        for (uint32_t cell : result.vectorscope) vecTotal += cell;
        const bool vecOk = (vecTotal == result.pixelsSampled);
        if (!vecOk) consistent = false;
        std::printf("vectorscope total %llu  %s\n",
                    static_cast<unsigned long long>(vecTotal), vecOk ? "ok" : "MISMATCH");

        // Each pixel contributes to both diamonds (once for G/B, once for
        // G/R), so the combined total is twice pixelsSampled, not equal to it.
        uint64_t tpTotal = 0;
        for (uint32_t cell : result.twinPeaks) tpTotal += cell;
        const uint64_t tpExpected = result.pixelsSampled * 2;
        const bool tpOk = (tpTotal == tpExpected);
        if (!tpOk) consistent = false;
        std::printf("twin peaks total %llu (expected %llu)  %s\n",
                    static_cast<unsigned long long>(tpTotal),
                    static_cast<unsigned long long>(tpExpected), tpOk ? "ok" : "MISMATCH");

        // Sanity-check the waveform trace: distinct rows must actually be
        // distinct, not collapsed together by an accidental averaging bug.
        // FillSynthetic sets g=v (vertical ramp) uniformly across every
        // column, so row 0 (v=0) should read near 0 and the last row
        // (v~1) near 1. Sampled away from the synthetic boost band, which
        // sits near the horizontal centre and would otherwise skew this.
        //
        // The column holding source pixel x = width/8, found the way the
        // engine assigns pixels to columns. A source narrower than the
        // 512-column grid leaves most columns with no pixel at all, holding
        // 0 - so the old fixed kWaveformColumns/8 landed on an empty column
        // at --small (64 px) and failed while every bin total was correct.
        // At HD and UHD this is still column 64.
        uint32_t sampleCol = 0;
        const uint64_t sampleX = static_cast<uint64_t>(width) / 8;
        for (uint32_t c = 0; c < kWaveformColumns; ++c)
            if ((static_cast<uint64_t>(c) * width) / kWaveformColumns <= sampleX) sampleCol = c;
        const float traceG0 = result.waveformTrace[WaveformTraceIndex(0, sampleCol, kPlaneG)];
        const float traceGLast = result.waveformTrace[
            WaveformTraceIndex(kWaveformTraceRows - 1, sampleCol, kPlaneG)];
        const bool traceOk = (traceG0 < 0.05f) && (traceGLast > 0.9f);
        if (!traceOk) consistent = false;
        std::printf("waveform trace row 0 G=%.4f, row %u G=%.4f (expect ~0 and ~1)  %s\n",
                    traceG0, kWaveformTraceRows - 1, traceGLast, traceOk ? "ok" : "MISMATCH");

        std::printf("\npreview scale benchmark:\n");
        for (float previewScale : { 1.0f, 0.75f, 0.5f, 0.25f })
        {
            PreviewResult preview;
            const auto tPreview = std::chrono::steady_clock::now();
            engine.BuildPreview(view, previewScale, preview);
            const double previewMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tPreview).count();

            const bool previewSizeOk = preview.rgb.size() ==
                static_cast<size_t>(preview.width) * preview.height * 3;
            const bool previewFitsCanvas = (preview.width <= kPreviewCanvasWidth) &&
                                           (preview.height <= kPreviewCanvasHeight);
            const bool previewOk = previewSizeOk && previewFitsCanvas &&
                                   (preview.width > 0) && (preview.height > 0);
            if (!previewOk) consistent = false;
            std::printf("  %5.0f%%  %ux%u  %6.2f ms  %s\n",
                        previewScale * 100.0f, preview.width, preview.height, previewMs,
                        previewOk ? "ok" : "MISMATCH");
        }

        std::printf("\n%s\n", consistent ? "Bin accounting consistent."
                                         : "BIN ACCOUNTING FAILED.");
        return consistent ? 0 : 1;
    }

    ScopePublisher publisher;
    if (!publisher.Start(shmName.c_str()))
    {
        std::printf("Failed to open shared memory.\n");
        return 1;
    }

    std::printf("Publishing to shared memory '%s' as instance 999.%s\n\n",
                shmName.c_str(),
                frameLimit > 0 ? "" : " Ctrl+C to stop.");

    double phase = 0.0;
    uint64_t frame = 0;

    for (;;)
    {
        FillSynthetic(pixels, width, height, phase);
        view.pixels = pixels.data();

        params.rowStep = 1;
        engine.Analyse(view, params, result);

        publisher.Publish(result, static_cast<double>(frame),
                          static_cast<uint32_t>(width), static_cast<uint32_t>(height), 999);

        if ((frame % 24) == 0)
        {
            std::printf("frame %6llu  bin %6.2f ms  max R %.4f\n",
                        static_cast<unsigned long long>(frame), result.millis, result.maxRGB[0]);
            std::fflush(stdout);
        }

        ++frame;
        phase += 0.05;
        if (frameLimit > 0 && frame >= static_cast<uint64_t>(frameLimit))
        {
            std::printf("published %d frames, exiting\n", frameLimit);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}
