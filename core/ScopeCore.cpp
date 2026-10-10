#include "ScopeCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace scopedeck
{

namespace
{

constexpr float kBinScale =
    static_cast<float>(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);

// Maps a sample onto a bin, clamping out-of-range values into the end bins. The
// true extremes travel separately in minRGB/maxRGB so clamping here loses nothing.
inline uint32_t BinOf(float p_Value, uint32_t p_BinCount)
{
    const float t = (p_Value - kBinRangeLow) * kBinScale;
    if (!(t > 0.0f)) return 0;                       // also catches NaN
    // +inf passes the guard above, and casting it to uint32 is undefined
    // behaviour - MSVC yields 0, so an infinite sample used to plot in the
    // BOTTOM bin, as if it were black. Clamping first puts it in the top bin,
    // which is what this range policy already says out-of-range values do.
    // Found by tools/ScopeConformance.cpp: the GPU saturates the same cast
    // instead, so the two paths disagreed and the CPU was the wrong one.
    if (t >= static_cast<float>(p_BinCount)) return p_BinCount - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= p_BinCount) ? (p_BinCount - 1) : bin;
}

constexpr float kChromaBinScale =
    static_cast<float>(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);

// Same clamp-into-end-bins policy as BinOf, against the chroma range instead.
inline uint32_t ChromaBinOf(float p_Value)
{
    const float t = (p_Value - kChromaRangeLow) * kChromaBinScale;
    if (!(t > 0.0f)) return 0;
    // +inf; see BinOf above
    if (t >= static_cast<float>(kVectorscopeSize)) return kVectorscopeSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kVectorscopeSize) ? (kVectorscopeSize - 1) : bin;
}

// The twin peaks "level" axis shares the waveform's own range and bin count
// (see the kTwinPeaksSize comment in ScopeTypes.h), so it reuses BinOf's
// precomputed kBinScale rather than deriving a second, identical one - this
// static_assert is what stops that reuse going silently wrong if the two
// ever diverge.
static_assert(kTwinPeaksSize == kWaveformLevels,
             "TwinPeaksLevelBinOf reuses BinOf's kBinScale, which is precomputed for kWaveformLevels");
inline uint32_t TwinPeaksLevelBinOf(float p_Value) { return BinOf(p_Value, kTwinPeaksSize); }

constexpr float kTwinPeaksDiffBinScale =
    static_cast<float>(kTwinPeaksSize - 1) / (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);

// Same clamp-into-end-bins policy as BinOf, against the twin peaks diff range.
inline uint32_t TwinPeaksDiffBinOf(float p_Value)
{
    const float t = (p_Value - kTwinPeaksDiffRangeLow) * kTwinPeaksDiffBinScale;
    if (!(t > 0.0f)) return 0;
    // +inf; see BinOf above
    if (t >= static_cast<float>(kTwinPeaksSize)) return kTwinPeaksSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kTwinPeaksSize) ? (kTwinPeaksSize - 1) : bin;
}

} // namespace

ScopeEngine::ScopeEngine()
{
    // Measured, not assumed: 6-8 threads beat 12-16 at UHD. Past that the private
    // bin buffers cost more to zero and merge than the extra parallelism returns.
    const unsigned hw = std::thread::hardware_concurrency();
    m_ThreadCount = static_cast<int>(hw ? std::min(hw, 8u) : 4u);
    m_Partials.resize(static_cast<size_t>(m_ThreadCount));
}

void ScopeEngine::SetThreadCount(int p_Count)
{
    if (p_Count < 1) p_Count = 1;
    if (p_Count > 64) p_Count = 64;
    m_ThreadCount = p_Count;
    m_Partials.resize(static_cast<size_t>(m_ThreadCount));
}

