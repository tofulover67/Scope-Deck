// Scope reduction. Portable C++ with no OFX and no platform headers, so it builds
// on Windows and macOS alike and can be exercised from a standalone test harness
// without Resolve in the loop.

#pragma once

#include "ScopeTypes.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace scopedeck
{

// A borrowed view of one frame. Never owns the pixels.
struct FrameView
{
    const float* pixels = nullptr;   // interleaved RGBA
    int          width  = 0;
    int          height = 0;
    int          rowBytes = 0;       // OFX allows this to be negative

    const float* Row(int p_Y) const
    {
        const char* base = reinterpret_cast<const char*>(pixels);
        return reinterpret_cast<const float*>(base + static_cast<ptrdiff_t>(p_Y) * rowBytes);
    }
};

// What the reduction needs to know about the signal. Only luma actually requires a
// color-space decision at bin time; everything else the reader can reinterpret.
struct ScopeParams
{
    uint32_t    colorSpace = kSpaceRec709;
    LumaWeights luma        = LumaWeightsFor(kSpaceRec709);

    // Analyse every Nth row. 1 reads every pixel.
    int         rowStep     = 1;
};

// A downscaled RGB8 copy of the frame, fit inside the fixed preview canvas
// preserving aspect ratio. width/height are 0 when there is nothing to publish
// (video publish off, or not yet built this frame).
struct PreviewResult
{
    std::vector<uint8_t> rgb;   // width*height*3, row-major, top-to-bottom
    uint32_t width  = 0;
    uint32_t height = 0;
};

// A float RGB copy of the frame for the app's GPU path to measure from, at most
// capWidth x capHeight. Deliberately NOT clipped the way PreviewResult is: the
// out-of-range values ToDisplay8 throws away are exactly what a scope exists to
struct ScopeResult
{
    std::vector<uint32_t> waveform;      // kWaveformCells, see WaveformIndex()
    std::vector<uint32_t> histogram;     // kHistogramCells, see HistogramIndex()
    std::vector<uint32_t> vectorscope;   // kVectorscopeTotalCells, see VectorscopeIndex()
    std::vector<uint32_t> twinPeaks;     // kTwinPeaksTotalCells, see TwinPeaksIndex()
    std::vector<float>    waveformTrace; // kWaveformTraceCells, see WaveformTraceIndex() -
                                         // raw samples, not counts; see ScopeEngine::BuildWaveformTrace
    PreviewResult          preview;      // see ScopeEngine::BuildPreview

    float    minRGB[3] = { 0.0f, 0.0f, 0.0f };
    float    maxRGB[3] = { 0.0f, 0.0f, 0.0f };
    float    probeRGB[3] = { 0.0f, 0.0f, 0.0f };   // centre pixel, unmodified
    uint64_t pixelsSampled = 0;
    double   millis = 0.0;

    // Echoed back so the publisher writes exactly what was used.
    uint32_t    colorSpace = kSpaceRec709;
    LumaWeights luma        = LumaWeightsFor(kSpaceRec709);
};

// The preview size for a p_SrcWidth x p_SrcHeight source at p_Scale: one uniform
// scale, shrunk further only if the result would overflow the fixed canvas
// (kPreviewCanvasWidth/Height), rounded, and at least 1x1. Shared by
// ScopeEngine::BuildPreview and the CUDA tap so the two cannot disagree - the
// GPU used to clamp each axis on its own, which squashed any source wider than
// the canvas (DCI 4K at 100% came out 3840x2160 instead of 3840x2025).
inline void PreviewSizeFor(int p_SrcWidth, int p_SrcHeight, float p_Scale,
                           uint32_t& p_Width, uint32_t& p_Height)
{
    if (p_Scale <= 0.0f) p_Scale = 0.01f;
    const float scale = std::min(p_Scale,
        std::min(static_cast<float>(kPreviewCanvasWidth) / p_SrcWidth,
                 static_cast<float>(kPreviewCanvasHeight) / p_SrcHeight));
    p_Width  = static_cast<uint32_t>(p_SrcWidth * scale + 0.5f);
    p_Height = static_cast<uint32_t>(p_SrcHeight * scale + 0.5f);
    p_Width  = std::min(std::max(p_Width, 1u), kPreviewCanvasWidth);
    p_Height = std::min(std::max(p_Height, 1u), kPreviewCanvasHeight);
}

class ScopeEngine
{
public:
    ScopeEngine();

    // Reduces one frame into p_Out.
    void Analyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out);

    // Scales one frame by p_Scale (e.g. 0.25 for a quarter-resolution preview,
    // 1.0 for native) into p_Out, clamped to fit inside the fixed preview canvas
    // (see kPreviewCanvasWidth/Height) if p_Scale would overflow it. Independent
    // of Analyse() - callers that only want scope bins never pay for this.
    void BuildPreview(const FrameView& p_Frame, float p_Scale, PreviewResult& p_Out);

    // Copies the frame as float RGB into p_Out for the app's GPU path, at most
    // p_CapWidth x p_CapHeight (0 for either means "no cap, use the source"),
    // and always within the fixed scope-source canvas.
    //
    // Downscaling subsamples rather than averaging, unlike BuildPreview. An
    // averaged pixel is a value that was never in the frame: it narrows the
    // distribution and hides the single hot pixel a scope is being read to
    // find. Subsampling keeps every value it reports true, which is also what
    // the CPU reduction's own rowStep does.
    //
    int ThreadCount() const { return m_ThreadCount; }

    // More threads is not automatically better: each one carries a private bin
    // buffer that must be zeroed and merged every frame, so the bookkeeping cost
    // grows with thread count while the per-pixel work shrinks.
    void SetThreadCount(int p_Count);

private:
    struct Partial
    {
        std::vector<uint32_t> waveform;
        std::vector<uint32_t> vectorscope;
        std::vector<uint32_t> twinPeaks;
        float    minRGB[3];
        float    maxRGB[3];
        uint64_t pixels;
    };

    void Reduce(const FrameView& p_Frame, const int* p_ColumnStart,
                const LumaWeights& p_Luma, int p_Y0, int p_Y1, int p_RowStep,
                Partial& p_Partial);

    // Fills p_Out.waveformTrace - independent of Reduce()'s row-banded,
    // multi-threaded histogram pass above: this only ever touches
    // kWaveformTraceRows distinct source rows (not every row, not
    // subsampled by p_RowStep), so it is cheap enough to run as one plain
    // single-threaded pass rather than fold into the threaded reduction and
    // complicate its partial-buffer merging for a step an order of
    // magnitude smaller than the main one.
    void BuildWaveformTrace(const FrameView& p_Frame, const int* p_ColumnStart,
                            const LumaWeights& p_Luma, ScopeResult& p_Out);

    int                  m_ThreadCount;
    std::vector<Partial> m_Partials;      // reused across frames to avoid reallocation
    std::vector<int>     m_ColumnStart;   // kWaveformColumns + 1 pixel boundaries
};

} // namespace scopedeck
