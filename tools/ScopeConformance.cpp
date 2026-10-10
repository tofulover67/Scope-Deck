// Conformance harness: one deterministic frame set, several reductions, an
// exact diff.
//
// This is the acceptance gate for the CUDA port and for every backend after
// it. It is written before the port rather than after, so that "correct" is
// defined by something that predates the code being judged by it.
//
// What it does:
//
//   - Generates a fixed frame set (kPatterns) at several frame sizes, across
//     every colour space and every Row Step.
//   - Reduces each frame with a deliberately plain, single-threaded reference
//     implementation written in this file, and with the real ScopeEngine at
//     several thread counts.
//   - Diffs them bit-exactly, reporting the first disagreement in each array
//     decoded into plane/column/level rather than as a flat index.
//   - Checks structural invariants that must hold for any backend at all:
//     sampling coverage, bin totals, and the histogram/waveform relationship.
//
// When the CUDA path lands it becomes one more candidate in the same loop,
// diffed against the same reference by the same code. Neither the matrix nor
// the comparison changes.
//
// Why exact and not a tolerance
// -----------------------------
// Every integer array here is order-independent: waveform, histogram,
// vectorscope and twin peaks are counts, and addition of counts commutes. With
// identical sampling, two correct backends must agree bit-for-bit, so any
// mismatch is a real sampling-coverage or bin-assignment bug rather than
// numerical drift. min/max/probe are selections, not accumulations, so they are
// exact too. Loosening any of this to a tolerance would destroy the only
// invariant that makes the harness worth running.
//
// The waveform trace is the one array with a genuine float accumulation in it -
// each cell is a box average over that column's pixel range (see
// ScopeEngine::BuildWaveformTrace) - so it is exact only as long as a backend
// sums the same values in the same order. That is a constraint on the CUDA
// kernel, not a reason to add a tolerance here: the bucket is a handful of
// pixels wide (7-8 at UHD), so a sequential per-thread sum reproduces it
// exactly, while a parallel reduction inside the bucket would not.
//
// Floats are compared by bit pattern, not by ==, with one documented exception:
// any NaN equals any other NaN, because IEEE-754 does not specify which payload
// an operation propagates and x86 and the GPU demonstrably differ. A backend
// that turns a NaN into a number, or a number into a NaN, still fails - only
// the payload is forgiven. See BitsEqual below.

#include "ScopeCore.h"

// A GPU backend joins as one more candidate, compiled in only for the
// GPU-enabled targets (see CMakeLists.txt): CUDA for scope_conformance_cuda,
// Metal for scope_conformance_metal. Both offer the same Available / Error /
// DeviceDescription / Analyse(..., timing) surface, so one harness body
// serves both through the aliases below. The CPU-only build is unchanged and
// still the one that runs everywhere.
#if defined(SCOPE_HAVE_CUDA)
#include "ScopeCuda.h"
#define SCOPE_HAVE_GPU 1
namespace scopedeck
{
    using GpuReducer       = CudaReducer;
    using GpuReduceTiming  = CudaReduceTiming;
    constexpr const char* kGpuBackendName = "cuda";
}
#elif defined(SCOPE_HAVE_METAL)
#include "ScopeMetal.h"
#define SCOPE_HAVE_GPU 1
namespace scopedeck
{
    using GpuReducer       = MetalReducer;
    using GpuReduceTiming  = MetalReduceTiming;
    constexpr const char* kGpuBackendName = "metal";
}
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace scopedeck;

namespace
{

// ---------------------------------------------------------------------------
// Bit-exact float comparison
// ---------------------------------------------------------------------------

uint32_t FloatBits(float p_Value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &p_Value, sizeof(bits));
    return bits;
}

// Equal bit patterns, with one deliberate exception: any NaN equals any other
// NaN. IEEE-754 does not specify which payload an operation propagates, and the
// two platforms genuinely differ - x86 carries an input NaN's payload through
// an addition (0x7fc00000) where the GPU produces 0x7fffffff. Neither is wrong,
// and a scope reading "NaN" means the same thing either way: no value. This is
// a statement about what a float *means*, not a numeric tolerance. Every finite
// value is still compared bit-for-bit, and a NaN appearing where a number
// belongs - or the reverse - still fails.
bool BitsEqual(float p_A, float p_B)
{
    const bool aNan = (p_A != p_A), bNan = (p_B != p_B);
    if (aNan || bNan) return aNan && bNan;
    return FloatBits(p_A) == FloatBits(p_B);
}

// Does this build contract a*b+c into a single fused multiply-add?
//
// It matters because bin assignment runs a luma dot product through a
// scale-and-floor: a luma sitting near a bin boundary lands in different bins
// on a contracted and a non-contracted path, and counts then shift between
// *adjacent* cells. That looks exactly like the coverage bug this harness
// exists to catch, and the natural reaction - loosening to a tolerance -
// destroys the invariant above. nvcc contracts by default, so the CUDA target
// must be built with -fmad=false.
//
// (1 + 2^-12) squared is 1 + 2^-11 + 2^-24. The trailing term is exactly half
// an ulp, so round-half-to-even drops it and a rounded product equals
// 1 + 2^-11 exactly; a fused operation keeps it and the difference survives.
// volatile keeps the compiler from folding the whole thing at compile time.
//
// This only observes the translation unit it is compiled in - it cannot see
// inside ScopeCore.cpp. Both are built from the same CMake flags, so a
// contraction setting that reaches one reaches the other, but this is a build
// check, not a proof about the engine.
bool FmaContractionDetected()
{
    volatile float a = 1.0f + 0x1p-12f;
    volatile float c = 1.0f + 0x1p-11f;
    const float d = a * a - c;
    return !BitsEqual(d, 0.0f);
}