void ScopeEngine::Reduce(const FrameView& p_Frame, const int* p_ColumnStart,
                         const LumaWeights& p_Luma, int p_Y0, int p_Y1, int p_RowStep,
                         Partial& p_Partial)
{
    // Private bins per thread, merged afterwards. Contended atomics across ~25M
    // increments would cost far more than the merge does.
    p_Partial.waveform.assign(static_cast<size_t>(kWaveformCells), 0u);
    p_Partial.vectorscope.assign(static_cast<size_t>(kVectorscopeTotalCells), 0u);
    p_Partial.twinPeaks.assign(static_cast<size_t>(kTwinPeaksTotalCells), 0u);
    p_Partial.pixels = 0;

    for (int c = 0; c < 3; ++c)
    {
        p_Partial.minRGB[c] =  std::numeric_limits<float>::infinity();
        p_Partial.maxRGB[c] = -std::numeric_limits<float>::infinity();
    }

    uint32_t* wave = p_Partial.waveform.data();
    uint32_t* vec  = p_Partial.vectorscope.data();
    uint32_t* tp   = p_Partial.twinPeaks.data();

    const int width = p_Frame.width;
    if (width <= 0) return;

    // Cb/Cr are derived from Y the same way for every pixel this thread sees, so
    // the (1 - K) reciprocals are computed once per call rather than once per pixel.
    const float cbScale = 0.5f / (1.0f - p_Luma.b);
    const float crScale = 0.5f / (1.0f - p_Luma.r);

    // Locals, not members reached through a reference. Keeping the running extremes
    // in registers rather than behind a pointer that the compiler must assume may
    // alias the bin arrays is worth more than it looks.
    float minR =  std::numeric_limits<float>::infinity();
    float minG =  minR, minB = minR;
    float maxR = -std::numeric_limits<float>::infinity();
    float maxG =  maxR, maxB = maxR;
    uint64_t counted = 0;

    const float lumaR = p_Luma.r;
    const float lumaG = p_Luma.g;
    const float lumaB = p_Luma.b;

    for (int y = p_Y0; y < p_Y1; y += p_RowStep)
    {
        const float* row = p_Frame.Row(y);
        if (!row) continue;

        // Column outer, pixels within the column inner. Every pixel in the inner
        // loop writes into the same 4 KB bin region, which stays in L1, and the
        // per-pixel column lookup disappears entirely.
        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            const int x0 = p_ColumnStart[col];
            const int x1 = p_ColumnStart[col + 1];
            if (x0 >= x1) continue;

            uint32_t* wcol = wave + static_cast<uint64_t>(col) * kWaveformLevels * kPlaneCount;
            const float* px = row + static_cast<ptrdiff_t>(x0) * 4;

            for (int x = x0; x < x1; ++x, px += 4)
            {
                const float r = px[0];
                const float g = px[1];
                const float b = px[2];

                if (r < minR) minR = r;
                if (r > maxR) maxR = r;
                if (g < minG) minG = g;
                if (g > maxG) maxG = g;
                if (b < minB) minB = b;
                if (b > maxB) maxB = b;

                const float luma = lumaR * r + lumaG * g + lumaB * b;

                // Histogram is NOT accumulated here. It is the waveform summed over
                // columns - identical bin count and range - so deriving it in the
                // merge halves the per-pixel increments, and the increments are the
                // bottleneck: each is a load-modify-store, and neighbouring pixels
                // usually hit the same bin, so they serialise on store forwarding.
                ++wcol[BinOf(r, kWaveformLevels) * kPlaneCount + kPlaneR];
                ++wcol[BinOf(g, kWaveformLevels) * kPlaneCount + kPlaneG];
                ++wcol[BinOf(b, kWaveformLevels) * kPlaneCount + kPlaneB];
                ++wcol[BinOf(luma, kWaveformLevels) * kPlaneCount + kPlaneY];

                const float cb = (b - luma) * cbScale;
                const float cr = (r - luma) * crScale;
                ++vec[VectorscopeIndex(VectorscopeBandOf(luma), ChromaBinOf(cb), ChromaBinOf(cr))];

                // Twin peaks: Green is the shared reference channel against
                // Blue (upper diamond) and against Red (lower diamond) - see
                // the kTwinPeaksSize comment in ScopeTypes.h for why level/
                // diff are each a plain average/half-difference.
                const float levelGB = (g + b) * 0.5f;
                const float diffGB  = (g - b) * 0.5f;
                ++tp[TwinPeaksIndex(kDiamondGreenBlue,
                                    TwinPeaksLevelBinOf(levelGB), TwinPeaksDiffBinOf(diffGB))];

                const float levelGR = (g + r) * 0.5f;
                const float diffGR  = (g - r) * 0.5f;
                ++tp[TwinPeaksIndex(kDiamondGreenRed,
                                    TwinPeaksLevelBinOf(levelGR), TwinPeaksDiffBinOf(diffGR))];
            }

            counted += static_cast<uint64_t>(x1 - x0);
        }
    }

    p_Partial.minRGB[0] = minR; p_Partial.maxRGB[0] = maxR;
    p_Partial.minRGB[1] = minG; p_Partial.maxRGB[1] = maxG;
    p_Partial.minRGB[2] = minB; p_Partial.maxRGB[2] = maxB;
    p_Partial.pixels = counted;
}

