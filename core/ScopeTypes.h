// Wire format shared between the OFX tap (writer) and the Scope Deck app (reader).
//
// Bump kVersion on any layout change; the reader refuses a version it does not
// recognise rather than misreading bytes, and the app names both numbers.
//
// There is no Python mirror of this format any more: app/scope_format.py exists
// only in the old Python tree, which is reference material and not a build
// dependency of anything here.
//
// The tap reduces each frame in-process and publishes only bin arrays, so what
// crosses this boundary is ~2 MB regardless of whether the timeline is HD or UHD.

#pragma once

#include <cstdint>

namespace scopedeck
{

constexpr uint32_t kMagic   = 0x44504353;   // 'SCPD'
constexpr uint32_t kVersion = 10;

// The block every Resolve tap publishes to and the app reads by default.
// Undecorated: the platform layer adds Local\ on Windows and a leading slash on
// POSIX. Defined once here rather than in both ScopeReader.cpp and
// ScopeShm.cpp, which made the reader and writer libraries impossible to link
// into one program - and a test of the publish protocol needs both.
inline constexpr const char* kShmName = "ScopeDeck.v1";

// The block ScopeTransmit (Premiere Pro) publishes to - the same layout as
// kShmName's, just a different name, so the app can offer Resolve and
// Premiere as separate inputs and neither overwrites the other when both are
// open. Undecorated, like kShmName.
inline constexpr const char* kShmNamePremiere = "ScopeDeck.Premiere.v1";

// Which space the tap believes it is looking at. Everything except luma can be
// reinterpreted by the reader after the fact, but luma is a per-pixel combination
// of R, G and B, so it must be committed to at bin time - which is the only reason
// the tap needs to know the color space at all.
//
// Coefficients are the Y row of each space's RGB-to-XYZ matrix, derived from
// each space's own published primaries and white point - never guessed. Where
// a known-good reference matrix exists this was cross-checked against it:
// Rec.709/601/2020 match the standard broadcast constants to 4 decimal places,
// and ACEScc/ACEScct's derived weights match the official ACES AP1-to-XYZ
// matrix (SMPTE/Academy S-2014-004) to 7 decimal places. DaVinci Wide Gamut is
// derived from Blackmagic's published primaries (R 0.8000/0.3130,
// G 0.1682/0.9877, B 0.0790/-0.1155, D65 white) and cross-checked against a
// community-published DWG-to-XYZ matrix to 4 decimal places - not an official
// Blackmagic-published Y row, but no longer a guess either.
enum ColorSpace : uint32_t
{
    kSpaceRec709  = 0,   // also sRGB - same primaries
    kSpaceRec601  = 1,
    kSpaceRec2020 = 2,   // also Rec.2100 for HDR
    kSpaceP3D65   = 3,
    kSpaceACEScc  = 4,   // shares AP1 primaries/weights with ACEScct - only the
    kSpaceACEScct = 5,   // log encoding curve differs, which luma doesn't see
    kSpaceDaVinciWideGamut = 6,
    kSpaceCustom  = 7,
    kSpaceCount   = 8
};

struct LumaWeights { float r, g, b; };

inline LumaWeights LumaWeightsFor(uint32_t p_Space)
{
    switch (p_Space)
    {
        case kSpaceRec601:  return { 0.2990f, 0.5870f, 0.1140f };
        case kSpaceRec2020: return { 0.2627f, 0.6780f, 0.0593f };
        // Derived from P3 primaries with a D65 white point.
        case kSpaceP3D65:   return { 0.2290f, 0.6917f, 0.0793f };
        // AP1 primaries, ACES (D60-based) white point. ACEScc and ACEScct
        // share this - see the enum comment above.
        case kSpaceACEScc:
        case kSpaceACEScct: return { 0.2722f, 0.6741f, 0.0537f };
        // Blackmagic's published DaVinci Wide Gamut primaries, D65 white.
        // Negative Kb is expected: DWG's blue primary sits outside the visible
        // spectrum locus (y < 0), same as several other wide-gamut spaces.
        case kSpaceDaVinciWideGamut: return { 0.2741f, 0.8736f, -0.1477f };
        case kSpaceRec709:
        default:            return { 0.2126f, 0.7152f, 0.0722f };
    }
}

inline const char* ColorSpaceName(uint32_t p_Space)
{
    switch (p_Space)
    {
        case kSpaceRec601:  return "Rec.601";
        case kSpaceRec2020: return "Rec.2020";
        case kSpaceP3D65:   return "P3-D65";
        case kSpaceACEScc:  return "ACEScc";
        case kSpaceACEScct: return "ACEScct";
        case kSpaceDaVinciWideGamut: return "DaVinci Wide Gamut";
        case kSpaceCustom:  return "Custom";
        case kSpaceRec709:
        default:            return "Rec.709";
    }
}

// Waveform resolution. 512 columns is finer than the eye resolves on a typical
// scope panel while keeping a plane at 512 KB; 256 levels matches 8-bit display
// granularity for the vertical axis.
constexpr uint32_t kWaveformColumns = 512;
constexpr uint32_t kWaveformLevels  = 256;
constexpr uint32_t kHistogramBins   = 256;

// Raw scanline samples for line-strip rendering ("Enhanced Render" - see
// README), a second, independent representation of the same frame alongside
// the waveform's binned histogram above. The histogram answers "how many
// pixels landed at this level", which is exactly what a filled/dot trace
// needs but cannot answer "which pixels were on the same source row", which
// is what a *line* trace needs - connecting two points only makes sense if
// they came from one continuous scanline. So this carries genuine,
// unaggregated per-row samples instead of counts: kWaveformTraceRows source
// rows, chosen by even stride (not averaged - averaging would blend
// distinct rows into one and defeat the point), each downsampled only
// column-wise onto the waveform's own kWaveformColumns buckets. Stored as
// raw float values, not counts - this is the one bin-like array in the wire
// format that isn't actually bins.
//
// 256 rows was picked to land the payload at a clean 2 MB (256 * 512 * 4
// planes * 4 bytes) - the same order of magnitude as the waveform histogram
// itself, not a value tied to any particular vertical accuracy target: a
// line trace's vertical position comes straight from the raw float value,
// with no level-bin quantization at all, so it does not inherit the
// waveform histogram's 256-level cap the way the Classic/Modern/Phosphor
// dot-trace rendering does.
constexpr uint32_t kWaveformTraceRows = 256;

// Vectorscope: a 2D histogram of Cb (x) x Cr (y), one per luma band rather than
// one combined trace - so the reader can isolate shadows/mids/highlights the way
// Resolve's own vectorscope does, without re-deriving luma from anything (it is
// already computed once per pixel for the Y waveform plane; banding it here is
// free). 256x256 is the same resolution as a waveform level axis, at 256 KB.
constexpr uint32_t kVectorscopeSize = 256;

enum VectorscopeBand : uint32_t { kBandLow = 0, kBandMid = 1, kBandHigh = 2, kBandCount = 3 };

// Matches Resolve's own vectorscope panel defaults (Low Range / High Range).
// Fixed at bin time rather than adjustable per-frame: the bands are binned once
// in the tap, so a reader-side threshold could only pick among precomputed
// bands, not slide continuously - these two cuts are the ones actually exposed
// in the reference UI this was matched against.
constexpr float kVectorscopeLowMax  = 0.30f;   // luma < this -> Low
constexpr float kVectorscopeHighMin = 0.70f;   // luma >= this -> High, else Mid

inline uint32_t VectorscopeBandOf(float p_Luma)
{
    if (p_Luma < kVectorscopeLowMax)  return kBandLow;
    if (p_Luma >= kVectorscopeHighMin) return kBandHigh;
    return kBandMid;
}

// R, G, B and luma. Parade uses the first three; luma waveform uses the fourth.
enum Plane : uint32_t { kPlaneR = 0, kPlaneG = 1, kPlaneB = 2, kPlaneY = 3, kPlaneCount = 4 };

constexpr uint64_t kWaveformCells  = uint64_t(kWaveformColumns) * kWaveformLevels * kPlaneCount;
constexpr uint64_t kHistogramCells = uint64_t(kHistogramBins) * kPlaneCount;
constexpr uint64_t kWaveformTraceCells = uint64_t(kWaveformTraceRows) * kWaveformColumns * kPlaneCount;
constexpr uint64_t kVectorscopeCells      = uint64_t(kVectorscopeSize) * kVectorscopeSize;
constexpr uint64_t kVectorscopeTotalCells = kVectorscopeCells * kBandCount;

// Cb/Cr range to bin against. Unlike the waveform's bin range, this has not been
// checked numerically against Resolve's own vectorscope - see README "Open
// questions". +-0.75 gives headroom above the +-0.5 that legal-range 100% bars
// reach, symmetric so 0 chroma sits at the exact centre of the grid.
constexpr float kChromaRangeLow  = -0.75f;
constexpr float kChromaRangeHigh =  0.75f;

// White Balance ("Twin Peaks"): a double-diamond RGB-channel-relationship
// scope, matching the classic broadcast "Diamond display" - doubled so Green
// is the shared reference channel against both Blue (upper diamond) and Red
// (lower diamond) in one view, the way Nobe OmniScope's Twin Peaks scope
// presents it. For each pixel and each pairing (G,B) and (G,R):
//   level = (main + other) / 2   -> vertical axis, black at one tip, white at
//                                    the other, sharing kBinRangeLow/High -
//                                    the average of two in-range values stays
//                                    in range, so no separate range is needed.
//   diff  = (main - other) / 2   -> horizontal axis, 0 when the pair matches
// A pixel where every channel matches (true grey/white, no color cast) has
// diff == 0 at every level, so it draws a single vertical line straight
// through the centre of both diamonds - any horizontal spread is a channel
// imbalance, which is exactly what this scope exists to show. Legal 0-100%
// signal reaches only the inner (0.0, 1.0)/(-0.5, +0.5) sub-square of this
// grid, same as the vectorscope's targets sitting inside its own wider
// +-0.75 chroma range - the app draws the diamond graticule there, leaving
// headroom on all sides for real above-white/below-black pixels to plot
// visibly outside it rather than being invisibly clamped to the edge.
constexpr uint32_t kTwinPeaksSize = 256;

enum TwinPeaksDiamond : uint32_t { kDiamondGreenBlue = 0, kDiamondGreenRed = 1, kDiamondCount = 2 };

constexpr uint64_t kTwinPeaksCells      = uint64_t(kTwinPeaksSize) * kTwinPeaksSize;
constexpr uint64_t kTwinPeaksTotalCells = kTwinPeaksCells * kDiamondCount;

// Live video preview: an RGB8 copy of the frame passing through the tap, scaled
// by the tap's own "Preview Scale" parameter (100/75/50/25% of source
// resolution), so the app can show what it is looking at - and, later, draw
// masks against it. 8-bit rather than full precision: this is a viewfinder, not
// a place to take color measurements from.
//
// The canvas below is a fixed upper bound sized for UHD (3840x2160) source
// footage - a source larger than that (e.g. an 8K timeline) is clamped
// proportionally even at the 100% setting, since the shared-memory slot has to
// be a compile-time constant size. The actual image is fit inside this canvas
// preserving the source aspect ratio, with its real per-frame dimensions
// carried in SlotHeader (previewWidth/previewHeight).
constexpr uint32_t kPreviewCanvasWidth  = 3840;
constexpr uint32_t kPreviewCanvasHeight = 2160;
constexpr uint64_t kPreviewCanvasBytes  = uint64_t(kPreviewCanvasWidth) * kPreviewCanvasHeight * 3;

// Three slots let the writer publish while the reader is still walking the
// previous frame, without either blocking the other.
constexpr uint32_t kSlotCount = 3;

// Values outside [0,1] are real - the tap measured up to 1.0332 - so scopes need a
// declared working range rather than assuming video legal. Everything below this
// range lands in bin 0 and everything above in the last bin, with the true extremes
// carried separately in minRGB/maxRGB so nothing is silently lost.
constexpr float kBinRangeLow  = -0.080f;
constexpr float kBinRangeHigh =  1.080f;

// Largest |diff| two channels each within [kBinRangeLow, kBinRangeHigh] can
// produce is the full span between them, halved - derived, not a fresh guess.
constexpr float kTwinPeaksDiffRange     = (kBinRangeHigh - kBinRangeLow) * 0.5f;
constexpr float kTwinPeaksDiffRangeLow  = -kTwinPeaksDiffRange;
constexpr float kTwinPeaksDiffRangeHigh =  kTwinPeaksDiffRange;

#pragma pack(push, 8)

struct SlotHeader
{
    // Seqlock. Odd means a writer is mid-update; a reader that sees an odd value,
    // or a different value before and after copying, retries.
    uint64_t sequence;