// ---------------------------------------------------------------------------
// Bin arithmetic
//
// Reimplemented here rather than shared with ScopeCore.cpp, whose copies live
// in an anonymous namespace. The duplication is the point: a shared helper
// could not disagree, and disagreement is what is being tested. Every constant
// and every expression form below is deliberately identical to the engine's -
// including the precomputed reciprocals, since (b - luma) * (0.5f / (1 - Kb))
// and (b - luma) * 0.5f / (1 - Kb) do not round the same way.
// ---------------------------------------------------------------------------

constexpr float kBinScale =
    static_cast<float>(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);

uint32_t RefBinOf(float p_Value, uint32_t p_BinCount)
{
    const float t = (p_Value - kBinRangeLow) * kBinScale;
    if (!(t > 0.0f)) return 0;
    // +inf; see ScopeCore.cpp's BinOf
    if (t >= static_cast<float>(p_BinCount)) return p_BinCount - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= p_BinCount) ? (p_BinCount - 1) : bin;
}

constexpr float kChromaBinScale =
    static_cast<float>(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);

uint32_t RefChromaBinOf(float p_Value)
{
    const float t = (p_Value - kChromaRangeLow) * kChromaBinScale;
    if (!(t > 0.0f)) return 0;
    if (t >= static_cast<float>(kVectorscopeSize)) return kVectorscopeSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kVectorscopeSize) ? (kVectorscopeSize - 1) : bin;
}

constexpr float kTwinPeaksDiffBinScale =
    static_cast<float>(kTwinPeaksSize - 1) / (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);

uint32_t RefTwinPeaksDiffBinOf(float p_Value)
{
    const float t = (p_Value - kTwinPeaksDiffRangeLow) * kTwinPeaksDiffBinScale;
    if (!(t > 0.0f)) return 0;
    if (t >= static_cast<float>(kTwinPeaksSize)) return kTwinPeaksSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kTwinPeaksSize) ? (kTwinPeaksSize - 1) : bin;
}

// ---------------------------------------------------------------------------
// The reference reduction
//
// Plain, single-threaded, and structured deliberately unlike ScopeEngine:
//
//   - Pixels are walked in x order and their column is looked up, rather than
//     walking columns and taking each one's pixel range. Same partition,
//     traversed from the opposite direction, so a bucket-boundary error shows
//     up as a disagreement instead of being reproduced identically.
//   - The histogram is binned directly per pixel instead of being derived by
//     summing the waveform over columns the way the engine does. That makes
//     the engine's derivation something this harness checks rather than
//     something it assumes.
//
// The column boundary convention is copied exactly, and that is not a detail:
// the engine assigns pixel x to the largest column c with floor(c*w/cols) <= x,
// which is NOT the same mapping as floor(x*cols/w). At w=3840 the two already
// disagree on pixel 7. Any backend must use the boundary table, not the
// apparently-equivalent direct division.
// ---------------------------------------------------------------------------

void ReferenceAnalyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out)
{
    p_Out.waveform.assign(static_cast<size_t>(kWaveformCells), 0u);
    p_Out.histogram.assign(static_cast<size_t>(kHistogramCells), 0u);
    p_Out.vectorscope.assign(static_cast<size_t>(kVectorscopeTotalCells), 0u);
    p_Out.twinPeaks.assign(static_cast<size_t>(kTwinPeaksTotalCells), 0u);
    p_Out.waveformTrace.assign(static_cast<size_t>(kWaveformTraceCells), 0.0f);
    p_Out.pixelsSampled = 0;
    p_Out.colorSpace = p_Params.colorSpace;
    p_Out.luma = p_Params.luma;
    for (int c = 0; c < 3; ++c) { p_Out.minRGB[c] = 0.0f; p_Out.maxRGB[c] = 0.0f; p_Out.probeRGB[c] = 0.0f; }

    const int width  = p_Frame.width;
    const int height = p_Frame.height;
    if (!p_Frame.pixels || width <= 0 || height <= 0) return;

    int rowStep = p_Params.rowStep;
    if (rowStep < 1) rowStep = 1;

    std::vector<int> colStart(static_cast<size_t>(kWaveformColumns) + 1);
    for (uint32_t col = 0; col <= kWaveformColumns; ++col)
    {
        const uint64_t x = (static_cast<uint64_t>(col) * static_cast<uint64_t>(width)) / kWaveformColumns;
        colStart[col] = static_cast<int>(x < static_cast<uint64_t>(width) ? x : static_cast<uint64_t>(width));
    }
    colStart[kWaveformColumns] = width;

    {
        const float* centreRow = p_Frame.Row(height / 2);
        if (centreRow)
        {
            const float* px = centreRow + static_cast<ptrdiff_t>(width / 2) * 4;
            p_Out.probeRGB[0] = px[0];
            p_Out.probeRGB[1] = px[1];
            p_Out.probeRGB[2] = px[2];
        }
    }

    const float lumaR = p_Params.luma.r;
    const float lumaG = p_Params.luma.g;
    const float lumaB = p_Params.luma.b;
    const float cbScale = 0.5f / (1.0f - lumaB);
    const float crScale = 0.5f / (1.0f - lumaR);

    float lo[3] = {  std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity() };
    float hi[3] = { -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity() };

    for (int y = 0; y < height; y += rowStep)
    {
        const float* row = p_Frame.Row(y);
        if (!row) continue;

        for (int x = 0; x < width; ++x)
        {
            const int col = static_cast<int>(
                std::upper_bound(colStart.begin(), colStart.end(), x) - colStart.begin()) - 1;

            const float* px = row + static_cast<ptrdiff_t>(x) * 4;
            const float r = px[0];
            const float g = px[1];
            const float b = px[2];

            if (r < lo[0]) lo[0] = r;
            if (r > hi[0]) hi[0] = r;
            if (g < lo[1]) lo[1] = g;
            if (g > hi[1]) hi[1] = g;
            if (b < lo[2]) lo[2] = b;
            if (b > hi[2]) hi[2] = b;

            const float luma = lumaR * r + lumaG * g + lumaB * b;

            const uint32_t binR = RefBinOf(r, kWaveformLevels);
            const uint32_t binG = RefBinOf(g, kWaveformLevels);
            const uint32_t binB = RefBinOf(b, kWaveformLevels);
            const uint32_t binY = RefBinOf(luma, kWaveformLevels);

            const uint32_t ucol = static_cast<uint32_t>(col);
            ++p_Out.waveform[WaveformIndex(kPlaneR, ucol, binR)];
            ++p_Out.waveform[WaveformIndex(kPlaneG, ucol, binG)];
            ++p_Out.waveform[WaveformIndex(kPlaneB, ucol, binB)];
            ++p_Out.waveform[WaveformIndex(kPlaneY, ucol, binY)];

            ++p_Out.histogram[HistogramIndex(kPlaneR, binR)];
            ++p_Out.histogram[HistogramIndex(kPlaneG, binG)];
            ++p_Out.histogram[HistogramIndex(kPlaneB, binB)];
            ++p_Out.histogram[HistogramIndex(kPlaneY, binY)];

            const float cb = (b - luma) * cbScale;
            const float cr = (r - luma) * crScale;
            ++p_Out.vectorscope[VectorscopeIndex(VectorscopeBandOf(luma),
                                                 RefChromaBinOf(cb), RefChromaBinOf(cr))];

            const float levelGB = (g + b) * 0.5f;
            const float diffGB  = (g - b) * 0.5f;
            ++p_Out.twinPeaks[TwinPeaksIndex(kDiamondGreenBlue,
                                             RefBinOf(levelGB, kTwinPeaksSize),
                                             RefTwinPeaksDiffBinOf(diffGB))];

            const float levelGR = (g + r) * 0.5f;
            const float diffGR  = (g - r) * 0.5f;
            ++p_Out.twinPeaks[TwinPeaksIndex(kDiamondGreenRed,
                                             RefBinOf(levelGR, kTwinPeaksSize),
                                             RefTwinPeaksDiffBinOf(diffGR))];

            ++p_Out.pixelsSampled;
        }
    }

    for (int c = 0; c < 3; ++c)
    {
        const bool sawAny = (p_Out.pixelsSampled > 0);
        p_Out.minRGB[c] = sawAny ? lo[c] : 0.0f;
        p_Out.maxRGB[c] = sawAny ? hi[c] : 0.0f;
    }

    // The trace picks its own evenly strided rows and ignores rowStep entirely
    // (see kWaveformTraceRows in ScopeTypes.h).
    for (uint32_t traceRow = 0; traceRow < kWaveformTraceRows; ++traceRow)
    {
        const int srcY = (kWaveformTraceRows > 1)
            ? static_cast<int>((uint64_t(traceRow) * uint64_t(height - 1)) / (kWaveformTraceRows - 1))
            : 0;
        const float* rowPixels = p_Frame.Row(srcY);
        if (!rowPixels) continue;

        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            const int x0 = colStart[col];
            const int x1 = colStart[col + 1];

            float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
            int counted = 0;
            for (int x = x0; x < x1; ++x)
            {
                const float* px = rowPixels + static_cast<ptrdiff_t>(x) * 4;
                sumR += px[0];
                sumG += px[1];
                sumB += px[2];
                ++counted;
            }

            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (counted > 0)
            {
                const float inv = 1.0f / static_cast<float>(counted);
                r = sumR * inv;
                g = sumG * inv;
                b = sumB * inv;
            }
            const float luma = lumaR * r + lumaG * g + lumaB * b;

            const uint64_t base = WaveformTraceIndex(traceRow, col, 0);
            p_Out.waveformTrace[base + kPlaneR] = r;
            p_Out.waveformTrace[base + kPlaneG] = g;
            p_Out.waveformTrace[base + kPlaneB] = b;
            p_Out.waveformTrace[base + kPlaneY] = luma;
        }
    }
}

// ---------------------------------------------------------------------------
// The frame set
//
// Every pattern is a pure function of (x, y, width, height), so a case is
// reproducible from its name alone with no stored fixtures.
//
// Sizes matter as much as content. 64x64 is narrower than the waveform is wide,
// so most of the 512 columns are empty and the ones that are not hold a single
// pixel - the degenerate end. 517x289 is prime-ish: every column holds one or
// two pixels, and neither dimension divides by any Row Step under test, so the
// remainder rows and the ragged last bucket are always in play. Those are the
// two places CPU/GPU divergence hides.
// ---------------------------------------------------------------------------

struct Pattern
{
    const char* name;
    void (*fill)(float* p_Px, int p_X, int p_Y, int p_W, int p_H);
};

void FillBlack(float* p_Px, int, int, int, int)
{
    p_Px[0] = p_Px[1] = p_Px[2] = 0.0f;
}

void FillWhite(float* p_Px, int, int, int, int)
{
    p_Px[0] = p_Px[1] = p_Px[2] = 1.0f;
}

void FillGrey(float* p_Px, int, int, int, int)
{
    p_Px[0] = p_Px[1] = p_Px[2] = 0.5f;
}