void ScopeEngine::BuildWaveformTrace(const FrameView& p_Frame, const int* p_ColumnStart,
                                     const LumaWeights& p_Luma, ScopeResult& p_Out)
{
    float* trace = p_Out.waveformTrace.data();
    const int height = p_Frame.height;
    if (height <= 0) return;

    for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
    {
        // Evenly strided, distinct source rows - never averaged together, so
        // each output row stays a genuine, connectable scanline (see the
        // kWaveformTraceRows comment in ScopeTypes.h for why that matters).
        const int srcY = (kWaveformTraceRows > 1)
            ? static_cast<int>((uint64_t(row) * uint64_t(height - 1)) / (kWaveformTraceRows - 1))
            : 0;
        const float* rowPixels = p_Frame.Row(srcY);
        if (!rowPixels) continue;

        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            const int x0 = p_ColumnStart[col];
            const int x1 = p_ColumnStart[col + 1];

            // Box-averaged across this column's pixel range, same boundary
            // convention Reduce() uses for the histogram - unlike the
            // between-rows stride above, averaging *within* one row's own
            // column bucket does not blend distinct scanlines together, so
            // it does not undermine the "genuine connectable line" property.
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
            const float luma = p_Luma.r * r + p_Luma.g * g + p_Luma.b * b;

            const uint64_t base = WaveformTraceIndex(row, col, 0);
            trace[base + kPlaneR] = r;
            trace[base + kPlaneG] = g;
            trace[base + kPlaneB] = b;
            trace[base + kPlaneY] = luma;
        }
    }
}