    double   timelineTime;      // Resolve's frame time for this render
    uint64_t frameIndex;        // monotonic publish counter from this instance

    uint32_t width;             // source frame size in pixels
    uint32_t height;
    uint32_t instanceId;        // which tap published this (Resolve makes several)
    uint32_t pixelsSampled;     // pixels actually binned, after any subsampling

    float    minRGB[3];         // true extremes, unclamped by bin range
    float    maxRGB[3];

    double   binMillis;         // cost of the reduction, for the app to display

    uint32_t colorSpace;       // ColorSpace the luma plane was computed in
    float    lumaCoeff[3];      // the weights actually used, so the reader never
                                // has to re-derive them and cannot disagree

    // Centre pixel, untouched. Exists to be cross-checked against Resolve's own
    // color picker on the same frame: if they match, the tap is demonstrably
    // sampling the same point in the pipeline Resolve reports from, which is the
    // only way to confirm placement rather than infer it from value ranges.
    float    probeRGB[3];

    // Actual size of the live preview image within the fixed canvas (see
    // kPreviewCanvasWidth/Height) - 0x0 means video publish was off this frame,
    // so the reader should keep showing whatever it last had, or nothing.
    uint32_t previewWidth;
    uint32_t previewHeight;

    uint32_t reserved[4];
};

struct ShmHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t slotCount;