// 100% bars, in the standard order. Vertical bars are the interesting case for
// a waveform: each one must land in its own contiguous run of columns, so a
// bucketing error smears a bar edge across the boundary.
void FillBars100(float* p_Px, int p_X, int, int p_W, int)
{
    static const float kBars[8][3] = {
        { 1.0f, 1.0f, 1.0f },   // white
        { 1.0f, 1.0f, 0.0f },   // yellow
        { 0.0f, 1.0f, 1.0f },   // cyan
        { 0.0f, 1.0f, 0.0f },   // green
        { 1.0f, 0.0f, 1.0f },   // magenta
        { 1.0f, 0.0f, 0.0f },   // red
        { 0.0f, 0.0f, 1.0f },   // blue
        { 0.0f, 0.0f, 0.0f },   // black
    };
    int bar = (p_X * 8) / (p_W > 0 ? p_W : 1);
    if (bar > 7) bar = 7;
    p_Px[0] = kBars[bar][0];
    p_Px[1] = kBars[bar][1];
    p_Px[2] = kBars[bar][2];
}

// Three different ramps so a channel mix-up is obvious in the diff output.
void FillGradient(float* p_Px, int p_X, int p_Y, int p_W, int p_H)
{
    const float u = (p_W > 1) ? static_cast<float>(p_X) / static_cast<float>(p_W - 1) : 0.0f;
    const float v = (p_H > 1) ? static_cast<float>(p_Y) / static_cast<float>(p_H - 1) : 0.0f;
    p_Px[0] = u;
    p_Px[1] = v;
    p_Px[2] = 0.5f * (u + v);
}

// One lit pixel in an otherwise black frame, in the bottom-right corner. The
// corner is where a remainder bug drops a pixel, and a scope that loses a lone
// highlight is failing at the one job someone opened it for.
void FillHotPixel(float* p_Px, int p_X, int p_Y, int p_W, int p_H)
{
    const bool hot = (p_X == p_W - 1) && (p_Y == p_H - 1);
    p_Px[0] = p_Px[1] = p_Px[2] = hot ? 1.0f : 0.0f;
}

// Values outside [0,1] are real - the tap has measured 1.0332 in the wild - so
// this sweeps from below kBinRangeLow to above kBinRangeHigh and plants the
// measured value exactly. Everything below the range must land in bin 0 and
// everything above in the last bin, on every backend, while minRGB/maxRGB carry
// the true extremes through unclamped.
void FillOutOfRange(float* p_Px, int p_X, int p_Y, int p_W, int p_H)
{
    const float u = (p_W > 1) ? static_cast<float>(p_X) / static_cast<float>(p_W - 1) : 0.0f;
    const float v = (p_H > 1) ? static_cast<float>(p_Y) / static_cast<float>(p_H - 1) : 0.0f;
    p_Px[0] = -0.25f + 1.60f * u;            // spans both ends of the bin range
    p_Px[1] = (p_X == p_W / 2) ? 1.0332f : (1.20f * v - 0.10f);
    p_Px[2] = (p_Y == 0) ? -0.5f : 1.0332f;
}

// NaN and both infinities, scattered deterministically over mid grey.
//
// Not in the original plan, and cheap to add: BinOf rejects NaN through
// !(t > 0.0f), and the engine's min/max use bare < and > so a NaN never
// becomes an extreme. CUDA's fminf/fmaxf propagate differently, which would
// silently change published minRGB/maxRGB on exactly the frames a colourist
// is most likely to be staring at.
void FillNanInf(float* p_Px, int p_X, int p_Y, int p_W, int p_H)
{
    p_Px[0] = p_Px[1] = p_Px[2] = 0.5f;
    const int which = (p_X + p_Y * 7) % 23;
    if (which == 0)  p_Px[0] = std::numeric_limits<float>::quiet_NaN();
    if (which == 5)  p_Px[1] = std::numeric_limits<float>::infinity();
    if (which == 11) p_Px[2] = -std::numeric_limits<float>::infinity();
    (void)p_W; (void)p_H;
}

const Pattern kPatterns[] = {
    { "black",      FillBlack      },
    { "white",      FillWhite      },
    { "grey",       FillGrey       },
    { "bars100",    FillBars100    },
    { "gradient",   FillGradient   },
    { "hotpixel",   FillHotPixel   },
    { "outofrange", FillOutOfRange },
    { "naninf",     FillNanInf     },
};

