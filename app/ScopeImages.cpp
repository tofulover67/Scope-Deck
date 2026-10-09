#include <future>
#include <cmath>
#include <algorithm>
#include <vector>
#include "ScopeImages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace scopedeck
{

namespace
{

// The old app's trace curve: normalise, apply gain, clip, then a 1/2.2 gamma so a
// thin trace over a handful of pixels is still visible next to a solid flat field.
// Kept as one function because every scope here shares it and they must not drift.
inline float TraceIntensity(float p_Norm, float p_Gain)
{
    const float scaled = std::clamp(p_Norm * p_Gain, 0.0f, 1.0f);
    return std::pow(scaled, 1.0f / 2.2f);
}

inline uint8_t ToByte(float p_Value)
{
    return uint8_t(std::clamp(p_Value, 0.0f, 255.0f) + 0.5f);
}

// 75% colour bars, the amplitude a vectorscope's target boxes are drawn for.
constexpr float kBarAmplitude = 0.75f;

struct BarRgb { const char* name; float r, g, b; Rgb8 draw; };

// Order matches a standard vectorscope face going clockwise from R.
// `draw` is the colour the box is stroked in - the bar's own hue, so R reads red
// and Cy cyan without having to read the label. Deliberately not full-intensity
// primaries: a pure 255 box next to a trace of the same hue is hard to separate.
constexpr BarRgb kBars[6] = {
    { "R",  1.0f, 0.0f, 0.0f, { 235,  70,  70 } },
    { "Mg", 1.0f, 0.0f, 1.0f, { 226,  86, 220 } },
    { "B",  0.0f, 0.0f, 1.0f, {  86, 122, 235 } },
    { "Cy", 0.0f, 1.0f, 1.0f, {  80, 210, 214 } },
    { "G",  0.0f, 1.0f, 0.0f, {  86, 206, 106 } },
    { "Yl", 1.0f, 1.0f, 0.0f, { 222, 206,  80 } },
};

} // namespace

Rgb8 TintForPlane(uint32_t p_Plane)
{
    switch (p_Plane)
    {
        case kPlaneR: return kTintR;
        case kPlaneG: return kTintG;
        case kPlaneB: return kTintB;
        case kPlaneY:
        default:      return kTintY;
    }
}

void Image::Resize(int p_Width, int p_Height)
{
    width  = p_Width;
    height = p_Height;
    pixels.assign(size_t(p_Width) * p_Height * 4, 0);
}

void Image::Clear()
{
    std::fill(pixels.begin(), pixels.end(), uint8_t(0));
}

float SamplesPerColumn(const ScopeFrame& p_Frame)
{
    // pixelsSampled is what the tap actually binned, which is not width*height
    // when it subsampled. Deriving rows from it rather than from height means a
    // subsampled frame still normalises correctly instead of reading dim.
    const float rowsSampled =
        std::max(float(p_Frame.pixelsSampled) / std::max(1.0f, float(p_Frame.width)), 1.0f);

    const float perColumn =
        (float(p_Frame.width) / float(kWaveformColumns)) * rowsSampled;

    return std::max(perColumn, 1.0f);
}

void BuildWaveformImage(const ScopeFrame& p_Frame,
                        uint32_t p_Plane,
                        Rgb8 p_Tint,
                        float p_Gain,
                        int p_LevelLo,
                        int p_LevelHi,
                        Image& p_Out,
                        const uint32_t* p_CountsOverride,
                        float p_ReferenceOverride)
{
    const uint32_t planes[1] = { p_Plane };
    // One plane is the composite of one plane; sharing the path means the single
    // and multi-plane cases cannot drift in their normalisation.
    (void)p_Tint;
    BuildWaveformComposite(p_Frame, planes, 1, p_Gain, p_LevelLo, p_LevelHi, p_Out,
                           p_CountsOverride, p_ReferenceOverride);
}

void BuildWaveformComposite(const ScopeFrame& p_Frame,
                            const uint32_t* p_Planes,
                            int p_PlaneCount,
                            float p_Gain,
                            int p_LevelLo,
                            int p_LevelHi,
                            Image& p_Out,
                            const uint32_t* p_CountsOverride,
                            float p_ReferenceOverride)
{
    if ((!p_CountsOverride && p_Frame.waveform.size() < kWaveformCells) || p_PlaneCount <= 0)
    {
        p_Out.Resize(0, 0);
        return;
    }

    const int lo = std::clamp(p_LevelLo, 0, int(kWaveformLevels) - 1);
    const int hi = std::clamp(p_LevelHi, lo + 1, int(kWaveformLevels));
    const int rows = hi - lo;

    p_Out.Resize(int(kWaveformColumns), rows);

    const float reference = p_ReferenceOverride > 0.0f ? p_ReferenceOverride : SamplesPerColumn(p_Frame);
    const uint32_t* counts = p_CountsOverride ? p_CountsOverride : p_Frame.waveform.data();

    for (int y = 0; y < rows; ++y)
    {
        // Row 0 is the top of the image and must be the *highest* level - the old
        // app got this with a transpose plus flipud. Writing the flip into the
        // index instead avoids materialising an intermediate.
        const int level = hi - 1 - y;

        uint8_t* row = &p_Out.pixels[size_t(y) * p_Out.width * 4];

        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            float accR = 0.0f, accG = 0.0f, accB = 0.0f;

            for (int p = 0; p < p_PlaneCount; ++p)
            {
                const uint32_t plane = p_Planes[p];
                const uint32_t count = counts[WaveformIndex(plane, col, uint32_t(level))];
                if (count == 0) continue;

                const float intensity = TraceIntensity(float(count) / reference, p_Gain);
                const Rgb8  tint      = TintForPlane(plane);

                // Additive, matching QPainter's CompositionMode_Plus. Clamped once
                // at the end rather than per plane, so three planes at half
                // intensity reach white the way the old app's compositor did.
                accR += intensity * float(tint.r);
                accG += intensity * float(tint.g);
                accB += intensity * float(tint.b);
            }

            uint8_t* px = &row[size_t(col) * 4];
            px[0] = ToByte(accR);
            px[1] = ToByte(accG);
            px[2] = ToByte(accB);

            // Alpha carries the trace itself, so the panel background shows through
            // where nothing landed instead of the image painting a black rectangle
            // over the graticule underneath it.
            px[3] = std::max({ px[0], px[1], px[2] });
        }
    }
}

void BuildVectorscopeImage(const ScopeFrame& p_Frame,
                           uint32_t p_BandMask,
                           float p_Gain,
                           bool p_Colorize,
                           float p_ZoomFactor,
                           Image& p_Out,
                           const uint32_t* p_CountsOverride)
{
    if (!p_CountsOverride && p_Frame.vectorscope.size() < kVectorscopeTotalCells)
    {
        p_Out.Resize(0, 0);
        return;
    }

    const int size = int(kVectorscopeSize);
    p_Out.Resize(size, size);

    const uint32_t* cells = p_CountsOverride ? p_CountsOverride : p_Frame.vectorscope.data();

    // Summing the requested bands first, then finding the peak of that sum, is
    // what makes "All" identical to a single combined trace rather than three
    // separately-normalised ones laid on top of each other.
    std::vector<uint32_t> summed(size_t(size) * size, 0u);
    for (uint32_t band = 0; band < kBandCount; ++band)
    {
        if ((p_BandMask & (1u << band)) == 0) continue;
        for (int cr = 0; cr < size; ++cr)
            for (int cb = 0; cb < size; ++cb)
                summed[size_t(cr) * size + cb] += cells[VectorscopeIndex(band, uint32_t(cb), uint32_t(cr))];
    }

    uint32_t peak = 0;
    for (uint32_t v : summed) peak = std::max(peak, v);
    if (peak == 0)
    {
        // A genuinely empty vectorscope - a fully desaturated frame - is a real
        // state, not a failure. Leave the image transparent so the graticule and
        // targets still draw underneath.
        return;
    }

    const float invPeak = 1.0f / float(peak);
    const float chromaSpan = p_Frame.chromaRangeHigh - p_Frame.chromaRangeLow;

    // 2x Zoom crops to a centred sub-region of the bins and stretches it over the
    // same image, rather than scaling the drawn rectangle. Cropping the data is
    // what keeps the graticule, targets and any readout agreeing with the trace -
    // scaling the rectangle would move the trace and leave everything else behind.
    const float zoom = (p_ZoomFactor > 0.0f) ? p_ZoomFactor : 1.0f;
    const float half = float(size) * 0.5f;

    for (int y = 0; y < size; ++y)
    {
        // Cr increases upward on a vectorscope face, so row 0 is the highest Cr -
        // the flipud in the old app's vector_image().
        const int crOut = size - 1 - y;
        const int crBin = int(half + (float(crOut) - half) / zoom);
        if (crBin < 0 || crBin >= size) continue;

        uint8_t* row = &p_Out.pixels[size_t(y) * size * 4];

        for (int cbOut = 0; cbOut < size; ++cbOut)
        {
            const int cbBin = int(half + (float(cbOut) - half) / zoom);
            if (cbBin < 0 || cbBin >= size) continue;

            const uint32_t count = summed[size_t(crBin) * size + cbBin];
            if (count == 0) continue;

            const float intensity = TraceIntensity(float(count) * invPeak, p_Gain);

            float r = 255.0f, g = 255.0f, b = 255.0f;
            if (p_Colorize)
            {
                // Hue and saturation come from the bin's own Cb/Cr position, not
                // from the luma of whatever landed there - a vectorscope has no Y
                // axis, so a fixed reference lightness is used, the same
                // convention a hardware scope's colorized mode uses.
                const float cb = p_Frame.chromaRangeLow + (float(cbBin) + 0.5f) / float(size) * chromaSpan;
                const float cr = p_Frame.chromaRangeLow + (float(crBin) + 0.5f) / float(size) * chromaSpan;

                const float kr = p_Frame.lumaCoeff[0];
                const float kg = p_Frame.lumaCoeff[1];
                const float kb = p_Frame.lumaCoeff[2];

                // Invert the tap's own Cb/Cr derivation at a mid reference luma.
                const float yRef = 0.5f;
                const float rr = yRef + cr * (2.0f * (1.0f - kr));
                const float bb = yRef + cb * (2.0f * (1.0f - kb));
                const float gg = (kg != 0.0f) ? (yRef - kr * rr - kb * bb) / kg : yRef;

                // Normalise to the brightest channel so every hue reaches full
                // intensity; without this the blue corner of the face is nearly
                // black and reads as "no data" rather than "blue".
                const float maxC = std::max({ rr, gg, bb, 1e-4f });
                r = std::clamp(rr / maxC, 0.0f, 1.0f) * 255.0f;
                g = std::clamp(gg / maxC, 0.0f, 1.0f) * 255.0f;
                b = std::clamp(bb / maxC, 0.0f, 1.0f) * 255.0f;
            }

            uint8_t* px = &row[size_t(cbOut) * 4];
            px[0] = ToByte(r * intensity);
            px[1] = ToByte(g * intensity);
            px[2] = ToByte(b * intensity);
            px[3] = ToByte(255.0f * intensity);
        }
    }
}