void ScopeEngine::Analyse(const FrameView& p_Frame, const ScopeParams& p_Params,
                          ScopeResult& p_Out)
{
    const auto tStart = std::chrono::steady_clock::now();

    int p_RowStep = p_Params.rowStep;
    p_Out.colorSpace = p_Params.colorSpace;
    p_Out.luma = p_Params.luma;

    p_Out.waveform.assign(static_cast<size_t>(kWaveformCells), 0u);
    p_Out.histogram.assign(static_cast<size_t>(kHistogramCells), 0u);
    p_Out.vectorscope.assign(static_cast<size_t>(kVectorscopeTotalCells), 0u);
    p_Out.twinPeaks.assign(static_cast<size_t>(kTwinPeaksTotalCells), 0u);
    p_Out.waveformTrace.assign(static_cast<size_t>(kWaveformTraceCells), 0.0f);
    p_Out.pixelsSampled = 0;

    if (!p_Frame.pixels || (p_Frame.width <= 0) || (p_Frame.height <= 0))
    {
        p_Out.millis = 0.0;
        return;
    }

    if (p_RowStep < 1) p_RowStep = 1;

    // Pixel boundaries for each waveform column, computed once per frame and shared
    // by every thread (read-only, so no synchronisation needed).
    m_ColumnStart.resize(static_cast<size_t>(kWaveformColumns) + 1);
    for (uint32_t col = 0; col <= kWaveformColumns; ++col)
    {
        const uint64_t x = (static_cast<uint64_t>(col) * static_cast<uint64_t>(p_Frame.width))
                         / kWaveformColumns;
        m_ColumnStart[col] = static_cast<int>(x < static_cast<uint64_t>(p_Frame.width)
                                              ? x : static_cast<uint64_t>(p_Frame.width));
    }
    m_ColumnStart[kWaveformColumns] = p_Frame.width;
    const int* columnStart = m_ColumnStart.data();

    // Centre pixel, read once and passed through untouched. Cheap, and it is the
    // only value in the payload that can be compared directly against Resolve's own
    // picker to prove where in the pipeline this tap actually sits.
    {
        const float* centreRow = p_Frame.Row(p_Frame.height / 2);
        if (centreRow)
        {
            const float* px = centreRow + static_cast<ptrdiff_t>(p_Frame.width / 2) * 4;
            p_Out.probeRGB[0] = px[0];
            p_Out.probeRGB[1] = px[1];
            p_Out.probeRGB[2] = px[2];
        }
    }

    // One band per thread. Each band starts on a multiple of p_RowStep so
    // subsampling stays evenly spaced across band boundaries.
    const int threads = std::max(1, std::min(m_ThreadCount, p_Frame.height));
    const int rowsPer = (p_Frame.height + threads - 1) / threads;

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));
    std::vector<int> bands;            // the t of every band that actually ran
    bands.reserve(static_cast<size_t>(threads));

    for (int t = 0; t < threads; ++t)
    {
        int y0 = t * rowsPer;
        const int y1 = std::min(y0 + rowsPer, p_Frame.height);
        if (y0 >= y1) break;

        if (y0 % p_RowStep) y0 += p_RowStep - (y0 % p_RowStep);
        if (y0 >= y1) continue;

        bands.push_back(t);
        Partial& partial = m_Partials[static_cast<size_t>(t)];
        const LumaWeights luma = p_Params.luma;
        workers.emplace_back([this, &p_Frame, columnStart, luma, y0, y1, p_RowStep, &partial]()
        {
            Reduce(p_Frame, columnStart, luma, y0, y1, p_RowStep, partial);
        });
    }

    // Runs on this thread while the workers above run on theirs - safe to
    // overlap since it only reads p_Frame/columnStart (read-only, already
    // shared with the workers) and writes p_Out.waveformTrace, an array none
    // of the Partial buffers or workers ever touch.
    BuildWaveformTrace(p_Frame, columnStart, p_Params.luma, p_Out);

    for (std::thread& worker : workers) worker.join();

    // Merge.
    float lo[3] = {  std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity() };
    float hi[3] = { -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity() };

    uint32_t* wave = p_Out.waveform.data();
    uint32_t* hist = p_Out.histogram.data();
    uint32_t* vec  = p_Out.vectorscope.data();
    uint32_t* tp   = p_Out.twinPeaks.data();

    // Only the bands that ran. A band whose rows hold no multiple of p_RowStep
    // spawns no worker (the `continue` above), so the workers do not line up
    // with the first N partials: walking workers.size() here dropped the last
    // band and merged whatever the previous frame had left in a skipped band's
    // buffer. Any frame shorter than threads x rowStep rows hit it; HD never
    // did, which is why conformance's 64x64 and 517x289 never saw it.
    for (int t : bands)
    {
        const Partial& partial = m_Partials[static_cast<size_t>(t)];

        const uint32_t* pw = partial.waveform.data();
        for (uint64_t i = 0; i < kWaveformCells; ++i) wave[i] += pw[i];

        const uint32_t* pv = partial.vectorscope.data();
        for (uint64_t i = 0; i < kVectorscopeTotalCells; ++i) vec[i] += pv[i];

        const uint32_t* ptp = partial.twinPeaks.data();
        for (uint64_t i = 0; i < kTwinPeaksTotalCells; ++i) tp[i] += ptp[i];

        for (int c = 0; c < 3; ++c)
        {
            if (partial.minRGB[c] < lo[c]) lo[c] = partial.minRGB[c];
            if (partial.maxRGB[c] > hi[c]) hi[c] = partial.maxRGB[c];
        }

        p_Out.pixelsSampled += partial.pixels;
    }

    // Derive the histogram from the merged waveform: bin counts summed over every
    // column. Exact, not an approximation - both use the same bin count and range.
    static_assert(kHistogramBins == kWaveformLevels,
                  "histogram derivation assumes histogram bins match waveform levels");
    for (uint32_t col = 0; col < kWaveformColumns; ++col)
    {
        const uint32_t* wcol = wave + static_cast<uint64_t>(col) * kWaveformLevels * kPlaneCount;
        for (uint32_t level = 0; level < kWaveformLevels; ++level)
            for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
                hist[HistogramIndex(plane, level)] += wcol[level * kPlaneCount + plane];
    }

    for (int c = 0; c < 3; ++c)
    {
        const bool sawAny = (p_Out.pixelsSampled > 0);
        p_Out.minRGB[c] = sawAny ? lo[c] : 0.0f;
        p_Out.maxRGB[c] = sawAny ? hi[c] : 0.0f;
    }

    const auto delta = std::chrono::steady_clock::now() - tStart;
    p_Out.millis = std::chrono::duration<double, std::milli>(delta).count();
}