// Bench-only, and deliberately NOT in kPatterns: adding it there would change
// the gate's documented 960-case count for no correctness gain, since a
// pseudo-random field exercises no mapping the patterns above do not.
//
// It exists because of §3's aggregation table, where content falls into three
// classes: real footage (neighbouring pixels share a bin, so aggregation wins
// big), the degenerate case (every pixel in ONE cell - black/white/grey above),
// and fully spread, where there is nothing to aggregate and the kernel pays
// full price. Spread is the slowest case under aggregation and the one that
// cannot occur in footage, which makes it the honest upper bound to quote.
// A hash of (x, y) rather than a PRNG, so a thread's value depends on nothing
// but its own coordinates and the frame is reproducible.
void FillSpread(float* p_Px, int p_X, int p_Y, int, int)
{
    uint32_t h = static_cast<uint32_t>(p_X) * 0x9E3779B1u ^ static_cast<uint32_t>(p_Y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    for (int c = 0; c < 3; ++c)
    {
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        p_Px[c] = static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
    }
}

const Pattern kBenchPatterns[] = {
    { "gradient (footage-like)", FillGradient   },
    { "spread (worst for aggregation)", FillSpread },
    { "black (fade to black)",   FillBlack      },
    { "white (blown highlights)", FillWhite     },
    { "grey (all one cell)",     FillGrey       },
    { "bars100",                 FillBars100    },
    { "hotpixel",                FillHotPixel   },
    { "outofrange",              FillOutOfRange },
};

struct Frame
{
    std::vector<float> pixels;
    int width  = 0;
    int height = 0;

    FrameView View() const
    {
        FrameView view;
        view.pixels   = pixels.data();
        view.width    = width;
        view.height   = height;
        view.rowBytes = width * 4 * static_cast<int>(sizeof(float));
        return view;
    }
};

void BuildFrame(const Pattern& p_Pattern, int p_Width, int p_Height, Frame& p_Out)
{
    p_Out.width  = p_Width;
    p_Out.height = p_Height;
    p_Out.pixels.assign(static_cast<size_t>(p_Width) * p_Height * 4, 0.0f);

    for (int y = 0; y < p_Height; ++y)
    {
        float* row = p_Out.pixels.data() + static_cast<size_t>(y) * p_Width * 4;
        for (int x = 0; x < p_Width; ++x)
        {
            float* px = row + static_cast<size_t>(x) * 4;
            p_Pattern.fill(px, x, y, p_Width, p_Height);
            px[3] = 1.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Diffing
// ---------------------------------------------------------------------------

std::string DecodeWaveform(uint64_t p_Index)
{
    const uint32_t plane = static_cast<uint32_t>(p_Index % kPlaneCount);
    const uint64_t rest  = p_Index / kPlaneCount;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "plane=%u column=%u level=%u",
                  plane,
                  static_cast<uint32_t>(rest / kWaveformLevels),
                  static_cast<uint32_t>(rest % kWaveformLevels));
    return buf;
}

std::string DecodeHistogram(uint64_t p_Index)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "plane=%u bin=%u",
                  static_cast<uint32_t>(p_Index / kHistogramBins),
                  static_cast<uint32_t>(p_Index % kHistogramBins));
    return buf;
}

std::string DecodeVectorscope(uint64_t p_Index)
{
    const uint64_t band = p_Index / kVectorscopeCells;
    const uint64_t rest = p_Index % kVectorscopeCells;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "band=%u cr=%u cb=%u",
                  static_cast<uint32_t>(band),
                  static_cast<uint32_t>(rest / kVectorscopeSize),
                  static_cast<uint32_t>(rest % kVectorscopeSize));
    return buf;
}

std::string DecodeTwinPeaks(uint64_t p_Index)
{
    const uint64_t diamond = p_Index / kTwinPeaksCells;
    const uint64_t rest    = p_Index % kTwinPeaksCells;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "diamond=%s level=%u diff=%u",
                  diamond == kDiamondGreenBlue ? "G-B" : "G-R",
                  static_cast<uint32_t>(rest / kTwinPeaksSize),
                  static_cast<uint32_t>(rest % kTwinPeaksSize));
    return buf;
}

std::string DecodeTrace(uint64_t p_Index)
{
    const uint32_t plane = static_cast<uint32_t>(p_Index % kPlaneCount);
    const uint64_t rest  = p_Index / kPlaneCount;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "plane=%u row=%u column=%u",
                  plane,
                  static_cast<uint32_t>(rest / kWaveformColumns),
                  static_cast<uint32_t>(rest % kWaveformColumns));
    return buf;
}

bool DiffCounts(const char* p_Name, const std::vector<uint32_t>& p_Ref,
                const std::vector<uint32_t>& p_Cand,
                std::string (*p_Decode)(uint64_t), std::vector<std::string>& p_Errors)
{
    if (p_Ref.size() != p_Cand.size())
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: size %zu vs %zu", p_Name, p_Ref.size(), p_Cand.size());
        p_Errors.push_back(buf);
        return false;
    }

    uint64_t differing = 0;
    uint64_t firstIndex = 0;
    for (size_t i = 0; i < p_Ref.size(); ++i)
    {
        if (p_Ref[i] != p_Cand[i])
        {
            if (differing == 0) firstIndex = i;
            ++differing;
        }
    }
    if (differing == 0) return true;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s: %llu cells differ; first at %s (reference %u, candidate %u)",
                  p_Name, static_cast<unsigned long long>(differing),
                  p_Decode(firstIndex).c_str(), p_Ref[firstIndex], p_Cand[firstIndex]);
    p_Errors.push_back(buf);
    return false;
}

bool DiffFloats(const char* p_Name, const std::vector<float>& p_Ref,
                const std::vector<float>& p_Cand,
                std::string (*p_Decode)(uint64_t), std::vector<std::string>& p_Errors)
{
    if (p_Ref.size() != p_Cand.size())
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: size %zu vs %zu", p_Name, p_Ref.size(), p_Cand.size());
        p_Errors.push_back(buf);
        return false;
    }

    uint64_t differing = 0;
    uint64_t firstIndex = 0;
    for (size_t i = 0; i < p_Ref.size(); ++i)
    {
        if (!BitsEqual(p_Ref[i], p_Cand[i]))
        {
            if (differing == 0) firstIndex = i;
            ++differing;
        }
    }
    if (differing == 0) return true;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s: %llu cells differ; first at %s (reference %.9g [%08x], candidate %.9g [%08x])",
                  p_Name, static_cast<unsigned long long>(differing),
                  p_Decode(firstIndex).c_str(),
                  static_cast<double>(p_Ref[firstIndex]), FloatBits(p_Ref[firstIndex]),
                  static_cast<double>(p_Cand[firstIndex]), FloatBits(p_Cand[firstIndex]));
    p_Errors.push_back(buf);
    return false;
}