    uint64_t slotSize;          // bytes per slot, header included
    uint64_t writeCounter;      // total publishes; newest slot = (counter-1) % slotCount

    uint32_t waveformColumns;   // echoed so the reader can validate its assumptions
    uint32_t waveformLevels;
    uint32_t histogramBins;
    uint32_t planeCount;

    float    binRangeLow;
    float    binRangeHigh;

    uint32_t vectorscopeSize;
    float    chromaRangeLow;
    float    chromaRangeHigh;

    uint32_t vectorscopeBands;   // echoed kBandCount
    float    lowRangeMax;        // luma < this -> Low band
    float    highRangeMin;       // luma >= this -> High band

    uint32_t previewCanvasWidth;
    uint32_t previewCanvasHeight;

    uint32_t twinPeaksSize;         // echoed kTwinPeaksSize
    float    twinPeaksDiffRangeLow;
    float    twinPeaksDiffRangeHigh;

    uint32_t waveformTraceRows;     // echoed kWaveformTraceRows
};

#pragma pack(pop)

// Slot layout: SlotHeader, then the waveform cells, then the histogram cells,
// then the vectorscope cells (kBandCount bands back to back), then the twin
// peaks cells (kDiamondCount diamonds back to back), then the waveform trace
// samples (raw floats, not counts - see kWaveformTraceRows), then the
// preview image (RGB8, row-major, previewWidth*previewHeight*3 bytes
// actually meaningful within the fixed-size canvas region reserved here).
constexpr uint64_t kWaveformOffset     = sizeof(SlotHeader);
constexpr uint64_t kHistogramOffset    = kWaveformOffset + kWaveformCells * sizeof(uint32_t);
constexpr uint64_t kVectorscopeOffset  = kHistogramOffset + kHistogramCells * sizeof(uint32_t);
constexpr uint64_t kTwinPeaksOffset    = kVectorscopeOffset + kVectorscopeTotalCells * sizeof(uint32_t);
constexpr uint64_t kWaveformTraceOffset = kTwinPeaksOffset + kTwinPeaksTotalCells * sizeof(uint32_t);
constexpr uint64_t kPreviewOffset      = kWaveformTraceOffset + kWaveformTraceCells * sizeof(float);
constexpr uint64_t kSlotSize           = kPreviewOffset + kPreviewCanvasBytes;
constexpr uint64_t kTotalShmSize       = sizeof(ShmHeader) + kSlotSize * kSlotCount;