namespace
{

inline uint8_t ToDisplay8(float p_Value)
{
    // The tap treats the signal as already display-referred (see the waveform's
    // own bin range going past [0,1] for the same reason) - clipping to [0,1]
    // here is a viewfinder decision, not a measurement one; this image is never
    // read for color, only shown and (later) drawn on for masking.
    if (!(p_Value > 0.0f)) return 0;
    if (p_Value >= 1.0f) return 255;
    return static_cast<uint8_t>(p_Value * 255.0f + 0.5f);
}

} // namespace

void ScopeEngine::BuildPreview(const FrameView& p_Frame, float p_Scale, PreviewResult& p_Out)
{
    const int srcWidth  = p_Frame.width;
    const int srcHeight = p_Frame.height;

    if (!p_Frame.pixels || (srcWidth <= 0) || (srcHeight <= 0))
    {
        p_Out.rgb.clear();
        p_Out.width = 0;
        p_Out.height = 0;
        return;
    }

    // The requested scale, clamped so the result still fits the fixed canvas -
    // only engages for sources bigger than the canvas even at 100% (see
    // kPreviewCanvasWidth/Height), so 100% is exact for anything up to UHD.
    // Shared with the CUDA tap; see PreviewSizeFor.
    uint32_t dstWidth = 0, dstHeight = 0;
    PreviewSizeFor(srcWidth, srcHeight, p_Scale, dstWidth, dstHeight);

    p_Out.width  = dstWidth;
    p_Out.height = dstHeight;
    p_Out.rgb.assign(static_cast<size_t>(dstWidth) * dstHeight * 3, 0u);

    // Pixel boundaries per destination row/column, same technique as the
    // waveform's column boundaries: every source pixel is visited exactly once,
    // box-averaged into whichever destination cell it falls in.
    std::vector<int> rowStart(static_cast<size_t>(dstHeight) + 1);
    std::vector<int> colStart(static_cast<size_t>(dstWidth) + 1);
    for (uint32_t y = 0; y <= dstHeight; ++y)
        rowStart[y] = static_cast<int>((static_cast<uint64_t>(y) * srcHeight) / dstHeight);
    rowStart[dstHeight] = srcHeight;
    for (uint32_t x = 0; x <= dstWidth; ++x)
        colStart[x] = static_cast<int>((static_cast<uint64_t>(x) * srcWidth) / dstWidth);
    colStart[dstWidth] = srcWidth;

    uint8_t* dst = p_Out.rgb.data();

    // Row-sharded across threads, same idea as Reduce()'s row bands - but with
    // no partial-buffer merge needed at all: every thread only ever writes the
    // destination rows it was given, and every destination row is written by
    // exactly one thread, so there is nothing to combine afterwards. This was
    // the single biggest per-frame cost in the tap (~24ms single-threaded at
    // UHD/100% Preview Scale, more than the whole multi-threaded Reduce()
    // pass), which is what actually eats into Resolve's own frame budget and
    // shows up as dropped frames on heavy UHD grades - measured with
    // scope_selftest --publish --hd, see README.
    const auto renderRows = [&](uint32_t p_DstY0, uint32_t p_DstY1)
    {
        for (uint32_t dy = p_DstY0; dy < p_DstY1; ++dy)
        {
            const int y0 = rowStart[dy];
            const int y1 = rowStart[dy + 1];

            for (uint32_t dx = 0; dx < dstWidth; ++dx)
            {
                const int x0 = colStart[dx];
                const int x1 = colStart[dx + 1];

                // Point sample the center of the destination cell to avoid
                // reading the entire source frame when heavily downscaled.
                const int y_mid = (y0 + y1) / 2;
                const int x_mid = (x0 + x1) / 2;
                
                uint8_t* out = dst + (static_cast<size_t>(dstHeight - 1 - dy) * dstWidth + dx) * 3;
                
                const float* row = p_Frame.Row(y_mid);
                if (row)
                {
                    const float* px = row + static_cast<ptrdiff_t>(x_mid) * 4;
                    out[0] = ToDisplay8(px[0]);
                    out[1] = ToDisplay8(px[1]);
                    out[2] = ToDisplay8(px[2]);
                }
                else
                {
                    out[0] = 0;
                    out[1] = 0;
                    out[2] = 0;
                }
            }
        }
    };

    const int threads = std::max(1, std::min(m_ThreadCount, static_cast<int>(dstHeight)));
    if (threads <= 1)
    {
        renderRows(0, dstHeight);
        return;
    }

    const uint32_t rowsPer = (dstHeight + static_cast<uint32_t>(threads) - 1)
                            / static_cast<uint32_t>(threads);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t)
    {
        const uint32_t y0 = static_cast<uint32_t>(t) * rowsPer;
        if (y0 >= dstHeight) break;
        const uint32_t y1 = std::min(y0 + rowsPer, dstHeight);
        workers.emplace_back([&renderRows, y0, y1]() { renderRows(y0, y1); });
    }
    for (std::thread& worker : workers) worker.join();
}