void DiffTriple(const char* p_Name, const float* p_Ref, const float* p_Cand,
                std::vector<std::string>& p_Errors)
{
    for (int c = 0; c < 3; ++c)
    {
        if (BitsEqual(p_Ref[c], p_Cand[c])) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s[%d]: reference %.9g [%08x], candidate %.9g [%08x]",
                      p_Name, c,
                      static_cast<double>(p_Ref[c]), FloatBits(p_Ref[c]),
                      static_cast<double>(p_Cand[c]), FloatBits(p_Cand[c]));
        p_Errors.push_back(buf);
    }
}

void DiffResults(const ScopeResult& p_Ref, const ScopeResult& p_Cand,
                 std::vector<std::string>& p_Errors)
{
    DiffCounts("waveform",    p_Ref.waveform,    p_Cand.waveform,    DecodeWaveform,    p_Errors);
    DiffCounts("histogram",   p_Ref.histogram,   p_Cand.histogram,   DecodeHistogram,   p_Errors);
    DiffCounts("vectorscope", p_Ref.vectorscope, p_Cand.vectorscope, DecodeVectorscope, p_Errors);
    DiffCounts("twinPeaks",   p_Ref.twinPeaks,   p_Cand.twinPeaks,   DecodeTwinPeaks,   p_Errors);
    DiffFloats("waveformTrace", p_Ref.waveformTrace, p_Cand.waveformTrace, DecodeTrace, p_Errors);

    DiffTriple("minRGB",  p_Ref.minRGB,  p_Cand.minRGB,  p_Errors);
    DiffTriple("maxRGB",  p_Ref.maxRGB,  p_Cand.maxRGB,  p_Errors);
    DiffTriple("probeRGB", p_Ref.probeRGB, p_Cand.probeRGB, p_Errors);

    if (p_Ref.pixelsSampled != p_Cand.pixelsSampled)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "pixelsSampled: reference %llu, candidate %llu",
                      static_cast<unsigned long long>(p_Ref.pixelsSampled),
                      static_cast<unsigned long long>(p_Cand.pixelsSampled));
        p_Errors.push_back(buf);
    }
}

// ---------------------------------------------------------------------------
// Structural invariants
//
// These hold for any correct backend regardless of what any other backend does,
// so they catch a bug that both paths share - which a diff by construction
// cannot. Sampling coverage is the important one: pixelsSampled must be exactly
// the rows Row Step selects times the full frame width, and nothing else.
// ---------------------------------------------------------------------------

