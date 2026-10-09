// CPU trace builders: bin arrays in, RGBA images out.
//
// Ported from the PySide6 app's trace_image() / vector_image(), deliberately as
// the same arithmetic rather than a fresh design, so a trace that looks wrong can
// be diffed against the old app on the same frame instead of argued about.
//
// These run on the CPU on purpose. At phase 1 sizes the work is trivial - a
// waveform image is 512x256 and a vectorscope 256x256, which is well under a
// millisecond - and having a CPU implementation first gives the eventual GPU path
// the reference to be checked against, the same doctrine the old app used for its
// preview shaders.

#pragma once

#include "ScopeReader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace scopedeck
{

struct Rgb8 { uint8_t r, g, b; };

// float[3] in 0..1 (an ImGui::ColorEdit3 value) -> Rgb8, clamped.
inline Rgb8 Rgb8FromFloat3(const float p_Rgb[3])
{
    auto toByte = [](float p_V) { return uint8_t(std::clamp(p_V, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return { toByte(p_Rgb[0]), toByte(p_Rgb[1]), toByte(p_Rgb[2]) };
}

// The old app's PLANE_TINTS, unchanged - these are what a user comparing the two
// side by side will expect.
constexpr Rgb8 kTintR = { 255,  74,  74 };
constexpr Rgb8 kTintG = {  74, 230, 118 };
constexpr Rgb8 kTintB = {  94, 142, 255 };
constexpr Rgb8 kTintY = { 235, 240, 245 };

Rgb8 TintForPlane(uint32_t p_Plane);

// An RGBA8 image, row 0 at the top, ready for Texture::UploadRGBA.
struct Image
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;   // width * height * 4

    void Resize(int p_Width, int p_Height);
    void Clear();
    bool Empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

// How many samples a full column would hold if the frame were evenly spread over
// the waveform's columns. Every trace is normalised against this rather than
// against its own peak, so a bright frame and a dark one are drawn on the same
// scale and the trace means something absolute.
float SamplesPerColumn(const ScopeFrame& p_Frame);

// Waveform, one plane, drawn into p_Out at kWaveformColumns x (level span).
//
// p_LevelLo/p_LevelHi select a contiguous slice of the 256 levels, which is how
// the Range control (Full / Shadows / Highlights) works - it crops the level axis
// rather than rescaling the data.
// p_CountsOverride/p_ReferenceOverride let a caller substitute a masked
// re-bin (MaskedScopeBins::waveform/referenceSamples) for the frame's own -
// see the "Masked scope re-binning" section below. Null/0 (the default) uses
// p_Frame.waveform and SamplesPerColumn(p_Frame) as before.
void BuildWaveformImage(const ScopeFrame& p_Frame,
                        uint32_t p_Plane,
                        Rgb8 p_Tint,
                        float p_Gain,
                        int p_LevelLo,
                        int p_LevelHi,
                        Image& p_Out,
                        const uint32_t* p_CountsOverride = nullptr,
                        float p_ReferenceOverride = 0.0f);

// The YRGB mode: several planes additively composited into one image, so a colour
// cast reads directly against the luma it departs from. Matches the old app's
// QPainter CompositionMode_Plus pass.
void BuildWaveformComposite(const ScopeFrame& p_Frame,
                            const uint32_t* p_Planes,
                            int p_PlaneCount,
                            float p_Gain,
                            int p_LevelLo,
                            int p_LevelHi,
                            Image& p_Out,
                            const uint32_t* p_CountsOverride = nullptr,
                            float p_ReferenceOverride = 0.0f);

// Vectorscope, one image at kVectorscopeSize square.
//
// p_BandMask is a bitmask over VectorscopeBand - "All" is the three bands summed
// at read time, which is exactly what the wire format intends (nothing extra is
// stored for it).
//
// Unlike the waveform, this normalises against the frame's own peak bin. A
// vectorscope has no absolute vertical scale to preserve, and normalising against
// a fixed reference leaves most real footage invisible.
// p_ZoomFactor crops the binned data to a centred sub-region before stretching it
// across the same image - 2.0 halves the Cb/Cr span shown. Done here rather than
// by scaling the drawn rectangle so the trace, graticule and readout all agree.
// p_CountsOverride substitutes a masked re-bin (MaskedScopeBins::vectorscope)
// for p_Frame.vectorscope - no reference override needed here, since this
// already normalises against its own peak bin rather than an external one.
void BuildVectorscopeImage(const ScopeFrame& p_Frame,
                           uint32_t p_BandMask,
                           float p_Gain,
                           bool p_Colorize,
                           float p_ZoomFactor,
                           Image& p_Out,
                           const uint32_t* p_CountsOverride = nullptr);

// Where a Rec.709 100%-saturation target for each primary/secondary lands, in
// 0..1 image coordinates within the vectorscope square. Derived from the frame's
// own luma coefficients rather than hardcoded, so the targets follow the colour
// space the tap says it binned in.
struct VectorTarget
{
    const char* name;
    float       x, y;      // 0..1 across the vectorscope image, y down
    Rgb8        color;     // the bar's own colour, so R reads red and Cy cyan
};

// Fills p_Out with the six targets (R, Mg, B, Cy, G, Yl) in that order.
//
// Positions are always in the frame's *true* chroma range, never a zoomed one:
// 2x Zoom magnifies the trace for a closer look against a stationary target, so
// scaling the targets with it would defeat the point of having a reference.
void VectorscopeTargets(const ScopeFrame& p_Frame, VectorTarget p_Out[6]);

// White Balance ("Twin Peaks"): a double Tektronix diamond display - upper
// diamond plots (G-B)/2, (G+B)/2), lower plots ((G-R)/2, (G+R)/2). diff=0
// (horizontal centre) is a perfect channel match at every level, which is the
// one line this scope exists to check a trace against; a colour cast bows the
// trace toward the tinted channel.
//
// Built as one tall image (kTwinPeaksSize wide, kTwinPeaksSize*2 tall) rather
// than two square ones, oriented so the two diamonds meet tip-to-tip at the
// image's own vertical centre: the upper diamond's white (G=B=1) tip is at
// the top, its black tip toward the centre; the lower diamond is the mirror
// of that, black toward the centre, white at the bottom. Matches the
// "Twin Peaks" name - it reads as one bowtie-shaped figure, not two
// unrelated squares stacked up.
// p_CountsOverride substitutes a masked re-bin (MaskedScopeBins::twinPeaks)
// for p_Frame.twinPeaks - each diamond normalises against its own peak, so
// no reference override is needed here either.
void BuildWhiteBalanceImage(const ScopeFrame& p_Frame, float p_Gain, Image& p_Out,
                            const uint32_t* p_CountsOverride = nullptr);

// level/diff (working-space units, see BuildWhiteBalanceImage's own comment)
// -> 0..1 fraction across the *whole* combined image (both diamonds stacked),
// matching the layout BuildWhiteBalanceImage draws and WhiteBalanceGraticule
// places its vertices in. Shared by that graticule and Enhanced Render's beam
// trace, so the graticule and either trace agree on where a value lands.
inline void TwinPeaksToPlot(float p_Level, float p_Diff, float p_BinRangeLow, float p_BinRangeHigh,
                            bool p_UpperDiamond, float& p_OutX, float& p_OutY)
{
    const float levelFrac = (p_Level - p_BinRangeLow) / (p_BinRangeHigh - p_BinRangeLow);
    const float diffFrac  = (p_Diff - kTwinPeaksDiffRangeLow) / (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);
    const float rowFrac   = p_UpperDiamond ? (1.0f - levelFrac) : levelFrac;
    p_OutX = diffFrac;
    p_OutY = p_UpperDiamond ? rowFrac * 0.5f : 0.5f + rowFrac * 0.5f;
}

// Where the diamond graticule's four vertices land, in 0..1 fractions of the
// combined image BuildWhiteBalanceImage produces - the *legal* 0..100% signal
// sub-range of the wider, padded bin range, same trick the vectorscope's
// target boxes use: a real above-white/below-black pixel plots visibly
// outside the diamond rather than being invisibly clamped to its edge.
struct DiamondGraticule { float topX, topY, bottomX, bottomY, leftX, leftY, rightX, rightY; };
DiamondGraticule WhiteBalanceGraticule(const ScopeFrame& p_Frame, bool p_UpperDiamond);

// The flesh-tone reference angle, measured the same way the old app's was. A
// direction to check hue against, not a symmetric axis - so it is drawn as one
// ray from the centre rather than a full diameter.
constexpr float kSkinToneAngleDeg = 123.0f;

// Cb/Cr -> 0..1 plot position, against an explicit chroma range so the caller can
// pass a zoomed range for the trace and the true one for the targets.
inline void ChromaToPlot(float p_Cb, float p_Cr, float p_RangeLo, float p_RangeHi,
                         float& p_OutX, float& p_OutY)
{
    const float span = p_RangeHi - p_RangeLo;
    p_OutX = (p_Cb - p_RangeLo) / span;
    p_OutY = 1.0f - (p_Cr - p_RangeLo) / span;   // Cr points up, y points down
}

// Cb/Cr from linearised RGB, the same derivation the tap uses at bin time. The one
// place this formula is written out on the app side.
inline void CbCrOfRgb(float p_R, float p_G, float p_B, const float p_LumaCoeff[3],
                      float& p_OutCb, float& p_OutCr)
{
    const float y = p_LumaCoeff[0] * p_R + p_LumaCoeff[1] * p_G + p_LumaCoeff[2] * p_B;
    p_OutCb = (p_B - y) * (0.5f / (1.0f - p_LumaCoeff[2]));
    p_OutCr = (p_R - y) * (0.5f / (1.0f - p_LumaCoeff[0]));
}

// --- Spatial mask ------------------------------------------------------------
//
// A user-drawn shape on the Source panel that restricts the preview-tier panels
// (Source itself, False Color, Skin Tone, Noise) to a region of interest - the
// old app's "qualifier" system, minus the Histogram channel/HSL qualifiers and
// the masked-scope re-derivation (waveform/histogram/vectorscope recomputed
// from the mask), which are a separate, much larger undertaking not attempted
// here. Global app state, not per-panel: the mask means the same region to
// every preview panel that reads it, the same way it did in the old app
// (MainWindow.spatial_mask_*, not VideoPanel's own).
enum class MaskShape { Ellipse, Rectangle, Gradient };

struct SpatialMask
{
    bool      hasGeometry = false;
    MaskShape shape       = MaskShape::Ellipse;
    bool      invert      = false;
    // Fractions of the image (0..1) for Ellipse/Rectangle's two opposite
    // corners, or Gradient's start/end point. `angle` (radians) rotates
    // Ellipse/Rectangle about their own centre; meaningless for Gradient.
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, angle = 0.0f;
};

// 1.0 = full colour, 0.0 = fully masked out, continuous only for Gradient -
// Ellipse/Rectangle are hard-edged, matching the old app (it never had a
// feather control either). `p_Aspect` is the image's width/height, needed so
// a non-square image rotates and scales the shape isotropically rather than
// by fraction-space, which would stretch it to the panel's own aspect.
float SpatialMaskWeightAt(const SpatialMask& p_Mask, float p_Fx, float p_Fy, float p_Aspect);

// Field-wise equality, used to decide whether a cached masked re-bin (see
// MaskedScopeBins below) is still good for the mask as currently drawn,
// rather than one frame stale from mid-drag.
inline bool SpatialMaskEquals(const SpatialMask& p_A, const SpatialMask& p_B)
{
    return p_A.hasGeometry == p_B.hasGeometry && p_A.shape == p_B.shape && p_A.invert == p_B.invert
        && p_A.x0 == p_B.x0 && p_A.y0 == p_B.y0 && p_A.x1 == p_B.x1 && p_A.y1 == p_B.y1
        && p_A.angle == p_B.angle;
}

// Blends toward a locally-desaturated grey by (1 - weight) - full colour stays
// untouched, fully masked pixels read as flat grey, matching the old app's
// "keep matched pixels at full colour" qualifier-preview convention (Resolve's
// Highlight, most keyers) rather than replacing masked pixels outright.
inline void ApplyMaskDim(uint8_t& p_R, uint8_t& p_G, uint8_t& p_B, float p_Weight)
{
    if (p_Weight >= 0.999f) return;
    const float r = float(p_R), g = float(p_G), b = float(p_B);
    const float grey = (r + g + b) / 3.0f * 0.2f;
    const float w = std::clamp(p_Weight, 0.0f, 1.0f);
    p_R = uint8_t(std::clamp(r * w + grey * (1.0f - w), 0.0f, 255.0f));
    p_G = uint8_t(std::clamp(g * w + grey * (1.0f - w), 0.0f, 255.0f));
    p_B = uint8_t(std::clamp(b * w + grey * (1.0f - w), 0.0f, 255.0f));
}

// --- Value qualifiers ---------------------------------------------------------
//
// Two independent selections, ANDed together, that restrict pixels by their own
// value rather than by screen position (the SpatialMask above) - the old app's
// Histogram per-channel drag-select and its dedicated HSL Qualifier panel. Both
// combine into the same restriction the spatial mask already feeds to preview
// dimming (ApplyMaskDim) and masked-scope re-binning (MaskedScopeBins below),
// mirroring MainWindow._apply_qualifiers's `channel_mask & hsl_mask`.

// Standard cylindrical HSL hue (degrees, 0-360), saturation and lightness (each
// 0-1) - the same formula colorsys.rgb_to_hls implements. Shared by the Skin
// Tone panel's HSL model and the Qualifier panel's HSL range below, so there is
// one HSL conversion in the app rather than two that could drift apart.
inline void HslHueSatLight(float p_R, float p_G, float p_B,
                           float& p_OutHue, float& p_OutSat, float& p_OutLight)
{
    const float maxc = std::max({ p_R, p_G, p_B });
    const float minc = std::min({ p_R, p_G, p_B });
    const float lightness = (maxc + minc) * 0.5f;
    const float delta = maxc - minc;

    p_OutLight = lightness;

    if (delta < 1e-6f) {
        p_OutSat = 0.0f;
        p_OutHue = 0.0f;
        return;
    }

    const float denom = std::max(1.0f - std::fabs(2.0f * lightness - 1.0f), 1e-6f);
    p_OutSat = delta / denom;

    float hue6;
    if (maxc == p_R)      hue6 = std::fmod((p_G - p_B) / delta, 6.0f);
    else if (maxc == p_G) hue6 = (p_B - p_R) / delta + 2.0f;
    else                  hue6 = (p_R - p_G) / delta + 4.0f;

    float hue = std::fmod(hue6 * 60.0f, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    p_OutHue = hue;
}

// The Histogram panel's own per-channel drag-select: one independent [lo, hi]
// range per plane (Y/R/G/B, kPlaneCount-indexed), ANDed together, and further
// ANDed across every open Histogram panel that has one active (see
// RebuildValueQualifier in main.cpp) - matching the old app's
// MainWindow.qualifiers list of HistogramPanel instances.
struct ChannelQualifier
{
    bool  active[kPlaneCount] = { false, false, false, false };
    float lo[kPlaneCount]     = { 0.0f, 0.0f, 0.0f, 0.0f };
    float hi[kPlaneCount]     = { 0.0f, 0.0f, 0.0f, 0.0f };

    bool AnyActive() const
    {
        for (bool a : active) if (a) return true;
        return false;
    }
};

// The dedicated Qualifier panel's Hue/Saturation/Luminance range - global state
// (not per-panel-instance), same reasoning as SpatialMask: it means the same
// selection to every panel that reads it, and two Qualifier panels open at once
// just both show/edit the one shared range.
struct HslQualifier
{
    bool  enabled = false;
    bool  invert  = false;
    float hueMin = 0.0f, hueMax = 360.0f;
    float satMin = 0.0f, satMax = 1.0f;
    float lumaMin = 0.0f, lumaMax = 1.0f;
};

struct ValueQualifier
{
    ChannelQualifier channel;
    HslQualifier     hsl;

    bool AnyActive() const { return channel.AnyActive() || hsl.enabled; }
};

inline bool ValueQualifierEquals(const ValueQualifier& p_A, const ValueQualifier& p_B)
{
    if (p_A.hsl.enabled != p_B.hsl.enabled || p_A.hsl.invert != p_B.hsl.invert
        || p_A.hsl.hueMin != p_B.hsl.hueMin || p_A.hsl.hueMax != p_B.hsl.hueMax
        || p_A.hsl.satMin != p_B.hsl.satMin || p_A.hsl.satMax != p_B.hsl.satMax
        || p_A.hsl.lumaMin != p_B.hsl.lumaMin || p_A.hsl.lumaMax != p_B.hsl.lumaMax)
        return false;
    for (uint32_t p = 0; p < kPlaneCount; ++p)
    {
        if (p_A.channel.active[p] != p_B.channel.active[p]) return false;
        if (p_A.channel.active[p] && (p_A.channel.lo[p] != p_B.channel.lo[p]
                                    || p_A.channel.hi[p] != p_B.channel.hi[p]))
            return false;
    }
    return true;
}

// true = pixel is selected (kept) by the active qualifiers - r/g/b are 0..1.
// An inactive ChannelQualifier or a disabled HslQualifier each contribute no
// restriction, exactly like old app's channel_mask/hsl_mask being None.
inline bool ValueQualifierPasses(const ValueQualifier& p_Q, float p_R, float p_G, float p_B,
                                 const float p_LumaCoeff[3])
{
    if (p_Q.channel.AnyActive())
    {
        const float values[kPlaneCount] = {
            p_R, p_G, p_B,
            p_LumaCoeff[0] * p_R + p_LumaCoeff[1] * p_G + p_LumaCoeff[2] * p_B
        }; // indices line up with kPlaneR=0, kPlaneG=1, kPlaneB=2, kPlaneY=3
        for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
        {
            if (!p_Q.channel.active[plane]) continue;
            if (values[plane] < p_Q.channel.lo[plane] || values[plane] > p_Q.channel.hi[plane])
                return false;
        }
    }
    if (p_Q.hsl.enabled)
    {
        float hue, sat, luma;
        HslHueSatLight(p_R, p_G, p_B, hue, sat, luma);
        bool inRange = hue >= p_Q.hsl.hueMin && hue <= p_Q.hsl.hueMax
                    && sat >= p_Q.hsl.satMin && sat <= p_Q.hsl.satMax
                    && luma >= p_Q.hsl.lumaMin && luma <= p_Q.hsl.lumaMax;
        if (p_Q.hsl.invert) inRange = !inRange;
        if (!inRange) return false;
    }
    return true;
}

// Combined spatial-mask weight (continuous, for Gradient) and value-qualifier
// pass/fail (boolean) multiplied into one 0..1 weight - mirrors the old app's
// `blend *= mask; blend *= weight` in _apply_qualifier_dim. r/g/b are 0..1.
// p_Spatial/p_Qualifier null (or inactive) contribute no restriction.
inline float CombinedMaskWeight(const SpatialMask* p_Spatial, float p_Fx, float p_Fy, float p_Aspect,
                                const ValueQualifier* p_Qualifier,
                                float p_R, float p_G, float p_B, const float p_LumaCoeff[3])
{
    float weight = (p_Spatial && p_Spatial->hasGeometry)
        ? SpatialMaskWeightAt(*p_Spatial, p_Fx, p_Fy, p_Aspect) : 1.0f;
    if (p_Qualifier && p_Qualifier->AnyActive()
        && !ValueQualifierPasses(*p_Qualifier, p_R, p_G, p_B, p_LumaCoeff))
    {
        weight = 0.0f;
    }
    return weight;
}

// Source (dimmed preview) - only needed while a mask or qualifier is active;
// otherwise the panel uploads the tap's own preview bytes directly with no CPU
// pass.
void BuildDimmedPreviewImage(const ScopeFrame& p_Frame,
                             const SpatialMask& p_Mask,
                             Image& p_Out,
                             const ValueQualifier* p_Qualifier = nullptr);

// --- Masked scope re-binning -------------------------------------------------
//
// The spatial mask only ever dimmed the preview-tier panels - it had no effect
// on Waveform/Histogram/Vectorscope/White Balance, because those come from the
// tap's own pre-binned wire-format arrays (ScopeFrame::waveform etc.), computed
// over the *full-resolution* frame before the app ever sees it. There is no way
// to restrict bins the tap already summed.
//
// The fix is the same one the old Python app used (see PORTING.md): re-bin from
// scratch, app-side, over the pixels the app actually has - the downscaled
// preview - counting only the pixels the mask keeps. That makes a masked
// readout exactly as precise as the preview image is (no super-black/white
// headroom past what ToDisplay8 already clipped), which is a real step down
// from the tap's own full-precision bins - the same trade the old app made,
// for the same reason: the alternative is asking the plugin to re-bin against
// a mask, which would mean changing the wire format and the plugin most users
// are not rebuilding, for a feature the CPU path already delivers today.
struct MaskedScopeBins
{
    std::vector<uint32_t> waveform;    // kWaveformCells
    std::vector<uint32_t> histogram;   // kHistogramCells
    std::vector<uint32_t> vectorscope; // kVectorscopeTotalCells
    std::vector<uint32_t> twinPeaks;   // kTwinPeaksTotalCells

    // The Waveform's own normalisation reference (see SamplesPerColumn) has no
    // meaning here: it is derived from the tap's full-resolution pixel count,
    // but these bins come from however many *preview* pixels the mask kept,
    // usually a much smaller and differently-shaped number. Average masked
    // pixels per column, so a masked trace is normalised against its own
    // sample density rather than the unmasked frame's.
    float referenceSamples = 1.0f;

    // True when the mask kept zero pixels (a Rectangle/Ellipse mask entirely
    // outside the frame, or an inverted one that swallowed the whole image) -
    // every bin above is legitimately zero, not just unpopulated yet, and a
    // panel should say so rather than draw an empty scope that looks broken.
    bool empty = true;

    // Enhanced Render's own masked equivalent - raw per-scanline samples, not
    // bins, same layout and meaning as ScopeFrame::waveformTrace
    // (WaveformTraceIndex, kWaveformTraceCells floats). Both Waveform's and
    // Vectorscope's Enhanced Render read the same trace array, so one shared
    // buffer here covers both.
    std::vector<float> waveformTrace;
};

// The reduction shared by BuildMaskedScopeBins below and by the Compare
// panel's still analysis (see StillImage.h) - the actual per-pixel binning
// loop, over any RGB8 buffer rather than specifically a ScopeFrame's preview.
// p_Mask null means "every pixel counts" - not "a mask that happens to be
// off": callers that want the unmasked case pass nullptr rather than a
// default-constructed SpatialMask, so there is exactly one way to ask for
// that and no way to get a different result by accident. Threaded the same
// way BuildFalseColorImage etc. are: one partial bin set per row band,
// merged at the end - contended atomics across every pixel would cost more
// than the merge does, the same reasoning ScopeCore.cpp's own Reduce() uses
// for the tap's full-resolution pass. Does not fill p_Out.waveformTrace -
// callers that want Enhanced Render's masked trace call
// BuildMaskedWaveformTrace themselves (a still has no per-scanline trace
// concept to speak of, only BuildMaskedScopeBins below needs it).
void BuildScopeBinsFromRGB(const uint8_t* p_Rgb, int p_Width, int p_Height,
                           const float p_LumaCoeff[3], const SpatialMask* p_Mask,
                           MaskedScopeBins& p_Out, const ValueQualifier* p_Qualifier = nullptr);

// A qualifier alone (no spatial mask) is enough reason to re-bin - unlike the
// spatial mask's own hasGeometry check, callers must pass whichever of the two
// (or both) is actually active.
void BuildMaskedScopeBins(const ScopeFrame& p_Frame, const SpatialMask& p_Mask, MaskedScopeBins& p_Out,
                          const ValueQualifier* p_Qualifier = nullptr);

// Enhanced Render's masked equivalent: the same per-scanline sampling
// BuildWaveformTrace uses (see ScopeReader.h's comment on
// ScopeFrame::waveformTrace for why raw samples, not bins, are needed for a
// line trace), but read from the preview pixels the app actually has and
// restricted to the mask - the same "keep entirely or drop entirely" rule
// BuildMaskedScopeBins uses. A column bucket the mask excluded entirely
// writes zero (black) rather than carrying the last real value forward, so a
// masked line trace goes dark where the mask excludes it instead of
// fabricating a value there.
void BuildMaskedWaveformTrace(const ScopeFrame& p_Frame, const SpatialMask& p_Mask, std::vector<float>& p_Out,
                              const ValueQualifier* p_Qualifier = nullptr);

// --- Enhanced Render --------------------------------------------------------
//
// A second, independent representation of the same frame: genuine per-scanline
// samples rather than counts, drawn as connected line strips. The binned waveform
// can answer "how many pixels landed at this level" but not "which pixels shared a
// source row", and connecting two points only means something if they came from
// one continuous scanline.

// False Color
struct FalseColorBand {
    float lo, hi;
    Rgb8 color;
};
void BuildFalseColorImage(const ScopeFrame& p_Frame,
                          const FalseColorBand* p_Bands,
                          int p_BandCount,
                          Image& p_Out,
                          const SpatialMask* p_Mask = nullptr,
                          const ValueQualifier* p_Qualifier = nullptr);

// Skin Tone
//
// Two ways to derive a per-pixel hue/saturation/luma triple (SkinToneModel):
//  - YCbCr: the same Cb/Cr derivation the vectorscope and false color panel
//    use - hue as the Cb/Cr angle, saturation as Cb/Cr magnitude normalised
//    against kSkinSaturationNorm, luma as the working-space's own weighted
//    RGB sum. The original, still-default, behaviour.
//  - HSL: the classic cylindrical RGB model, independent of any
//    working-space luma weights. Its own reference angle
//    (kSkinToneAngleHslDeg) is unrelated to kSkinToneAngleDeg - that one is a
//    Cb/Cr angle and means nothing on HSL's own hue wheel.
enum class SkinToneModel { YCbCr, Hsl };
enum class SkinToneColorMode { Default, Solid, Gradient };

constexpr float kSkinToneAngleHslDeg  = 25.0f;
constexpr float kSkinMinSaturation    = 0.03f;
constexpr float kSkinMinLuma          = 0.05f;
constexpr float kSkinMaxLuma          = 0.95f;
constexpr float kSkinSaturationNorm   = 0.6f;

struct SkinToneParams
{
    SkinToneModel     model     = SkinToneModel::YCbCr;
    SkinToneColorMode colorMode = SkinToneColorMode::Default;
    float             tolerance = 22.0f;

    bool  limitsEnabled = false;
    float hueMin = 0.0f, hueMax = 0.0f;
    float satMin = kSkinMinSaturation, satMax = 1.0f;
    float lumaMin = kSkinMinLuma, lumaMax = kSkinMaxLuma;

    bool greyNonSkin = true;
    Rgb8 solidColor{ 60, 225, 110 };
    Rgb8 gradientColorA{ 60, 225, 110 };
    Rgb8 gradientColorB{ 255, 70, 200 };
};

// Field-wise equality, the same job SpatialMaskEquals does: lets a panel skip
// rebuilding an image that would come out identical. Written out rather than
// memcmp'd because the struct has padding, which memcmp would compare too.
inline bool SkinToneParamsEquals(const SkinToneParams& p_A, const SkinToneParams& p_B)
{
    auto rgbEq = [](const Rgb8& a, const Rgb8& b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
    return p_A.model == p_B.model && p_A.colorMode == p_B.colorMode
        && p_A.tolerance == p_B.tolerance && p_A.limitsEnabled == p_B.limitsEnabled
        && p_A.hueMin == p_B.hueMin && p_A.hueMax == p_B.hueMax
        && p_A.satMin == p_B.satMin && p_A.satMax == p_B.satMax
        && p_A.lumaMin == p_B.lumaMin && p_A.lumaMax == p_B.lumaMax
        && p_A.greyNonSkin == p_B.greyNonSkin
        && rgbEq(p_A.solidColor, p_B.solidColor)
        && rgbEq(p_A.gradientColorA, p_B.gradientColorA)
        && rgbEq(p_A.gradientColorB, p_B.gradientColorB);
}

void BuildSkinToneImage(const ScopeFrame& p_Frame,
                        const SkinToneParams& p_Params,
                        Image& p_Out,
                        const SpatialMask* p_Mask = nullptr,
                        const ValueQualifier* p_Qualifier = nullptr);

// Noise
//
// Deliberately not mask-dimmed, matching the old app: noise is a deviation
// measurement, and visually masking it would suggest the mask changes what's
// being measured rather than just what's shown.
void BuildNoiseImage(const ScopeFrame& p_Frame,
                     int p_Method, // 0 = Highpass, 1 = Channel, 2 = Differenced
                     int p_Channel,
                     float p_Gain,
                     Image& p_Out);

// Focus Peaking: the source image (or its luma, greyed out) with a flat colour
// over every pixel whose local luma gradient clears p_Threshold - the same
// "in-focus edges glow" convention every peaking overlay uses.
void BuildFocusPeakingImage(const ScopeFrame& p_Frame,
                            float p_Threshold,
                            bool p_GrayscaleBg,
                            Rgb8 p_PeakColor,
                            Image& p_Out);

// Banding Analyzer: the image's own horizontal+vertical gradient magnitude,
// gained up. A smoothly graded region that should show no banding reads as
// near-black; a step in the gradient - true banding, or a compression
// artifact that looks like it - lights up in proportion to how sharp it is.
void BuildBandingAnalyzerImage(const ScopeFrame& p_Frame, float p_Gain, Image& p_Out);

// A/B Difference: |live - reference| * gain, or the live image dimmed to 20%
// when there is no reference yet (or it no longer matches the live frame's
// size) - a visible "nothing to compare against" state rather than a blank
// panel that looks broken. p_RefPixels/p_RefWidth/p_RefHeight are a captured
// preview frame (RGB8, same layout as ScopeFrame::preview); pass a null
// pointer for "no reference captured".
void BuildDifferenceImage(const ScopeFrame& p_Frame,
                          const uint8_t* p_RefPixels, int p_RefWidth, int p_RefHeight,
                          float p_Gain,
                          Image& p_Out);

// Grain Analyzer: a "grain size" energy profile, deliberately not the old
// app's 2D FFT. That version plotted true spatial frequency, but was a
// second-hand read even for its own author - answering "how big is the
// grain" by way of "how much energy is at each frequency" is an extra
// translation step, and only the 2D FFT dependency to get there. This
// answers the size question directly: p_Out[i] is how much the image's own
// luma changes when detail around kGrainRadii[i+1] pixels wide is smoothed
// away (RMS of blur(kGrainRadii[i]) - blur(kGrainRadii[i+1]), band 0 using
// the unblurred image as its low end) - a "how much grain lives at roughly
// this size" curve built from a run of box blurs, no transform. Nine bands
// on a gentler-than-doubling radius progression, not the original six on a
// doubling one, for a visibly less blocky curve.
//
// Raw RMS, in the same 0..1 luma units the frame itself uses - NOT
// normalised to the strongest band. A per-frame normalise would force every
// clip's peak band to read 1.0 regardless of how much grain is actually
// there, which is exactly why two clips at very different grain levels used
// to plot as near-identical shapes: only the *shape* survived normalising,
// never the *amount*. The caller applies its own Gain to bring this into a
// visible range - see GrainAnalyzerState's own comment for why that slider
// now does something.
constexpr int kGrainBandCount = 9;
constexpr int kGrainRadii[kGrainBandCount + 1] = { 0, 1, 2, 3, 4, 6, 8, 11, 16, 24 };

void BuildGrainAnalyzerProfile(const ScopeFrame& p_Frame, float p_Out[kGrainBandCount]);

// Moving-average blur along the *column* axis of a (rows, columns, planes) array.
//
// The axis matters and is not the last one. Blurring the last axis instead
// averages R, G, B and Y into each other - which is a real bug the old app shipped
// and fixed (see ERRORS.md, "Signal Pre-filter blurred across channels, not across
// columns"). It changes the output, so it looks like it works.
//
// The kernel rounds to the nearest odd width >= 1; anything under 1.5 is a no-op.
void BoxBlurColumns(std::vector<float>& p_Values, int p_Rows, int p_Columns,
                    int p_Planes, float p_Kernel);

// Survives across frames, because Smooth Trace is a temporal blend and needs the
// previous frame to blend against.
struct EnhancedTraceState
{
    std::vector<float> prevTrace;
    bool               hasPrev = false;

    void Reset() { prevTrace.clear(); hasPrev = false; }
};

// Signal Pre-filter, then Smooth Trace, then Low-Pass - in that order, which is
// the order the old app uses and is not arbitrary: the pre-filter is a bandwidth
// limit on the raw signal and has to run before anything else touches it, and the
// temporal blend has to see the same filtering both frames saw or it blends two
// differently-measured traces.
//
// Returns false when the frame carries no trace data (Enhanced Render was switched
// on but the reader has not been asked for the rows yet, which is true for exactly
// one frame after the toggle).
//
// p_TraceOverride substitutes a masked trace (MaskedScopeBins::waveformTrace)
// for p_Frame.waveformTrace - null (the default) uses the frame's own, exactly
// as before.
bool PrepareEnhancedTrace(const ScopeFrame& p_Frame,
                          bool p_SignalPrefilter,
                          bool p_SmoothTrace,
                          float p_LowPassKernel,
                          EnhancedTraceState& p_State,
                          std::vector<float>& p_Out,
                          const std::vector<float>* p_TraceOverride = nullptr);

// ---------------------------------------------------------------------------
// Color Cube
// ---------------------------------------------------------------------------
//
// sRGB -> CIE XYZ (D65) -> CIE L*a*b*, the standard formulas - ported from the
// old app's scope_format.py (srgb_to_xyz/xyz_to_lab), which is also still the
// only place in either tree this conversion exists; the still-unported
// Chromaticity panel will want the same XYZ step.

// r,g,b in 0..1 (sRGB, gamma-encoded). x,y,z come out roughly 0..1 (D65 white
// = 0.95047, 1.0, 1.08883).
void SrgbToXyz(float p_R, float p_G, float p_B, float& p_X, float& p_Y, float& p_Z);

// x,y,z as SrgbToXyz produces. L comes out 0..100, a/b roughly -128..127.
void XyzToLab(float p_X, float p_Y, float p_Z, float& p_L, float& p_A, float& p_B);

enum class ColorCubeSpace { RGB, XYZ, Lab, Count };

inline const char* ColorCubeSpaceName(ColorCubeSpace p_Space)
{
    switch (p_Space)
    {
        case ColorCubeSpace::RGB: return "RGB";
        case ColorCubeSpace::XYZ: return "XYZ";
        case ColorCubeSpace::Lab: return "Lab";
        default:                  return "?";
    }
}

// A sample's position (each axis normalised to roughly -1..1, matching
// whatever COLOR_CUBE_AXIS_RANGES-equivalent the space needs so RGB/XYZ/Lab
// all share one camera scale) plus the sRGB colour it's actually drawn in.
struct ColorCubePoint
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Rgb8  colour{};
};

// Capped, not exhaustive: rotating/projecting a few thousand points stays
// cheap every frame, and a cloud already reads as a cloud well before you'd
// need every pixel - same reasoning and same cap the old app's
// COLOR_CUBE_MAX_POINTS used.
constexpr int kColorCubeMaxPoints = 6000;

// Samples up to kColorCubeMaxPoints points from the frame's own preview
// pixels (already a downscaled viewfinder, not full source resolution) and
// positions each in the given colour space. The subsample is seeded from
// p_Frame.frameIndex specifically, not a persistent RNG - so repainting the
// same video frame for any reason (window focus, an unrelated setting
// changing) reproduces the identical point set instead of visibly
// re-jittering it, the same fix the old app made after finding the bug live.
// p_Mask/p_Qualifier (either or both may be null) drop samples outside the
// selection outright rather than dimming them, the same way
// BuildChromaticityTrace invalidates a masked sample - a point cloud has no
// equivalent of a dimmed pixel, and a cloud that still shows the excluded
// colours in grey would defeat the point of isolating a region. The cloud
// simply gets sparser as the selection narrows.
void BuildColorCubePoints(const ScopeFrame& p_Frame, ColorCubeSpace p_Space,
                          std::vector<ColorCubePoint>& p_Out,
                          const SpatialMask* p_Mask = nullptr,
                          const ValueQualifier* p_Qualifier = nullptr);

// --- Chromaticity (CIE 1931 x,y) --------------------------------------------
//
// Entirely app-side, derived from the preview the same way Color Cube is -
// there was never a wire-format reason this needed the OFX tap involved.
// Ported from the old app's ChromaticityPanel/scope_format.py; base (binned
// density) rendering only - Enhanced Render's exact per-pixel beam trace is
// deliberately not ported yet, same "needs its own decision" scoping the old
// app's own NEXT_STEPS_ENHANCED_RENDER.md left open for this panel (xy is a
// genuine division, so averaging RGB first the way waveform_trace does would
// reintroduce the same error Sat-vs-Luma measured for saturation).

constexpr float kChromaticityPlotXLo = 0.0f, kChromaticityPlotXHi = 0.8f;
constexpr float kChromaticityPlotYLo = 0.0f, kChromaticityPlotYHi = 0.9f;
constexpr int   kChromaticityGridSize = 480;   // the old app's own resolution bump (was 300)

// Excludes near-black pixels, where XYZ is dominated by sensor/quantisation
// noise and (x, y) is not meaningfully defined - same idea as the vectorscope/
// skin-tone panels' own saturation/luma gates, just against luminance directly.
constexpr float kChromaticityMinLuma = 0.02f;

struct Vec2 { float x = 0.0f, y = 0.0f; };

// r,g,b in 0..1 (sRGB). Via the same SrgbToXyz() used elsewhere - X/(X+Y+Z),
// Y/(X+Y+Z) - so this and Color Cube's XYZ point never derive chromaticity
// two different ways.
inline void ChromaticityXyOfRgb(float p_R, float p_G, float p_B, float& p_OutX, float& p_OutY)
{
    float x, y, z;
    SrgbToXyz(p_R, p_G, p_B, x, y, z);
    const float total = (x + y + z > 1e-9f) ? (x + y + z) : 1.0f;   // avoid 0/0 for pure black
    p_OutX = x / total;
    p_OutY = y / total;
}

// The CIE 1931 spectral locus (the outer horseshoe - every point is one
// wavelength of pure spectral light), from the CIE's own 1931 2-degree
// standard observer colour-matching functions at 5nm steps (CIE 018:2019,
// Table 6) - fetched from the source rather than approximated, same as the
// old app. Closes via the line of purples (last point back to the first) -
// caller's job, like the old app's own draw loop.
const std::vector<Vec2>& ChromaticitySpectralLocus();

// The Planckian (blackbody) locus across 1667K-25000K - Kim et al.'s CIE-
// recommended cubic-spline approximation, same coefficients as the old app.
// An open curve (a temperature trajectory, not a boundary) - do not close it.
const std::vector<Vec2>& ChromaticityBlackbodyLocus();

struct GamutTriangle
{
    const char* name;
    Vec2        primaries[3];
    Vec2        white;
    Rgb8        tint;
};

// Rec.709, P3-D65 and Rec.2020 primaries + shared D65 white, CIE xy - the
// industry-reference gamuts for "is this deliverable". Rec.2020's primaries
// are dramatically wider than Rec.709's despite sharing the same white,
// which is the entire point of drawing them together.
extern const GamutTriangle kGamutTriangles[3];

// Fraction of p_Xy that falls outside the triangle p_Primaries (barycentric
// sign test) - the quantitative "is this deliverable" readout that goes with
// the visual scatter/triangle overlay.
float FractionOutsideGamut(const std::vector<Vec2>& p_Xy, const Vec2 p_Primaries[3]);

struct ChromaticityAnalysis
{
    // kChromaticityGridSize x kChromaticityGridSize, row-major, row 0 = lowest
    // y (matches Image/ImGui's own top-left origin once drawn - flipped the
    // same way the vectorscope flips Cr).
    std::vector<uint32_t> counts;
    float fractionOutside[3] = { 0.0f, 0.0f, 0.0f };   // indices match kGamutTriangles
    bool  empty = true;
};

// Subsampled the same way the qualifier mask is (a ~200,000-pixel budget,
// stride = ceil(sqrt(total/budget))) - a density histogram and gamut-coverage
// percentage don't need every pixel. Not spatial-mask/qualifier-aware,
// matching the old app's own ChromaticityPanel._ensure_analysis exactly (it
// never read frame.qualifier_mask/spatial_mask_weight either) - a deliberate
// upstream omission carried over, not a new gap.
void BuildChromaticityAnalysis(const ScopeFrame& p_Frame, ChromaticityAnalysis& p_Out);

// Colorize: an approximate display colour per grid cell (via the inverse of
// SrgbToXyz's own matrix, renormalised to the brightest channel so every hue
// reaches full intensity) - computed once and cached, since it depends on
// nothing but the fixed grid geometry.
void BuildChromaticityImage(const ChromaticityAnalysis& p_Analysis, float p_Gain, bool p_Colorize,
                            Image& p_Out);

// --- Chromaticity Enhanced Render --------------------------------------------
//
// Exact per-pixel chromaticity, not BuildChromaticityAnalysis's binned
// density: CIE xy is a genuine X/(X+Y+Z) division, so deriving it from
// waveform_trace's already column-averaged RGB (the way Vectorscope/White
// Balance's Enhanced Render do, since Cb/Cr and their diamond coordinates
// are linear in RGB and averaging commutes with a linear transform) would
// reintroduce real error - the same reason Sat-vs-Luma reads preview_rgb
// directly instead. Old app precedent: ChromaticityPanel._samples/
// _trace_lines/_draw_enhanced, same technique as SatVsLumaPanel.
struct ChromaticitySample
{
    float x = 0.0f, y = 0.0f;
    Rgb8  rgb{};
    bool  valid = false;   // false: too dark (kChromaticityMinLuma) or masked out
};

// Fills p_Out with kWaveformTraceRows x kWaveformColumns samples - evenly
// strided, genuinely distinct preview pixels (never averaged, unlike
// waveform_trace), same rows/columns any per-scanline line trace in this app
// uses. p_Mask/p_Qualifier (either or both may be null) restrict `valid` the
// same way they restrict a masked scope's re-bin - a masked-out sample
// breaks the trace at that point rather than being drawn through.
void BuildChromaticityTrace(const ScopeFrame& p_Frame, const SpatialMask* p_Mask,
                            const ValueQualifier* p_Qualifier,
                            std::vector<ChromaticitySample>& p_Out);

// --- Sat vs Luma / Sat vs Hue -----------------------------------------------
//
// Both DaVinci Resolve "Curves"-style: a fixed axis-reference gradient (a
// black->white luma ramp, or a hue wheel) with a plain population histogram
// drawn over it. Sat vs Luma reuses the app's own existing full-precision
// Y-plane histogram (ScopeFrame::histogram/App::maskedScope.histogram) - no
// new per-pixel pass needed, since that data already exists every frame.
// Sat vs Hue needs its own pass (hue has no wire-format equivalent) - see
// BuildHueHistogramAnalysis below.
//
// An earlier version of both was a 2D density plot of saturation against
// luma/hue (real per-pixel saturation on the Y axis) - replaced after Trevor
// asked for Resolve's actual look instead, which plots population only.

// HSV-style saturation/hue (max-min chroma) - a third, distinct hue/sat
// formula in this app alongside HslHueSatLight (cylindrical HSL) and Skin
// Tone's own Cb/Cr-angle model, ported verbatim from the old app's
// rgb_to_sat_luma_hue for these two panels specifically. Luma is the
// working-space's own weighted RGB sum (ScopeFrame::lumaCoeff), not HSV's own
// max-channel "value" - so it agrees with every other scope's luma rather
// than being a different number that happens to share the name. Sat vs Luma
// no longer calls this (it reads the existing Y-plane histogram instead);
// Sat vs Hue still does, for its hue value and its near-neutral gate.
inline void RgbToSatLumaHue(float p_R, float p_G, float p_B, const float p_LumaCoeff[3],
                            float& p_OutSat, float& p_OutLuma, float& p_OutHueDeg)
{
    p_OutLuma = p_LumaCoeff[0] * p_R + p_LumaCoeff[1] * p_G + p_LumaCoeff[2] * p_B;

    const float maxc = std::max({ p_R, p_G, p_B });
    const float minc = std::min({ p_R, p_G, p_B });
    const float chroma = maxc - minc;
    p_OutSat = (maxc > 1e-6f) ? (chroma / maxc) : 0.0f;

    float hue = std::atan2(1.7320508075688772f * (p_G - p_B), 2.0f * p_R - p_G - p_B)
              * (180.0f / 3.14159265358979323846f);
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    p_OutHueDeg = hue;
}

// --- Sat vs Hue, DaVinci Resolve "Curves" style -----------------------------
//
// A plain 1D hue population histogram (how much of the image sits at each
// hue, full stop - saturation is only a gate, not a plotted quantity),
// drawn with the same line+trapezoid-fill technique DrawHistogramLane
// already uses, over a hue-wheel background - matching the look of
// Resolve's own Hue vs Sat/Hue vs Hue curve panels.
constexpr int kHueHistogramBins = 360;   // one per degree

struct HueHistogramAnalysis
{
    std::vector<uint32_t> counts;   // kHueHistogramBins
    uint32_t peak = 0;
    bool     empty = true;
};

// Excludes near-neutral pixels (sat < 0.02) the same way Chromaticity
// excludes near-black ones: a pixel with essentially no saturation has no
// meaningfully-defined hue, and RgbToSatLumaHue's atan2 still returns
// *some* angle for it (0 for an exactly neutral pixel) - counting those
// would bias the red bin with what is really just noise.
void BuildHueHistogramAnalysis(const ScopeFrame& p_Frame, const SpatialMask* p_Mask,
                               const ValueQualifier* p_Qualifier, HueHistogramAnalysis& p_Out);

// A fixed hue-wheel gradient strip (p_Width x 2, RGBA), dimmed to about half
// brightness so white graticule lines/labels/the histogram's own outline
// stay legible over every hue - independent of any frame data, so callers
// build this once and cache the resulting texture rather than rebuilding it
// every frame the way a real data image would need to be.
void BuildHueGradientImage(int p_Width, Image& p_Out);

// Sat vs Luma, the same DaVinci Resolve "Curves" style as Sat vs Hue: a
// black->white luma gradient strip (p_Width x 2, RGBA), full range, not
// dimmed - unlike the hue wheel, this background reaches genuine white at
// its own right edge, so a two-tone (dark+light) stroke is used for the
// overlaid histogram/graticule instead of dimming the background, or the
// bright end would blow out any plain white line drawn over it. Also
// independent of frame data - build once, cache the texture.
void BuildLumaGradientImage(int p_Width, Image& p_Out);

} // namespace scopedeck