namespace {

// IEEE-754 binary32 -> binary16, round to nearest even. Written out rather than
// pulled from a library because the tap links no maths dependencies and this is
// the only place that needs it.
//
// Infinities, NaN and subnormals are all handled rather than assumed away: the
// whole reason this buffer exists is to carry values the clipped preview path
// throws out, so it must not introduce a clipping step of its own. Values past
// half's 65504 saturate to infinity, which is still honest - the reader's bin
// maths clamps into the last bin exactly as it does for the CPU path.
inline uint16_t FloatToHalf(float p_Value)
{
    uint32_t bits;
    std::memcpy(&bits, &p_Value, sizeof(bits));

    const uint32_t sign     = (bits >> 16) & 0x8000u;
    const uint32_t rawExp   = (bits >> 23) & 0xFFu;
    const uint32_t mantissa = bits & 0x007FFFFFu;

    if (rawExp == 0xFFu)
        return static_cast<uint16_t>(sign | (mantissa ? 0x7E00u : 0x7C00u));

    const int32_t exp = static_cast<int32_t>(rawExp) - 127 + 15;
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u);        // overflows to inf
    if (exp <= 0)
    {
        if (exp < -10) return static_cast<uint16_t>(sign);    // underflows to zero
        // Subnormals round to nearest *even* like the normal path below, not
        // half-up: 2^-25 sits exactly halfway between zero and the smallest
        // subnormal, and half-up would push it to 2^-24 where every other
        // IEEE-754 implementation gives zero.
        const uint32_t sub     = mantissa | 0x00800000u;
        const uint32_t shift   = static_cast<uint32_t>(14 - exp);
        const uint32_t kept    = sub >> shift;
        const uint32_t dropped = sub & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        const uint32_t round   = (dropped > halfway
                                  || (dropped == halfway && (kept & 1u))) ? 1u : 0u;
        return static_cast<uint16_t>(sign | (kept + round));
    }

    uint16_t out = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10)
                                         | (mantissa >> 13));
    const uint32_t dropped = mantissa & 0x1FFFu;
    // Carrying into the exponent here is correct, not an overflow bug: rounding
    // 1.9995 up to 2.0 is exactly what round-to-nearest should do.
    if (dropped > 0x1000u || (dropped == 0x1000u && ((mantissa >> 13) & 1u)))
        ++out;
    return out;
}

} // namespace

} // namespace scopedeck