void CheckInvariants(const ScopeResult& p_Result, int p_Width, int p_Height, int p_RowStep,
                     std::vector<std::string>& p_Errors)
{
    char buf[256];

    const uint64_t sampledRows = (p_Height > 0)
        ? static_cast<uint64_t>((p_Height - 1) / p_RowStep) + 1 : 0;
    const uint64_t expected = sampledRows * static_cast<uint64_t>(p_Width);

    if (p_Result.pixelsSampled != expected)
    {
        std::snprintf(buf, sizeof(buf),
                      "coverage: pixelsSampled %llu, expected %llu (%llu rows x %d px)",
                      static_cast<unsigned long long>(p_Result.pixelsSampled),
                      static_cast<unsigned long long>(expected),
                      static_cast<unsigned long long>(sampledRows), p_Width);
        p_Errors.push_back(buf);
    }

    uint64_t waveTotal = 0;
    for (uint32_t v : p_Result.waveform) waveTotal += v;
    if (waveTotal != expected * kPlaneCount)
    {
        std::snprintf(buf, sizeof(buf), "waveform total %llu, expected %llu",
                      static_cast<unsigned long long>(waveTotal),
                      static_cast<unsigned long long>(expected * kPlaneCount));
        p_Errors.push_back(buf);
    }

    uint64_t histTotal = 0;
    for (uint32_t v : p_Result.histogram) histTotal += v;
    if (histTotal != expected * kPlaneCount)
    {
        std::snprintf(buf, sizeof(buf), "histogram total %llu, expected %llu",
                      static_cast<unsigned long long>(histTotal),
                      static_cast<unsigned long long>(expected * kPlaneCount));
        p_Errors.push_back(buf);
    }

    uint64_t vecTotal = 0;
    for (uint32_t v : p_Result.vectorscope) vecTotal += v;
    if (vecTotal != expected)
    {
        std::snprintf(buf, sizeof(buf), "vectorscope total %llu, expected %llu",
                      static_cast<unsigned long long>(vecTotal),
                      static_cast<unsigned long long>(expected));
        p_Errors.push_back(buf);
    }

    uint64_t tpTotal = 0;
    for (uint32_t v : p_Result.twinPeaks) tpTotal += v;
    if (tpTotal != expected * kDiamondCount)
    {
        std::snprintf(buf, sizeof(buf), "twinPeaks total %llu, expected %llu",
                      static_cast<unsigned long long>(tpTotal),
                      static_cast<unsigned long long>(expected * kDiamondCount));
        p_Errors.push_back(buf);
    }

    // The histogram must be the waveform summed over columns - same bin count,
    // same range, so this is an identity and not an approximation.
    for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
    {
        for (uint32_t level = 0; level < kWaveformLevels; ++level)
        {
            uint64_t colSum = 0;
            for (uint32_t col = 0; col < kWaveformColumns; ++col)
                colSum += p_Result.waveform[WaveformIndex(plane, col, level)];

            const uint32_t hist = p_Result.histogram[HistogramIndex(plane, level)];
            if (colSum != hist)
            {
                std::snprintf(buf, sizeof(buf),
                              "histogram/waveform mismatch at plane=%u bin=%u: %llu vs %u",
                              plane, level, static_cast<unsigned long long>(colSum), hist);
                p_Errors.push_back(buf);
                return;   // one is enough; they will all be wrong the same way
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------

struct Size { int width; int height; const char* note; };

const Size kSizes[] = {
    {  64,  64, "narrower than the waveform: most columns empty" },
    { 517, 289, "prime-ish: ragged buckets, no Row Step divides it" },
};

const int kRowSteps[] = { 1, 2, 3, 4, 8 };

// Candidate thread counts. 1 takes the banding code down to a single band, 3 is
// a count no frame height here divides evenly by, and 8 is the engine's own
// default ceiling.
const int kThreadCounts[] = { 1, 2, 3, 8 };

struct Options
{
    bool verbose = false;
    bool hd = false;
    bool bench = false;
    int  benchIters = 200;
    std::string filter;
};

#ifdef SCOPE_HAVE_GPU
// --- the adversarial-content bench -------------------------------------------
//
// Answers one question the ship gate cannot: the gate ran real graded footage,
// which §3 measured as the EASIEST case for warp aggregation, not the hardest.
// Warp aggregation was proven on the spike's 5 atomics per pixel; the shipping
// kernel runs 7 plus the trace, probe and min/max reductions. This drives that
// kernel - the same one the tap uses, launched by the same code conformance
// tests - over content chosen to be hostile to it.
//
// Kernel time only. The upload is reported separately and is not the tap's
// cost: the tap's frame is already resident on the card. Quoting the total here
// as "what the GPU tap costs" would be the fetch=/copy= conflation again.
int RunBench(GpuReducer& p_Cuda, const Options& p_Options)
{
    if (!p_Cuda.Available())
    {
        std::printf("bench: no %s device (%s)\n", kGpuBackendName, p_Cuda.Error());
        return 1;
    }

    const int width = 3840, height = 2160;
    std::printf("adversarial bench: %s\n", p_Cuda.DeviceDescription());
    std::printf("%dx%d, %d iterations per pattern, Rec.709, step=1\n", width, height, p_Options.benchIters);
    std::printf("kernel time only - upload excluded, because the tap never pays it\n\n");
    std::printf("  %-34s %8s %8s %8s %8s   %s\n",
                "content", "p50", "p90", "p99", "max", "(ms)");

    ScopeParams params;
    params.colorSpace = 0;
    params.luma = LumaWeightsFor(0);
    params.rowStep = 1;

    Frame frame;
    ScopeResult out;
    double worstP99 = 0.0;
    const char* worstName = "";

    for (const Pattern& pattern : kBenchPatterns)
    {
        BuildFrame(pattern, width, height, frame);
        const FrameView view = frame.View();

        std::vector<double> kernel;
        std::vector<double> upload;
        kernel.reserve(static_cast<size_t>(p_Options.benchIters));
        upload.reserve(static_cast<size_t>(p_Options.benchIters));

        // One untimed pass so allocation and any first-launch cost land outside
        // the sample - the same warmup effect that shows up as a 20-30 ms first
        // frame in the tap's own log.
        GpuReduceTiming warm;
        if (!p_Cuda.Analyse(view, params, out, &warm))
        {
            std::printf("  %-34s did not run (%s)\n", pattern.name, p_Cuda.Error());
            return 1;
        }

        for (int i = 0; i < p_Options.benchIters; ++i)
        {
            GpuReduceTiming t;
            if (!p_Cuda.Analyse(view, params, out, &t))
            {
                std::printf("  %-34s did not run (%s)\n", pattern.name, p_Cuda.Error());
                return 1;
            }
            if (t.valid) { kernel.push_back(t.msKernels); upload.push_back(t.msUpload); }
        }
        if (kernel.empty())
        {
            std::printf("  %-34s no timing available\n", pattern.name);
            return 1;
        }

        std::sort(kernel.begin(), kernel.end());
        std::sort(upload.begin(), upload.end());
        const size_t n = kernel.size();
        const double p50 = kernel[n / 2];
        const double p90 = kernel[(n * 90) / 100];
        const double p99 = kernel[(n * 99) / 100];
        const double mx  = kernel[n - 1];
        if (p99 > worstP99) { worstP99 = p99; worstName = pattern.name; }

        std::printf("  %-34s %8.3f %8.3f %8.3f %8.3f   upload %.2f\n",
                    pattern.name, p50, p90, p99, mx, upload[upload.size() / 2]);
    }

    std::printf("\nworst p99: %.3f ms (%s)\n", worstP99, worstName);
    std::printf("The ship gate's budget is 3-4 ms for the complete GPU tap, and the tap\n"
                "adds the passthrough and the publish leg on top of the figures above.\n");
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--verbose") options.verbose = true;
        else if (arg == "--hd") options.hd = true;
        else if (arg == "--bench") options.bench = true;
        else if (arg == "--bench-iters" && i + 1 < argc) options.benchIters = std::atoi(argv[++i]);
        else if (arg == "--filter" && i + 1 < argc) options.filter = argv[++i];
        else
        {
            std::printf("Usage: scope_conformance [--verbose] [--hd] [--filter <substring>]\n"
                        "                         [--bench [--bench-iters N]]\n\n"
                        "  --verbose      print every case, not just failures\n"
                        "  --hd           add a 1920x1080 sweep (slower)\n"
                        "  --filter       only run cases whose name contains this substring\n"
                        "  --bench        time the GPU kernels on adversarial content instead\n"
                        "                 of checking correctness (CUDA and Metal builds only)\n"
                        "  --bench-iters  iterations per pattern, default 200\n");
            return arg == "--help" ? 0 : 2;
        }
    }

    std::printf("Scope Deck conformance harness\n");
    std::printf("wire format v%u  waveform %ux%ux%u  vectorscope %u^2 x%u  "
                "twin peaks %u^2 x%u  trace %ux%u\n",
                kVersion, kWaveformColumns, kWaveformLevels, kPlaneCount,
                kVectorscopeSize, kBandCount, kTwinPeaksSize, kDiamondCount,
                kWaveformTraceRows, kWaveformColumns);

    if (FmaContractionDetected())
    {
        std::printf("\nFAIL: this build contracts a*b+c into an FMA.\n"
                    "      Bin assignment runs a luma dot product through a scale-and-floor,\n"
                    "      so a contracted build lands boundary values in different bins and\n"
                    "      every exact comparison below becomes meaningless. Build with\n"
                    "      contraction off (-fmad=false on nvcc, /fp:precise on MSVC).\n");
        return 1;
    }
    std::printf("fma contraction: off\n\n");

    std::vector<Size> sizes(std::begin(kSizes), std::end(kSizes));
    if (options.hd) sizes.push_back({ 1920, 1080, "HD, real bucket widths" });

    ScopeEngine engine;
    ScopeResult reference;
    ScopeResult candidate;
    Frame frame;

#ifdef SCOPE_HAVE_GPU
    GpuReducer cuda;
    if (options.bench) return RunBench(cuda, options);
    if (cuda.Available())
        std::printf("%s backend: %s\n\n", kGpuBackendName, cuda.DeviceDescription());
    else
        std::printf("%s backend: UNAVAILABLE (%s)\n\n", kGpuBackendName, cuda.Error());
    uint64_t cudaRuns = 0, cudaFailed = 0;
#else
    if (options.bench)
    {
        std::printf("--bench needs a GPU build (scope_conformance_cuda or scope_conformance_metal).\n");
        return 2;
    }
#endif

    uint64_t casesRun = 0, casesPassed = 0, comparisons = 0;
    std::vector<std::string> failures;

    for (const Size& size : sizes)
    {
        for (const Pattern& pattern : kPatterns)
        {
            BuildFrame(pattern, size.width, size.height, frame);
            const FrameView view = frame.View();

            for (uint32_t space = 0; space < kSpaceCount; ++space)
            {
                for (int rowStep : kRowSteps)
                {
                    char nameBuf[256];
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s %dx%d %s step=%d",
                                  pattern.name, size.width, size.height,
                                  ColorSpaceName(space), rowStep);
                    const std::string caseName = nameBuf;

                    if (!options.filter.empty() &&
                        caseName.find(options.filter) == std::string::npos) continue;

                    ScopeParams params;
                    params.colorSpace = space;
                    params.luma = LumaWeightsFor(space);
                    params.rowStep = rowStep;

                    ++casesRun;
                    std::vector<std::string> errors;

                    ReferenceAnalyse(view, params, reference);
                    CheckInvariants(reference, size.width, size.height, rowStep, errors);

                    for (int threads : kThreadCounts)
                    {
                        engine.SetThreadCount(threads);
                        engine.Analyse(view, params, candidate);
                        ++comparisons;

                        std::vector<std::string> engineErrors;
                        DiffResults(reference, candidate, engineErrors);
                        CheckInvariants(candidate, size.width, size.height, rowStep, engineErrors);

                        for (const std::string& e : engineErrors)
                        {
                            char buf[768];
                            std::snprintf(buf, sizeof(buf), "engine@%d threads: %s", threads, e.c_str());
                            errors.push_back(buf);
                        }
                    }

#ifdef SCOPE_HAVE_GPU
                    if (cuda.Available())
                    {
                        ScopeResult gpu;
                        ++comparisons;
                        ++cudaRuns;
                        if (!cuda.Analyse(view, params, gpu))
                        {
                            ++cudaFailed;
                            char buf[512];
                            std::snprintf(buf, sizeof(buf), "%s: did not run (%s)", kGpuBackendName, cuda.Error());
                            errors.push_back(buf);
                        }
                        else
                        {
                            std::vector<std::string> gpuErrors;
                            DiffResults(reference, gpu, gpuErrors);
                            CheckInvariants(gpu, size.width, size.height, rowStep, gpuErrors);
                            for (const std::string& e : gpuErrors)
                            {
                                char buf[768];
                                std::snprintf(buf, sizeof(buf), "%s: %s", kGpuBackendName, e.c_str());
                                errors.push_back(buf);
                            }
                        }
                    }
#endif

                    if (errors.empty())
                    {
                        ++casesPassed;
                        if (options.verbose) std::printf("  pass  %s\n", caseName.c_str());
                    }
                    else
                    {
                        std::printf("  FAIL  %s\n", caseName.c_str());
                        for (const std::string& e : errors)
                        {
                            std::printf("          %s\n", e.c_str());
                            failures.push_back(caseName + ": " + e);
                        }
                    }
                }
            }
        }
    }

    std::printf("\n%llu cases, %llu passed, %llu failed  (%llu backend comparisons)\n",
                static_cast<unsigned long long>(casesRun),
                static_cast<unsigned long long>(casesPassed),
                static_cast<unsigned long long>(casesRun - casesPassed),
                static_cast<unsigned long long>(comparisons));

#ifdef SCOPE_HAVE_GPU
    if (cudaRuns > 0)
        std::printf("%s backend ran %llu cases, %llu failed to execute\n",
                    kGpuBackendName,
                    static_cast<unsigned long long>(cudaRuns),
                    static_cast<unsigned long long>(cudaFailed));
#endif

    if (!failures.empty())
    {
        std::printf("\n%llu failure lines. Every comparison here is exact by design - see the\n"
                    "header comment before reaching for a tolerance.\n",
                    static_cast<unsigned long long>(failures.size()));
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