void BoxBlurColumns(std::vector<float>& p_Values, int p_Rows, int p_Columns,
                    int p_Planes, float p_Kernel)
{
    int width = std::max(1, int(std::lround(p_Kernel)));
    if (width <= 1) return;
    if (width % 2 == 0) ++width;

    const int pad = width / 2;

    // One row-and-plane at a time. A running window sum rather than a per-output
    // inner loop, so cost is O(columns) per row instead of O(columns * width) -
    // at 256 rows x 512 columns x 4 planes the difference is the whole budget.
    // static_cast, not size_t(...): `std::vector<float> line(size_t(p_Columns));`
    // is the most vexing parse - it declares a *function* named line taking a
    // size_t, and every subscript below then fails on it.
    std::vector<float> line(static_cast<size_t>(p_Columns));
    std::vector<float> out(static_cast<size_t>(p_Columns));

    for (int row = 0; row < p_Rows; ++row)
    {
        for (int plane = 0; plane < p_Planes; ++plane)
        {
            for (int col = 0; col < p_Columns; ++col)
                line[size_t(col)] = p_Values[WaveformTraceIndex(uint32_t(row), uint32_t(col),
                                                               uint32_t(plane))];

            // Edge padding, matching numpy's mode="edge": the first and last real
            // samples repeat outward. Zero padding would instead drag both ends of
            // every scanline toward black and draw a dip that is not in the signal.
            //
            // Tracks a running valid-sample count alongside the running sum, and
            // skips NaN entries out of both (a masked-out gap - see
            // BuildMaskedWaveformTrace - not a real sample to blur in), rather
            // than assuming every window is the full fixed width. Without this,
            // the very first NaN to slide into the window turns the running sum
            // itself to NaN - unlike the leaving/entering values, which are only
            // ever added or subtracted once each, that one NaN never "expires"
            // out of a plain running total, so it silently wrote NaN across
            // every remaining column of that row (and every row on that scanline
            // that ever windows over the same masked-out gap) instead of just
            // the columns actually inside the gap. A masked Waveform with any
            // Low-Pass Kernel at all reduced to a near-total blank this way -
            // this is not a hypothetical, it is what a real report traced back
            // to.
            double sum = 0.0;
            int count = 0;
            for (int k = -pad; k <= pad; ++k)
            {
                const float v = line[size_t(std::clamp(k, 0, p_Columns - 1))];
                if (!std::isnan(v)) { sum += v; ++count; }
            }

            for (int col = 0; col < p_Columns; ++col)
            {
                // The centre sample's own gap-ness passes straight through -
                // blurring should never manufacture a value where there was
                // none to begin with, only smooth values that already exist.
                out[size_t(col)] = std::isnan(line[size_t(col)]) ? line[size_t(col)]
                                  : (count > 0 ? float(sum / count) : line[size_t(col)]);

                const int leaving  = std::clamp(col - pad,     0, p_Columns - 1);
                const int entering = std::clamp(col + pad + 1, 0, p_Columns - 1);
                const float vLeaving  = line[size_t(leaving)];
                const float vEntering = line[size_t(entering)];
                if (!std::isnan(vEntering)) { sum += vEntering; ++count; }
                if (!std::isnan(vLeaving))  { sum -= vLeaving;  --count; }
            }

            for (int col = 0; col < p_Columns; ++col)
                p_Values[WaveformTraceIndex(uint32_t(row), uint32_t(col), uint32_t(plane))]
                    = out[size_t(col)];
        }
    }
}

bool PrepareEnhancedTrace(const ScopeFrame& p_Frame,
                          bool p_SignalPrefilter,
                          bool p_SmoothTrace,
                          float p_LowPassKernel,
                          EnhancedTraceState& p_State,
                          std::vector<float>& p_Out,
                          const std::vector<float>* p_TraceOverride)
{
    const std::vector<float>* source = p_TraceOverride ? p_TraceOverride : &p_Frame.waveformTrace;
    if (source->size() < kWaveformTraceCells)
    {
        p_State.Reset();
        return false;
    }

    p_Out = *source;

    const int rows    = int(kWaveformTraceRows);
    const int columns = int(kWaveformColumns);
    const int planes  = int(kPlaneCount);

    if (p_SignalPrefilter)
    {
        // A fixed small kernel on the raw signal, deliberately not the adjustable
        // Low-Pass below. Labelled honestly rather than as "(EBU R103)" - there is
        // no verified copy of that filter's response here, so claiming the
        // standard would be a claim this code cannot back.
        BoxBlurColumns(p_Out, rows, columns, planes, 3.0f);
    }

    if (p_SmoothTrace)
    {
        if (p_State.hasPrev && p_State.prevTrace.size() == p_Out.size())
        {
            for (size_t i = 0; i < p_Out.size(); ++i)
            {
                // A NaN entry (see BuildMaskedWaveformTrace) means "no data
                // here," not a real sample - blending it in either direction
                // would be wrong, and blending it in at all is actively
                // dangerous here specifically: 0.5*x + 0.5*NaN is NaN no
                // matter what x is, so one column going NaN for even a
                // single frame would otherwise poison that column in
                // prevTrace forever after, with every following frame
                // re-blending against the same stuck NaN regardless of how
                // good its own new sample is - a permanent gap from one bad
                // frame, not the temporary smoothing blip this is meant to
                // be. A masked-out current sample stays a gap (not
                // blended with a stale previous value, which would draw a
                // ghost of data that no longer exists); a stale-NaN
                // previous sample is treated as "nothing to blend against
                // yet" and just passes the current value through
                // unsmoothed, the same as this column's own first-ever
                // frame.
                if (std::isnan(p_Out[i])) continue;
                if (!std::isnan(p_State.prevTrace[i]))
                    p_Out[i] = 0.5f * p_Out[i] + 0.5f * p_State.prevTrace[i];
            }
        }
        p_State.prevTrace = p_Out;
        p_State.hasPrev   = true;
    }
    else
    {
        // Dropped rather than kept, so re-enabling Smooth Trace blends against the
        // next live frame instead of against whatever was on screen minutes ago.
        p_State.Reset();
    }

    if (p_LowPassKernel >= 1.5f)
        BoxBlurColumns(p_Out, rows, columns, planes, p_LowPassKernel);

    return true;
}

void VectorscopeTargets(const ScopeFrame& p_Frame, VectorTarget p_Out[6])
{
    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];

    const float cbScale = 0.5f / (1.0f - kb);
    const float crScale = 0.5f / (1.0f - kr);

    const float span = p_Frame.chromaRangeHigh - p_Frame.chromaRangeLow;

    for (int i = 0; i < 6; ++i)
    {
        const float r = kBars[i].r * kBarAmplitude;
        const float g = kBars[i].g * kBarAmplitude;
        const float b = kBars[i].b * kBarAmplitude;

        const float y  = kr * r + kg * g + kb * b;
        const float cb = (b - y) * cbScale;
        const float cr = (r - y) * crScale;

        p_Out[i].name  = kBars[i].name;
        p_Out[i].color = kBars[i].draw;
        // y down, and Cr points up on the face - so the axis is inverted here the
        // same way the image rows are.
        ChromaToPlot(cb, cr, p_Frame.chromaRangeLow, p_Frame.chromaRangeHigh,
                     p_Out[i].x, p_Out[i].y);
        (void)span;
    }
}

void BuildWhiteBalanceImage(const ScopeFrame& p_Frame, float p_Gain, Image& p_Out,
                            const uint32_t* p_CountsOverride)
{
    if (!p_CountsOverride && p_Frame.twinPeaks.size() < kTwinPeaksTotalCells) {
        p_Out.Resize(0, 0);
        return;
    }

    const int size = int(kTwinPeaksSize);
    p_Out.Resize(size, size * 2);

    const uint32_t* cells = p_CountsOverride ? p_CountsOverride : p_Frame.twinPeaks.data();

    for (int diamond = 0; diamond < 2; ++diamond)
    {
        uint32_t peak = 0;
        for (int i = 0; i < int(kTwinPeaksCells); ++i)
            peak = std::max(peak, cells[uint64_t(diamond) * kTwinPeaksCells + i]);
        if (peak == 0) continue;   // a genuinely grey-balanced/empty frame - leave it transparent
        const float invPeak = 1.0f / float(peak);

        for (int level = 0; level < size; ++level)
        {
            // Upper diamond (0): white (level=high) at the top of the image.
            // Lower diamond (1): the mirror - white at the bottom, black
            // toward the shared centre. See BuildWhiteBalanceImage's own
            // comment in ScopeImages.h for why.
            const int rowOut = (diamond == 0) ? (size - 1 - level) : (size + level);
            uint8_t* row = &p_Out.pixels[size_t(rowOut) * size * 4];

            for (int diff = 0; diff < size; ++diff)
            {
                const uint32_t count = cells[TwinPeaksIndex(uint32_t(diamond), uint32_t(level), uint32_t(diff))];
                if (count == 0) continue;

                const float intensity = TraceIntensity(float(count) * invPeak, p_Gain);
                const uint8_t v = ToByte(intensity * 255.0f);

                uint8_t* px = &row[diff * 4];
                px[0] = v; px[1] = v; px[2] = v;
                // Alpha carries the trace, same convention as the waveform's own
                // density image - nothing landed reads as "no pixel here", not black.
                px[3] = v;
            }
        }
    }
}

DiamondGraticule WhiteBalanceGraticule(const ScopeFrame& p_Frame, bool p_UpperDiamond)
{
    auto point = [&](float p_LevelSignal, float p_DiffSignal, float& p_OutX, float& p_OutY)
    {
        TwinPeaksToPlot(p_LevelSignal, p_DiffSignal, p_Frame.binRangeLow, p_Frame.binRangeHigh,
                       p_UpperDiamond, p_OutX, p_OutY);
    };

    DiamondGraticule g{};
    point(1.0f, 0.0f,  g.topX,    g.topY);
    point(0.0f, 0.0f,  g.bottomX, g.bottomY);
    point(0.5f, -0.5f, g.leftX,   g.leftY);
    point(0.5f, 0.5f,  g.rightX,  g.rightY);
    return g;
}


float SpatialMaskWeightAt(const SpatialMask& p_Mask, float p_Fx, float p_Fy, float p_Aspect)
{
    if (!p_Mask.hasGeometry) return 1.0f;

    float weight;
    if (p_Mask.shape == MaskShape::Gradient)
    {
        const float dx = p_Mask.x1 - p_Mask.x0;
        const float dy = p_Mask.y1 - p_Mask.y0;
        const float lenSq = dx * dx + dy * dy;
        if (lenSq < 1e-9f)
        {
            weight = 1.0f;
        }
        else
        {
            const float t = std::clamp(((p_Fx - p_Mask.x0) * dx + (p_Fy - p_Mask.y0) * dy) / lenSq, 0.0f, 1.0f);
            weight = 1.0f - t;
        }
    }
    else
    {
        const float cx = (p_Mask.x0 + p_Mask.x1) * 0.5f;
        const float cy = (p_Mask.y0 + p_Mask.y1) * 0.5f;
        const float rx = std::fabs(p_Mask.x1 - p_Mask.x0) * 0.5f;
        const float ry = std::fabs(p_Mask.y1 - p_Mask.y0) * 0.5f;

        const float c = std::cos(-p_Mask.angle);
        const float s = std::sin(-p_Mask.angle);
        const float dx = (p_Fx - cx) * p_Aspect;
        const float dy = (p_Fy - cy);
        const float rotX = dx * c - dy * s;
        const float rotY = dx * s + dy * c;

        const float rxAsp = std::max(rx * p_Aspect, 1e-6f);
        const float ryMax = std::max(ry, 1e-6f);

        bool inside;
        if (p_Mask.shape == MaskShape::Rectangle)
            inside = std::fabs(rotX) <= rxAsp && std::fabs(rotY) <= ryMax;
        else
            inside = (rotX * rotX) / (rxAsp * rxAsp) + (rotY * rotY) / (ryMax * ryMax) <= 1.0f;

        weight = inside ? 1.0f : 0.0f;
    }

    return p_Mask.invert ? 1.0f - weight : weight;
}