// Indexing helpers, so writer and reader cannot disagree about ordering.
//
// Waveform is [column][level][plane] - planes interleaved at each cell, not stored
// as four separate images. This is a performance decision, measured not assumed:
// the R, G, B and Y samples of one pixel nearly always land on neighbouring levels,
// so interleaving puts all four increments on the same cache line or its neighbour.
// The obvious [plane][column][level] layout places them 512 KB apart and cost four
// cache misses per pixel - 23 ns/px single-threaded at UHD.
inline uint64_t WaveformIndex(uint32_t p_Plane, uint32_t p_Column, uint32_t p_Level)
{
    return (uint64_t(p_Column) * kWaveformLevels + p_Level) * kPlaneCount + p_Plane;
}

inline uint64_t HistogramIndex(uint32_t p_Plane, uint32_t p_Bin)
{
    return uint64_t(p_Plane) * kHistogramBins + p_Bin;
}

// Bands back to back, each row-major with row = Cr (vertical), column = Cb
// (horizontal) - so a reader can reshape the flat array straight into
// (bands, rows=Cr, cols=Cb) with no transpose, matching a standard
// vectorscope's axes (Cb right, Cr up).
inline uint64_t VectorscopeIndex(uint32_t p_Band, uint32_t p_CbBin, uint32_t p_CrBin)
{
    return uint64_t(p_Band) * kVectorscopeCells + uint64_t(p_CrBin) * kVectorscopeSize + p_CbBin;
}

// Row-major, row = level (vertical), column = diff (horizontal) - same
// convention as VectorscopeIndex, so a reader reshapes straight into
// (diamonds, rows=level, cols=diff) with no transpose.
inline uint64_t TwinPeaksIndex(uint32_t p_Diamond, uint32_t p_LevelBin, uint32_t p_DiffBin)
{
    return uint64_t(p_Diamond) * kTwinPeaksCells + uint64_t(p_LevelBin) * kTwinPeaksSize + p_DiffBin;
}

// [row][column][plane], same interleaving rationale as WaveformIndex - one
// row's four planes at a given column sit together, since a line-strip
// renderer touches all four for one vertex before moving to the next column.
inline uint64_t WaveformTraceIndex(uint32_t p_Row, uint32_t p_Column, uint32_t p_Plane)
{
    return (uint64_t(p_Row) * kWaveformColumns + p_Column) * kPlaneCount + p_Plane;
}

} // namespace scopedeck