void BuildDimmedPreviewImage(const ScopeFrame& p_Frame, const SpatialMask& p_Mask, Image& p_Out,
                             const ValueQualifier* p_Qualifier)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const float aspect = float(width) / float(std::max(height, 1));
    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();
    const float* lumaCoeff = p_Frame.lumaCoeff;
    const bool hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();

    // Split by row range across threads, same as BuildScopeBinsFromRGB - each
    // thread only ever reads its own input rows and writes its own output
    // rows, so unlike that function's bin histograms there is nothing to
    // merge afterwards. Measured ~49ms single-threaded at UHD (this is a
    // real per-frame cost, not a one-off), which is what actually motivated
    // threading it rather than the source pixel count alone.
    const int numThreads = std::max(1, std::min(int(std::thread::hardware_concurrency()), height));
    const int rowsPerThread = height / numThreads;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < numThreads; ++t)
    {
        const int startY = t * rowsPerThread;
        const int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=, &p_Mask]()
        {
            for (int y = startY; y < endY; ++y)
            {
                const float fy = float(y) / float(std::max(height - 1, 1));
                for (int x = 0; x < width; ++x)
                {
                    const float fx = float(x) / float(std::max(width - 1, 1));
                    const int i = y * width + x;
                    uint8_t r = inPx[i*3 + 0], g = inPx[i*3 + 1], b = inPx[i*3 + 2];
                    const float weight = CombinedMaskWeight(&p_Mask, fx, fy, aspect,
                        hasQualifier ? p_Qualifier : nullptr,
                        r / 255.0f, g / 255.0f, b / 255.0f, lumaCoeff);
                    ApplyMaskDim(r, g, b, weight);
                    outPx[i*4 + 0] = r;
                    outPx[i*4 + 1] = g;
                    outPx[i*4 + 2] = b;
                    outPx[i*4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

void BuildFalseColorImage(const ScopeFrame& p_Frame,
                          const FalseColorBand* p_Bands,
                          int p_BandCount,
                          Image& p_Out,
                          const SpatialMask* p_Mask,
                          const ValueQualifier* p_Qualifier)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    // 256-entry LUT for luma to color
    uint8_t colorLUT[256][4];
    for (int i = 0; i < 256; ++i) {
        float luma = i / 255.0f;
        uint8_t gray = ToByte(luma * 255.0f);
        colorLUT[i][0] = gray;
        colorLUT[i][1] = gray;
        colorLUT[i][2] = gray;
        colorLUT[i][3] = 255;
        for (int j = 0; j < p_BandCount; ++j) {
            if (luma >= p_Bands[j].lo && luma < p_Bands[j].hi) {
                colorLUT[i][0] = p_Bands[j].color.r;
                colorLUT[i][1] = p_Bands[j].color.g;
                colorLUT[i][2] = p_Bands[j].color.b;
                break;
            }
        }
    }

    const bool  hasMask = p_Mask != nullptr && p_Mask->hasGeometry;
    const SpatialMask maskCopy = hasMask ? *p_Mask : SpatialMask{};
    const bool  hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();
    const float aspect = float(width) / float(std::max(height, 1));

    // Not p_Frame.lumaCoeff directly: the worker lambda captures by value, and
    // capturing a reference parameter that way copies the *referent* - the
    // whole ScopeFrame, preview buffer included - once per worker thread,
    // every frame. Reading three floats through it cost ~100ms/frame here,
    // the same bug and the same magnitude as BuildSkinToneImage's. It only
    // showed up once a mask was drawn because that is the only branch that
    // mentions p_Frame, and a capture is decided by what the body names, not
    // by which branch runs.
    const float lumaCoeff[3] = { kr, kg, kb };

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=, &colorLUT]() {
            for (int y = startY; y < endY; ++y) {
                const float fy = float(y) / float(std::max(height - 1, 1));
                for (int x = 0; x < width; ++x) {
                    int i = y * width + x;
                    float r = float(inPx[i*3 + 0]);
                    float g = float(inPx[i*3 + 1]);
                    float b = float(inPx[i*3 + 2]);

                    float lumaF = (kr * r + kg * g + kb * b);
                    int luma = std::clamp(int(lumaF), 0, 255);

                    uint8_t outR = colorLUT[luma][0];
                    uint8_t outG = colorLUT[luma][1];
                    uint8_t outB = colorLUT[luma][2];

                    if (hasMask || hasQualifier)
                    {
                        const float fx = float(x) / float(std::max(width - 1, 1));
                        const float weight = CombinedMaskWeight(hasMask ? &maskCopy : nullptr, fx, fy, aspect,
                            hasQualifier ? p_Qualifier : nullptr,
                            r / 255.0f, g / 255.0f, b / 255.0f, lumaCoeff);
                        ApplyMaskDim(outR, outG, outB, weight);
                    }

                    outPx[i*4 + 0] = outR;
                    outPx[i*4 + 1] = outG;
                    outPx[i*4 + 2] = outB;
                    outPx[i*4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

namespace
{
    // hue - reference, wrapped to [-180, 180) - the shortest signed way around
    // the circle rather than straight subtraction, so a reference near 0/360
    // doesn't see every far-side hue as maximally different.
    inline float SignedHueDiff(float p_Hue, float p_Reference)
    {
        float d = std::fmod(p_Hue - p_Reference + 180.0f, 360.0f);
        if (d < 0.0f) d += 360.0f;
        return d - 180.0f;
    }

    // A (hueMin, hueMax) pair from the Hue Range sliders, reshaped into the
    // same (center, half_width) form the single-angle-plus-tolerance model
    // uses. Wrap-aware: hueMax is treated as hueMin plus however far
    // clockwise it takes to reach it, so a genuine 350->20 wrap still
    // measures a 30 degree window rather than a negative one.
    inline void HueRangeCenterHalfWidth(float p_HueMin, float p_HueMax,
                                        float& p_OutCenter, float& p_OutHalfWidth)
    {
        float width = std::fmod(p_HueMax - p_HueMin, 360.0f);
        if (width < 0.0f) width += 360.0f;
        if (width <= 1e-6f) width = 360.0f;   // min == max means the whole wheel, not nothing

        float center = std::fmod(p_HueMin + width * 0.5f, 360.0f);
        if (center < 0.0f) center += 360.0f;

        p_OutCenter = center;
        p_OutHalfWidth = width * 0.5f;
    }

    // HslHueSatLight moved to ScopeImages.h - shared with the Qualifier panel's
    // HSL range test (ValueQualifierPasses), not just this panel's own model.
} // namespace

void BuildSkinToneImage(const ScopeFrame& p_Frame, const SkinToneParams& p_Params, Image& p_Out,
                        const SpatialMask* p_Mask, const ValueQualifier* p_Qualifier)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];

    const float cbFactor = 0.5f / (1.0f - kb);
    const float crFactor = 0.5f / (1.0f - kr);

    float center, halfWidth, useSatMin, useSatMax, useLumaMin, useLumaMax;
    if (p_Params.limitsEnabled)
    {
        HueRangeCenterHalfWidth(p_Params.hueMin, p_Params.hueMax, center, halfWidth);
        useSatMin = p_Params.satMin; useSatMax = p_Params.satMax;
        useLumaMin = p_Params.lumaMin; useLumaMax = p_Params.lumaMax;
    }
    else
    {
        center = (p_Params.model == SkinToneModel::Hsl) ? kSkinToneAngleHslDeg : kSkinToneAngleDeg;
        halfWidth = p_Params.tolerance;
        useSatMin = kSkinMinSaturation; useSatMax = 1.0f;
        useLumaMin = kSkinMinLuma; useLumaMax = kSkinMaxLuma;
    }
    const float halfWidthSafe = std::max(halfWidth, 0.01f);

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    const bool  isHsl       = (p_Params.model == SkinToneModel::Hsl);
    const bool  greyNonSkin = p_Params.greyNonSkin;
    const auto  colorMode   = p_Params.colorMode;
    const Rgb8  solidColor  = p_Params.solidColor;
    const Rgb8  gradA       = p_Params.gradientColorA;
    const Rgb8  gradB       = p_Params.gradientColorB;

    const bool  hasMask = p_Mask != nullptr && p_Mask->hasGeometry;
    const SpatialMask maskCopy = hasMask ? *p_Mask : SpatialMask{};
    const bool  hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();
    const float aspect = float(width) / float(std::max(height, 1));

    // The worker lambda below captures by value, so it must not touch
    // p_Frame: capturing a reference parameter by copy copies the *referent*,
    // and ScopeFrame owns the whole preview image (plus the waveform trace).
    // Reading p_Frame.lumaCoeff inside the lambda therefore deep-copied the
    // entire frame once per worker thread, every frame - tens of megabytes
    // per thread on a large preview. It made this the most expensive panel in
    // the app by an order of magnitude (measured at ~106ms/frame against
    // ~7ms for Waveform, 82% of total frame time). These three floats are
    // all the workers ever needed from it.
    const float lumaCoeff[3] = { kr, kg, kb };

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=]() {
            for (int y = startY; y < endY; ++y) {
                const float fy = float(y) / float(std::max(height - 1, 1));
                for (int x = 0; x < width; ++x) {
                    int i = y * width + x;
                    float r = inPx[i*3 + 0] / 255.0f;
                    float g = inPx[i*3 + 1] / 255.0f;
                    float b = inPx[i*3 + 2] / 255.0f;

                    float hue, saturation, luma;
                    if (isHsl)
                    {
                        HslHueSatLight(r, g, b, hue, saturation, luma);
                    }
                    else
                    {
                        luma = kr * r + kg * g + kb * b;
                        const float cb = (b - luma) * cbFactor;
                        const float cr = (r - luma) * crFactor;
                        saturation = std::clamp(std::hypot(cb, cr) / kSkinSaturationNorm, 0.0f, 1.0f);
                        hue = std::atan2(cr, cb) * (180.0f / 3.14159265358979323846f);
                        if (hue < 0.0f) hue += 360.0f;
                    }

                    const float diff = SignedHueDiff(hue, center);
                    const float absDiff = std::fabs(diff);
                    const bool inBand = absDiff <= halfWidth &&
                                        saturation >= useSatMin && saturation <= useSatMax &&
                                        luma >= useLumaMin && luma <= useLumaMax;

                    if (inBand)
                    {
                        const float t = std::clamp(absDiff / halfWidthSafe, 0.0f, 1.0f);
                        Rgb8 matched;
                        if (colorMode == SkinToneColorMode::Solid)
                        {
                            matched = solidColor;
                        }
                        else if (colorMode == SkinToneColorMode::Gradient)
                        {
                            matched.r = ToByte(gradA.r * (1.0f - t) + gradB.r * t);
                            matched.g = ToByte(gradA.g * (1.0f - t) + gradB.g * t);
                            matched.b = ToByte(gradA.b * (1.0f - t) + gradB.b * t);
                        }
                        else
                        {
                            // Default: green at the centre of the window, magenta at the edge.
                            matched.r = ToByte(60.0f  * (1.0f - t) + 255.0f * t);
                            matched.g = ToByte(225.0f * (1.0f - t) + 70.0f  * t);
                            matched.b = ToByte(110.0f * (1.0f - t) + 200.0f * t);
                        }
                        outPx[i*4 + 0] = matched.r;
                        outPx[i*4 + 1] = matched.g;
                        outPx[i*4 + 2] = matched.b;
                    }
                    else if (greyNonSkin)
                    {
                        const uint8_t grey = ToByte(std::clamp(luma, 0.0f, 1.0f) * 255.0f);
                        outPx[i*4 + 0] = grey;
                        outPx[i*4 + 1] = grey;
                        outPx[i*4 + 2] = grey;
                    }
                    else
                    {
                        outPx[i*4 + 0] = inPx[i*3 + 0];
                        outPx[i*4 + 1] = inPx[i*3 + 1];
                        outPx[i*4 + 2] = inPx[i*3 + 2];
                    }

                    if (hasMask || hasQualifier)
                    {
                        const float fx = float(x) / float(std::max(width - 1, 1));
                        const float weight = CombinedMaskWeight(hasMask ? &maskCopy : nullptr, fx, fy, aspect,
                            hasQualifier ? p_Qualifier : nullptr,
                            r, g, b, lumaCoeff);
                        ApplyMaskDim(outPx[i*4 + 0], outPx[i*4 + 1], outPx[i*4 + 2], weight);
                    }

                    outPx[i*4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

static float GetNoiseChannel(float r, float g, float b, int channel, const float* lumaCoeff) {
    if (channel == 0) return r;
    if (channel == 1) return g;
    if (channel == 2) return b;
    return lumaCoeff[0] * r + lumaCoeff[1] * g + lumaCoeff[2] * b;
}

void BuildNoiseImage(const ScopeFrame& p_Frame, int p_Method, int p_Channel, float p_Gain, Image& p_Out)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=, &p_Frame]() {
            for (int y = startY; y < endY; ++y) {
                for (int x = 0; x < width; ++x) {
                    int i = y * width + x;
                    float r = inPx[i*3 + 0] / 255.0f;
                    float g = inPx[i*3 + 1] / 255.0f;
                    float b = inPx[i*3 + 2] / 255.0f;
                    
                    float shown = 0.5f;

                    if (p_Method == 1) { // Channel
                        float val = GetNoiseChannel(r, g, b, p_Channel, p_Frame.lumaCoeff);
                        shown = 0.5f + (val - 0.5f) * p_Gain;
                    } else {
                        float sumR = 0, sumG = 0, sumB = 0;
                        int count = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                int nx = std::clamp(x + dx, 0, width - 1);
                                int ny = std::clamp(y + dy, 0, height - 1);
                                int nidx = (ny * width + nx) * 3;
                                sumR += inPx[nidx + 0] / 255.0f;
                                sumG += inPx[nidx + 1] / 255.0f;
                                sumB += inPx[nidx + 2] / 255.0f;
                                count++;
                            }
                        }
                        float meanR = sumR / count;
                        float meanG = sumG / count;
                        float meanB = sumB / count;
                        
                        if (p_Method == 0) { // Highpass
                            float val = GetNoiseChannel(r, g, b, p_Channel, p_Frame.lumaCoeff);
                            float meanVal = GetNoiseChannel(meanR, meanG, meanB, p_Channel, p_Frame.lumaCoeff);
                            shown = 0.5f + (val - meanVal) * p_Gain;
                        } else if (p_Method == 2) { // Differenced
                            float diffB = b - meanB;
                            float diffG = g - meanG;
                            shown = 0.5f + (diffB - diffG) * p_Gain;
                        }
                    }
                    
                    uint8_t outVal = ToByte(std::clamp(shown, 0.0f, 1.0f) * 255.0f);
                    outPx[i*4 + 0] = outVal;
                    outPx[i*4 + 1] = outVal;
                    outPx[i*4 + 2] = outVal;
                    outPx[i*4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

void BuildFocusPeakingImage(const ScopeFrame& p_Frame, float p_Threshold, bool p_GrayscaleBg,
                            Rgb8 p_PeakColor, Image& p_Out)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];

    // Central difference, one pixel either side, matching the old app's
    // dx/dy - a 3-tap gradient rather than a full Sobel, cheap enough to run
    // every frame and sharp enough that a soft-focus edge stays quiet.
    auto lumaAt = [&](int x, int y) -> float {
        const int i = (y * width + x) * 3;
        return kr * (inPx[i + 0] / 255.0f) + kg * (inPx[i + 1] / 255.0f) + kb * (inPx[i + 2] / 255.0f);
    };

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=]() {
            for (int y = startY; y < endY; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int i = y * width + x;

                    float dx = 0.0f, dy = 0.0f;
                    if (x > 0 && x < width - 1)
                        dx = std::fabs(lumaAt(x + 1, y) - lumaAt(x - 1, y)) * 0.5f;
                    if (y > 0 && y < height - 1)
                        dy = std::fabs(lumaAt(x, y + 1) - lumaAt(x, y - 1)) * 0.5f;

                    uint8_t r, g, b;
                    if (p_GrayscaleBg) {
                        const uint8_t grey = ToByte(std::clamp(lumaAt(x, y), 0.0f, 1.0f) * 255.0f);
                        r = g = b = grey;
                    } else {
                        r = inPx[i * 3 + 0];
                        g = inPx[i * 3 + 1];
                        b = inPx[i * 3 + 2];
                    }

                    if (dx + dy > p_Threshold) {
                        r = p_PeakColor.r;
                        g = p_PeakColor.g;
                        b = p_PeakColor.b;
                    }

                    outPx[i * 4 + 0] = r;
                    outPx[i * 4 + 1] = g;
                    outPx[i * 4 + 2] = b;
                    outPx[i * 4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

void BuildBandingAnalyzerImage(const ScopeFrame& p_Frame, float p_Gain, Image& p_Out)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=]() {
            for (int y = startY; y < endY; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int i = y * width + x;

                    for (int c = 0; c < 3; ++c) {
                        const float here = inPx[i * 3 + c] / 255.0f;
                        const float right = (x < width - 1) ? inPx[(i + 1) * 3 + c] / 255.0f : here;
                        const float below = (y < height - 1) ? inPx[(i + width) * 3 + c] / 255.0f : here;
                        const float diff = (std::fabs(right - here) + std::fabs(below - here)) * p_Gain;
                        // Channel c to position c. The old app wrote BGR here because
                        // its buffer became a QImage RGB32, which is B,G,R,A in memory;
                        // carried into an RGBA texture that order showed red as blue.
                        outPx[i * 4 + c] = ToByte(std::clamp(diff, 0.0f, 1.0f) * 255.0f);
                    }
                    outPx[i * 4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

void BuildDifferenceImage(const ScopeFrame& p_Frame,
                          const uint8_t* p_RefPixels, int p_RefWidth, int p_RefHeight,
                          float p_Gain,
                          Image& p_Out)
{
    if (!p_Frame.HasPreview()) {
        p_Out.Resize(0, 0);
        return;
    }

    const int width = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    p_Out.Resize(width, height);

    const uint8_t* inPx = p_Frame.preview.data();
    uint8_t* outPx = p_Out.pixels.data();

    const bool haveRef = p_RefPixels != nullptr && p_RefWidth == width && p_RefHeight == height;

    int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> futures;
    int rowsPerThread = height / numThreads;

    for (int t = 0; t < numThreads; ++t) {
        int startY = t * rowsPerThread;
        int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=]() {
            for (int y = startY; y < endY; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int i = y * width + x;
                    for (int c = 0; c < 3; ++c) {
                        uint8_t v;
                        if (haveRef) {
                            const float diff = std::fabs(float(inPx[i * 3 + c]) - float(p_RefPixels[i * 3 + c])) * p_Gain;
                            v = ToByte(std::clamp(diff, 0.0f, 255.0f));
                        } else {
                            v = ToByte(inPx[i * 3 + c] * 0.2f);
                        }
                        outPx[i * 4 + c] = v;   // RGBA, not the old app's BGR - see BuildBandingAnalyzerImage
                    }
                    outPx[i * 4 + 3] = 255;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

namespace
{
    // Mirrors ScopeCore.cpp's own BinOf/ChromaBinOf/TwinPeaksLevelBinOf/
    // TwinPeaksDiffBinOf, which live in that file's own anonymous namespace
    // and are not reachable from here - this build does not link ScopeCore.cpp
    // (see PORTING.md), so the four bin-mapping helpers are duplicated rather
    // than shared. Same clamp-into-end-bins policy: an out-of-range sample
    // still counts, just in whichever end bin it overshot, and Mask* is a
    // prefix rather than a name collision because ODR means these must not
    // silently become the plugin's own if ScopeCore.cpp is ever linked in too.
    constexpr float kMaskBinScale = float(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);
    inline uint32_t MaskBinOf(float p_Value, uint32_t p_BinCount)
    {
        const float t = (p_Value - kBinRangeLow) * kMaskBinScale;
        if (!(t > 0.0f)) return 0;
        const uint32_t bin = uint32_t(t);
        return (bin >= p_BinCount) ? (p_BinCount - 1) : bin;
    }

    constexpr float kMaskChromaBinScale = float(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);
    inline uint32_t MaskChromaBinOf(float p_Value)
    {
        const float t = (p_Value - kChromaRangeLow) * kMaskChromaBinScale;
        if (!(t > 0.0f)) return 0;
        const uint32_t bin = uint32_t(t);
        return (bin >= kVectorscopeSize) ? (kVectorscopeSize - 1) : bin;
    }

    static_assert(kTwinPeaksSize == kWaveformLevels,
                 "MaskTwinPeaksLevelBinOf reuses kMaskBinScale, which is precomputed for kWaveformLevels");
    inline uint32_t MaskTwinPeaksLevelBinOf(float p_Value) { return MaskBinOf(p_Value, kTwinPeaksSize); }

    constexpr float kMaskTwinPeaksDiffBinScale =
        float(kTwinPeaksSize - 1) / (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);
    inline uint32_t MaskTwinPeaksDiffBinOf(float p_Value)
    {
        const float t = (p_Value - kTwinPeaksDiffRangeLow) * kMaskTwinPeaksDiffBinScale;
        if (!(t > 0.0f)) return 0;
        const uint32_t bin = uint32_t(t);
        return (bin >= kTwinPeaksSize) ? (kTwinPeaksSize - 1) : bin;
    }
} // namespace

void BuildScopeBinsFromRGB(const uint8_t* p_Rgb, int p_Width, int p_Height,
                           const float p_LumaCoeff[3], const SpatialMask* p_Mask,
                           MaskedScopeBins& p_Out, const ValueQualifier* p_Qualifier)
{
    p_Out.waveform.assign(size_t(kWaveformCells), 0u);
    p_Out.histogram.assign(size_t(kHistogramCells), 0u);
    p_Out.vectorscope.assign(size_t(kVectorscopeTotalCells), 0u);
    p_Out.twinPeaks.assign(size_t(kTwinPeaksTotalCells), 0u);
    p_Out.empty = true;

    if (!p_Rgb || p_Width <= 0 || p_Height <= 0) return;

    const int width  = p_Width;
    const int height = p_Height;

    const uint8_t* inPx = p_Rgb;
    const float kr = p_LumaCoeff[0];
    const float kg = p_LumaCoeff[1];
    const float kb = p_LumaCoeff[2];
    const float cbScale = 0.5f / (1.0f - kb);
    const float crScale = 0.5f / (1.0f - kr);
    const float aspect = float(width) / float(std::max(height, 1));
    const bool  hasMask = p_Mask != nullptr;
    const SpatialMask maskCopy = hasMask ? *p_Mask : SpatialMask{};
    const bool  hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();

    // Private bins per thread, merged afterwards - same reasoning as
    // ScopeCore.cpp's Reduce(): contended atomics across every preview pixel
    // would cost more than the merge below does.
    const int numThreads = std::max(1, std::min(int(std::thread::hardware_concurrency()), height));

    std::vector<std::vector<uint32_t>> partialWave(numThreads);
    std::vector<std::vector<uint32_t>> partialVec(numThreads);
    std::vector<std::vector<uint32_t>> partialTp(numThreads);
    std::vector<uint64_t>              partialCount(size_t(numThreads), 0);

    for (int t = 0; t < numThreads; ++t)
    {
        partialWave[t].assign(size_t(kWaveformCells), 0u);
        partialVec[t].assign(size_t(kVectorscopeTotalCells), 0u);
        partialTp[t].assign(size_t(kTwinPeaksTotalCells), 0u);
    }

    const int rowsPerThread = height / numThreads;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < numThreads; ++t)
    {
        const int startY = t * rowsPerThread;
        const int endY = (t == numThreads - 1) ? height : startY + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [&, t, startY, endY]()
        {
            uint32_t* wave = partialWave[t].data();
            uint32_t* vec  = partialVec[t].data();
            uint32_t* tp   = partialTp[t].data();
            uint64_t  counted = 0;

            for (int y = startY; y < endY; ++y)
            {
                const float fy = float(y) / float(std::max(height - 1, 1));
                const uint8_t* row = inPx + size_t(y) * size_t(width) * 3;

                for (int x = 0; x < width; ++x)
                {
                    const float fx = float(x) / float(std::max(width - 1, 1));

                    const float r = row[x * 3 + 0] / 255.0f;
                    const float g = row[x * 3 + 1] / 255.0f;
                    const float b = row[x * 3 + 2] / 255.0f;
                    const float luma = kr * r + kg * g + kb * b;

                    // The same >= 0.5 threshold the old app's qualifier used to
                    // turn a Gradient mask's continuous weight into an in/out
                    // decision - a pixel either counts fully or is dropped, never
                    // partially counted, so the bins stay genuine pixel counts.
                    // The value qualifier is already boolean, so it needs no
                    // threshold of its own.
                    if (hasMask && SpatialMaskWeightAt(maskCopy, fx, fy, aspect) < 0.5f) continue;
                    if (hasQualifier && !ValueQualifierPasses(*p_Qualifier, r, g, b, p_LumaCoeff)) continue;

                    const uint32_t col = std::min(uint32_t(x) * kWaveformColumns / uint32_t(width),
                                                  kWaveformColumns - 1);
                    uint32_t* wcol = wave + uint64_t(col) * kWaveformLevels * kPlaneCount;
                    ++wcol[MaskBinOf(r, kWaveformLevels) * kPlaneCount + kPlaneR];
                    ++wcol[MaskBinOf(g, kWaveformLevels) * kPlaneCount + kPlaneG];
                    ++wcol[MaskBinOf(b, kWaveformLevels) * kPlaneCount + kPlaneB];
                    ++wcol[MaskBinOf(luma, kWaveformLevels) * kPlaneCount + kPlaneY];

                    const float cb = (b - luma) * cbScale;
                    const float cr = (r - luma) * crScale;
                    ++vec[VectorscopeIndex(VectorscopeBandOf(luma), MaskChromaBinOf(cb), MaskChromaBinOf(cr))];

                    const float levelGB = (g + b) * 0.5f;
                    const float diffGB  = (g - b) * 0.5f;
                    ++tp[TwinPeaksIndex(kDiamondGreenBlue,
                                        MaskTwinPeaksLevelBinOf(levelGB), MaskTwinPeaksDiffBinOf(diffGB))];

                    const float levelGR = (g + r) * 0.5f;
                    const float diffGR  = (g - r) * 0.5f;
                    ++tp[TwinPeaksIndex(kDiamondGreenRed,
                                        MaskTwinPeaksLevelBinOf(levelGR), MaskTwinPeaksDiffBinOf(diffGR))];

                    ++counted;
                }
            }

            partialCount[t] = counted;
        }));
    }
    for (auto& f : futures) f.wait();

    uint32_t* wave = p_Out.waveform.data();
    uint32_t* vec  = p_Out.vectorscope.data();
    uint32_t* tp   = p_Out.twinPeaks.data();
    uint64_t  total = 0;

    for (int t = 0; t < numThreads; ++t)
    {
        const uint32_t* pw = partialWave[t].data();
        for (uint64_t i = 0; i < kWaveformCells; ++i) wave[i] += pw[i];

        const uint32_t* pv = partialVec[t].data();
        for (uint64_t i = 0; i < kVectorscopeTotalCells; ++i) vec[i] += pv[i];

        const uint32_t* ptp = partialTp[t].data();
        for (uint64_t i = 0; i < kTwinPeaksTotalCells; ++i) tp[i] += ptp[i];

        total += partialCount[t];
    }

    // Histogram derived from the merged waveform, exactly like the tap's own
    // Analyse() does - same bin count and range, so summing over columns is exact.
    static_assert(kHistogramBins == kWaveformLevels, "histogram derivation assumes matching bin counts");
    uint32_t* hist = p_Out.histogram.data();
    for (uint32_t col = 0; col < kWaveformColumns; ++col)
    {
        const uint32_t* wcol = wave + uint64_t(col) * kWaveformLevels * kPlaneCount;
        for (uint32_t level = 0; level < kWaveformLevels; ++level)
            for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
                hist[HistogramIndex(plane, level)] += wcol[level * kPlaneCount + plane];
    }

    p_Out.referenceSamples = std::max(float(total) / float(kWaveformColumns), 1.0f);
    p_Out.empty = (total == 0);
}

void BuildMaskedScopeBins(const ScopeFrame& p_Frame, const SpatialMask& p_Mask, MaskedScopeBins& p_Out,
                          const ValueQualifier* p_Qualifier)
{
    const bool hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();
    if (!p_Frame.HasPreview() || (!p_Mask.hasGeometry && !hasQualifier))
    {
        p_Out.waveform.assign(size_t(kWaveformCells), 0u);
        p_Out.histogram.assign(size_t(kHistogramCells), 0u);
        p_Out.vectorscope.assign(size_t(kVectorscopeTotalCells), 0u);
        p_Out.twinPeaks.assign(size_t(kTwinPeaksTotalCells), 0u);
        p_Out.waveformTrace.clear();
        p_Out.empty = true;
        return;
    }

    const SpatialMask* maskArg = p_Mask.hasGeometry ? &p_Mask : nullptr;
    BuildScopeBinsFromRGB(p_Frame.preview.data(), int(p_Frame.previewWidth), int(p_Frame.previewHeight),
                         p_Frame.lumaCoeff, maskArg, p_Out, p_Qualifier);
    BuildMaskedWaveformTrace(p_Frame, p_Mask, p_Out.waveformTrace, p_Qualifier);
}

void BuildMaskedWaveformTrace(const ScopeFrame& p_Frame, const SpatialMask& p_Mask, std::vector<float>& p_Out,
                              const ValueQualifier* p_Qualifier)
{
    p_Out.assign(size_t(kWaveformTraceCells), 0.0f);

    const bool hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();
    if (!p_Frame.HasPreview() || (!p_Mask.hasGeometry && !hasQualifier)) return;

    const int width  = int(p_Frame.previewWidth);
    const int height = int(p_Frame.previewHeight);
    if (width <= 0 || height <= 0) return;

    const float kNaN = std::numeric_limits<float>::quiet_NaN();

    const uint8_t* inPx = p_Frame.preview.data();
    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];
    const float aspect = float(width) / float(std::max(height, 1));
    const SpatialMask maskCopy = p_Mask;
    const float* lumaCoeffPtr = p_Frame.lumaCoeff;

    // Split by trace-row range across threads: WaveformTraceIndex lays rows
    // out row-major, so different rows never touch the same p_Out indices
    // and there is nothing to merge afterwards. This inner loop - a per-pixel
    // mask test across the full image width, repeated for every one of the
    // (fixed, resolution-independent) kWaveformTraceRows rows - measured as
    // the dominant, largely resolution-independent cost of the masked-scope
    // path, which is what makes it worth threading on its own rather than
    // relying on a smaller preview resolution to shrink it away.
    const int numThreads = std::max(1, std::min(int(std::thread::hardware_concurrency()),
                                                  int(kWaveformTraceRows)));
    const uint32_t rowsPerThread = kWaveformTraceRows / uint32_t(numThreads);
    std::vector<std::future<void>> futures;

    for (int t = 0; t < numThreads; ++t)
    {
        const uint32_t startRow = uint32_t(t) * rowsPerThread;
        const uint32_t endRow = (t == numThreads - 1) ? kWaveformTraceRows : startRow + rowsPerThread;

        futures.push_back(std::async(std::launch::async, [=, &maskCopy, &p_Out]()
        {
            for (uint32_t row = startRow; row < endRow; ++row)
            {
                // Evenly strided, distinct source rows - same reasoning as
                // BuildWaveformTrace's own comment: averaging *between* rows
                // would blend distinct scanlines and defeat the point of a
                // line trace.
                const int srcY = (kWaveformTraceRows > 1)
                    ? int((uint64_t(row) * uint64_t(height - 1)) / (kWaveformTraceRows - 1))
                    : 0;
                const float fy = float(srcY) / float(std::max(height - 1, 1));
                const uint8_t* rowPixels = inPx + size_t(srcY) * size_t(width) * 3;

                for (uint32_t col = 0; col < kWaveformColumns; ++col)
                {
                    const int x0 = int((uint64_t(col) * uint64_t(width)) / kWaveformColumns);
                    const int x1 = int((uint64_t(col + 1) * uint64_t(width)) / kWaveformColumns);

                    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
                    int counted = 0;
                    for (int x = x0; x < x1; ++x)
                    {
                        const float fx = float(x) / float(std::max(width - 1, 1));
                        if (SpatialMaskWeightAt(maskCopy, fx, fy, aspect) < 0.5f) continue;

                        const float px = rowPixels[x * 3 + 0] / 255.0f;
                        const float py = rowPixels[x * 3 + 1] / 255.0f;
                        const float pz = rowPixels[x * 3 + 2] / 255.0f;
                        if (hasQualifier && !ValueQualifierPasses(*p_Qualifier, px, py, pz, lumaCoeffPtr)) continue;

                        sumR += px;
                        sumG += py;
                        sumB += pz;
                        ++counted;
                    }

                    // NaN, not 0, when nothing in this column fell inside the
                    // mask: 0 is a real, displayable value (pure black), and
                    // every reader of this trace already breaks its drawn
                    // line on a NaN sample the same way it breaks on an
                    // out-of-range one - a real gap where there is genuinely
                    // nothing measured, not a fabricated plunge to the
                    // bottom of the waveform that looks like real picture
                    // information.
                    float r = kNaN, g = kNaN, b = kNaN, luma = kNaN;
                    if (counted > 0)
                    {
                        const float inv = 1.0f / float(counted);
                        r = sumR * inv;
                        g = sumG * inv;
                        b = sumB * inv;
                        luma = kr * r + kg * g + kb * b;
                    }

                    const uint64_t base = WaveformTraceIndex(row, col, 0);
                    p_Out[base + kPlaneR] = r;
                    p_Out[base + kPlaneG] = g;
                    p_Out[base + kPlaneB] = b;
                    p_Out[base + kPlaneY] = luma;
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();
}

namespace
{
    // Separable box blur of a single-channel float plane, edge-clamped. A
    // sliding window sum keeps each pass O(w*h) rather than O(w*h*radius) -
    // the profile below calls this six times a frame, and staying cheap here
    // is the entire reason it exists instead of an FFT.
    void BoxBlur2D(const std::vector<float>& p_Src, int p_Width, int p_Height, int p_Radius,
                   std::vector<float>& p_Out)
    {
        if (p_Radius <= 0) { p_Out = p_Src; return; }

        std::vector<float> temp(p_Src.size());
        const float invWindow = 1.0f / float(2 * p_Radius + 1);

        // Both passes of a separable blur are embarrassingly parallel - every
        // row of the horizontal pass, and every column of the vertical one,
        // is an independent sliding window. Grain Analyzer runs nine of these
        // over the whole preview every frame and was measured at ~75ms
        // single-threaded, which is most of a frame on its own.
        //
        // Captures are by reference (`[&, ...]`, with only the loop bounds by
        // value), never by value: the buffers here are whole-image float
        // vectors, and copying one per worker is exactly the bug that made
        // Skin Tone and False Color cost ~100ms each. Safe because every
        // future is waited on before this function returns.
        const int threads = std::max(1, int(std::thread::hardware_concurrency()));

        {
            std::vector<std::future<void>> futures;
            const int rowsPerThread = std::max(1, (p_Height + threads - 1) / threads);
            for (int y0 = 0; y0 < p_Height; y0 += rowsPerThread)
            {
                const int y1 = std::min(p_Height, y0 + rowsPerThread);
                futures.push_back(std::async(std::launch::async, [&, y0, y1]() {
                    for (int y = y0; y < y1; ++y)
                    {
                        const float* row = &p_Src[size_t(y) * p_Width];
                        float sum = 0.0f;
                        for (int x = -p_Radius; x <= p_Radius; ++x)
                            sum += row[std::clamp(x, 0, p_Width - 1)];

                        for (int x = 0; x < p_Width; ++x)
                        {
                            temp[size_t(y) * p_Width + x] = sum * invWindow;
                            sum += row[std::clamp(x + p_Radius + 1, 0, p_Width - 1)]
                                 - row[std::clamp(x - p_Radius, 0, p_Width - 1)];
                        }
                    }
                }));
            }
            for (auto& f : futures) f.wait();
        }

        p_Out.resize(p_Src.size());

        // The vertical pass carries a running sum *down* each column, so the
        // obvious loop (one column at a time) strides through memory by a
        // whole row per step - it touches one float per cache line and throws
        // the other fifteen away. That made the pass memory-bound: splitting
        // it across cores bought far less than it should have (~1.9x on the
        // whole analyzer, where the work is embarrassingly parallel).
        //
        // Carrying kColumnBlock running sums at once instead turns every
        // access into a sequential run along a row, so each cache line is
        // used in full. Bit-identical: each column still accumulates exactly
        // the same values in the same order, only interleaved with its
        // neighbours.
        constexpr int kColumnBlock = 32;
        {
            std::vector<std::future<void>> futures;
            const int blocksTotal   = (p_Width + kColumnBlock - 1) / kColumnBlock;
            const int blocksPerTask = std::max(1, (blocksTotal + threads - 1) / threads);

            for (int b0 = 0; b0 < blocksTotal; b0 += blocksPerTask)
            {
                const int b1 = std::min(blocksTotal, b0 + blocksPerTask);
                futures.push_back(std::async(std::launch::async, [&, b0, b1]() {
                    float sums[kColumnBlock];
                    for (int b = b0; b < b1; ++b)
                    {
                        const int x0 = b * kColumnBlock;
                        const int bw = std::min(kColumnBlock, p_Width - x0);

                        for (int c = 0; c < bw; ++c) sums[c] = 0.0f;
                        for (int y = -p_Radius; y <= p_Radius; ++y)
                        {
                            const float* src = &temp[size_t(std::clamp(y, 0, p_Height - 1)) * p_Width + x0];
                            for (int c = 0; c < bw; ++c) sums[c] += src[c];
                        }

                        for (int y = 0; y < p_Height; ++y)
                        {
                            float* dst = &p_Out[size_t(y) * p_Width + x0];
                            for (int c = 0; c < bw; ++c) dst[c] = sums[c] * invWindow;

                            const float* add = &temp[size_t(std::clamp(y + p_Radius + 1, 0, p_Height - 1)) * p_Width + x0];
                            const float* sub = &temp[size_t(std::clamp(y - p_Radius, 0, p_Height - 1)) * p_Width + x0];
                            for (int c = 0; c < bw; ++c) sums[c] += add[c] - sub[c];
                        }
                    }
                }));
            }
            for (auto& f : futures) f.wait();
        }
    }
} // namespace

void BuildGrainAnalyzerProfile(const ScopeFrame& p_Frame, float p_Out[kGrainBandCount])
{
    for (int i = 0; i < kGrainBandCount; ++i) p_Out[i] = 0.0f;

    if (!p_Frame.HasPreview()) return;

    const int width  = p_Frame.previewWidth;
    const int height = p_Frame.previewHeight;
    const size_t pixelCount = size_t(width) * size_t(height);
    if (pixelCount == 0) return;

    const float kr = p_Frame.lumaCoeff[0];
    const float kg = p_Frame.lumaCoeff[1];
    const float kb = p_Frame.lumaCoeff[2];

    const uint8_t* inPx = p_Frame.preview.data();
    std::vector<float> luma(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i)
        luma[i] = kr * (inPx[i * 3 + 0] / 255.0f)
                + kg * (inPx[i * 3 + 1] / 255.0f)
                + kb * (inPx[i * 3 + 2] / 255.0f);

    std::vector<float> prevBlur = luma;   // radius 0: the unblurred image
    std::vector<float> nextBlur;

    for (int band = 0; band < kGrainBandCount; ++band)
    {
        // A radius past half the smaller image dimension has already blurred
        // across the whole frame - clamped so the last band or two on a small
        // preview do not silently recompute an identical, wasted blur.
        const int radius = std::min(kGrainRadii[band + 1], std::max(1, std::min(width, height) / 2));
        BoxBlur2D(luma, width, height, radius, nextBlur);

        double sumSq = 0.0;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const double d = double(prevBlur[i]) - double(nextBlur[i]);
            sumSq += d * d;
        }
        // Raw RMS - see this function's own declaration in ScopeImages.h for
        // why this is deliberately not normalised against its own peak band.
        p_Out[band] = float(std::sqrt(sumSq / double(pixelCount)));

        prevBlur.swap(nextBlur);
    }
}

// ---------------------------------------------------------------------------
// Color Cube
// ---------------------------------------------------------------------------

void SrgbToXyz(float p_R, float p_G, float p_B, float& p_X, float& p_Y, float& p_Z)
{
    auto linearize = [](float c) {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const float r = linearize(p_R), g = linearize(p_G), b = linearize(p_B);

    // sRGB -> XYZ, D65 white point - the standard matrix.
    p_X = 0.4124564f * r + 0.3575761f * g + 0.1804375f * b;
    p_Y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    p_Z = 0.0193339f * r + 0.1191920f * g + 0.9503041f * b;
}

void XyzToLab(float p_X, float p_Y, float p_Z, float& p_L, float& p_A, float& p_B)
{
    constexpr float kXn = 0.95047f, kYn = 1.0f, kZn = 1.08883f;   // D65 white
    const float xr = p_X / kXn, yr = p_Y / kYn, zr = p_Z / kZn;

    constexpr float kEpsilon = 0.008856f;
    auto f = [](float t) { return (t > kEpsilon) ? std::cbrt(t) : (903.3f * t + 16.0f) / 116.0f; };
    const float fx = f(xr), fy = f(yr), fz = f(zr);

    p_L = 116.0f * fy - 16.0f;
    p_A = 500.0f * (fx - fy);
    p_B = 200.0f * (fy - fz);
}

void BuildColorCubePoints(const ScopeFrame& p_Frame, ColorCubeSpace p_Space,
                          std::vector<ColorCubePoint>& p_Out,
                          const SpatialMask* p_Mask, const ValueQualifier* p_Qualifier)
{
    p_Out.clear();
    if (!p_Frame.HasPreview()) return;

    const uint64_t totalPixels = uint64_t(p_Frame.previewWidth) * uint64_t(p_Frame.previewHeight);
    if (totalPixels == 0) return;

    const int sampleCount = int(std::min<uint64_t>(totalPixels, uint64_t(kColorCubeMaxPoints)));
    p_Out.reserve(size_t(sampleCount));

    // Evenly strided rather than a true random subsample, unlike the old
    // app's numpy RNG choice(replace=False) - O(sampleCount) regardless of
    // preview resolution instead of O(totalPixels) to draw the sample from,
    // and spreads just as evenly across the frame for a point cloud's
    // purposes. Still deterministic per frame, same reasoning the old app's
    // frame-seeded RNG existed for: the start offset is hashed from the
    // frame index rather than drifting across repaints, so redrawing the
    // same video frame - a focus change, an unrelated setting - reproduces
    // the identical points instead of visibly re-jittering them.
    const uint64_t stride = std::max<uint64_t>(1, totalPixels / uint64_t(sampleCount));
    const uint64_t offset = (p_Frame.frameIndex * 2654435761ull) % stride;

    const uint8_t* px = p_Frame.preview.data();

    const bool  hasMask      = p_Mask != nullptr && p_Mask->hasGeometry;
    const bool  hasQualifier = p_Qualifier != nullptr && p_Qualifier->AnyActive();
    const int   width        = int(p_Frame.previewWidth);
    const int   height       = int(p_Frame.previewHeight);
    const float aspect       = float(width) / float(std::max(height, 1));

    for (int i = 0; i < sampleCount; ++i)
    {
        const uint64_t idx = (offset + uint64_t(i) * stride) % totalPixels;
        const uint8_t* p = px + idx * 3;
        const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;

        // Same 0.5 threshold and same "drop it" handling the chromaticity
        // trace uses, so a region isolated on one scope is the same region on
        // the other.
        if (hasMask || hasQualifier)
        {
            const float fx = float(idx % uint64_t(width))  / float(std::max(width  - 1, 1));
            const float fy = float(idx / uint64_t(width))  / float(std::max(height - 1, 1));
            if (hasMask && SpatialMaskWeightAt(*p_Mask, fx, fy, aspect) < 0.5f)
                continue;
            if (hasQualifier && !ValueQualifierPasses(*p_Qualifier, r, g, b, p_Frame.lumaCoeff))
                continue;
        }

        ColorCubePoint pt;
        pt.colour = { p[0], p[1], p[2] };

        switch (p_Space)
        {
            case ColorCubeSpace::RGB:
                pt.x = r * 2.0f - 1.0f;
                pt.y = g * 2.0f - 1.0f;
                pt.z = b * 2.0f - 1.0f;
                break;

            case ColorCubeSpace::XYZ:
            {
                float x, y, z;
                SrgbToXyz(r, g, b, x, y, z);
                // D65 white bounds (x: 0..0.95047, y: 0..1, z: 0..1.08883),
                // the same per-axis ranges the old app's
                // COLOR_CUBE_AXIS_RANGES used, so RGB/XYZ/Lab all share one
                // camera scale.
                pt.x = (x / 0.95047f) * 2.0f - 1.0f;
                pt.y = y * 2.0f - 1.0f;
                pt.z = (z / 1.08883f) * 2.0f - 1.0f;
                break;
            }

            case ColorCubeSpace::Lab:
            {
                float x, y, z, L, a, bLab;
                SrgbToXyz(r, g, b, x, y, z);
                XyzToLab(x, y, z, L, a, bLab);
                pt.x = (L / 100.0f) * 2.0f - 1.0f;                       // L: 0..100
                pt.y = std::clamp(a / 128.0f, -1.0f, 1.0f);              // a*: ~-128..127
                pt.z = std::clamp(bLab / 128.0f, -1.0f, 1.0f);           // b*: ~-128..127
                break;
            }

            default: break;
        }

        p_Out.push_back(pt);
    }
}

// ---------------------------------------------------------------------------
// Chromaticity (CIE 1931 x,y)
// ---------------------------------------------------------------------------

const std::vector<Vec2>& ChromaticitySpectralLocus()
{
    static std::vector<Vec2> locus;
    if (!locus.empty()) return locus;

    // CIE 1931 2-degree standard observer colour-matching functions (CIE
    // 018:2019, Table 6), 5nm steps, 380-700nm - same source table as the old
    // app's scope_format.py, fetched rather than approximated.
    static const float kCmf[][3] = {
        { 0.001368f, 0.000039f, 0.006450f }, { 0.002236f, 0.000064f, 0.010550f },
        { 0.004243f, 0.000120f, 0.020050f }, { 0.007650f, 0.000217f, 0.036210f },
        { 0.014310f, 0.000396f, 0.067850f }, { 0.023190f, 0.000640f, 0.110200f },
        { 0.043510f, 0.001210f, 0.207400f }, { 0.077630f, 0.002180f, 0.371300f },
        { 0.134380f, 0.004000f, 0.645600f }, { 0.214770f, 0.007300f, 1.039050f },
        { 0.283900f, 0.011600f, 1.385600f }, { 0.328500f, 0.016840f, 1.622960f },
        { 0.348280f, 0.023000f, 1.747060f }, { 0.348060f, 0.029800f, 1.782600f },
        { 0.336200f, 0.038000f, 1.772110f }, { 0.318700f, 0.048000f, 1.744100f },
        { 0.290800f, 0.060000f, 1.669200f }, { 0.251100f, 0.073900f, 1.528100f },
        { 0.195360f, 0.090980f, 1.287640f }, { 0.142100f, 0.112600f, 1.041900f },
        { 0.095640f, 0.139020f, 0.812950f }, { 0.057950f, 0.169300f, 0.616200f },
        { 0.032010f, 0.208020f, 0.465180f }, { 0.014700f, 0.258600f, 0.353300f },
        { 0.004900f, 0.323000f, 0.272000f }, { 0.002400f, 0.407300f, 0.212300f },
        { 0.009300f, 0.503000f, 0.158200f }, { 0.029100f, 0.608200f, 0.111700f },
        { 0.063270f, 0.710000f, 0.078250f }, { 0.109600f, 0.793200f, 0.057250f },
        { 0.165500f, 0.862000f, 0.042160f }, { 0.225750f, 0.914850f, 0.029840f },
        { 0.290400f, 0.954000f, 0.020300f }, { 0.359700f, 0.980300f, 0.013400f },
        { 0.433450f, 0.994950f, 0.008750f }, { 0.512050f, 1.000000f, 0.005750f },
        { 0.594500f, 0.995000f, 0.003900f }, { 0.678400f, 0.978600f, 0.002750f },
        { 0.762100f, 0.952000f, 0.002100f }, { 0.842500f, 0.915400f, 0.001800f },
        { 0.916300f, 0.870000f, 0.001650f }, { 0.978600f, 0.816300f, 0.001400f },
        { 1.026300f, 0.757000f, 0.001100f }, { 1.056700f, 0.694900f, 0.001000f },
        { 1.062200f, 0.631000f, 0.000800f }, { 1.045600f, 0.566800f, 0.000600f },
        { 1.002600f, 0.503000f, 0.000340f }, { 0.938400f, 0.441200f, 0.000240f },
        { 0.854450f, 0.381000f, 0.000190f }, { 0.751400f, 0.321000f, 0.000100f },
        { 0.642400f, 0.265000f, 0.000050f }, { 0.541900f, 0.217000f, 0.000030f },
        { 0.447900f, 0.175000f, 0.000020f }, { 0.360800f, 0.138200f, 0.000010f },
        { 0.283500f, 0.107000f, 0.000000f }, { 0.218700f, 0.081600f, 0.000000f },
        { 0.164900f, 0.061000f, 0.000000f }, { 0.121200f, 0.044580f, 0.000000f },
        { 0.087400f, 0.032000f, 0.000000f }, { 0.063600f, 0.023200f, 0.000000f },
        { 0.046770f, 0.017000f, 0.000000f }, { 0.032900f, 0.011920f, 0.000000f },
        { 0.022700f, 0.008210f, 0.000000f }, { 0.015840f, 0.005723f, 0.000000f },
        { 0.011359f, 0.004102f, 0.000000f },
    };

    locus.reserve(sizeof(kCmf) / sizeof(kCmf[0]));
    for (const float (&c)[3] : kCmf)
    {
        const float total = c[0] + c[1] + c[2];
        locus.push_back({ c[0] / total, c[1] / total });
    }
    return locus;
}

const std::vector<Vec2>& ChromaticityBlackbodyLocus()
{
    static std::vector<Vec2> locus;
    if (!locus.empty()) return locus;

    // Kim et al.'s CIE-recommended cubic-spline approximation of the
    // Planckian locus, valid 1667K-25000K - same coefficients as the old
    // app, denser below 4000K where the curve bends fastest.
    std::vector<float> temps;
    for (int i = 0; i < 40; ++i) temps.push_back(1667.0f + (4000.0f - 1667.0f) * float(i) / 39.0f);
    for (int i = 1; i < 60; ++i) temps.push_back(4000.0f + (25000.0f - 4000.0f) * float(i) / 59.0f);

    locus.reserve(temps.size());
    for (float t : temps)
    {
        const float t2 = t * t, t3 = t2 * t;
        float x;
        if (t <= 4000.0f)
            x = -0.2661239e9f / t3 - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f;
        else
            x = -3.0258469e9f / t3 + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;

        const float x2 = x * x, x3 = x2 * x;
        float y;
        if (t <= 2222.0f)
            y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
        else if (t <= 4000.0f)
            y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
        else
            y = 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;

        locus.push_back({ x, y });
    }
    return locus;
}

// Standard primaries + shared D65 white, CIE xy - ITU-R BT.709, SMPTE EG
// 432-1 (Display P3), ITU-R BT.2020.
const GamutTriangle kGamutTriangles[3] = {
    { "Rec.709",  { {0.640f,0.330f}, {0.300f,0.600f}, {0.150f,0.060f} }, {0.3127f,0.3290f}, {235,240,245} },
    { "P3-D65",   { {0.680f,0.320f}, {0.265f,0.690f}, {0.150f,0.060f} }, {0.3127f,0.3290f}, {255,196, 60} },
    { "Rec.2020", { {0.708f,0.292f}, {0.170f,0.797f}, {0.131f,0.046f} }, {0.3127f,0.3290f}, {110,200,255} },
};

float FractionOutsideGamut(const std::vector<Vec2>& p_Xy, const Vec2 p_Primaries[3])
{
    if (p_Xy.empty()) return 0.0f;

    auto sign = [](float px, float py, float ax, float ay, float bx, float by) {
        return (px - bx) * (ay - by) - (ax - bx) * (py - by);
    };
    const Vec2& a = p_Primaries[0];
    const Vec2& b = p_Primaries[1];
    const Vec2& c = p_Primaries[2];

    size_t outside = 0;
    for (const Vec2& p : p_Xy)
    {
        const float d1 = sign(p.x, p.y, a.x, a.y, b.x, b.y);
        const float d2 = sign(p.x, p.y, b.x, b.y, c.x, c.y);
        const float d3 = sign(p.x, p.y, c.x, c.y, a.x, a.y);
        const bool hasNeg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
        const bool hasPos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
        if (hasNeg && hasPos) ++outside;   // straddles an edge sign - outside the triangle
    }
    return float(outside) / float(p_Xy.size());
}

void BuildChromaticityAnalysis(const ScopeFrame& p_Frame, ChromaticityAnalysis& p_Out)
{
    const int gridSize = kChromaticityGridSize;
    p_Out.counts.assign(size_t(gridSize) * size_t(gridSize), 0u);
    p_Out.fractionOutside[0] = p_Out.fractionOutside[1] = p_Out.fractionOutside[2] = 0.0f;
    p_Out.empty = true;

    if (!p_Frame.HasPreview()) return;

    const int width  = int(p_Frame.previewWidth);
    const int height = int(p_Frame.previewHeight);
    if (width <= 0 || height <= 0) return;

    // Same ~200,000-pixel budget as the qualifier mask's own subsampling
    // (_qualifier_analysis_stride in the old app) - a density histogram and
    // gamut-coverage percentage don't need every pixel.
    const uint64_t total = uint64_t(width) * uint64_t(height);
    constexpr uint64_t kBudget = 200000;
    const int stride = (total <= kBudget) ? 1
        : std::max(1, int(std::ceil(std::sqrt(double(total) / double(kBudget)))));

    const uint8_t* px = p_Frame.preview.data();
    const float kr = p_Frame.lumaCoeff[0], kg = p_Frame.lumaCoeff[1], kb = p_Frame.lumaCoeff[2];
    const float xSpan = kChromaticityPlotXHi - kChromaticityPlotXLo;
    const float ySpan = kChromaticityPlotYHi - kChromaticityPlotYLo;

    std::vector<Vec2> points;
    points.reserve(size_t(std::min<uint64_t>(total / uint64_t(stride) / uint64_t(stride) + 1, kBudget)));

    for (int y = 0; y < height; y += stride)
    {
        const uint8_t* row = px + size_t(y) * size_t(width) * 3;
        for (int x = 0; x < width; x += stride)
        {
            const float r = row[x * 3 + 0] / 255.0f;
            const float g = row[x * 3 + 1] / 255.0f;
            const float b = row[x * 3 + 2] / 255.0f;
            const float luma = kr * r + kg * g + kb * b;
            if (luma < kChromaticityMinLuma) continue;

            float cx, cy;
            ChromaticityXyOfRgb(r, g, b, cx, cy);
            points.push_back({ cx, cy });

            const int bx = int((cx - kChromaticityPlotXLo) / xSpan * float(gridSize));
            const int by = int((cy - kChromaticityPlotYLo) / ySpan * float(gridSize));
            if (bx < 0 || bx >= gridSize || by < 0 || by >= gridSize) continue;
            ++p_Out.counts[size_t(by) * size_t(gridSize) + size_t(bx)];
        }
    }

    p_Out.empty = points.empty();
    for (int g = 0; g < 3; ++g)
        p_Out.fractionOutside[g] = FractionOutsideGamut(points, kGamutTriangles[g].primaries);
}

namespace
{
    inline float ChromaticityEncodeSrgb(float p_Linear)
    {
        const float c = std::max(p_Linear, 0.0f);
        return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    }

    // An approximate display colour per grid cell, for Colorize - inverse of
    // SrgbToXyz's own matrix (the standard sRGB D65 XYZ->linear matrix) at a
    // fixed luminance, gamma-encoded and renormalised to the brightest
    // channel: many (x, y) points are wildly outside the sRGB gamut, and
    // clamping without renormalising would just paint them black. Computed
    // once - the grid geometry never changes - not per frame.
    const std::vector<Rgb8>& ChromaticityWheel()
    {
        static std::vector<Rgb8> wheel;
        if (!wheel.empty()) return wheel;

        const int size = kChromaticityGridSize;
        wheel.assign(size_t(size) * size_t(size), Rgb8{ 0, 0, 0 });

        const float xSpan = kChromaticityPlotXHi - kChromaticityPlotXLo;
        const float ySpan = kChromaticityPlotYHi - kChromaticityPlotYLo;

        for (int gy = 0; gy < size; ++gy)
        {
            const float y = kChromaticityPlotYLo + (float(gy) + 0.5f) / float(size) * ySpan;
            const float ySafe = (std::fabs(y) > 1e-6f) ? y : 1e-6f;

            for (int gx = 0; gx < size; ++gx)
            {
                const float x = kChromaticityPlotXLo + (float(gx) + 0.5f) / float(size) * xSpan;
                const float X = x / ySafe;
                const float Z = (1.0f - x - y) / ySafe;

                // Standard inverse of the sRGB->XYZ matrix SrgbToXyz uses (Y = 1).
                const float rl =  3.2404542f * X - 1.5371385f - 0.4985314f * Z;
                const float gl = -0.9692660f * X + 1.8760108f + 0.0415560f * Z;
                const float bl =  0.0556434f * X - 0.2040259f + 1.0572252f * Z;

                float r = ChromaticityEncodeSrgb(rl);
                float g = ChromaticityEncodeSrgb(gl);
                float b = ChromaticityEncodeSrgb(bl);
                const float peak = std::max({ r, g, b, 1e-4f });
                r = std::clamp(r / peak, 0.0f, 1.0f);
                g = std::clamp(g / peak, 0.0f, 1.0f);
                b = std::clamp(b / peak, 0.0f, 1.0f);

                wheel[size_t(gy) * size_t(size) + size_t(gx)] =
                    Rgb8{ ToByte(r * 255.0f), ToByte(g * 255.0f), ToByte(b * 255.0f) };
            }
        }
        return wheel;
    }
} // namespace

void BuildChromaticityImage(const ChromaticityAnalysis& p_Analysis, float p_Gain, bool p_Colorize,
                            Image& p_Out)
{
    const int size = kChromaticityGridSize;
    p_Out.Resize(size, size);
    if (p_Analysis.empty) return;

    uint32_t peak = 0;
    for (uint32_t v : p_Analysis.counts) peak = std::max(peak, v);
    if (peak == 0) return;   // a genuinely empty diagram - leave transparent, matching the vectorscope

    const float invPeak = 1.0f / float(peak);
    const std::vector<Rgb8>* wheel = p_Colorize ? &ChromaticityWheel() : nullptr;

    for (int outY = 0; outY < size; ++outY)
    {
        // Flip: y increases upward on the diagram (point_of()'s own
        // plot.bottom() - ty*height), so image row 0 (top of screen) reads
        // the *highest* y bin - same reasoning the vectorscope flips Cr.
        const int gy = size - 1 - outY;
        uint8_t* row = &p_Out.pixels[size_t(outY) * size_t(size) * 4];

        for (int gx = 0; gx < size; ++gx)
        {
            const uint32_t count = p_Analysis.counts[size_t(gy) * size_t(size) + size_t(gx)];
            if (count == 0) continue;

            const float intensity = TraceIntensity(float(count) * invPeak, p_Gain);

            float r = 255.0f, g = 255.0f, b = 255.0f;
            if (wheel)
            {
                const Rgb8& c = (*wheel)[size_t(gy) * size_t(size) + size_t(gx)];
                r = float(c.r); g = float(c.g); b = float(c.b);
            }

            uint8_t* px = &row[size_t(gx) * 4];
            px[0] = ToByte(r * intensity);
            px[1] = ToByte(g * intensity);
            px[2] = ToByte(b * intensity);
            px[3] = ToByte(255.0f * intensity);
        }
    }
}

void BuildChromaticityTrace(const ScopeFrame& p_Frame, const SpatialMask* p_Mask,
                            const ValueQualifier* p_Qualifier,
                            std::vector<ChromaticitySample>& p_Out)
{
    p_Out.assign(size_t(kWaveformTraceRows) * size_t(kWaveformColumns), ChromaticitySample{});

    if (!p_Frame.HasPreview()) return;

    const int width  = int(p_Frame.previewWidth);
    const int height = int(p_Frame.previewHeight);
    if (width <= 0 || height <= 0) return;

    const uint8_t* px = p_Frame.preview.data();
    const float kr = p_Frame.lumaCoeff[0], kg = p_Frame.lumaCoeff[1], kb = p_Frame.lumaCoeff[2];
    const float aspect = float(width) / float(std::max(height, 1));
    const bool hasMask      = p_Mask && p_Mask->hasGeometry;
    const bool hasQualifier = p_Qualifier && p_Qualifier->AnyActive();

    for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
    {
        // Evenly strided, distinct source rows/columns - same reasoning as
        // BuildWaveformTrace/BuildMaskedWaveformTrace's own comment:
        // averaging *between* samples would defeat the whole point of a
        // line trace, doubly so here since the coordinate itself is
        // nonlinear in RGB.
        const int srcY = (kWaveformTraceRows > 1)
            ? int((uint64_t(row) * uint64_t(height - 1)) / (kWaveformTraceRows - 1)) : 0;
        const float fy = float(srcY) / float(std::max(height - 1, 1));
        const uint8_t* rowPixels = px + size_t(srcY) * size_t(width) * 3;

        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            const int srcX = (kWaveformColumns > 1)
                ? int((uint64_t(col) * uint64_t(width - 1)) / (kWaveformColumns - 1)) : 0;
            const float fx = float(srcX) / float(std::max(width - 1, 1));

            const float r = rowPixels[srcX * 3 + 0] / 255.0f;
            const float g = rowPixels[srcX * 3 + 1] / 255.0f;
            const float b = rowPixels[srcX * 3 + 2] / 255.0f;
            const float luma = kr * r + kg * g + kb * b;

            ChromaticitySample& s = p_Out[size_t(row) * size_t(kWaveformColumns) + col];
            s.rgb   = { rowPixels[srcX * 3 + 0], rowPixels[srcX * 3 + 1], rowPixels[srcX * 3 + 2] };
            s.valid = luma >= kChromaticityMinLuma;

            if (s.valid && hasMask && SpatialMaskWeightAt(*p_Mask, fx, fy, aspect) < 0.5f)
                s.valid = false;
            if (s.valid && hasQualifier && !ValueQualifierPasses(*p_Qualifier, r, g, b, p_Frame.lumaCoeff))
                s.valid = false;

            if (s.valid) ChromaticityXyOfRgb(r, g, b, s.x, s.y);
        }
    }
}

// ---------------------------------------------------------------------------
// Sat vs Luma / Sat vs Hue
// ---------------------------------------------------------------------------

namespace
{
    // The classic HSV hue wheel (120-degree-apart cosines) - a fixed
    // reference gradient, not derived from any actual pixel, matching
    // Resolve's own Hue vs Sat curve background.
    inline Rgb8 SatPlotHueWheelColor(float p_HueDeg)
    {
        constexpr float kPi = 3.14159265358979323846f;
        const float rad = p_HueDeg * (kPi / 180.0f);
        const float r = std::clamp(0.5f + 0.5f * std::cos(rad), 0.0f, 1.0f);
        const float g = std::clamp(0.5f + 0.5f * std::cos(rad - 2.0f * kPi / 3.0f), 0.0f, 1.0f);
        const float b = std::clamp(0.5f + 0.5f * std::cos(rad - 4.0f * kPi / 3.0f), 0.0f, 1.0f);
        return Rgb8{ ToByte(r * 255.0f), ToByte(g * 255.0f), ToByte(b * 255.0f) };
    }
} // namespace

void BuildHueHistogramAnalysis(const ScopeFrame& p_Frame, const SpatialMask* p_Mask,
                               const ValueQualifier* p_Qualifier, HueHistogramAnalysis& p_Out)
{
    p_Out.counts.assign(size_t(kHueHistogramBins), 0u);
    p_Out.peak  = 0;
    p_Out.empty = true;

    if (!p_Frame.HasPreview()) return;

    const int width  = int(p_Frame.previewWidth);
    const int height = int(p_Frame.previewHeight);
    if (width <= 0 || height <= 0) return;

    const uint64_t total = uint64_t(width) * uint64_t(height);
    constexpr uint64_t kBudget = 200000;
    const int stride = (total <= kBudget) ? 1
        : std::max(1, int(std::ceil(std::sqrt(double(total) / double(kBudget)))));

    const uint8_t* px = p_Frame.preview.data();
    const float* lumaCoeff = p_Frame.lumaCoeff;
    const float aspect = float(width) / float(std::max(height, 1));
    const bool hasMask      = p_Mask && p_Mask->hasGeometry;
    const bool hasQualifier = p_Qualifier && p_Qualifier->AnyActive();
    constexpr float kMinSat = 0.02f;

    bool any = false;
    for (int y = 0; y < height; y += stride)
    {
        const float fy = float(y) / float(std::max(height - 1, 1));
        const uint8_t* row = px + size_t(y) * size_t(width) * 3;

        for (int x = 0; x < width; x += stride)
        {
            const float fx = float(x) / float(std::max(width - 1, 1));
            if (hasMask && SpatialMaskWeightAt(*p_Mask, fx, fy, aspect) < 0.5f) continue;

            const float r = row[x * 3 + 0] / 255.0f;
            const float g = row[x * 3 + 1] / 255.0f;
            const float b = row[x * 3 + 2] / 255.0f;
            if (hasQualifier && !ValueQualifierPasses(*p_Qualifier, r, g, b, lumaCoeff)) continue;

            float sat, luma, hueDeg;
            RgbToSatLumaHue(r, g, b, lumaCoeff, sat, luma, hueDeg);
            if (sat < kMinSat) continue;

            const int bin = std::clamp(int(hueDeg / 360.0f * float(kHueHistogramBins)),
                                       0, kHueHistogramBins - 1);
            ++p_Out.counts[size_t(bin)];
            any = true;
        }
    }

    p_Out.empty = !any;

    // No smoothing: a brief attempt at circular moving-average smoothing was
    // tried here and reverted - it flattened genuinely narrow, real spikes
    // (a saturated single-hue prop, confirmed by a direct side-by-side
    // comparison against Resolve's own Hue vs Sat scope on the same
    // footage) rather than just removing single-degree-bin jaggedness. The
    // raw per-degree counts are what actually matched Resolve's own curve.
    for (uint32_t c : p_Out.counts) p_Out.peak = std::max(p_Out.peak, c);
}

void BuildHueGradientImage(int p_Width, Image& p_Out)
{
    const int width = std::max(1, p_Width);
    p_Out.Resize(width, 2);

    // Dimmed to ~55% - the reference this was built to match (Resolve's own
    // Hue vs Sat curve background) is a muted version of the wheel, not a
    // fully saturated one, precisely so white graticule/text/the histogram's
    // own outline read clearly over every hue rather than disappearing into
    // a bright patch of matching colour.
    constexpr float kDim = 0.55f;

    for (int x = 0; x < width; ++x)
    {
        const Rgb8 c = SatPlotHueWheelColor((float(x) + 0.5f) / float(width) * 360.0f);
        const uint8_t r = ToByte(float(c.r) * kDim);
        const uint8_t g = ToByte(float(c.g) * kDim);
        const uint8_t b = ToByte(float(c.b) * kDim);
        for (int y = 0; y < 2; ++y)
        {
            uint8_t* p = &p_Out.pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
}

void BuildLumaGradientImage(int p_Width, Image& p_Out)
{
    const int width = std::max(1, p_Width);
    p_Out.Resize(width, 2);

    for (int x = 0; x < width; ++x)
    {
        const uint8_t v = ToByte((float(x) + 0.5f) / float(width) * 255.0f);
        for (int y = 0; y < 2; ++y)
        {
            uint8_t* p = &p_Out.pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
}

} // namespace scopedeck


