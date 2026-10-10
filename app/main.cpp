// Scope Deck - native C++ / Dear ImGui build.
//
// Phase 1: Source, Waveform, Histogram and Vectorscope, reading wire format v9
// from the existing ScopeTap.ofx unchanged. Nothing here writes the format, and
// core/ScopeTypes.h is a verbatim copy of the plugin's - so if a scope looks
// wrong, the app is the only thing that changed.

#include "AudioMeterBridge.h"
#include "AudioAnalysis.h"
#include "PanelCommon.h"
#include "ScopeImages.h"
#include "ScopeControl.h"
#include "ScopeReader.h"
#include "ScreenCapture.h"
#include "StillImage.h"
#include "SubtitleBridge.h"
#include "Texture.h"
#include "TimecodeBridge.h"
#include "Theme.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImRect, DockBuilder - both live here, not in the public header
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// windows.h must come before GLFW: GLFW's header pulls in the GL headers, which
// define APIENTRY themselves, and minwindef.h then redefines it. Including it
// first lets the GL headers see the existing definition and leave it alone.
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

#include <GLFW/glfw3.h>

#ifdef _WIN32
  // The HWND behind a GLFW window, for Solo Fills the Screen's switch - see
  // SetBorderlessFullScreen.
  #define GLFW_EXPOSE_NATIVE_WIN32
  #include <GLFW/glfw3native.h>
  #include <dwmapi.h>
#else
  #include <csignal>    // SIGPIPE, see main()
  #include <dirent.h>   // ListLayoutPresets
  #include <sys/stat.h> // mkdir, GetPresetsDir
#endif
#ifdef __APPLE__
  #include "mac/MacPlatform.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>     // PaceToDisplay
#include <utility>
#include <vector>
#include <deque>
#include <string>

using namespace scopedeck;

namespace
{

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

enum class WaveformMode { Luma, Parade, YRGB };
enum class WaveformRange { Full, Shadows, Highlights };

// Which IRE convention the graticule and readouts use. See PanelCommon.h - the
// same signal reads 97.0 or 105.9 depending on this, so it is a switch.
enum class ScaleMode { FullRange, LegalRange };

constexpr float kDefaultGain = 8.0f;

// What a cached enhanced trace was filtered from. PrepareEnhancedTrace runs
// the pre-filter, temporal blend and low-pass over the whole trace buffer;
// none of that depends on anything but the frame and these settings, so
// repeating it for a second render of the same frame produces the identical
// buffer.
//
// Caching it also fixes a real bug rather than only saving time: Smooth Trace
// is a *temporal* blend against the previous result, so re-running it several
// times per video frame blended the same frame in repeatedly and made the
// smoothing converge faster the higher the app's frame rate. Keyed on the
// frame index, the blend advances exactly once per frame either way.
struct EnhancedTraceCache
{
    uint64_t       frameIndex = UINT64_MAX;
    bool           prefilter  = false;
    bool           smooth     = false;
    float          kernel     = -1.0f;
    bool           masked     = false;
    SpatialMask    mask{};
    ValueQualifier qualifier{};
    bool           ready      = false;

    // Bumped every time the trace is actually recomputed, so a cache built
    // from the trace (EnhancedRunCache) can key on one number instead of
    // repeating the frame-plus-settings comparison above.
    uint64_t       generation = 0;
};

// The projected geometry of one Enhanced Render pass: contiguous runs of
// screen-space points, ready to hand to ImGui.
//
// Projecting the trace into the panel is the "walk" in the profiler's split,
// and it was measured at 4.5 ms of the 5.3 ms a Waveform pass cost per refresh
// - re-done 60 times a second on a held frame, because nothing it reads (the
// prepared trace, the rectangle, the range, the gain) had changed. This keeps
// the result; a hit pays the emit alone.
//
// Keyed on the prepared trace's generation rather than on the frame and every
// filter setting: PrepareEnhancedTraceCached already decides when the trace
// changes and bumps the generation exactly then. Everything else that shapes
// the projection is in the key explicitly.
struct EnhancedRunCache
{
    struct Run
    {
        uint32_t first = 0;     // index into points (and pointColour, when used)
        uint32_t count = 0;
        ImU32    colour = 0;    // plain runs only
    };

    uint64_t generation = UINT64_MAX;
    ImVec2   rectMin{}, rectMax{};
    float    lo = 0.0f, hi = 0.0f;   // whichever span the scope projects over
    float    gain = 0.0f;
    bool     colorize = false;
    uint32_t variant = 0;            // Waveform: planes and cell
    bool     valid = false;

    std::vector<ImVec2> points;      // every run, back to back
    std::vector<ImU32>  pointColour; // one per point for coloured runs, else empty
    std::vector<Run>    runs;

    bool Matches(uint64_t p_Generation, const ImRect& p_Rect, float p_Lo, float p_Hi,
                 float p_Gain, bool p_Colorize, uint32_t p_Variant) const
    {
        return valid && generation == p_Generation
            && rectMin.x == p_Rect.Min.x && rectMin.y == p_Rect.Min.y
            && rectMax.x == p_Rect.Max.x && rectMax.y == p_Rect.Max.y
            && lo == p_Lo && hi == p_Hi && gain == p_Gain
            && colorize == p_Colorize && variant == p_Variant;
    }

    void Begin(uint64_t p_Generation, const ImRect& p_Rect, float p_Lo, float p_Hi,
               float p_Gain, bool p_Colorize, uint32_t p_Variant)
    {
        generation = p_Generation;
        rectMin = p_Rect.Min;
        rectMax = p_Rect.Max;
        lo = p_Lo;
        hi = p_Hi;
        gain = p_Gain;
        colorize = p_Colorize;
        variant = p_Variant;
        points.clear();
        pointColour.clear();
        runs.clear();
        valid = true;
    }

    // Runs shorter than two points are dropped, as the emitters always did.
    void AddRun(const ImVec2* p_Points, int p_Count, ImU32 p_Colour)
    {
        if (p_Count < 2) return;
        runs.push_back({ uint32_t(points.size()), uint32_t(p_Count), p_Colour });
        points.insert(points.end(), p_Points, p_Points + p_Count);
    }

    void AddRun(const ImVec2* p_Points, const ImU32* p_Colours, int p_Count)
    {
        if (p_Count < 2) return;
        runs.push_back({ uint32_t(points.size()), uint32_t(p_Count), 0 });
        points.insert(points.end(), p_Points, p_Points + p_Count);
        pointColour.insert(pointColour.end(), p_Colours, p_Colours + p_Count);
    }
};


struct WaveformState
{
    WaveformMode  mode  = WaveformMode::Parade;
    WaveformRange range = WaveformRange::Full;
    ScaleMode     scale = ScaleMode::FullRange;
    float         gain  = kDefaultGain;

    // Enhanced Render and its dependents. The three below only apply while
    // enhancedRender is on, and the settings window hides them when it is off -
    // the same "enable to see more" grouping the old app used.
    // On by default: this line-strip trace is the look the app is aiming at, and
    // the binned density trace is the fallback rather than the other way round.
    bool          enhancedRender  = true;
    bool          smoothTrace     = false;
    float         lowPassKernel   = 0.0f;
    bool          signalPrefilter = false;

    // Panel-owned rather than shared, even though the Vectorscope has a control
    // with the same name: this one tints a Luma-mode trace by the source pixel's
    // own colour. Two different controls that happen to share a word.
    bool          colorize        = false;

    // Hovering reports the exact value under the pointer, which is the precise
    // readout - reading it off a gridline by eye is not.
    bool          showCursor      = true;

    EnhancedTraceState traceState;
    std::vector<float> preparedTrace;
    EnhancedTraceCache traceCache;
    EnhancedRunCache   enhancedRuns[3];   // one per parade cell; [0] for the single-image modes

    Image         image;

    // One texture per parade cell, not one reused three times.
    //
    // ImGui's API is immediate-mode but its *rendering* is deferred: AddImage
    // records a command holding an ImTextureID and nothing is drawn until
    // RenderDrawData at the end of the frame. Three cells sharing one texture
    // therefore all sample whatever was uploaded last - which drew a blue R and a
    // blue G, an error that looks like a tinting bug rather than a lifetime one.
    // Index 0 also serves the single-image Luma and YRGB modes.
    Texture       textures[3];

    bool          dirty = true;

    // What the density textures were built from - see FalseColorState. One key
    // covers all three parade cells, since they are built together.
    uint64_t       builtFrameIndex = UINT64_MAX;
    WaveformMode   builtMode       = WaveformMode::Parade;
    float          builtGain       = 0.0f;
    int            builtLo         = -1;
    int            builtHi         = -1;
    bool           builtMasked     = false;
    SpatialMask    builtMask{};
    ValueQualifier builtQualifier{};
};

// How the channels are laid out.
//
// No separate RGB mode: unticking Y in YRGB *is* RGB, so offering both would be
// two controls for one choice.
enum class HistogramChannels { Luma, Stacked, YRGB };

struct HistogramState
{
    HistogramChannels channels = HistogramChannels::YRGB;

    // Which channels YRGB shows, each its own row. Ignored by Luma (Y only) and
    // Stacked (R/G/B over one another).
    bool  showY = true, showR = true, showG = true, showB = true;

    bool  logScale = false;

    // Same readout as the waveform's, on this scope's own axis.
    bool  showCursor = true;

    // Per-channel qualifier: click-drag directly on a lane selects a value
    // range on that plane - see UpdateHistogramQualifierInput. One independent
    // range per plane (Y/R/G/B), ANDed together and, via RebuildValueQualifier,
    // with every other open Histogram panel's own active ranges - matching the
    // old app's MainWindow.qualifiers list of HistogramPanel instances. Stacked
    // mode has no distinct per-channel lane to drag on, so it doesn't support
    // qualifying, same as the old app.
    bool  qualifyActive[kPlaneCount] = { false, false, false, false };
    float qualifyLo[kPlaneCount]     = { 0.0f, 0.0f, 0.0f, 0.0f };
    float qualifyHi[kPlaneCount]     = { 0.0f, 0.0f, 0.0f, 0.0f };

    // Drag-in-progress state, not persisted.
    bool     qualifyDragging   = false;
    uint32_t qualifyDragPlane  = kPlaneY;
    float    qualifyDragAnchor = 0.0f;
    bool     qualifyDragMoved  = false;
};

// Which luma band(s) the trace reflects. One control rather than three
// checkboxes, matching the old app: it is a single question, and 3-Stack could
// never have been combined with the others anyway.
enum class VectorRange { All, Low, Mid, High, Stack };

struct VectorscopeState
{
    VectorRange range = VectorRange::All;
    float   gain          = 6.0f;
    bool    colorize      = false;
    bool    showTargets   = true;
    float   targetSize    = 9.0f;    // box edge in pixels, the old app's default
    float   targetOpacity = 1.0f;    // independent of the Graticule slider
    bool    showSkinLine  = true;
    bool    zoom2x        = false;

    // Enhanced Render, as on the Waveform - on by default, same reasoning as
    // Waveform's own. Colorize is deliberately NOT hidden with this group the
    // way the Waveform's is: here it already means something for the density
    // trace (the hue wheel), so hiding it would hide a live control.
    bool    enhancedRender  = true;
    bool    smoothTrace     = false;
    float   lowPassKernel   = 0.0f;
    bool    signalPrefilter = false;

    EnhancedTraceState traceState;
    std::vector<float> preparedTrace;
    EnhancedTraceCache traceCache;
    EnhancedRunCache   enhancedRuns[3];   // one per 3-Stack cell, like the textures

    Image   image;

    // One per 3-Stack cell. ImGui's rendering is deferred, so cells sharing a
    // texture would all sample the last upload - the parade bug, again.
    Texture textures[3];

    bool    dirty = true;

    // What each cell's texture was built from - see FalseColorState. Per cell,
    // because 3-Stack pushes three band masks through the one staging image.
    struct Built
    {
        uint64_t       frameIndex = UINT64_MAX;
        uint32_t       bandMask   = 0;
        float          gain       = 0.0f;
        bool           colorize   = false;
        bool           zoom2x     = false;
        bool           masked     = false;
        SpatialMask    mask{};
        ValueQualifier qualifier{};
    };
    Built   built[3];
};

// Framing guides drawn over the Source panel's picture. Ported from the old
// app's COMPOSITION_GRIDS, plus the Fibonacci spiral, which it never had.
enum class CompositionGrid
{
    None, Thirds, GoldenRatio, CenterCross, Crop185, Crop239, Fibonacci, Count
};

// Which corner the Fibonacci spiral coils into. The four are mirrors of one
// construction rather than four constructions - see DrawFibonacciSpiral.
enum class SpiralCorner { BottomRight, BottomLeft, TopRight, TopLeft };

struct SourceState
{
    Texture texture;
    Image   dimmedImage;   // only built while a spatial mask is active
    bool    dirty = true;

    // What the texture holds - see FalseColorState. The mask and qualifier
    // only matter while builtDimmed is set; the plain upload ignores them.
    uint64_t       builtFrameIndex = UINT64_MAX;
    bool           builtDimmed     = false;
    SpatialMask    builtMask{};
    ValueQualifier builtQualifier{};

    CompositionGrid grid         = CompositionGrid::None;
    SpiralCorner    spiralCorner = SpiralCorner::BottomRight;
};

// Which part of the spatial mask a left-drag is currently manipulating.
// "Move" and the four corners apply to Ellipse/Rectangle; Start/End to
// Gradient's two points; None means no drag is in progress.
enum class MaskHandle { None, Move, X0Y0, X1Y0, X0Y1, X1Y1, Rotate, Start, End };

// Transient input state for editing App::spatialMask on the Source panel -
// lives on App (not a local), since it has to survive from one frame's mouse
// press to a later frame's release. Never persisted: same reasoning as the
// mask itself (see SpatialMask's own comment).
struct MaskInputState
{
    bool        drawingNew = false;   // Ctrl/Shift-drag in progress, defining a brand new mask
    float       startFx = 0.0f, startFy = 0.0f;
    bool        moved = false;        // false at release means "just a click" -> clear instead of keep a 0-size mask

    MaskHandle  editing = MaskHandle::None;
    SpatialMask editStartGeometry;
    float       editAnchorFx = 0.0f, editAnchorFy = 0.0f;

    // Which panel's own UpdateSpatialMaskInput call started the current edit -
    // the mask is shared across every Source panel open at once (it means the
    // same region regardless of which one drew it), but the mouse is only
    // ever really over one of them. Without this, every *other* open Source
    // panel's own call this same frame would also see editing/drawingNew set
    // and reinterpret the same mouse position against its own (differently
    // positioned) fit rect, overwriting whatever the panel actually being
    // dragged in just wrote - the shared mask fighting itself every frame a
    // second Source panel happens to be open.
    int         owningPanelId = -1;
};

enum class FalseColorMode { StandardIre, ElZoneSystem, ArriLogC, RedIpp2, SonyVenice, Count };

inline const char* FalseColorModeName(FalseColorMode p_Mode)
{
    switch (p_Mode)
    {
        case FalseColorMode::StandardIre:  return "Standard IRE";
        case FalseColorMode::ElZoneSystem: return "EL Zone System";
        case FalseColorMode::ArriLogC:     return "ARRI LogC";
        case FalseColorMode::RedIpp2:      return "RED IPP2";
        case FalseColorMode::SonyVenice:   return "Sony Venice";
        default: return "?";
    }
}

// (low, high) are fractions of the preview's own 8-bit range, entry order does
// not matter since bands do not overlap. Follows the common cinematography
// convention (purple/blue flags near-black clipping, green flags the ~40%
// mid-grey reference, pink flags the skin-tone reference, yellow/red flag
// near-white and clipped highlights); exact thresholds are a reasonable first
// calibration per mode, not a measured one - ported verbatim from the old
// app's FALSE_COLOR_BANDS_MAP.
const FalseColorBand* FalseColorBandsForMode(FalseColorMode p_Mode, int& p_OutCount)
{
    static const FalseColorBand kStandardIre[] = {
        { 0.000f, 0.020f, { 140, 0, 200 } },
        { 0.020f, 0.050f, { 0, 80, 255 } },
        { 0.380f, 0.420f, { 0, 200, 0 } },
        { 0.490f, 0.540f, { 255, 140, 180 } },
        { 0.970f, 0.995f, { 255, 220, 0 } },
        { 0.995f, 1.001f, { 255, 0, 0 } },
    };
    static const FalseColorBand kElZoneSystem[] = {
        { 0.000f, 0.020f, { 80, 0, 120 } },
        { 0.020f, 0.060f, { 0, 50, 180 } },
        { 0.060f, 0.120f, { 0, 120, 255 } },
        { 0.120f, 0.200f, { 0, 200, 220 } },
        { 0.200f, 0.300f, { 0, 200, 120 } },
        { 0.300f, 0.370f, { 150, 220, 0 } },
        { 0.370f, 0.430f, { 0, 255, 0 } },
        { 0.430f, 0.520f, { 255, 140, 200 } },
        { 0.520f, 0.650f, { 255, 220, 0 } },
        { 0.650f, 0.800f, { 255, 140, 0 } },
        { 0.800f, 0.950f, { 255, 60, 0 } },
        { 0.950f, 1.001f, { 255, 0, 0 } },
    };
    static const FalseColorBand kArriLogC[] = {
        { 0.000f, 0.025f, { 140, 0, 200 } },
        { 0.025f, 0.040f, { 0, 80, 255 } },
        { 0.380f, 0.420f, { 0, 200, 0 } },
        { 0.480f, 0.520f, { 255, 140, 180 } },
        { 0.970f, 0.990f, { 255, 220, 0 } },
        { 0.990f, 1.001f, { 255, 0, 0 } },
    };
    static const FalseColorBand kRedIpp2[] = {
        { 0.000f, 0.020f, { 120, 0, 180 } },
        { 0.020f, 0.100f, { 0, 100, 255 } },
        { 0.380f, 0.420f, { 0, 220, 0 } },
        { 0.460f, 0.520f, { 255, 140, 180 } },
        { 0.900f, 0.980f, { 255, 220, 0 } },
        { 0.980f, 1.001f, { 255, 0, 0 } },
    };
    static const FalseColorBand kSonyVenice[] = {
        { 0.000f, 0.030f, { 140, 0, 200 } },
        { 0.030f, 0.120f, { 0, 80, 255 } },
        { 0.390f, 0.430f, { 0, 200, 0 } },
        { 0.520f, 0.580f, { 255, 140, 180 } },
        { 0.950f, 0.985f, { 255, 140, 0 } },
        { 0.985f, 1.001f, { 255, 0, 0 } },
    };

    switch (p_Mode)
    {
        case FalseColorMode::ElZoneSystem: p_OutCount = IM_ARRAYSIZE(kElZoneSystem); return kElZoneSystem;
        case FalseColorMode::ArriLogC:     p_OutCount = IM_ARRAYSIZE(kArriLogC);     return kArriLogC;
        case FalseColorMode::RedIpp2:      p_OutCount = IM_ARRAYSIZE(kRedIpp2);      return kRedIpp2;
        case FalseColorMode::SonyVenice:   p_OutCount = IM_ARRAYSIZE(kSonyVenice);   return kSonyVenice;
        default:                           p_OutCount = IM_ARRAYSIZE(kStandardIre);  return kStandardIre;
    }
}

struct FalseColorState
{
    FalseColorMode mode = FalseColorMode::StandardIre;
    bool           showScale = true;
    Image          image;
    Texture        texture;
    bool           dirty = true;

    // What the cached image/texture was built from. Rebuilding a full-size
    // per-pixel image (and re-uploading it) on every render frame produced an
    // identical result whenever none of these changed - which, on a held
    // frame, is every frame.
    uint64_t       builtFrameIndex = UINT64_MAX;
    FalseColorMode builtMode       = FalseColorMode::StandardIre;
    SpatialMask    builtMask{};
    ValueQualifier builtQualifier{};
};

// SkinToneModel / SkinToneColorMode are declared in ScopeImages.h, alongside
// the build function they parameterise.

struct SkinToneState
{
    SkinToneModel     model = SkinToneModel::YCbCr;
    SkinToneColorMode colorMode = SkinToneColorMode::Default;
    bool              limitsEnabled = false;
    bool              greyNonSkin = true;
    float             tolerance = 22.0f;
    float             hueMin = kSkinToneAngleDeg - 22.0f;
    float             hueMax = kSkinToneAngleDeg + 22.0f;
    float             satMin = kSkinMinSaturation;
    float             satMax = 1.0f;
    float             lumaMin = kSkinMinLuma;
    float             lumaMax = kSkinMaxLuma;
    float             solidColor[3]     = { 60.0f / 255.0f, 225.0f / 255.0f, 110.0f / 255.0f };
    float             gradientColorA[3] = { 60.0f / 255.0f, 225.0f / 255.0f, 110.0f / 255.0f };
    float             gradientColorB[3] = { 255.0f / 255.0f,  70.0f / 255.0f, 200.0f / 255.0f };

    Image             image;
    Texture           texture;
    bool              dirty = true;

    // See FalseColorState's equivalent - the params are compared rather than
    // the widgets watched, so any route that changes them (a preset, a
    // restored layout) invalidates the cache too, not just a live edit.
    uint64_t          builtFrameIndex = UINT64_MAX;
    SkinToneParams    builtParams{};
    SpatialMask       builtMask{};
    ValueQualifier    builtQualifier{};
};

enum class NoiseMethod { Highpass, Channel, Differenced };

struct NoiseState
{
    NoiseMethod method = NoiseMethod::Channel;
    int         channel = 2;
    float       gain = 1.0f;

    Image       image;
    Texture     texture;
    bool        dirty = true;

    // What the texture was built from - see FalseColorState.
    uint64_t    builtFrameIndex = UINT64_MAX;
    NoiseMethod builtMethod     = NoiseMethod::Channel;
    int         builtChannel    = -1;
    float       builtGain       = 0.0f;
};

enum class PeakColor { Red, Cyan, Yellow, Green };

inline Rgb8 PeakColorRgb(PeakColor p_Color)
{
    switch (p_Color)
    {
        case PeakColor::Cyan:   return { 0, 255, 255 };
        case PeakColor::Yellow: return { 255, 255, 0 };
        case PeakColor::Green:  return { 0, 255, 0 };
        default:                return { 255, 0, 0 };   // Red
    }
}

inline const char* PeakColorName(PeakColor p_Color)
{
    switch (p_Color)
    {
        case PeakColor::Cyan:   return "Cyan";
        case PeakColor::Yellow: return "Yellow";
        case PeakColor::Green:  return "Green";
        default:                return "Red";
    }
}

struct FocusPeakingState
{
    float     threshold   = 0.20f;
    bool      grayscaleBg = false;
    PeakColor color       = PeakColor::Red;

    Image     image;
    Texture   texture;
    bool      dirty = true;

    // What the texture was built from - see FalseColorState.
    uint64_t  builtFrameIndex  = UINT64_MAX;
    float     builtThreshold   = -1.0f;
    bool      builtGrayscaleBg = false;
    PeakColor builtColor       = PeakColor::Red;
};

struct BandingAnalyzerState
{
    float   gain = 16.0f;

    Image   image;
    Texture texture;
    bool    dirty = true;

    // What the texture was built from - see FalseColorState.
    uint64_t builtFrameIndex = UINT64_MAX;
    float    builtGain       = 0.0f;
};

// A/B Difference against a captured reference still. The reference itself
// (refPixels/refWidth/refHeight) is live-captured pixel data, not a setting -
// the same "grading work, not a layout preference" reasoning that keeps the
// spatial mask out of settings persistence - so only gain is saved.
struct DifferenceState
{
    float      gain = 1.0f;
    StillImage still;   // captured from Source, or loaded from disk - see StillImage.h

    Image   image;
    Texture texture;
    bool    dirty = true;

    // What the texture was built from - see FalseColorState. The still is
    // identified by its generation, which every capture/load bumps.
    uint64_t builtFrameIndex      = UINT64_MAX;
    float    builtGain            = 0.0f;
    uint64_t builtStillGeneration = UINT64_MAX;
};

// Which scope to draw over the still, computed from the still's OWN pixels
// rather than the live signal - the point is a reference you can hold a
// scope's shape against, not a live readout. To compare against the current
// source, put an ordinary live Waveform/Vectorscope panel next to this one;
// Compare deliberately does not try to show two signals worth of scope in
// one panel.
enum class CompareOverlay { None, Waveform, Vectorscope, Histogram, Count };

inline const char* CompareOverlayName(CompareOverlay p_Overlay)
{
    switch (p_Overlay)
    {
        case CompareOverlay::Waveform:    return "Waveform";
        case CompareOverlay::Vectorscope: return "Vectorscope";
        case CompareOverlay::Histogram:   return "Histogram";
        default:                          return "None";
    }
}

struct CompareState
{
    StillImage     still;
    CompareOverlay overlay        = CompareOverlay::None;
    float          overlayOpacity = 0.85f;

    Image   image;      // the still's own RGBA upload
    Texture texture;
    Texture overlayTexture;   // Waveform/Vectorscope's own composite, reused across frames

    // The still is static - nothing to re-derive every frame the way a live
    // signal's bins have to be. Rebuilt only when still.generation changes,
    // via BuildScopeBinsFromRGB (the same per-pixel reduction the masked
    // live scopes use, just over a picture instead of the current preview).
    MaskedScopeBins overlayBins;
    uint64_t        overlayBinsGeneration = 0;

    // What overlayTexture was built from. The bins are the still's, so the
    // only live inputs are the frame's chroma range and luma weights, which the
    // Vectorscope builder reads for its wheel geometry - keyed on those values
    // rather than on the frame itself, so a new frame with the same ranges
    // (every frame of a clip) does not rebuild a picture of a static still.
    CompareOverlay builtOverlay    = CompareOverlay::None;
    uint64_t       builtGeneration = UINT64_MAX;
    float          builtChromaLo   = 0.0f;
    float          builtChromaHi   = 0.0f;
    float          builtLuma[3]    = { 0.0f, 0.0f, 0.0f };

    // Which still `texture` holds. Size alone was the key once, and a second
    // Capture from Source - same size every time - kept showing the first.
    uint64_t uploadedGeneration = UINT64_MAX;
};

struct GrainAnalyzerState
{
    // A real sensitivity control, unlike its first pass: BuildGrainAnalyzerProfile
    // now returns raw RMS energy (see its own comment in ScopeImages.h for
    // why it no longer self-normalises), which for real footage sits
    // somewhere in the low hundredths - too small to read as a curve at
    // gain 1. 25 is a first calibration to bring typical grain into a
    // visible fraction of the plot, not a measured constant; the slider
    // exists precisely because "typical" varies a lot by source.
    float gain = 25.0f;
    float profile[kGrainBandCount] = {};

    // gain is deliberately not part of this key: it scales the curve at draw
    // time and does not affect the profile itself, so dragging the slider
    // must not trigger nine full-image blurs per frame.
    uint64_t builtFrameIndex = UINT64_MAX;
};

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------
//
// The old app's General -> Preferences window. Options whose subsystem does not
// exist in this build yet are shown disabled with a tooltip saying what they are
// waiting on, rather than hidden: the set of preferences is part of knowing what
// the app does, and a silently missing one reads as a regression.


// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------
//
// A panel is a *section* of the deck rather than a fixed scope: its `kind`
// decides what it draws, and right-click -> Content switches that in place while
// keeping the section where it is. That is why every kind's state lives on every
// panel rather than there being one Waveform and one Histogram - switching away
// and back should not forget how you had it set up.
//
// The idle kinds cost almost nothing: their Image buffers and GL textures are only
// allocated by the draw path that uses them.

enum class PanelKind { Source, Waveform, Histogram, Vectorscope, FalseColor, SkinTone, Noise, Timecode, WhiteBalance, FocusPeaking, BandingAnalyzer, Difference, GrainAnalyzer, Compare, AudioMeter, ColorCube, Qualifier, Chromaticity, LumaVsSat, HueVsSat, Goniometer, SpectrumAnalyzer, Count };

// PrepareEnhancedTrace, skipped when the previous result is still valid -
// see EnhancedTraceCache for what "valid" means and why the temporal blend
// makes this a correctness fix as well as a saving. p_Out is left holding
// the previous result on a hit, which is exactly the buffer the caller would
// otherwise have recomputed.
static bool PrepareEnhancedTraceCached(const ScopeFrame& p_Frame,
                                       bool p_Prefilter, bool p_Smooth, float p_Kernel,
                                       EnhancedTraceState& p_State,
                                       std::vector<float>& p_Out,
                                       const std::vector<float>* p_MaskedTrace,
                                       const SpatialMask& p_Mask,
                                       const ValueQualifier& p_Qualifier,
                                       EnhancedTraceCache& p_Cache)
{
    const bool masked = (p_MaskedTrace != nullptr);
    if (p_Cache.frameIndex == p_Frame.frameIndex
        && p_Cache.prefilter == p_Prefilter && p_Cache.smooth == p_Smooth
        && p_Cache.kernel == p_Kernel && p_Cache.masked == masked
        && SpatialMaskEquals(p_Cache.mask, p_Mask)
        && ValueQualifierEquals(p_Cache.qualifier, p_Qualifier))
    {
        return p_Cache.ready;
    }

    const bool ready = PrepareEnhancedTrace(p_Frame, p_Prefilter, p_Smooth, p_Kernel,
                                            p_State, p_Out, p_MaskedTrace);

    p_Cache.frameIndex = p_Frame.frameIndex;
    p_Cache.prefilter  = p_Prefilter;
    p_Cache.smooth     = p_Smooth;
    p_Cache.kernel     = p_Kernel;
    p_Cache.masked     = masked;
    p_Cache.mask       = p_Mask;
    p_Cache.qualifier  = p_Qualifier;
    p_Cache.ready      = ready;
    ++p_Cache.generation;
    return ready;
}

const char* PanelKindName(PanelKind p_Kind)
{
    switch (p_Kind)
    {
        case PanelKind::Source:          return "Source";
        case PanelKind::Waveform:        return "Waveform";
        case PanelKind::Histogram:       return "Histogram";
        case PanelKind::Vectorscope:     return "Vectorscope";
        case PanelKind::FalseColor:      return "False Color";
        case PanelKind::SkinTone:        return "Skin Tone";
        case PanelKind::Noise:           return "Noise";
        case PanelKind::Timecode:        return "Timecode";
        case PanelKind::WhiteBalance:    return "White Balance";
        case PanelKind::FocusPeaking:    return "Focus Peaking";
        case PanelKind::BandingAnalyzer: return "Banding Analyzer";
        case PanelKind::Difference:      return "A/B Difference";
        case PanelKind::GrainAnalyzer:   return "Grain Analyzer";
        case PanelKind::Compare:         return "Compare";
        case PanelKind::AudioMeter:      return "Audio Meter";
        case PanelKind::ColorCube:       return "Color Cube";
        case PanelKind::Qualifier:       return "Qualifier";
        case PanelKind::Chromaticity:    return "Chromaticity";
        case PanelKind::LumaVsSat:       return "Luma vs Sat";
        case PanelKind::HueVsSat:        return "Hue vs Sat";
        case PanelKind::Goniometer:      return "Goniometer";
        case PanelKind::SpectrumAnalyzer: return "Spectrum Analyzer";
        default:                         return "?";
    }
}

struct TimecodeState {};

// Nothing here: the HSL range/invert/enabled state this panel edits is global
// (App::valueQualifier.hsl, like spatialMask), so any number of Qualifier
// panels open at once just all show/edit the one shared range - no per-panel
// copy to keep in sync, unlike the old app's sync_from_host.
struct QualifierPanelState {};

struct AudioMeterState
{
    bool peakHold = true;

    // Held-peak decay, one entry per channel - transient view state, not a
    // setting (same reasoning as PanelCommon's ZoomPan, see the "Deliberately
    // NOT saved" note in CollectSettingFields below), since the meter should
    // start cold on a fresh launch rather than reopen holding a stale peak
    // from whatever was playing when the app last closed.
    float  heldPeakDb[AudioMeterLevels::kMaxChannels];
    double heldPeakAtSec[AudioMeterLevels::kMaxChannels] = {};

    // The bar's own smoothed level, separate from the peak-hold marker above -
    // meter ballistics (fast attack, slower release) rather than snapping
    // straight to whatever the capture thread's latest ~10-20ms packet
    // happened to read. Real audio varies buffer-to-buffer far more than a
    // meter reads as "the level," and drawing that raw and unsmoothed every
    // video frame is what read as flicker/jumpy vertical movement.
    float displayDb[AudioMeterLevels::kMaxChannels];

    AudioMeterState()
    {
        for (float& db : heldPeakDb) db = -100.0f;
        for (float& db : displayDb)  db = -100.0f;
    }
};

// --- 3x3 rotation matrix helpers, for the Color Cube's orbit camera --------
//
// Row-major, p' = M * p. Kept minimal and local rather than pulling in a
// general math library - three small helpers is all the orbit camera needs.

inline void Mat3RotY(float p_Deg, float p_Out[9])
{
    const float r = p_Deg * (3.14159265358979323846f / 180.0f);
    const float c = std::cos(r), s = std::sin(r);
    // Matches the panel's original x1 = x*c + z*s; z1 = -x*s + z*c; y1 = y.
    p_Out[0] = c;  p_Out[1] = 0.0f; p_Out[2] = s;
    p_Out[3] = 0.0f; p_Out[4] = 1.0f; p_Out[5] = 0.0f;
    p_Out[6] = -s; p_Out[7] = 0.0f; p_Out[8] = c;
}

inline void Mat3RotX(float p_Deg, float p_Out[9])
{
    const float r = p_Deg * (3.14159265358979323846f / 180.0f);
    const float c = std::cos(r), s = std::sin(r);
    // Matches the panel's original y2 = y*c - z*s; z2 = y*s + z*c; x2 = x.
    p_Out[0] = 1.0f; p_Out[1] = 0.0f; p_Out[2] = 0.0f;
    p_Out[3] = 0.0f; p_Out[4] = c;    p_Out[5] = -s;
    p_Out[6] = 0.0f; p_Out[7] = s;    p_Out[8] = c;
}

inline void Mat3Mul(const float p_A[9], const float p_B[9], float p_Out[9])
{
    float r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += p_A[i * 3 + k] * p_B[k * 3 + j];
            r[i * 3 + j] = sum;
        }
    std::memcpy(p_Out, r, sizeof(r));
}

inline void Mat3Apply(const float p_M[9], float p_X, float p_Y, float p_Z,
                      float& p_OutX, float& p_OutY, float& p_OutZ)
{
    p_OutX = p_M[0] * p_X + p_M[1] * p_Y + p_M[2] * p_Z;
    p_OutY = p_M[3] * p_X + p_M[4] * p_Y + p_M[5] * p_Z;
    p_OutZ = p_M[6] * p_X + p_M[7] * p_Y + p_M[8] * p_Z;
}

// The default/reset view. Still off-axis so the cube reads as a solid rather
// than a flat square, but only just: the old app's -20 elevation looked down
// at the cloud steeply enough to foreshorten it, and the neutral axis (which
// is what the panel is mostly read along) sat at a strong diagonal. At -6 the
// cube is nearly edge-on to its own base plane, which is the view Trevor
// asked for. Zoom starts backed off rather than filling the panel, leaving
// the cloud room to sit inside the frame.
constexpr float kColorCubeDefaultAzimuth   =  30.0f;
constexpr float kColorCubeDefaultElevation =  -6.0f;
constexpr float kColorCubeDefaultZoom      =  0.7f;

// Below the default, so "zoom out" still has somewhere to go from the view
// the panel opens at - the previous floor was the old default's own scale,
// which left the very first scroll-out doing nothing.
constexpr float kColorCubeMinZoom = 1.0f / 2.5f;
constexpr float kColorCubeMaxZoom = 16.0f;

// Builds the camera as a turntable: azimuth spins the cube about the world's
// own vertical axis - which is the green (G) axis, the one ColorCubeProject
// maps to screen-up - and elevation then tips that result toward or away
// from the viewer. Yaw inner, pitch outer: M = RotX(elevation) * RotY(azimuth).
//
// Composing in this fixed order from two angles, rather than accumulating
// drag steps into the matrix itself, is what keeps the cube sitting flat on
// its plane. Folding each step in about the camera's *own* axes (a free
// trackball, which this was) lets roll creep in: drag right then up then
// left and the cube comes back tilted, because those axes have themselves
// rotated in between. A turntable has no roll term at all, so "horizontal
// drag spins it on the spot, vertical drag raises the eye" holds at every
// orientation, and the G axis stays put.
inline void ColorCubeBuildRotation(float p_AzimuthDeg, float p_ElevationDeg, float p_Out[9])
{
    float ry[9]; Mat3RotY(p_AzimuthDeg,   ry);
    float rx[9]; Mat3RotX(p_ElevationDeg, rx);
    Mat3Mul(rx, ry, p_Out);
}

struct ColorCubeState
{
    ColorCubeSpace space   = ColorCubeSpace::RGB;
    int            spread  = 1;   // 1-10: stretches the cloud off the neutral axis for legibility
    int            density = 2;   // 1-6: drawn point size
    bool           invertX = false;  // flips which way a horizontal drag orbits
    bool           invertY = false;  // flips which way a vertical drag orbits

    // Camera - transient view state, not persisted (same reasoning as
    // PanelCommon's ZoomPan): reopening a cube rotated to some angle with no
    // memory of why would be as baffling as reopening zoomed into a corner.
    //
    // A turntable: the camera is these two angles, and `rotation` is only
    // ever derived from them (see ColorCubeBuildRotation). Azimuth spins the
    // cube about the green axis, elevation raises or lowers the eye, and
    // elevation is clamped to +/-90 so it can look straight down or straight
    // up but never tip past vertical onto its head.
    //
    // This replaced a free trackball that composed each drag step about the
    // camera's current screen axes. That had no roll *term*, but accumulated
    // roll anyway - orbit around a corner and the cube came back visibly
    // tilted off its plane, with no way to straighten it short of pressing
    // Z. Two angles cannot represent a rolled orientation in the first
    // place, so the cube is structurally incapable of drifting off level.
    float azimuthDeg   = kColorCubeDefaultAzimuth;
    float elevationDeg = kColorCubeDefaultElevation;
    float rotation[9];
    float zoom     = kColorCubeDefaultZoom;
    float panX     = 0.0f;
    float panY     = 0.0f;

    // Rebuilt only when the underlying video frame actually changes (see
    // frameIndex below) - re-sampling and colour-converting a few thousand
    // points is real work, and every DrawColorCubePanel call needs the same
    // set for input hit-testing as well as drawing.
    std::vector<ColorCubePoint> points;
    uint64_t                    pointsFrameIndex = UINT64_MAX;
    ColorCubeSpace              pointsSpace      = ColorCubeSpace::RGB;

    // The mask/qualifier the cached points were sampled through. Without
    // these the cloud would only pick up a mask edit when the frame index
    // happened to change - correct-looking during playback, visibly stale on
    // a held frame, which is exactly when a mask is usually being dragged.
    SpatialMask                 pointsMask{};
    ValueQualifier              pointsQualifier{};

    ColorCubeState() { ColorCubeBuildRotation(azimuthDeg, elevationDeg, rotation); }
};

struct WhiteBalanceState
{
    float   gain = 1.0f;

    // Enhanced Render, the same three dependents as Waveform's/Vectorscope's
    // own - on by default, same as those two (this used to be the one
    // exception, matching the old app's own default; every scope now opens
    // in Enhanced Render, with Preferences -> "Disable Enhanced Render" as
    // the one global override for low-power systems).
    bool    enhancedRender  = true;
    bool    smoothTrace     = false;
    float   lowPassKernel   = 0.0f;
    bool    signalPrefilter = false;

    // Enhanced Render's own tint by the sample's real RGB - there is no
    // pre-existing Colorize on this scope's density path to reinterpret the
    // way the Vectorscope's is, so this is purely Enhanced Render's and hides
    // with the rest of that group. On by default, matching the old app.
    bool    colorize = true;

    EnhancedTraceState traceState;
    std::vector<float> preparedTrace;
    EnhancedTraceCache traceCache;
    EnhancedRunCache   enhancedRuns;

    Image   image;
    Texture texture;
    bool    dirty = true;

    // What the density texture was built from - see FalseColorState.
    uint64_t       builtFrameIndex = UINT64_MAX;
    float          builtGain       = 0.0f;
    bool           builtMasked     = false;
    SpatialMask    builtMask{};
    ValueQualifier builtQualifier{};
};

// CIE 1931 chromaticity: base (binned density) rendering only - see
// ScopeImages.h's own comment on why Enhanced Render isn't offered here yet.
struct ChromaticityState
{
    float gain          = 20.0f;
    bool  colorize       = true;
    bool  showLocus      = true;
    bool  showBlackbody  = true;

    // Enhanced Render: exact per-pixel chromaticity as a beam trace instead
    // of the binned density map - see ScopeImages.h's own comment on
    // BuildChromaticityTrace for why this reads preview pixels directly
    // rather than the shared waveform_trace mixin every other Enhanced
    // Render uses. Off by default, matching the old app.
    bool enhancedRender = false;

    // Recomputed at most once per new frame (ChromaticityAnalysis is a real
    // per-pixel pass, same reasoning App::maskedScope caches by frame index)
    // - not once per repaint, which every mouse move/zoom step would trigger.
    uint64_t              analysisFrame = UINT64_MAX;
    ChromaticityAnalysis   analysis;

    // The Enhanced Render trace, cached the same way - also invalidated by a
    // mask/qualifier change, unlike the density analysis above, since
    // BuildChromaticityTrace (unlike BuildChromaticityAnalysis) honours both.
    uint64_t                          traceFrame = UINT64_MAX;
    SpatialMask                       traceForMask;
    ValueQualifier                    traceForQualifier;
    bool                              traceHasAnyValid = false;
    std::vector<ChromaticitySample>   trace;

    Image   image;
    Texture texture;
};

// Sat vs Luma, DaVinci Resolve "Curves" style: a black->white luma gradient
// background with a plain luma population histogram drawn over it. Reuses
// the app's own existing full-precision Y-plane histogram
// (ScopeFrame::histogram / App::maskedScope.histogram) rather than running a
// fresh per-pixel pass over the preview - that data already exists every
// frame, and is more precise than anything resampled from the preview would
// be. An earlier version was a 2D density plot of real saturation against
// luma; replaced after Trevor asked for Resolve's actual look instead.
struct LumaVsSatState
{
    bool logScale = false;

    // The luma-gradient background never changes frame to frame - built
    // once, not rebuilt (and re-uploaded) every draw the way a real data
    // image would need to be.
    Texture gradientTexture;
    bool    gradientBuilt = false;
};

// Sat vs Hue, DaVinci Resolve "Curves" style: a hue-wheel background with a
// plain hue population histogram drawn over it, the same line+fill
// technique the Histogram panel itself uses - see BuildHueHistogramAnalysis's
// own comment for why this needs its own per-pixel pass (hue has no
// wire-format equivalent the way luma does).
struct HueVsSatState
{
    bool logScale = false;

    uint64_t              analysisFrame = UINT64_MAX;
    SpatialMask            analysisForMask;
    ValueQualifier         analysisForQualifier;
    HueHistogramAnalysis   analysis;

    // The hue-wheel background never changes frame to frame - built once,
    // not rebuilt (and re-uploaded) every draw the way a real data image is.
    Texture gradientTexture;
    bool    gradientBuilt = false;
};

// Classic diamond-oriented stereo scope: mid (L+R) up the screen, side (L-R)
// across it, so mono/correlated content draws a vertical line and a fully
// out-of-phase signal draws a horizontal one - the same convention every
// hardware/software goniometer uses. Reads AudioMeterBridge's own raw ring
// buffer directly (see its own comment) - the level meter's peak/RMS have
// already thrown the waveform away by the time this panel would see them.
struct GoniometerState
{
    float gain = 1.0f;   // 0.25-4: scales the trace without changing the graticule
    int   density = 2;   // 1-6: drawn dot size, same control/range Color Cube's own uses

    // Off by default: a dot cloud, one point per sample - restored as the
    // default after Trevor asked for it back having tried the connected
    // beam trace as the only option. Enhanced Render (below) is that same
    // beam trace, kept as an opt-in instead - see DrawGoniometerPanel.
    bool enhancedRender = false;
};

// Real-time frequency-distribution view, 20Hz-20kHz log-scaled - low-end
// rumble, 50/60Hz ground-loop hum, and high-frequency hiss each have a
// natural home on this axis that a level meter or goniometer can't show.
struct SpectrumAnalyzerState
{
    static constexpr int kFftSize = 4096;   // ~85ms window at 48kHz - enough low-end resolution to separate 50 from 60Hz

    float minDb = -80.0f;

    // Per-bin display value, decayed toward the current spectrum rather than
    // replaced outright - a plain per-frame FFT redraw is too jittery to
    // read (the same "raw jumpiness reads as flicker" reasoning the Audio
    // Meter's own peak/RMS ballistics exist for for), a peak-hold-style
    // decay keeps transient content visible for a beat instead of a
    // continuous strobe.
    std::vector<float> displayDb;
};

struct Panel
{
    int         id   = 0;
    PanelKind   kind = PanelKind::Waveform;
    PanelCommon common;

    SourceState      source;
    WaveformState    waveform;
    HistogramState   histogram;
    VectorscopeState vectorscope;
    FalseColorState  falseColor;
    SkinToneState    skinTone;
    NoiseState        noise;
    TimecodeState     timecode;
    QualifierPanelState qualifierPanel;
    WhiteBalanceState whiteBalance;
    ChromaticityState chromaticity;
    LumaVsSatState    lumaVsSat;
    HueVsSatState     hueVsSat;
    GoniometerState       goniometer;
    SpectrumAnalyzerState spectrumAnalyzer;
    FocusPeakingState    focusPeaking;
    BandingAnalyzerState bandingAnalyzer;
    DifferenceState      difference;
    GrainAnalyzerState   grainAnalyzer;
    CompareState         compare;
    AudioMeterState      audioMeter;
    ColorCubeState       colorCube;

    // "Waveform###panel3": ImGui takes the part after ### as the window ID and the
    // part before it as the label. That split is what lets Content change the
    // title without the window losing its place in the dock tree - the dock node
    // remembers a window by ID, and an ID change would look like a different
    // window appearing somewhere else.
    void MakeWindowName(char* p_Out, size_t p_Size) const
    {
        std::snprintf(p_Out, p_Size, "%s###panel%d", PanelKindName(kind), id);
    }
};

// Where a new section goes relative to the one it was added from.
enum class AddDirection { Left, Right, Above, Below };

// Applied between frames: docking work has to happen outside the window it
// rearranges, and a new panel cannot be pushed into the list while it is being
// iterated.
struct PendingAdd
{
    bool         active  = false;
    int          fromId  = 0;
    AddDirection dir     = AddDirection::Right;
};

enum class SubtitlePosition { Bottom, Top };

// Real font faces, not synthesized emphasis - ImGui draws whatever glyphs a
// font actually contains and has no notion of faking a bold or a slant, so
// each of these is loaded from its own face at startup (see main()).
enum class SubtitleStyle { Regular, Bold, Italic, BoldItalic, Count };

// None leaves the text sitting directly on the picture, which is only
// readable with the drop shadow on - the two settings are alternatives to
// each other more than independent toggles.
enum class SubtitleBackdrop { None, Box };

struct Preferences
{
    bool show = false;

    // Live.
    bool rememberLayoutOnClose       = true;

    // Off by default - every scope now opens in Enhanced Render (see
    // WaveformState/VectorscopeState/WhiteBalanceState's own comments), and
    // this is the one global override, for a machine where the line-trace
    // cost is the difference between smooth and choppy. Checked in addition
    // to each panel's own Enhanced Render checkbox, not instead of it - this
    // never changes what a panel's own setting says, only whether that
    // setting is allowed to draw anything this session.
    bool disableEnhancedRender = false;

    // Space brings the host's own window to the front - Resolve or Premiere,
    // whichever is the input. Not a real transport bridge (there isn't one;
    // see ERRORS.md in the old tree and this preference's own checkbox
    // tooltip for why), just a focus switch. Off by default: stealing focus
    // on a keypress is a real side effect to opt into, not something to
    // inherit silently. The name predates Premiere and stays for the saved key.
    bool spaceFocusesResolve = false;

    // Which host the app reads: 0 = Resolve (ScopeTap), 1 = Premiere Pro
    // (ScopeTransmit). Remembered, unlike Screen Capture, which always needs
    // a region picked this session.
    int  hostInput = 0;

    // Solo fills the monitor: borderless, over the taskbar, with the menu
    // bar, status bar and the panel's own tab and close button hidden. Esc
    // or un-soloing puts the window back exactly where it was.
    bool soloFullScreen = false;

    // Handed to the Premiere plugin every frame (see ScopeControl.h) - its
    // counterparts of Scope Tap's own node parameters. Preview Scale is a
    // percentage, 0 meaning automatic; Row Step is 1..16.
    int  premierePreviewScale = 0;
    int  premiereRowStep      = 1;

    bool showSubtitles          = false;

    // Subtitle overlay appearance. Sizes and offsets are percentages of the
    // fitted image's height rather than pixels, so the overlay keeps its
    // proportions as a Source panel is resized or undocked onto a different
    // display - a fixed pixel size looks right in one panel and swamps or
    // vanishes in another.
    float            subtitleSizePct     = 5.0f;    // cap height as % of image height
    float            subtitleMarginPct   = 5.0f;    // gap from the top/bottom edge
    SubtitleStyle    subtitleStyle       = SubtitleStyle::Regular;
    SubtitlePosition subtitlePosition    = SubtitlePosition::Bottom;
    float            subtitleTextColor[3]   = { 1.0f, 1.0f, 1.0f };
    SubtitleBackdrop subtitleBackdrop    = SubtitleBackdrop::Box;
    float            subtitleBoxColor[3]    = { 0.0f, 0.0f, 0.0f };
    float            subtitleBoxOpacity  = 63.0f;   // %
    bool             subtitleShadow      = false;
    float            subtitleShadowSize  = 8.0f;    // % of the text's own size

    // Waiting on subsystems this build does not have yet.
    bool persistGridViewImages  = false;   // needs the Grid View panel
    int  playbackFrameDelay     = 0;       // needs a frame history to delay against
};

// Where app.frame comes from. Every panel reads app.frame and nothing else, so
// this is the only place the two inputs differ - see ScreenCapture.h.
enum class InputSource { Resolve, Premiere, ScreenCapture };

// The two hosts publish blocks of the same layout under different names, so
// one can be read without the other overwriting it.
const char* HostShmName(InputSource p_Host)
{
    return (p_Host == InputSource::Premiere) ? kShmNamePremiere : kShmName;
}

InputSource HostFromPrefs(int p_HostInput)
{
    return (p_HostInput == 1) ? InputSource::Premiere : InputSource::Resolve;
}

struct App
{
    ScopeReader      reader;
    ScopeFrame       frame;
    bool             haveFrame = false;
    double           lastOpenAttempt = -1.0;

    // Resolve or Premiere follows prefs.hostInput every frame (see
    // RenderOneFrame) - so a host restored from the .ini takes effect without
    // any extra plumbing - unless Screen Capture is on.
    InputSource         input = InputSource::Resolve;
    std::string         readerName;      // the block reader is open on, or empty
    ControlChannel      premiereControl; // Preview Scale / Row Step for ScopeTransmit
    ScreenCaptureSource capture;
    ScreenRegion        captureRegion;   // last region picked this session

    // Frames left before the region picker opens. Not opened straight from
    // the menu click: the picker screenshots the desktop first, and the frame
    // being drawn at that moment still shows the open Input menu.
    int                 regionPickCountdown = 0;

    std::vector<Panel> panels;
    int                nextPanelId = 0;

    // -1 when nothing is soloed. A soloed panel is drawn by simply not submitting
    // the others: ImGui gives a dock node entirely to its one visible child and
    // leaves the split ratios untouched, so un-soloing restores the layout exactly.
    int                soloPanelId = -1;

    // Solo Fills the Screen (Preferences): the OS window made to cover its
    // monitor, and how it was before, to put it back. Null while no window is
    // full screen. See UpdateSoloFullScreen.
    struct FullScreenSolo
    {
        GLFWwindow* window    = nullptr;
        bool        isMain    = false;   // the main window: also drops its menu and status bar

        // How the window was, to put it back exactly - maximized included.
#ifdef _WIN32
        WINDOWPLACEMENT placement{};
        LONG_PTR        style = 0;
#else
        int         x = 0, y = 0, width = 0, height = 0;
        bool        decorated = true;
        bool        maximized = false;
#endif

        // The cross-fade around each switch: out on the window as it is,
        // switch while it is invisible, then in on the window as it becomes.
        enum class Phase { Idle, FadeOut, Hold, FadeIn };
        Phase       phase      = Phase::Idle;
        double      phaseStart = 0.0;
        int         holdFrames = 0;
        GLFWwindow* fading     = nullptr;   // the window whose opacity is moving
    };
    FullScreenSolo     fullScreen;

    PendingAdd         pendingAdd;

    // A vector, not a single id: PlatformRequestClose (the native OS "X") fires
    // for every window sharing that viewport in the same frame - a popped-out
    // window grown with Add Section holds more than one panel, and a scalar here
    // meant each one's close request overwrote the last, so only one panel ever
    // actually closed and the window never fully went away.
    std::vector<int>  closePanelIds;

    Preferences      prefs;

#ifdef __APPLE__
    // The main window's position and size, remembered across launches (see
    // PlaceMainWindow). Windows launches maximized instead and keeps nothing:
    // see main() for why the two platforms differ here.
    struct MainWindowGeometry
    {
        int  x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
        bool saved     = false;   // true once a launch has recorded anything
    };
    MainWindowGeometry mainWindow;
#endif

    bool             layoutBuilt        = false;
    bool             forceDefaultLayout = false;

    // Global, not per-panel: the spatial mask means the same region to every
    // preview panel that reads it (Source, False Color, Skin Tone), same as
    // the old app's MainWindow.spatial_mask_*. Deliberately excluded from
    // settings persistence - live grading work, not a standing preference.
    SpatialMask      spatialMask;
    MaskInputState   maskInput;

    // Value qualifiers: the Histogram panel's own per-channel drag-select
    // (rebuilt every frame from every open Histogram panel's active ranges -
    // see RebuildValueQualifier) plus the dedicated Qualifier panel's HSL
    // range (edited directly, global like spatialMask). Combined with it
    // (AND'd) wherever the spatial mask already restricts a preview or
    // masked-scope re-bin - see CombinedMaskWeight/ValueQualifierPasses.
    ValueQualifier   valueQualifier;

    // Waveform/Histogram/Vectorscope/White Balance re-binned from the preview,
    // restricted to spatialMask - see BuildMaskedScopeBins's own comment for why
    // this exists at all (the tap's own bins are already summed over the full
    // frame by the time the app sees them). Recomputed once per DrawFrame call,
    // not once per panel - every masked panel that frame reads the same result.
    MaskedScopeBins  maskedScope;
    bool             maskedScopeValid    = false;
    uint64_t         maskedScopeFrame    = UINT64_MAX;
    SpatialMask      maskedScopeForMask;
    ValueQualifier   maskedScopeForQualifier;

    // Layout Presets (General menu): named snapshots of the whole ImGui .ini -
    // dock structure, window positions and every panel setting the settings
    // handler above already knows how to write/read, since a preset is exactly
    // what gets written to layout.ini, just saved under a name instead of
    // overwritten by the next autosave. Applying one is a structural change
    // (LoadIniSettingsFromMemory rebuilds the dock tree), so it is queued here
    // and applied at the same point in the frame as pendingAdd/closePanelIds -
    // never from inside the menu that requested it.
    bool             showSavePresetDialog = false;
    char             savePresetNameBuf[64] = "";
    std::string      pendingLoadPreset;
    std::string      confirmDeletePreset;
};

// Recomputes App::maskedScope when the mask is on and either a new frame
// arrived or the mask itself changed (mid-drag) since the last compute -
// otherwise leaves the cached result alone, since re-binning is real
// per-pixel work and every masked panel this frame shares one result. Call
// once per DrawFrame, before any panel reads maskedScope.
// Rebuilds App::valueQualifier's channel half from every open Histogram
// panel's own active per-plane ranges, ANDing them together - two panels both
// qualifying, say, Red just narrows to the intersection of their two ranges,
// which is exactly what ANDing two range tests already means. Leaves the HSL
// half untouched: that one is edited directly (global, like spatialMask), not
// derived from any panel. Call once per frame, before EnsureMaskedScopeBins.
void RebuildValueQualifier(App& p_App)
{
    ChannelQualifier combined;
    for (uint32_t plane = 0; plane < kPlaneCount; ++plane)
    {
        float lo = 0.0f, hi = 0.0f;
        bool  any = false;
        for (const Panel& panel : p_App.panels)
        {
            if (panel.kind != PanelKind::Histogram) continue;
            const HistogramState& hs = panel.histogram;
            if (!hs.qualifyActive[plane]) continue;
            if (!any) { lo = hs.qualifyLo[plane]; hi = hs.qualifyHi[plane]; any = true; }
            else      { lo = std::max(lo, hs.qualifyLo[plane]); hi = std::min(hi, hs.qualifyHi[plane]); }
        }
        combined.active[plane] = any;
        combined.lo[plane]     = lo;
        combined.hi[plane]     = hi;
    }
    p_App.valueQualifier.channel = combined;
}

void EnsureMaskedScopeBins(App& p_App)
{
    if (!p_App.spatialMask.hasGeometry && !p_App.valueQualifier.AnyActive())
    {
        p_App.maskedScopeValid = false;
        return;
    }

    const bool frameChanged     = p_App.maskedScopeFrame != p_App.frame.frameIndex;
    const bool maskChanged      = !SpatialMaskEquals(p_App.maskedScopeForMask, p_App.spatialMask);
    const bool qualifierChanged = !ValueQualifierEquals(p_App.maskedScopeForQualifier, p_App.valueQualifier);

    if (p_App.maskedScopeValid && !frameChanged && !maskChanged && !qualifierChanged) return;

    BuildMaskedScopeBins(p_App.frame, p_App.spatialMask, p_App.maskedScope, &p_App.valueQualifier);
    p_App.maskedScopeFrame        = p_App.frame.frameIndex;
    p_App.maskedScopeForMask      = p_App.spatialMask;
    p_App.maskedScopeForQualifier = p_App.valueQualifier;
    p_App.maskedScopeValid        = true;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
//
// Every panel setting and preference is written into the same file ImGui already
// uses for the dock layout, through ImGui's own settings-handler mechanism. That
// is deliberate rather than a second settings file: one file, one save timer, one
// lifecycle, and "Remember Layout and Settings on Close" governs all of it at once.
//
// ImGui loads the .ini during the first NewFrame, so values are back in place
// before any panel draws its first frame.

enum class FieldType { Bool, Int, Float };

struct SettingField
{
    const char* key;
    FieldType   type;
    void*       ptr;
};

// The enums below are stored as ints. Their underlying type is int, so a cast
// through int* is well-defined in practice; the asserts make a future change of
// underlying type fail loudly here rather than silently corrupt a saved file.
static_assert(sizeof(WaveformMode)  == sizeof(int), "WaveformMode must round-trip as int");
static_assert(sizeof(WaveformRange) == sizeof(int), "WaveformRange must round-trip as int");
static_assert(sizeof(ScaleMode)     == sizeof(int), "ScaleMode must round-trip as int");
static_assert(sizeof(VectorRange)   == sizeof(int), "VectorRange must round-trip as int");
static_assert(sizeof(HistogramChannels) == sizeof(int), "HistogramChannels must round-trip as int");
static_assert(sizeof(PanelKind)     == sizeof(int), "PanelKind must round-trip as int");
static_assert(sizeof(PeakColor)     == sizeof(int), "PeakColor must round-trip as int");
static_assert(sizeof(CompareOverlay) == sizeof(int), "CompareOverlay must round-trip as int");
static_assert(sizeof(ColorCubeSpace) == sizeof(int), "ColorCubeSpace must round-trip as int");

void CollectCommonFields(PanelCommon& p_Common, const char* p_Prefix,
                         std::vector<SettingField>& p_Out,
                         std::deque<std::string>& p_KeyStorage)
{
    auto key = [&](const char* p_Name) -> const char*
    {
        p_KeyStorage.push_back(std::string(p_Prefix) + "." + p_Name);
        return p_KeyStorage.back().c_str();
    };

    p_Out.push_back({ key("graticule"),  FieldType::Float, &p_Common.graticuleBrightness });
    p_Out.push_back({ key("zoomable"),   FieldType::Bool,  &p_Common.zoomable });
    p_Out.push_back({ key("boundLines"), FieldType::Bool,  &p_Common.boundingLinesEnabled });

    for (int i = 0; i < 4; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "line%dOn", i);
        p_Out.push_back({ key(name), FieldType::Bool, &p_Common.boundingLines[i].enabled });

        std::snprintf(name, sizeof(name), "line%dCode", i);
        p_Out.push_back({ key(name), FieldType::Float, &p_Common.boundingLines[i].code });

        for (int c = 0; c < 3; ++c)
        {
            std::snprintf(name, sizeof(name), "line%dCol%d", i, c);
            p_Out.push_back({ key(name), FieldType::Float, &p_Common.boundingLines[i].color[c] });
        }
    }
}

// Rebuilt on demand rather than cached, because the key strings are owned by
// p_KeyStorage and every SettingField::key below is a raw pointer into one of
// its elements.
//
// p_KeyStorage is a std::deque, not a std::vector, specifically so those
// pointers stay valid as more keys are pushed: a vector's push_back can
// reallocate its whole backing buffer once capacity runs out, which would
// silently dangle every pointer already handed out for earlier keys in the
// very same call - exactly the bug that hit Trevor's real layout.ini
// (p_KeyStorage.reserve(64 + panels*96) was sized for an older, smaller
// per-panel field count; each new panel type's settings added since then
// pushed the real count past that estimate, the vector reallocated
// mid-build, and "kind" - captured before the reallocation - silently
// stopped comparing against the real key, so most panels lost track of what
// they were and read back at the freshly-constructed Panel's default kind).
// A deque never has this failure mode: push_back only ever allocates a new
// block and never moves already-constructed elements, so a reference or
// pointer to an existing element survives any amount of further growth. No
// reserve() call is needed (or possible - deque doesn't have one) because
// there is no capacity to run out of.
void CollectSettingFields(App& p_App,
                          std::vector<SettingField>& p_Out,
                          std::deque<std::string>& p_KeyStorage)
{
    p_Out.clear();
    p_KeyStorage.clear();

    // --- Preferences -------------------------------------------------------
    p_Out.push_back({ "prefs.rememberLayout", FieldType::Bool, &p_App.prefs.rememberLayoutOnClose });
    p_Out.push_back({ "prefs.disableEnhanced", FieldType::Bool, &p_App.prefs.disableEnhancedRender });
    p_Out.push_back({ "prefs.spaceFocusesResolve", FieldType::Bool, &p_App.prefs.spaceFocusesResolve });
    p_Out.push_back({ "prefs.hostInput",     FieldType::Int,  &p_App.prefs.hostInput });
    p_Out.push_back({ "prefs.soloFullScreen", FieldType::Bool, &p_App.prefs.soloFullScreen });
#ifdef __APPLE__
    p_Out.push_back({ "window.saved",     FieldType::Bool, &p_App.mainWindow.saved });
    p_Out.push_back({ "window.x",         FieldType::Int,  &p_App.mainWindow.x });
    p_Out.push_back({ "window.y",         FieldType::Int,  &p_App.mainWindow.y });
    p_Out.push_back({ "window.width",     FieldType::Int,  &p_App.mainWindow.width });
    p_Out.push_back({ "window.height",    FieldType::Int,  &p_App.mainWindow.height });
    p_Out.push_back({ "window.maximized", FieldType::Bool, &p_App.mainWindow.maximized });
#endif
    p_Out.push_back({ "prefs.prPreviewScale", FieldType::Int,  &p_App.prefs.premierePreviewScale });
    p_Out.push_back({ "prefs.prRowStep",      FieldType::Int,  &p_App.prefs.premiereRowStep });
    p_Out.push_back({ "prefs.showSubtitles", FieldType::Bool, &p_App.prefs.showSubtitles });
    p_Out.push_back({ "prefs.subSize",    FieldType::Float, &p_App.prefs.subtitleSizePct });
    p_Out.push_back({ "prefs.subMargin",  FieldType::Float, &p_App.prefs.subtitleMarginPct });
    p_Out.push_back({ "prefs.subPos",     FieldType::Int,
                      reinterpret_cast<int*>(&p_App.prefs.subtitlePosition) });
    p_Out.push_back({ "prefs.subStyle",   FieldType::Int,
                      reinterpret_cast<int*>(&p_App.prefs.subtitleStyle) });
    p_Out.push_back({ "prefs.subBackdrop", FieldType::Int,
                      reinterpret_cast<int*>(&p_App.prefs.subtitleBackdrop) });
    p_Out.push_back({ "prefs.subBoxOpacity", FieldType::Float, &p_App.prefs.subtitleBoxOpacity });
    p_Out.push_back({ "prefs.subShadow",     FieldType::Bool,  &p_App.prefs.subtitleShadow });
    p_Out.push_back({ "prefs.subShadowSize", FieldType::Float, &p_App.prefs.subtitleShadowSize });
    for (int c = 0; c < 3; ++c)
    {
        static const char* kTextKeys[3] = { "prefs.subTextR", "prefs.subTextG", "prefs.subTextB" };
        static const char* kBoxKeys[3]  = { "prefs.subBoxR",  "prefs.subBoxG",  "prefs.subBoxB"  };
        p_Out.push_back({ kTextKeys[c], FieldType::Float, &p_App.prefs.subtitleTextColor[c] });
        p_Out.push_back({ kBoxKeys[c],  FieldType::Float, &p_App.prefs.subtitleBoxColor[c] });
    }
    p_Out.push_back({ "deck.solo",            FieldType::Int,  &p_App.soloPanelId });

    // --- Per panel ---------------------------------------------------------
    //
    // Keyed by the panel's own id rather than its position, so a section closed in
    // the middle does not silently shift every later panel's settings onto the
    // wrong section.
    for (Panel& panel : p_App.panels)
    {
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "p%d", panel.id);

        auto key = [&](const char* p_Name) -> const char*
        {
            p_KeyStorage.push_back(std::string(prefix) + "." + p_Name);
            return p_KeyStorage.back().c_str();
        };

        p_Out.push_back({ key("kind"), FieldType::Int, reinterpret_cast<int*>(&panel.kind) });

        WaveformState& wf = panel.waveform;
        p_Out.push_back({ key("wf.mode"),      FieldType::Int,   reinterpret_cast<int*>(&wf.mode) });
        p_Out.push_back({ key("wf.range"),     FieldType::Int,   reinterpret_cast<int*>(&wf.range) });
        p_Out.push_back({ key("wf.scale"),     FieldType::Int,   reinterpret_cast<int*>(&wf.scale) });
        p_Out.push_back({ key("wf.gain"),      FieldType::Float, &wf.gain });
        p_Out.push_back({ key("wf.enhanced"),  FieldType::Bool,  &wf.enhancedRender });
        p_Out.push_back({ key("wf.smooth"),    FieldType::Bool,  &wf.smoothTrace });
        p_Out.push_back({ key("wf.lowPass"),   FieldType::Float, &wf.lowPassKernel });
        p_Out.push_back({ key("wf.prefilter"), FieldType::Bool,  &wf.signalPrefilter });
        p_Out.push_back({ key("wf.colorize"),  FieldType::Bool,  &wf.colorize });
        p_Out.push_back({ key("wf.cursor"),    FieldType::Bool,  &wf.showCursor });

        HistogramState& hs = panel.histogram;
        p_Out.push_back({ key("hs.channels"), FieldType::Int, reinterpret_cast<int*>(&hs.channels) });
        p_Out.push_back({ key("hs.showY"), FieldType::Bool, &hs.showY });
        p_Out.push_back({ key("hs.showR"), FieldType::Bool, &hs.showR });
        p_Out.push_back({ key("hs.showG"), FieldType::Bool, &hs.showG });
        p_Out.push_back({ key("hs.showB"), FieldType::Bool, &hs.showB });
        p_Out.push_back({ key("hs.log"),    FieldType::Bool, &hs.logScale });
        p_Out.push_back({ key("hs.cursor"), FieldType::Bool, &hs.showCursor });

        ChromaticityState& ch = panel.chromaticity;
        p_Out.push_back({ key("ch.gain"),      FieldType::Float, &ch.gain });
        p_Out.push_back({ key("ch.colorize"),  FieldType::Bool,  &ch.colorize });
        p_Out.push_back({ key("ch.locus"),     FieldType::Bool,  &ch.showLocus });
        p_Out.push_back({ key("ch.blackbody"), FieldType::Bool,  &ch.showBlackbody });
        p_Out.push_back({ key("ch.enhanced"),  FieldType::Bool,  &ch.enhancedRender });

        p_Out.push_back({ key("sl.log"), FieldType::Bool, &panel.lumaVsSat.logScale });
        p_Out.push_back({ key("sh.log"), FieldType::Bool, &panel.hueVsSat.logScale });

        p_Out.push_back({ key("gon.gain"),     FieldType::Float, &panel.goniometer.gain });
        p_Out.push_back({ key("gon.density"),  FieldType::Int,   &panel.goniometer.density });
        p_Out.push_back({ key("gon.enhanced"), FieldType::Bool,  &panel.goniometer.enhancedRender });
        p_Out.push_back({ key("spa.minDb"), FieldType::Float, &panel.spectrumAnalyzer.minDb });

        VectorscopeState& vs = panel.vectorscope;
        p_Out.push_back({ key("vs.range"),         FieldType::Int,   reinterpret_cast<int*>(&vs.range) });
        p_Out.push_back({ key("vs.gain"),          FieldType::Float, &vs.gain });
        p_Out.push_back({ key("vs.colorize"),      FieldType::Bool,  &vs.colorize });
        p_Out.push_back({ key("vs.targets"),       FieldType::Bool,  &vs.showTargets });
        p_Out.push_back({ key("vs.targetSize"),    FieldType::Float, &vs.targetSize });
        p_Out.push_back({ key("vs.targetOpacity"), FieldType::Float, &vs.targetOpacity });
        p_Out.push_back({ key("vs.skinLine"),      FieldType::Bool,  &vs.showSkinLine });
        p_Out.push_back({ key("vs.zoom2x"),        FieldType::Bool,  &vs.zoom2x });
        p_Out.push_back({ key("vs.enhanced"),      FieldType::Bool,  &vs.enhancedRender });
        p_Out.push_back({ key("vs.smooth"),        FieldType::Bool,  &vs.smoothTrace });
        p_Out.push_back({ key("vs.lowPass"),       FieldType::Float, &vs.lowPassKernel });
        p_Out.push_back({ key("vs.prefilter"),     FieldType::Bool,  &vs.signalPrefilter });

        FalseColorState& fc = panel.falseColor;
        p_Out.push_back({ key("fc.mode"),      FieldType::Int,  reinterpret_cast<int*>(&fc.mode) });
        p_Out.push_back({ key("fc.showScale"), FieldType::Bool, &fc.showScale });

        SkinToneState& sk = panel.skinTone;
        p_Out.push_back({ key("sk.model"),     FieldType::Int,   reinterpret_cast<int*>(&sk.model) });
        p_Out.push_back({ key("sk.colorMode"), FieldType::Int,   reinterpret_cast<int*>(&sk.colorMode) });
        p_Out.push_back({ key("sk.limits"),    FieldType::Bool,  &sk.limitsEnabled });
        p_Out.push_back({ key("sk.grey"),      FieldType::Bool,  &sk.greyNonSkin });
        p_Out.push_back({ key("sk.tolerance"), FieldType::Float, &sk.tolerance });
        p_Out.push_back({ key("sk.hueMin"),    FieldType::Float, &sk.hueMin });
        p_Out.push_back({ key("sk.hueMax"),    FieldType::Float, &sk.hueMax });
        p_Out.push_back({ key("sk.satMin"),    FieldType::Float, &sk.satMin });
        p_Out.push_back({ key("sk.satMax"),    FieldType::Float, &sk.satMax });
        p_Out.push_back({ key("sk.lumaMin"),   FieldType::Float, &sk.lumaMin });
        p_Out.push_back({ key("sk.lumaMax"),   FieldType::Float, &sk.lumaMax });
        for (int c = 0; c < 3; ++c)
        {
            char name[24];
            std::snprintf(name, sizeof(name), "sk.solid%d", c);
            p_Out.push_back({ key(name), FieldType::Float, &sk.solidColor[c] });
            std::snprintf(name, sizeof(name), "sk.gradA%d", c);
            p_Out.push_back({ key(name), FieldType::Float, &sk.gradientColorA[c] });
            std::snprintf(name, sizeof(name), "sk.gradB%d", c);
            p_Out.push_back({ key(name), FieldType::Float, &sk.gradientColorB[c] });
        }

        WhiteBalanceState& wb = panel.whiteBalance;
        p_Out.push_back({ key("wb.gain"),       FieldType::Float, &wb.gain });
        p_Out.push_back({ key("wb.enhanced"),   FieldType::Bool,  &wb.enhancedRender });
        p_Out.push_back({ key("wb.smooth"),     FieldType::Bool,  &wb.smoothTrace });
        p_Out.push_back({ key("wb.lowPass"),    FieldType::Float, &wb.lowPassKernel });
        p_Out.push_back({ key("wb.prefilter"),  FieldType::Bool,  &wb.signalPrefilter });
        p_Out.push_back({ key("wb.colorize"),   FieldType::Bool,  &wb.colorize });

        FocusPeakingState& fp = panel.focusPeaking;
        p_Out.push_back({ key("fp.threshold"), FieldType::Float, &fp.threshold });
        p_Out.push_back({ key("fp.grayBg"),    FieldType::Bool,  &fp.grayscaleBg });
        p_Out.push_back({ key("fp.color"),     FieldType::Int,   reinterpret_cast<int*>(&fp.color) });

        BandingAnalyzerState& ba = panel.bandingAnalyzer;
        p_Out.push_back({ key("ba.gain"), FieldType::Float, &ba.gain });

        DifferenceState& df = panel.difference;
        p_Out.push_back({ key("df.gain"), FieldType::Float, &df.gain });

        GrainAnalyzerState& ga = panel.grainAnalyzer;
        p_Out.push_back({ key("ga.gain"), FieldType::Float, &ga.gain });

        // The still itself is live-captured/loaded pixel data, not a setting -
        // same reasoning as Difference's own still, deliberately left off.
        CompareState& cmp = panel.compare;
        p_Out.push_back({ key("cmp.overlay"), FieldType::Int,   reinterpret_cast<int*>(&cmp.overlay) });
        p_Out.push_back({ key("cmp.opacity"), FieldType::Float, &cmp.overlayOpacity });

        // heldPeakDb/heldPeakAtSec are live meter state, not a setting - same
        // reasoning as Compare's own still, deliberately left off.
        AudioMeterState& am = panel.audioMeter;
        p_Out.push_back({ key("am.peakHold"), FieldType::Bool, &am.peakHold });

        // Matches the old app's own PERSISTED_SETTINGS = ("space", "spread",
        // "density") for this panel - the camera (yaw/pitch/zoom/pan) is
        // deliberately excluded there too, same reasoning as everywhere else
        // a view/orbit is transient rather than a setting.
        ColorCubeState& cc = panel.colorCube;
        p_Out.push_back({ key("cc.space"),   FieldType::Int, reinterpret_cast<int*>(&cc.space) });
        p_Out.push_back({ key("cc.spread"),  FieldType::Int, &cc.spread });
        p_Out.push_back({ key("cc.density"), FieldType::Int, &cc.density });
        p_Out.push_back({ key("cc.invertX"), FieldType::Bool, &cc.invertX });
        p_Out.push_back({ key("cc.invertY"), FieldType::Bool, &cc.invertY });

        SourceState& src = panel.source;
        p_Out.push_back({ key("src.grid"), FieldType::Int, reinterpret_cast<int*>(&src.grid) });
        p_Out.push_back({ key("src.spiralCorner"), FieldType::Int,
                          reinterpret_cast<int*>(&src.spiralCorner) });

        CollectCommonFields(panel.common, prefix, p_Out, p_KeyStorage);
    }

    // Deliberately NOT saved: ZoomPan (a transient view, and reopening zoomed into
    // a corner with no memory of why would be baffling) and showSettings (settings
    // windows should not reopen themselves on launch).
}

// The handler's UserData carries the App, so nothing here needs a global.
struct SettingsStore
{
    App*                      app = nullptr;
    std::vector<SettingField> fields;
    std::deque<std::string>   keyStorage;

    void Refresh() { if (app) CollectSettingFields(*app, fields, keyStorage); }
};

SettingsStore g_SettingsStore;

void* ScopeDeckSettings_ReadOpen(ImGuiContext*, ImGuiSettingsHandler* p_Handler, const char* p_Name)
{
    // One entry, named "State". Returning non-null tells ImGui to feed us lines.
    if (std::strcmp(p_Name, "State") != 0) return nullptr;
    return p_Handler->UserData;
}

void ScopeDeckSettings_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* p_Entry, const char* p_Line)
{
    SettingsStore* store = static_cast<SettingsStore*>(p_Entry);
    if (!store || !store->app) return;

    if (store->fields.empty()) store->Refresh();

    const char* eq = std::strchr(p_Line, '=');
    if (!eq) return;

    // The roster line rebuilds the panel list before any per-panel key arrives.
    // Ids are preserved, not renumbered, so the keys that follow still match.
    if (std::strncmp(p_Line, "deck.panels=", 12) == 0)
    {
        store->app->panels.clear();
        store->app->nextPanelId = 0;

        const char* cursor = p_Line + 12;
        while (*cursor)
        {
            const int id = std::atoi(cursor);

            Panel panel;
            panel.id = id;
            store->app->panels.push_back(std::move(panel));
            store->app->nextPanelId = (id + 1 > store->app->nextPanelId)
                                      ? id + 1 : store->app->nextPanelId;

            const char* comma = std::strchr(cursor, ',');
            if (!comma) break;
            cursor = comma + 1;
        }

        // The field table names panels by id, so it has to be rebuilt now that the
        // roster has changed - otherwise every following line is looked up against
        // the old panels and silently dropped.
        store->Refresh();
        return;
    }

    const size_t keyLen = size_t(eq - p_Line);
    const char*  value  = eq + 1;

    for (const SettingField& f : store->fields)
    {
        if (std::strlen(f.key) != keyLen) continue;
        if (std::strncmp(f.key, p_Line, keyLen) != 0) continue;

        switch (f.type)
        {
            case FieldType::Bool:  *static_cast<bool*>(f.ptr)  = (std::atoi(value) != 0); break;
            case FieldType::Int:   *static_cast<int*>(f.ptr)   = std::atoi(value);        break;
            case FieldType::Float: *static_cast<float*>(f.ptr) = float(std::atof(value)); break;
        }
        return;
    }

    // An unknown key is a setting from a newer or older build. Ignored rather than
    // treated as corruption - dropping one stale line should never cost the user
    // their whole layout.
}

void ScopeDeckSettings_WriteAll(ImGuiContext*, ImGuiSettingsHandler* p_Handler, ImGuiTextBuffer* p_Buf)
{
    SettingsStore* store = static_cast<SettingsStore*>(p_Handler->UserData);
    if (!store || !store->app) return;

    store->Refresh();

    p_Buf->appendf("[%s][State]\n", p_Handler->TypeName);

    // The roster first: the per-panel keys below are meaningless until the panels
    // they name exist, and ReadLine below recreates them when it sees this.
    p_Buf->append("deck.panels=");
    for (size_t i = 0; i < store->app->panels.size(); ++i)
        p_Buf->appendf("%s%d", i ? "," : "", store->app->panels[i].id);
    p_Buf->append("\n");

    for (const SettingField& f : store->fields)
    {
        switch (f.type)
        {
            case FieldType::Bool:  p_Buf->appendf("%s=%d\n", f.key, *static_cast<bool*>(f.ptr) ? 1 : 0); break;
            case FieldType::Int:   p_Buf->appendf("%s=%d\n", f.key, *static_cast<int*>(f.ptr));          break;
            case FieldType::Float: p_Buf->appendf("%s=%.4f\n", f.key, *static_cast<float*>(f.ptr));      break;
        }
    }
    p_Buf->append("\n");
}

// Must be called after CreateContext and before the first NewFrame, which is when
// ImGui reads the .ini.
void InstallSettingsHandler(App& p_App)
{
    g_SettingsStore.app = &p_App;
    g_SettingsStore.Refresh();

    ImGuiSettingsHandler handler;
    handler.TypeName   = "ScopeDeck";
    handler.TypeHash   = ImHashStr("ScopeDeck");
    handler.ReadOpenFn = ScopeDeckSettings_ReadOpen;
    handler.ReadLineFn = ScopeDeckSettings_ReadLine;
    handler.WriteAllFn = ScopeDeckSettings_WriteAll;
    handler.UserData   = &g_SettingsStore;

    ImGui::AddSettingsHandler(&handler);
}

// Panels change settings through ordinary widgets, which ImGui does not know are
// settings - so nothing marks the file dirty on its own. Called once a frame with
// a hash of the saved values, it asks for a save only when something actually
// changed. Without it, a setting changed and then quit within the save interval
// would be lost.
void MarkSettingsDirtyIfChanged()
{
    if (!g_SettingsStore.app) return;

    g_SettingsStore.Refresh();

    ImGuiID hash = 0;
    for (const SettingField& f : g_SettingsStore.fields)
    {
        switch (f.type)
        {
            case FieldType::Bool:
            {
                const int v = *static_cast<bool*>(f.ptr) ? 1 : 0;
                hash = ImHashData(&v, sizeof(v), hash);
                break;
            }
            case FieldType::Int:
                hash = ImHashData(f.ptr, sizeof(int), hash);
                break;
            case FieldType::Float:
                hash = ImHashData(f.ptr, sizeof(float), hash);
                break;
        }
    }

    static ImGuiID s_LastHash = 0;
    static bool    s_Primed   = false;

    if (s_Primed && hash != s_LastHash)
        ImGui::MarkIniSettingsDirty();

    s_LastHash = hash;
    s_Primed   = true;
}

// ---------------------------------------------------------------------------
// Layout Presets (General menu)
// ---------------------------------------------------------------------------
//
// A preset is just a named snapshot of the same .ini text the settings handler
// above already writes to layout.ini every autosave - dock structure, window
// positions/viewports, and every panel setting. Saving one is
// SaveIniSettingsToDisk under a different filename; loading one is
// LoadIniSettingsFromDisk, the exact call ImGui itself makes to read
// layout.ini at startup, which is what gets the whole dock tree, every
// window's position, and this app's own "[ScopeDeck][State]" section (see
// ScopeDeckSettings_ReadLine above, whose "deck.panels=" line rebuilds
// p_App.panels to match) restored for free, instead of a second hand-rolled
// serialization of "which content sits where" like the old Python app's
// _serialize_layout needed.

std::string GetPresetsDir()
{
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (base && *base)
    {
        const std::string root = std::string(base) + "\\ScopeDeck";
        const std::string dir  = root + "\\presets";
        ::CreateDirectoryA(root.c_str(), nullptr);   // harmless if it exists
        ::CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
#elif defined(__APPLE__)
    // ~/Library/Application Support/Scope Deck/presets - beside layout.ini.
    const std::string dir = mac::AppSupportDir() + "/presets";
    ::mkdir(dir.c_str(), 0755);   // harmless if it exists
    return dir;
#else
    const char* home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.config/scopedeck_presets";
#endif
    return "scopedeck_presets";   // last resort, old behaviour
}

// Preset names become filenames as-is, so anything a filesystem would choke on
// (path separators, a drive-letter colon, ...) is dropped rather than
// rejected outright - the old Python app never validated the name it got back
// from its own input dialog either.
std::string SanitizePresetFileName(const std::string& p_Name)
{
    std::string out;
    for (unsigned char c : p_Name)
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_')
            out.push_back(char(c));
    return out;
}

std::string PresetPath(const std::string& p_Name)
{
#ifdef _WIN32
    const char* sep = "\\";
#else
    const char* sep = "/";
#endif
    return GetPresetsDir() + sep + SanitizePresetFileName(p_Name) + ".ini";
}

std::vector<std::string> ListLayoutPresets()
{
    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE h = ::FindFirstFileA((GetPresetsDir() + "\\*.ini").c_str(), &findData);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string name = findData.cFileName;
            if (name.size() > 4) names.push_back(name.substr(0, name.size() - 4));
        } while (::FindNextFileA(h, &findData));
        ::FindClose(h);
    }
#else
    if (DIR* dir = ::opendir(GetPresetsDir().c_str()))
    {
        while (dirent* entry = ::readdir(dir))
        {
            const std::string name = entry->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0)
                names.push_back(name.substr(0, name.size() - 4));
        }
        ::closedir(dir);
    }
#endif
    std::sort(names.begin(), names.end());
    return names;
}

void SaveLayoutPreset(const std::string& p_Name)
{
    // Just a filename, not io.IniFilename: that would make ImGui start
    // autosaving *over* the preset on its own timer instead of the real
    // layout.ini. This one call always captures the current in-memory state,
    // not whatever was on disk the last time the autosave timer fired.
    ImGui::SaveIniSettingsToDisk(PresetPath(p_Name).c_str());
}

void DeleteLayoutPreset(const std::string& p_Name)
{
    std::remove(PresetPath(p_Name).c_str());
}

// Deferred the same way ApplyPendingAdd/ApplyPendingClose are: LoadIniSettingsFromDisk
// has ImGui's own dock settings handler clear and rebuild the whole dock tree, which
// is exactly the kind of structural change that is not safe from inside the menu item
// that requested it, mid-submission.
void ApplyPendingLoadPreset(App& p_App)
{
    if (p_App.pendingLoadPreset.empty()) return;
    const std::string name = p_App.pendingLoadPreset;
    p_App.pendingLoadPreset.clear();

    ImGui::LoadIniSettingsFromDisk(PresetPath(name).c_str());
}

// ---------------------------------------------------------------------------
// Shared drawing helpers
// ---------------------------------------------------------------------------

// The rectangle a panel actually plots into: the content region, inset so the
// graticule's outer line is not clipped by the panel border.
ImRect PlotRect(float p_Inset = 1.0f)
{
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 32.0f);
    size.y = std::max(size.y, 32.0f);
    return ImRect(ImVec2(lo.x + p_Inset, lo.y + p_Inset),
                  ImVec2(lo.x + size.x - p_Inset, lo.y + size.y - p_Inset));
}

void DrawPanelBackground(ImDrawList* p_Draw, const ImRect& p_Rect)
{
    p_Draw->AddRectFilled(p_Rect.Min, p_Rect.Max, kColBackground);
}

// A masked Waveform/Histogram/Vectorscope/White Balance that legitimately has
// nothing to show (the mask kept zero pixels - an inverted mask that swallows
// the whole frame, say) draws a blank scope with nothing wrong on screen to
// explain why. Called after a masked panel's own content, so this sits above
// an empty graticule rather than under it.
void DrawMaskEmptyNotice(ImDrawList* p_Draw, const ImRect& p_Rect)
{
    const char* msg = "Spatial mask matches no pixels.";
    const ImVec2 size = ImGui::CalcTextSize(msg);
    p_Draw->AddText(ImVec2(p_Rect.GetCenter().x - size.x * 0.5f, p_Rect.GetCenter().y - size.y * 0.5f),
                    kColMuted, msg);
}

// A right-aligned label just inside the plot's left edge, for IRE/value gridlines.
void DrawAxisLabel(ImDrawList* p_Draw, ImVec2 p_At, const char* p_Text)
{
    const ImVec2 size = ImGui::CalcTextSize(p_Text);
    p_Draw->AddText(ImVec2(p_At.x, p_At.y - size.y * 0.5f), kColAxisLabel, p_Text);
}

// Fits w x h inside the rect, preserving aspect. Scopes and video both need this
// and it is the kind of arithmetic that is quietly wrong in one of two copies.
ImRect FitAspect(const ImRect& p_Into, float p_Width, float p_Height)
{
    if (p_Width <= 0.0f || p_Height <= 0.0f) return p_Into;

    const float availW = p_Into.GetWidth();
    const float availH = p_Into.GetHeight();
    const float scale  = std::min(availW / p_Width, availH / p_Height);

    const float w = p_Width * scale;
    const float h = p_Height * scale;
    const float x = p_Into.Min.x + (availW - w) * 0.5f;
    const float y = p_Into.Min.y + (availH - h) * 0.5f;

    return ImRect(ImVec2(x, y), ImVec2(x + w, y + h));
}

// ---------------------------------------------------------------------------
// Waveform
// ---------------------------------------------------------------------------

void WaveformLevelSpan(const ScopeFrame& p_Frame, WaveformRange p_Range, int& p_Lo, int& p_Hi)
{
    const int levels = int(kWaveformLevels);

    if (p_Range == WaveformRange::Full)
    {
        p_Lo = 0;
        p_Hi = levels;
        return;
    }

    // Shadows and Highlights are the bottom/top 15% of the *nominal 0-1 signal*,
    // not of the bin range - but each still runs out to the bin range's own
    // extreme on its own side, so clipping at the edge stays visible while
    // zoomed. That asymmetry is deliberate and is what the old app does.
    const float loLevel = p_Frame.LevelOfValue(0.0f);
    const float hiLevel = p_Frame.LevelOfValue(1.0f);
    const float span    = (hiLevel - loLevel) * 0.15f;

    if (p_Range == WaveformRange::Shadows)
    {
        p_Lo = 0;
        p_Hi = std::clamp(int(std::lround(loLevel + span)), 1, levels);
    }
    else
    {
        p_Lo = std::clamp(int(std::lround(hiLevel - span)), 0, levels - 1);
        p_Hi = levels;
    }
}

// Where a working-space value sits vertically in the plot, 0 at the bottom.
// Returns false when it falls outside the level span the Range control selected.
bool WaveformValueToY(const ScopeFrame& p_Frame, const ImRect& p_Rect,
                      int p_Lo, int p_Hi, float p_Value, float& p_OutY)
{
    const float level = p_Frame.LevelOfValue(p_Value);
    if (level < float(p_Lo) || level > float(p_Hi)) return false;

    const float t = (level - float(p_Lo)) / float(p_Hi - p_Lo);
    p_OutY = p_Rect.Max.y - t * p_Rect.GetHeight();
    return true;
}

void DrawWaveformGraticule(ImDrawList* p_Draw, const ImRect& p_Rect, const ImRect& p_Panel,
                           const WaveformState& p_St, const PanelCommon& p_Common,
                           const ScopeFrame& p_Frame, int p_Lo, int p_Hi, int p_Cells)
{
    const float brightness = p_Common.graticuleBrightness;
    const bool  legalScale = (p_St.scale == ScaleMode::LegalRange);

    // Every 20 IRE, placed by the value each mark corresponds to - so switching
    // Full/Legal moves the lines rather than just relabelling them, which is the
    // whole point of the control.
    for (int ire = 0; ire <= 100; ire += 20)
    {
        float y = 0.0f;
        if (!WaveformValueToY(p_Frame, p_Rect, p_Lo, p_Hi, ValueOfIre(float(ire), legalScale), y))
            continue;

        // 0 and 100 are the limits that matter, so they are the saturated lines;
        // the subdivisions are faint enough to read the trace past.
        const bool major = (ire == 0 || ire == 100);
        p_Draw->AddLine(ImVec2(p_Rect.Min.x, y), ImVec2(p_Rect.Max.x, y),
                        GraticuleColor(major ? kColGratMajor : kColGratMinor, brightness),
                        1.0f);

        // Labels live in the gutter left of the plot and stay pinned there while
        // the plot zooms - a zoomed scope whose numbers scrolled away is a
        // picture, not an instrument.
        if (y < p_Panel.Min.y || y > p_Panel.Max.y) continue;

        char label[8];
        std::snprintf(label, sizeof(label), "%d", ire);
        const ImVec2 size = ImGui::CalcTextSize(label);
        p_Draw->AddText(ImVec2(p_Rect.Min.x - size.x - 6.0f, y - size.y * 0.5f),
                        GraticuleColor(kColGratLabel, brightness), label);
    }

    // Parade cell separators.
    for (int i = 1; i < p_Cells; ++i)
    {
        const float x = p_Rect.Min.x + p_Rect.GetWidth() * float(i) / float(p_Cells);
        p_Draw->AddLine(ImVec2(x, p_Rect.Min.y), ImVec2(x, p_Rect.Max.y),
                        GraticuleColor(kColBorder, brightness), 1.0f);
    }

    p_Draw->AddRect(p_Rect.Min, p_Rect.Max, GraticuleColor(kColGraticule, brightness));
}

// Value under the pointer, reported exactly rather than read off a gridline.
//
// Both scopes show the same three numbers - IRE on the panel's own scale, the
// working-space float, and the 10-bit code - because those are the three units a
// colourist actually quotes, and converting between them in your head while
// looking at a trace is how mistakes happen.
void DrawCursorReadout(ImDrawList* p_Draw, const ImRect& p_Panel, const ImRect& p_Plot,
                       float p_Value, bool p_Legal, bool p_Horizontal, ImVec2 p_At)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%.1f IRE   %.4f   code %.0f",
                  IreOfValue(p_Value, p_Legal), p_Value, CodeOfValue(p_Value));

    if (p_Horizontal)
        AddDashedLine(p_Draw, ImVec2(p_Plot.Min.x, p_At.y), ImVec2(p_Plot.Max.x, p_At.y), kColCursor);
    else
        AddDashedLine(p_Draw, ImVec2(p_At.x, p_Plot.Min.y), ImVec2(p_At.x, p_Plot.Max.y), kColCursor);

    const ImVec2 size = ImGui::CalcTextSize(text);

    // Nudged so the label never leaves the panel, and never sits under the
    // pointer where the thing being measured is.
    ImVec2 pos = p_Horizontal
        ? ImVec2(p_Plot.Min.x + 6.0f, p_At.y - size.y - 3.0f)
        : ImVec2(p_At.x + 6.0f,       p_Panel.Min.y + 4.0f);

    pos.x = ImClamp(pos.x, p_Panel.Min.x + 2.0f, p_Panel.Max.x - size.x - 2.0f);
    pos.y = ImClamp(pos.y, p_Panel.Min.y + 2.0f, p_Panel.Max.y - size.y - 2.0f);

    p_Draw->AddRectFilled(ImVec2(pos.x - 3.0f, pos.y - 1.0f),
                          ImVec2(pos.x + size.x + 3.0f, pos.y + size.y + 1.0f),
                          IM_COL32(0, 0, 0, 170));
    p_Draw->AddText(pos, kColCursorLabel, text);
}

// User-placed reference lines at a 10-bit code value. Drawn after the graticule
// and after the trace, since the whole point is to read the signal against them.
//
// Deliberately NOT dimmed by the Graticule slider: these are a measurement the
// user placed at a colour they chose, not part of the background grid.
void DrawBoundingLines(ImDrawList* p_Draw, const ImRect& p_Rect, const PanelCommon& p_Common,
                       const ScopeFrame& p_Frame, int p_Lo, int p_Hi)
{
    if (!p_Common.boundingLinesEnabled) return;

    for (const BoundingLine& line : p_Common.boundingLines)
    {
        if (!line.enabled) continue;

        float y = 0.0f;
        if (!WaveformValueToY(p_Frame, p_Rect, p_Lo, p_Hi, ValueOfCode(line.code), y))
            continue;

        const ImU32 colour = IM_COL32(uint8_t(line.color[0] * 255.0f),
                                      uint8_t(line.color[1] * 255.0f),
                                      uint8_t(line.color[2] * 255.0f), 255);

        p_Draw->AddLine(ImVec2(p_Rect.Min.x, y), ImVec2(p_Rect.Max.x, y), colour, 1.0f);

        char label[16];
        std::snprintf(label, sizeof(label), "%.0f", line.code);
        const ImVec2 size = ImGui::CalcTextSize(label);
        p_Draw->AddText(ImVec2(p_Rect.Max.x - size.x - 4.0f, y - size.y - 1.0f), colour, label);
    }
}

// Enhanced Render: one line strip per source scanline, per plane.
//
// Each row of the trace is a real scanline sampled onto the waveform's columns, so
// consecutive points genuinely belong together and a line between them means
// something. Drawn with a low alpha and let to accumulate - 256 rows overlapping
// is what produces the density, rather than any explicit intensity calculation.
// Per-sample beam colour for Colorize, normalised so the brightest channel is
// full - brightness is already carried by the trace's alpha, so spending it
// again here would say the same thing twice and leave dark pixels invisible.
//
// Written out rather than using std::max on an initializer_list and three
// divides, because that spelling measured badly: with Colorize on, the White
// Balance walk cost 5.7 ms a frame against 0.7 ms with it off, at identical
// geometry - about 19 ns per point for a normalise and a clamp, over 262,144
// points. The initializer_list overload of std::max does not reliably reduce
// to three comparisons, and the three divides are serially dependent.
//
// r * (1/peak) is not bit-identical to r / peak. The result is immediately
// quantised to 8 bits for display, where a 1-ULP difference cannot survive.
inline ImU32 BeamColour(float p_R, float p_G, float p_B, uint8_t p_Alpha)
{
    float peak = p_R > p_G ? p_R : p_G;
    if (p_B > peak) peak = p_B;
    if (peak < 1e-4f) peak = 1e-4f;
    const float inv = 1.0f / peak;

    auto channel = [](float p_V)
    {
        p_V = p_V < 0.0f ? 0.0f : (p_V > 1.0f ? 1.0f : p_V);
        return static_cast<uint8_t>(p_V * 255.0f);
    };

    return IM_COL32(channel(p_R * inv), channel(p_G * inv), channel(p_B * inv), p_Alpha);
}

// Set SCOPE_DECK_PROFILE=1 to show the Enhanced Render cost readout in the
// status bar. Off by default: it is developer instrumentation, and a colourist
// reading a scope does not need a vertex count in their status bar.
//
// An environment variable rather than a preference, for the same reason
// ScopeTap uses one for its GPU kill switch: the moment you want this is when
// something is already behaving oddly, and it should not depend on the app
// having loaded settings correctly first. Read once - it cannot change while
// the app runs.
const bool g_ProfileEnabled = []
{
    const char* v = std::getenv("SCOPE_DECK_PROFILE");
    return v && v[0] && !(v[0] == '0' && !v[1]);
}();

// How frames are paced to the display. On Windows the GL swap's own vertical
// sync is NOT used: NVIDIA's OpenGL driver waits for the vblank by spinning,
// and that spin was measured (2026-10-09, RTX 5070 Ti, driver 617.42) at a
// full core - ~1,000 ms/s of CPU, most of it kernel time - with one cheap
// panel open and the feed parked. Swapping with no interval and then waiting
// on the compositor instead (DwmFlush, at the end of RenderOneFrame) kept the
// same 60 fps and cut that to ~25 ms/s. Windowed GL is composited by DWM
// regardless, and the full-screen Solo window is deliberately kept one row
// taller than the monitor so it stays composited too (see EnterBorderless),
// so nothing here tears.
//
// SCOPE_DECK_GL_VSYNC=1 is the kill switch back to the driver's own sync,
// an environment variable for the same reason SCOPE_DECK_PROFILE is one.
#ifdef _WIN32
const bool g_GlVsync = []
{
    const char* v = std::getenv("SCOPE_DECK_GL_VSYNC");
    return v && v[0] && !(v[0] == '0' && !v[1]);
}();
#else
const bool g_GlVsync = true;
#endif

// Enhanced Render cost, measured live rather than reasoned about.
//
// Two rounds of reasoning about this were wrong - first that the cost was the
// per-segment AddLine overhead, then that it was the per-sample colour maths.
// Rather than guess a third time, every Enhanced Render pass now accumulates
// its own wall time and the geometry it emitted, and the status bar shows both.
// Reset once per frame in DrawFrame.
//
// Note the asymmetry the vertex counts are there to expose: a shared polyline
// emits 2 vertices per point, while per-segment colouring needs 4 per segment,
// because neighbouring segments can no longer share a vertex. Colorize
// therefore roughly doubles the geometry no matter how the calls are batched.
struct EnhancedProfile
{
    double   ms = 0.0;
    uint64_t segments = 0;
    uint64_t vertices = 0;
    double   msEmit = 0.0;     // time inside the geometry emitters only
    int      passes = 0;
    int      fallbacks = 0;    // runs that could not take the shared-vertex path
};
EnhancedProfile g_EnhancedProfile;

// The status bar is drawn before the panels are, so reading the live counters
// there always showed zero - which is exactly what the first run of this
// instrumentation reported. This carries the completed previous frame instead,
// which is what a per-frame cost readout wants anyway: stable to read rather
// than flickering as panels accumulate into it mid-frame.
EnhancedProfile g_EnhancedProfileLast;

// Times one geometry emission and adds it to msEmit. The walk is then
// (ms - msEmit), which is the split that finally distinguishes "generating
// vertices is expensive" from "reading the trace and projecting it is
// expensive" - two explanations that looked identical in the totals.
struct EmitTimer
{
    std::chrono::steady_clock::time_point start;
    EmitTimer()
    {
        if (g_ProfileEnabled) start = std::chrono::steady_clock::now();
    }
    ~EmitTimer()
    {
        if (!g_ProfileEnabled) return;
        g_EnhancedProfile.msEmit += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
};

struct EnhancedTimer
{
    std::chrono::steady_clock::time_point start;
    EnhancedTimer()
    {
        if (!g_ProfileEnabled) return;
        ++g_EnhancedProfile.passes;
        start = std::chrono::steady_clock::now();
    }
    ~EnhancedTimer()
    {
        if (!g_ProfileEnabled) return;
        g_EnhancedProfile.ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
};

void AddColouredPolyline(ImDrawList* p_Draw, const ImVec2* p_Points,
                         const ImU32* p_PointColours, int p_PointCount);

// Replays a cached Enhanced Render pass into the draw list. This is the whole
// per-refresh cost of a pass whose inputs have not changed: the walk that
// filled the cache ran once, when they last did.
//
// Coloured runs go through the shared-vertex polyline for every scope,
// Waveform included. Waveform's Colorize used to draw one AddLine per
// segment in that segment's own flat colour - 130,816 calls a frame at UHD,
// measured at ~3.3 ms of emit on their own against ~0.3 ms for the same
// geometry as polylines. The positions are the same points either way; the
// only difference is that a segment's tint now interpolates between its two
// samples' colours instead of holding the leading one, which the Vectorscope
// and White Balance traces have always done.
void EmitEnhancedRuns(ImDrawList* p_Draw, const EnhancedRunCache& p_Cache)
{
    const bool coloured = !p_Cache.pointColour.empty();
    for (const EnhancedRunCache::Run& run : p_Cache.runs)
    {
        const ImVec2* pts = p_Cache.points.data() + run.first;
        if (!coloured)
        {
            g_EnhancedProfile.segments += run.count - 1;
            g_EnhancedProfile.vertices += uint64_t(run.count) * 2;   // shared between segments
            EmitTimer emitTimer;
            p_Draw->AddPolyline(pts, int(run.count), run.colour, ImDrawFlags_None, 1.0f);
        }
        else
        {
            AddColouredPolyline(p_Draw, pts, p_Cache.pointColour.data() + run.first, int(run.count));
        }
    }
}

void DrawEnhancedTrace(ImDrawList* p_Draw, const ImRect& p_Rect, const WaveformState& p_St,
                       const ScopeFrame& p_Frame, const uint32_t* p_Planes, int p_PlaneCount,
                       int p_Lo, int p_Hi, int p_CellIndex, int p_CellCount,
                       EnhancedRunCache& p_Cache)
{
    EnhancedTimer profileThisPass;

    // Everything the projection below reads, folded into the cache key: which
    // planes, and which cell of how many. Plane ids are small, so a bitmask.
    uint32_t variant = uint32_t(p_CellIndex) << 8 | uint32_t(p_CellCount) << 12;
    for (int p = 0; p < p_PlaneCount; ++p) variant |= 1u << p_Planes[p];

    if (!p_Cache.Matches(p_St.traceCache.generation, p_Rect, float(p_Lo), float(p_Hi),
                         p_St.gain, p_St.colorize, variant))
    {
        p_Cache.Begin(p_St.traceCache.generation, p_Rect, float(p_Lo), float(p_Hi),
                      p_St.gain, p_St.colorize, variant);

        const float cellW = p_Rect.GetWidth() / float(p_CellCount);
        const float x0    = p_Rect.Min.x + cellW * float(p_CellIndex);

        // Alpha per line, scaled by Gain the way the density trace is.
        //
        // This has to be very low - a few units out of 255. 256 scanlines overlapping
        // is what produces the density, and the whole reason this trace reads as
        // continuous tone rather than as a solid block is that no single line is
        // visible on its own. Set it an order of magnitude higher (0.10 was the first
        // attempt) and every plane saturates to flat colour within a few rows, losing
        // exactly the structure the line trace exists to show.
        const float alpha = std::clamp(p_St.gain / kDefaultGain * 0.028f, 0.004f, 1.0f);

        std::vector<ImVec2> points;
        std::vector<bool>   valid;
        std::vector<ImU32>  colours;
        points.reserve(kWaveformColumns);
        valid.reserve(kWaveformColumns);
        colours.reserve(kWaveformColumns);

        for (int p = 0; p < p_PlaneCount; ++p)
        {
            const uint32_t plane = p_Planes[p];
            const Rgb8     tint  = TintForPlane(plane);
            const ImU32    colour = IM_COL32(tint.r, tint.g, tint.b, uint8_t(alpha * 255.0f));

            // Colorize tints each sample by the source pixel's own colour instead of
            // the plane's tint. Only meaningful on the luma plane - on a parade cell
            // the plane's own tint *is* the information - and only reachable here
            // because the trace carries R, G and B for the same row and column, not
            // just the plane being drawn.
            const bool colorizeThisPlane = p_St.colorize && plane == kPlaneY;

            for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
            {
                points.clear();
                valid.clear();
                colours.clear();

                for (uint32_t col = 0; col < kWaveformColumns; ++col)
                {
                    const float value =
                        p_St.preparedTrace[WaveformTraceIndex(row, col, plane)];

                    // NaN (see BuildMaskedWaveformTrace's own comment) means a
                    // masked column with nothing measured in it, not a real
                    // sample - recorded as invalid rather than clamped, so the
                    // run-splitting below leaves a genuine gap there instead of
                    // plunging to whatever LevelOfValue(NaN) would otherwise
                    // clamp to (the bottom of the waveform, indistinguishable
                    // from a real black sample).
                    if (std::isnan(value))
                    {
                        points.push_back(ImVec2());
                        valid.push_back(false);
                        colours.push_back(0);
                        continue;
                    }

                    const float level = p_Frame.LevelOfValue(value);

                    // Clamp into the visible span rather than dropping the point: a
                    // super-white sample leaving a gap in the line would read as "no
                    // signal here", the opposite of what it means.
                    const float clamped = std::clamp(level, float(p_Lo), float(p_Hi));
                    const float t = (clamped - float(p_Lo)) / float(p_Hi - p_Lo);

                    points.push_back(ImVec2(x0 + cellW * (float(col) + 0.5f) / float(kWaveformColumns),
                                            p_Rect.Max.y - t * p_Rect.GetHeight()));
                    valid.push_back(true);

                    if (colorizeThisPlane)
                    {
                        float rgb[3];
                        for (int c = 0; c < 3; ++c)
                            rgb[c] = p_St.preparedTrace[WaveformTraceIndex(row, col, uint32_t(c))];

                        // Normalised to the brightest channel so a dark pixel still
                        // shows its hue rather than fading to black - the trace's
                        // vertical position already carries the luminance. See
                        // BeamColour for why this is not spelled with std::max on an
                        // initializer_list and three divides: that form measured at
                        // ~19 ns a point, and this loop runs 131,072 times per
                        // rebuild on the luma plane.
                        colours.push_back(BeamColour(rgb[0], rgb[1], rgb[2], uint8_t(alpha * 255.0f)));
                    }
                }

                // Kept as separate runs over each contiguous valid stretch, not
                // one straight through the whole row - a masked-out gap has
                // nothing to connect across.
                size_t runStart = 0;
                while (runStart < points.size())
                {
                    if (!valid[runStart]) { ++runStart; continue; }
                    size_t runEnd = runStart;
                    while (runEnd < points.size() && valid[runEnd]) ++runEnd;
                    if (colorizeThisPlane)
                        p_Cache.AddRun(&points[runStart], &colours[runStart], int(runEnd - runStart));
                    else
                        p_Cache.AddRun(&points[runStart], int(runEnd - runStart), colour);
                    runStart = runEnd;
                }
            }
        }
    }

    EmitEnhancedRuns(p_Draw, p_Cache);
}

// Every Waveform control from the old app, in the old app's order.
//
// A window per panel instance, opened from that panel's own right-click menu, so
// two Waveforms can be set up differently - which is the arrangement the old app
// moved to, and the reason these settings live on the panel rather than globally.
void DrawWaveformSettings(Panel& p_Panel)
{
    WaveformState& p_St  = p_Panel.waveform;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    PositionSettingsWindow(common);

    if (!ImGui::Begin("Waveform Settings", &common.showSettings,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // Measured for the next open, so the placement above can keep the window
    // inside a panel too small to hold it at the default anchor.
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Mode");
    const char* modes[] = { "Luma", "Parade", "YRGB" };
    int modeIndex = int(p_St.mode);
    if (ImGui::Combo("##mode", &modeIndex, modes, IM_ARRAYSIZE(modes)))
        p_St.mode = WaveformMode(modeIndex);

    SettingsRowLabel("Range");
    const char* ranges[] = { "Full", "Shadows (bottom 15%)", "Highlights (top 15%)" };
    int rangeIndex = int(p_St.range);
    if (ImGui::Combo("##range", &rangeIndex, ranges, IM_ARRAYSIZE(ranges)))
        p_St.range = WaveformRange(rangeIndex);

    SettingsRowLabel("Scale");
    const char* scales[] = { "Full range", "Legal range" };
    int scaleIndex = int(p_St.scale);
    if (ImGui::Combo("##scale", &scaleIndex, scales, IM_ARRAYSIZE(scales)))
        p_St.scale = ScaleMode(scaleIndex);

    SettingsSliderFloat("Gain", &p_St.gain, 1.0f, 60.0f, kDefaultGain);

    SettingsRowLabel("Cursor readout");
    ImGui::Checkbox("##cursor", &p_St.showCursor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hovering reports the exact value under the pointer -\n"
                          "IRE, the working-space float, and the 10-bit code.");

    ImGui::SeparatorText("Enhanced Render");

    SettingsRowLabel("Enhanced Render");
    ImGui::Checkbox("##enhanced", &p_St.enhancedRender);

    // The dependent rows appear only while Enhanced Render is on, matching the old
    // app - they do nothing otherwise, and a control that does nothing is worse
    // than one that is not there.
    if (p_St.enhancedRender)
    {
        SettingsRowLabel("Smooth Trace");
        if (ImGui::Checkbox("##smooth", &p_St.smoothTrace) && !p_St.smoothTrace)
            p_St.traceState.Reset();

        SettingsSliderFloat("Low-Pass Filter", &p_St.lowPassKernel, 0.0f, 20.0f, 0.0f);

        SettingsRowLabel("Signal Pre-filter");
        ImGui::Checkbox("##prefilter", &p_St.signalPrefilter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A generic bandwidth-limiting pass on the raw signal.\n"
                              "Not a verified EBU R103 implementation.");

        SettingsRowLabel("Colorize");
        ImGui::Checkbox("##colorize", &p_St.colorize);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tints the trace by each source pixel's own colour.\n"
                              "Luma mode only.");
    }

    DrawSharedSettings(common);

    ImGui::End();
}

void DrawWaveformPanel(App& p_App, Panel& p_Panel)
{
    WaveformState& st     = p_Panel.waveform;
    PanelCommon&   common = p_Panel.common;

    // The menu bar keeps the three controls reached constantly while grading.
    // Everything else lives in Settings, so the bar does not become a second,
    // competing home for the same options.
    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    // A gutter down the left for the IRE labels, so they sit beside the plot
    // rather than on top of the trace.
    const float gutter = ImGui::CalcTextSize("100").x + 10.0f;
    const ImRect plotArea(ImVec2(panel.Min.x + gutter, panel.Min.y), panel.Max);

    common.zoom.HandleInput(plotArea, common.zoomable);

    if (!p_App.haveFrame)
    {
        draw->AddRect(plotArea.Min, plotArea.Max,
                      GraticuleColor(kColGraticule, common.graticuleBrightness));
        if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
            p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    // Everything below draws into the zoomed rectangle and is clipped to the
    // panel's own. The graticule is transformed too - a zoomed trace read against
    // a fixed grid would give wrong numbers, which is worse than not zooming.
    draw->PushClipRect(panel.Min, panel.Max, true);
    const ImRect rect = common.zoom.Apply(plotArea);

    int lo = 0, hi = int(kWaveformLevels);
    WaveformLevelSpan(p_App.frame, st.range, lo, hi);

    const int cells = (st.mode == WaveformMode::Parade) ? 3 : 1;
    DrawWaveformGraticule(draw, rect, plotArea, st, common, p_App.frame, lo, hi, cells);

    static const uint32_t kParadePlanes[3] = { kPlaneR, kPlaneG, kPlaneB };
    static const uint32_t kLumaPlanes[1]   = { kPlaneY };
    static const uint32_t kYrgbPlanes[4]   = { kPlaneY, kPlaneR, kPlaneG, kPlaneB };

    // A spatial mask restricts what this panel reads, same as it already
    // restricts what the preview-tier panels show - see EnsureMaskedScopeBins.
    const bool             maskActive     = p_App.maskedScopeValid;
    const uint32_t* const  waveformCounts = maskActive ? p_App.maskedScope.waveform.data() : nullptr;
    const float            waveformRef    = maskActive ? p_App.maskedScope.referenceSamples : 0.0f;

    // Enhanced Render replaces the density trace rather than overlaying it - they
    // are two representations of the same frame, and drawing both at once reads as
    // a doubled signal. While a mask is active, it reads the masked trace
    // (App::maskedScope.waveformTrace) instead of the frame's own - same
    // "restrict, don't fabricate" rule the masked bins use.
    const std::vector<float>* maskedTrace = maskActive ? &p_App.maskedScope.waveformTrace : nullptr;
    const bool enhancedReady =
        st.enhancedRender && !p_App.prefs.disableEnhancedRender &&
        PrepareEnhancedTraceCached(p_App.frame, st.signalPrefilter, st.smoothTrace,
                                   st.lowPassKernel, st.traceState, st.preparedTrace,
                                   maskedTrace, p_App.spatialMask, p_App.valueQualifier,
                                   st.traceCache);
    if (enhancedReady)
    {
        if (st.mode == WaveformMode::Parade)
        {
            for (int i = 0; i < 3; ++i)
                DrawEnhancedTrace(draw, rect, st, p_App.frame, &kParadePlanes[i], 1,
                                  lo, hi, i, 3, st.enhancedRuns[i]);
        }
        else if (st.mode == WaveformMode::Luma)
        {
            DrawEnhancedTrace(draw, rect, st, p_App.frame, kLumaPlanes, 1, lo, hi, 0, 1,
                              st.enhancedRuns[0]);
        }
        else
        {
            DrawEnhancedTrace(draw, rect, st, p_App.frame, kYrgbPlanes, 4, lo, hi, 0, 1,
                              st.enhancedRuns[0]);
        }
    }
    else
    {
        // Density path. The image is rebuilt only when the frame, or something
        // it was built from, has changed - the same guard as DrawFalseColorPanel.
        // The app redraws at display rate, the feed arrives at timeline rate
        // (or not at all while parked), so without this the identical picture
        // was rebuilt and re-uploaded every refresh.
        const bool stale = st.builtFrameIndex != p_App.frame.frameIndex
                        || st.builtMode   != st.mode
                        || st.builtGain   != st.gain
                        || st.builtLo     != lo
                        || st.builtHi     != hi
                        || st.builtMasked != maskActive
                        || !SpatialMaskEquals(st.builtMask, p_App.spatialMask)
                        || !ValueQualifierEquals(st.builtQualifier, p_App.valueQualifier);
        if (stale)
        {
            if (st.mode == WaveformMode::Parade)
            {
                for (int i = 0; i < 3; ++i)
                {
                    Image cell;
                    BuildWaveformImage(p_App.frame, kParadePlanes[i], TintForPlane(kParadePlanes[i]),
                                       st.gain, lo, hi, cell, waveformCounts, waveformRef);
                    if (!cell.Empty())
                        st.textures[i].UploadRGBA(cell.pixels.data(), cell.width, cell.height);
                }
            }
            else
            {
                if (st.mode == WaveformMode::Luma)
                    BuildWaveformComposite(p_App.frame, kLumaPlanes, 1, st.gain, lo, hi, st.image,
                                           waveformCounts, waveformRef);
                else
                    BuildWaveformComposite(p_App.frame, kYrgbPlanes, 4, st.gain, lo, hi, st.image,
                                           waveformCounts, waveformRef);

                if (!st.image.Empty())
                    st.textures[0].UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }

            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtMode       = st.mode;
            st.builtGain       = st.gain;
            st.builtLo         = lo;
            st.builtHi         = hi;
            st.builtMasked     = maskActive;
            st.builtMask       = p_App.spatialMask;
            st.builtQualifier  = p_App.valueQualifier;
        }

        if (st.mode == WaveformMode::Parade)
        {
            const float cellW = rect.GetWidth() / 3.0f;
            for (int i = 0; i < 3; ++i)
            {
                if (!st.textures[i].Valid()) continue;
                const ImVec2 cellMin(rect.Min.x + cellW * float(i), rect.Min.y);
                const ImVec2 cellMax(rect.Min.x + cellW * float(i + 1) - 1.0f, rect.Max.y);
                draw->AddImage(st.textures[i].ImGuiHandle(), cellMin, cellMax);
            }
        }
        else if (st.textures[0].Valid())
        {
            draw->AddImage(st.textures[0].ImGuiHandle(), rect.Min, rect.Max);
        }
    }

    if (maskActive && p_App.maskedScope.empty)
        DrawMaskEmptyNotice(draw, rect);

    // After the trace: these are references the signal is read against, so the
    // trace must not cover them.
    DrawBoundingLines(draw, rect, common, p_App.frame, lo, hi);

    // Cursor readout: the value under the pointer, reported rather than eyeballed.
    if (st.showCursor && ImGui::IsWindowHovered() && plotArea.Contains(ImGui::GetIO().MousePos))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        // Inverse of the level -> y mapping the graticule uses, so the number
        // always agrees with the line it is next to.
        const float t     = (rect.Max.y - mouse.y) / rect.GetHeight();
        const float level = float(lo) + t * float(hi - lo);
        const float value = p_App.frame.ValueAtLevel(level);

        DrawCursorReadout(draw, plotArea, rect, value,
                          st.scale == ScaleMode::LegalRange, true, mouse);
    }

    draw->PopClipRect();
    draw->AddRect(plotArea.Min, plotArea.Max,
                  GraticuleColor(kColGraticule, common.graticuleBrightness));

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);

}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

// One histogram lane: the filled curve for a single channel inside p_Lane.
//
// p_Peak is passed in rather than taken per lane, because every lane on screen has
// to share one vertical scale. Scaling each to its own peak would draw a clipped
// blue channel and a balanced one identically - which is the one thing a parade of
// histograms exists to tell apart.
void DrawHistogramLane(ImDrawList* p_Draw, const ImRect& p_Lane, const ImRect& p_Content,
                       const ScopeFrame& p_Frame, uint32_t p_Plane,
                       uint32_t p_Peak, bool p_Log, uint8_t p_FillAlpha,
                       const uint32_t* p_CountsOverride = nullptr)
{
    if (p_Peak == 0) return;

    const Rgb8  tint = TintForPlane(p_Plane);
    const ImU32 fill = IM_COL32(tint.r, tint.g, tint.b, p_FillAlpha);
    const ImU32 line = IM_COL32(tint.r, tint.g, tint.b, 255);

    const float bottom  = p_Lane.Max.y - 1.0f;
    const float height  = p_Lane.GetHeight() - 2.0f;
    const float logPeak = std::log1p(float(p_Peak));

    const uint32_t* hist = p_CountsOverride ? p_CountsOverride : p_Frame.histogram.data();

    std::vector<ImVec2> outline;
    outline.reserve(kHistogramBins);

    ImVec2 prevPoint(0.0f, 0.0f);
    bool   havePrev = false;

    for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
    {
        const uint32_t count = hist[HistogramIndex(p_Plane, bin)];

        const float norm = p_Log
            ? (logPeak > 0.0f ? std::log1p(float(count)) / logPeak : 0.0f)
            : float(count) / float(p_Peak);

        const float x = p_Content.Min.x
                      + p_Content.GetWidth() * float(bin) / float(kHistogramBins - 1);
        const float y = bottom - norm * height;

        // Trapezoid between consecutive bins, so the area under the curve is solid
        // and its top edge follows the curve. Fixed-width bars leave visible
        // striping once the panel is wider than 256 x bar width.
        if (havePrev)
            p_Draw->AddQuadFilled(prevPoint, ImVec2(x, y),
                                  ImVec2(x, bottom), ImVec2(prevPoint.x, bottom), fill);

        prevPoint = ImVec2(x, y);
        havePrev  = true;

        outline.push_back(ImVec2(x, y));
    }

    if (outline.size() >= 2)
        p_Draw->AddPolyline(outline.data(), int(outline.size()), line, ImDrawFlags_None, 1.0f);

    p_Draw->AddLine(ImVec2(p_Content.Min.x, bottom), ImVec2(p_Content.Max.x, bottom), line, 1.0f);
}

// Maps a screen x inside `p_Content` to a value in the frame's own bin-range
// units - the exact inverse of the gridline x = content.Min.x + t*width
// mapping DrawHistogramPanel already draws its IRE gridlines against, so a
// dragged range lines up with the same axis the lanes themselves use.
float HistogramValueOfX(const ScopeFrame& p_Frame, const ImRect& p_Content, float p_X)
{
    const float t = p_Content.GetWidth() > 0.0f
        ? std::clamp((p_X - p_Content.Min.x) / p_Content.GetWidth(), 0.0f, 1.0f) : 0.0f;
    return p_Frame.binRangeLow + t * (p_Frame.binRangeHigh - p_Frame.binRangeLow);
}

// Click-drag directly on a lane selects a value range on that plane - see
// HistogramState::qualifyActive's own comment. A plain click (no movement
// past the anchor) clears that lane's qualifier instead of leaving a
// zero-width one that would match almost nothing - the same "click a
// qualified lane again to turn it back off" the old app used.
void UpdateHistogramQualifierInput(App& p_App, HistogramState& p_St,
                                   const uint32_t* p_Planes, int p_PlaneCount,
                                   const ImRect* p_LaneRects, const ImRect& p_Content)
{
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const ImVec2 pos = ImGui::GetIO().MousePos;

    if (!p_St.qualifyDragging && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        for (int i = 0; i < p_PlaneCount; ++i)
        {
            if (!p_LaneRects[i].Contains(pos)) continue;
            p_St.qualifyDragging   = true;
            p_St.qualifyDragPlane  = p_Planes[i];
            p_St.qualifyDragAnchor = HistogramValueOfX(p_App.frame, p_Content, pos.x);
            p_St.qualifyDragMoved  = false;
            p_St.qualifyActive[p_Planes[i]] = true;
            p_St.qualifyLo[p_Planes[i]] = p_St.qualifyDragAnchor;
            p_St.qualifyHi[p_Planes[i]] = p_St.qualifyDragAnchor;
            break;
        }
    }

    if (p_St.qualifyDragging)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float value = HistogramValueOfX(p_App.frame, p_Content, pos.x);
            if (value != p_St.qualifyDragAnchor) p_St.qualifyDragMoved = true;
            const uint32_t plane = p_St.qualifyDragPlane;
            p_St.qualifyLo[plane] = std::min(p_St.qualifyDragAnchor, value);
            p_St.qualifyHi[plane] = std::max(p_St.qualifyDragAnchor, value);
        }
        else
        {
            if (!p_St.qualifyDragMoved)
                p_St.qualifyActive[p_St.qualifyDragPlane] = false;
            p_St.qualifyDragging = false;
        }
    }
}

// The active [lo, hi] band on a lane the drag above selected - a translucent
// fill plus two edge lines, matching the old app's own qualifier overlay
// exactly (same colours: white 40-alpha fill, gold 1px edges).
void DrawHistogramQualifierOverlay(ImDrawList* p_Draw, const ImRect& p_Lane, const ImRect& p_Content,
                                   const ScopeFrame& p_Frame, float p_Lo, float p_Hi)
{
    const float range = p_Frame.binRangeHigh - p_Frame.binRangeLow;
    if (range <= 0.0f) return;

    const float x0 = p_Content.Min.x + (p_Lo - p_Frame.binRangeLow) / range * p_Content.GetWidth();
    const float x1 = p_Content.Min.x + (p_Hi - p_Frame.binRangeLow) / range * p_Content.GetWidth();

    p_Draw->AddRectFilled(ImVec2(x0, p_Lane.Min.y), ImVec2(std::max(x1, x0 + 1.0f), p_Lane.Max.y),
                          IM_COL32(255, 255, 255, 40));
    const ImU32 edge = IM_COL32(255, 210, 90, 255);
    p_Draw->AddLine(ImVec2(x0, p_Lane.Min.y), ImVec2(x0, p_Lane.Max.y), edge, 1.0f);
    p_Draw->AddLine(ImVec2(x1, p_Lane.Min.y), ImVec2(x1, p_Lane.Max.y), edge, 1.0f);
}

void DrawHistogramPanel(App& p_App, Panel& p_Panel)
{
    HistogramState& st     = p_Panel.histogram;
    PanelCommon&    common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    common.zoom.HandleInput(panel, common.zoomable);

    if (!p_App.haveFrame || p_App.frame.histogram.size() < kHistogramCells)
    {
        draw->AddRect(panel.Min, panel.Max, GraticuleColor(kColGraticule, common.graticuleBrightness));
        if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
            p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    // Which planes are on screen, and whether they each get their own row.
    //
    // There is no separate RGB mode: unticking Y in YRGB is RGB, and offering both
    // would be two controls for one choice.
    uint32_t planes[4];
    int      planeCount = 0;
    bool     lanes      = true;

    switch (st.channels)
    {
        case HistogramChannels::Luma:
            planes[planeCount++] = kPlaneY;
            break;

        case HistogramChannels::Stacked:
            // One plot, channels overlapping - the old app's Stacked. Fixed to
            // R/G/B: the point is comparing the three against each other, and luma
            // laid over them is a different question.
            planes[planeCount++] = kPlaneR;
            planes[planeCount++] = kPlaneG;
            planes[planeCount++] = kPlaneB;
            lanes = false;
            break;

        case HistogramChannels::YRGB:
        default:
            if (st.showY) planes[planeCount++] = kPlaneY;
            if (st.showR) planes[planeCount++] = kPlaneR;
            if (st.showG) planes[planeCount++] = kPlaneG;
            if (st.showB) planes[planeCount++] = kPlaneB;
            break;
    }

    if (planeCount == 0)
    {
        draw->AddRect(panel.Min, panel.Max, GraticuleColor(kColGraticule, common.graticuleBrightness));
        if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
            p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(panel.Min, panel.Max, true);

    const ImRect content = common.zoom.Apply(panel);

    const float brightness  = common.graticuleBrightness;
    const float scaleHeight = ImGui::GetTextLineHeight() + 4.0f;

    // A spatial mask restricts what this panel reads, same as it already
    // restricts what the preview-tier panels show - see EnsureMaskedScopeBins.
    const bool      maskActive  = p_App.maskedScopeValid;
    const uint32_t* histCounts  = maskActive ? p_App.maskedScope.histogram.data() : p_App.frame.histogram.data();

    // One vertical scale across every channel on screen.
    uint32_t peak = 0;
    for (int i = 0; i < planeCount; ++i)
        for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
            peak = std::max(peak, histCounts[HistogramIndex(planes[i], bin)]);

    // Gridlines run the full height, behind every row, so a value lines up across
    // channels by eye without counting rows.
    for (int ire = 0; ire <= 100; ire += 20)
    {
        const float value = ValueOfIre(float(ire), false);
        const float t = (value - p_App.frame.binRangeLow)
                        / (p_App.frame.binRangeHigh - p_App.frame.binRangeLow);
        if (t < 0.0f || t > 1.0f) continue;

        const float x = content.Min.x + t * content.GetWidth();

        // 0 and 100 saturated, the subdivisions faint - the same convention as the
        // waveform, so a value reads the same on either scope.
        const bool major = (ire == 0 || ire == 100);
        draw->AddLine(ImVec2(x, panel.Min.y + scaleHeight), ImVec2(x, panel.Max.y),
                      GraticuleColor(major ? kColGratMajor : kColGratMinor, brightness), 1.0f);

        if (x < panel.Min.x || x > panel.Max.x) continue;

        char label[8];
        std::snprintf(label, sizeof(label), "%d", ire);
        const ImVec2 size = ImGui::CalcTextSize(label);

        // Kept clear of the close button, which sits in the same top-right corner -
        // the 100 label ran straight underneath it otherwise.
        const float buttonZone = ImGui::GetFontSize() * 1.15f + 10.0f;
        const float labelX = ImClamp(x - size.x * 0.5f,
                                     panel.Min.x + 2.0f,
                                     panel.Max.x - buttonZone - size.x);

        draw->AddText(ImVec2(labelX, panel.Min.y),
                      GraticuleColor(kColHistogramScale, brightness), label);
    }

    if (lanes)
    {
        const float rowHeight = (panel.GetHeight() - scaleHeight) / float(planeCount);

        ImRect laneRects[4];
        for (int i = 0; i < planeCount; ++i)
        {
            const float top = panel.Min.y + scaleHeight + rowHeight * float(i);
            laneRects[i] = ImRect(ImVec2(panel.Min.x, top), ImVec2(panel.Max.x, top + rowHeight));
        }

        // Before drawing, not after: a drag in progress must show this same
        // frame's updated range, not one frame stale - same ordering reason
        // UpdateSpatialMaskInput runs before BuildDimmedPreviewImage.
        UpdateHistogramQualifierInput(p_App, st, planes, planeCount, laneRects, content);

        for (int i = 0; i < planeCount; ++i)
        {
            DrawHistogramLane(draw, laneRects[i], content, p_App.frame, planes[i], peak, st.logScale, 48, histCounts);
            if (st.qualifyActive[planes[i]])
                DrawHistogramQualifierOverlay(draw, laneRects[i], content, p_App.frame,
                                              st.qualifyLo[planes[i]], st.qualifyHi[planes[i]]);
        }
    }
    else
    {
        // All three over one another. ImGui's draw list has no additive blend, so
        // this is alpha-over rather than the old app's CompositionMode_Plus: the
        // overlap reads a little darker than it did, but each channel's own curve
        // and edge stay legible, which is what the mode is for.
        const ImRect lane(ImVec2(panel.Min.x, panel.Min.y + scaleHeight),
                          ImVec2(panel.Max.x, panel.Max.y));

        for (int i = 0; i < planeCount; ++i)
            DrawHistogramLane(draw, lane, content, p_App.frame, planes[i], peak, st.logScale, 40, histCounts);
    }

    if (maskActive && p_App.maskedScope.empty)
        DrawMaskEmptyNotice(draw, panel);

    // Vertical here, not horizontal: the histogram's value axis is its x axis, so a
    // bounding line at code 940 is a line down the plot, not across it.
    if (common.boundingLinesEnabled)
    {
        for (const BoundingLine& bl : common.boundingLines)
        {
            if (!bl.enabled) continue;

            const float value = ValueOfCode(bl.code);
            const float t = (value - p_App.frame.binRangeLow)
                            / (p_App.frame.binRangeHigh - p_App.frame.binRangeLow);
            if (t < 0.0f || t > 1.0f) continue;

            const float x = content.Min.x + t * content.GetWidth();
            const ImU32 colour = IM_COL32(uint8_t(bl.color[0] * 255.0f),
                                          uint8_t(bl.color[1] * 255.0f),
                                          uint8_t(bl.color[2] * 255.0f), 255);

            draw->AddLine(ImVec2(x, panel.Min.y + scaleHeight), ImVec2(x, panel.Max.y), colour, 1.0f);

            char label[16];
            std::snprintf(label, sizeof(label), "%.0f", bl.code);
            draw->AddText(ImVec2(x + 3.0f, panel.Min.y + scaleHeight + 2.0f), colour, label);
        }
    }

    // Cursor readout on this scope's own axis: value runs horizontally here, so
    // the line is vertical.
    if (st.showCursor && ImGui::IsWindowHovered() && panel.Contains(ImGui::GetIO().MousePos))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        const float t = (mouse.x - content.Min.x) / content.GetWidth();
        const float value = p_App.frame.binRangeLow
                          + t * (p_App.frame.binRangeHigh - p_App.frame.binRangeLow);

        const ImRect plot(ImVec2(panel.Min.x, panel.Min.y + scaleHeight), panel.Max);
        DrawCursorReadout(draw, panel, plot, value, false, false, mouse);
    }

    draw->PopClipRect();

    draw->AddRect(panel.Min, panel.Max, GraticuleColor(kColGraticule, brightness));

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

// Enhanced Render for the vectorscope: the beam, rather than a density cloud.
//
// Same trace rows as the waveform's, read differently - each sample's R, G and B
// become a Cb/Cr position, and consecutive samples on one scanline are connected.
//
// Unlike the waveform, a run is broken where two neighbouring samples are far
// apart in chroma. A steep transition between adjacent columns *is* the signal on
// a waveform, but here it would draw a straight line through the middle of the
// wheel every time a scanline crosses from a neutral background into an isolated
// saturated highlight and back - a line through colours that are not in the frame.

// Draws a polyline whose points each carry their own colour.
//
// Colorize used to be drawn as one AddLine per segment, which meant flat colour
// per segment - and flat per-segment colour cannot share vertices between
// neighbouring segments, so it emitted 4 vertices per segment where the plain
// path emits 2 per point. Measured on a UHD White Balance panel: identical
// 261,226 segments either way, but 1,044,904 vertices and 6.7 ms with Colorize
// on against 524,244 vertices and 1.7 ms with it off. The cost was the
// geometry, not the call overhead - an earlier attempt at batching the AddLine
// calls changed nothing, because the vertex count was never the thing it fixed.
//
// The call sites already record one colour per *point*; the old code simply
// took each segment's colour from its first point and discarded the rest. So
// this emits the same shared-vertex geometry ImGui's own AddPolyline produces -
// 2 vertices per point, joins averaged between adjacent segments - with each
// point carrying its own colour. Colour now interpolates along a segment
// instead of stepping at it, which on a beam trace of neighbouring samples is
// not a visible change.
//
// Geometry otherwise matches ImGui's textured anti-aliased path exactly: same
// normals, same half-draw size, same line-texture UVs, same winding. Where
// ImGui would not take that path, this falls back to per-segment AddLine rather
// than guessing at its other variants.
void AddColouredPolyline(ImDrawList* p_Draw, const ImVec2* p_Points,
                         const ImU32* p_PointColours, int p_PointCount)
{
    if (p_PointCount < 2) return;
    EmitTimer emitTimer;
    const int segments = p_PointCount - 1;

    g_EnhancedProfile.segments += uint64_t(segments);

    constexpr float kThickness = 1.0f;
    constexpr int   kIntegerThickness = 1;

    const bool useTexture =
        (p_Draw->Flags & ImDrawListFlags_AntiAliasedLines) &&
        (p_Draw->Flags & ImDrawListFlags_AntiAliasedLinesUseTex) &&
        (kIntegerThickness < IM_DRAWLIST_TEX_LINES_WIDTH_MAX) &&
        (p_Draw->_FringeScale == 1.0f);

    if (!useTexture)
    {
        // Count what this branch really emits, not what the fast one would.
        // An earlier version counted the shared-vertex figure at the top of the
        // function regardless of which path ran, which would have reported a
        // halved vertex count with unchanged time - exactly the shape of a fast
        // path that is silently not being taken.
        g_EnhancedProfile.vertices += uint64_t(segments) * 4;   // per-segment quads
        ++g_EnhancedProfile.fallbacks;
        for (int i = 0; i < segments; ++i)
            p_Draw->AddLine(p_Points[i], p_Points[i + 1], p_PointColours[i], kThickness);
        return;
    }

    g_EnhancedProfile.vertices += uint64_t(p_PointCount) * 2;   // shared between segments

    // Reused across calls - this runs hundreds of times a frame and must not
    // allocate. Single-threaded UI drawing, so a function-local static is safe.
    static std::vector<ImVec2> normals;
    static std::vector<ImVec2> edges;
    normals.resize(size_t(p_PointCount));
    edges.resize(size_t(p_PointCount) * 2);

    for (int i = 0; i < segments; ++i)
    {
        float dx = p_Points[i + 1].x - p_Points[i].x;
        float dy = p_Points[i + 1].y - p_Points[i].y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > 0.0f)
        {
            const float invLen = 1.0f / std::sqrt(d2);
            dx *= invLen;
            dy *= invLen;
        }
        normals[size_t(i)] = ImVec2(dy, -dx);
    }
    normals[size_t(p_PointCount - 1)] = normals[size_t(segments - 1)];

    // Matches ImGui: thickness/2 plus one pixel, because the +1 is tied to how
    // the line texture itself was generated.
    const float halfDrawSize = kThickness * 0.5f + 1.0f;

    edges[0] = ImVec2(p_Points[0].x + normals[0].x * halfDrawSize,
                      p_Points[0].y + normals[0].y * halfDrawSize);
    edges[1] = ImVec2(p_Points[0].x - normals[0].x * halfDrawSize,
                      p_Points[0].y - normals[0].y * halfDrawSize);

    for (int i1 = 0; i1 < segments; ++i1)
    {
        const int i2 = i1 + 1;

        // Averaged join normal, then ImGui's IM_FIXNORMAL2F - spelled out
        // because that macro is local to imgui_draw.cpp. It lengthens the
        // normal at a corner so the two edges still meet, capped so a hairpin
        // cannot send it to infinity.
        float dmx = (normals[size_t(i1)].x + normals[size_t(i2)].x) * 0.5f;
        float dmy = (normals[size_t(i1)].y + normals[size_t(i2)].y) * 0.5f;
        {
            const float d2 = dmx * dmx + dmy * dmy;
            if (d2 > 0.000001f)
            {
                float invLen2 = 1.0f / d2;
                if (invLen2 > 100.0f) invLen2 = 100.0f;
                dmx *= invLen2;
                dmy *= invLen2;
            }
        }
        dmx *= halfDrawSize;
        dmy *= halfDrawSize;

        edges[size_t(i2) * 2 + 0] = ImVec2(p_Points[i2].x + dmx, p_Points[i2].y + dmy);
        edges[size_t(i2) * 2 + 1] = ImVec2(p_Points[i2].x - dmx, p_Points[i2].y - dmy);
    }

    const ImVec4 texUvs = p_Draw->_Data->TexUvLines[kIntegerThickness];
    const ImVec2 uv0(texUvs.x, texUvs.y);
    const ImVec2 uv1(texUvs.z, texUvs.w);

    p_Draw->PrimReserve(segments * 6, p_PointCount * 2);

    const unsigned int base = p_Draw->_VtxCurrentIdx;
    for (int i1 = 0; i1 < segments; ++i1)
    {
        const unsigned int idx1 = base + unsigned(i1) * 2;
        const unsigned int idx2 = idx1 + 2;
        p_Draw->_IdxWritePtr[0] = (ImDrawIdx)(idx2 + 0);
        p_Draw->_IdxWritePtr[1] = (ImDrawIdx)(idx1 + 0);
        p_Draw->_IdxWritePtr[2] = (ImDrawIdx)(idx1 + 1);
        p_Draw->_IdxWritePtr[3] = (ImDrawIdx)(idx2 + 1);
        p_Draw->_IdxWritePtr[4] = (ImDrawIdx)(idx1 + 1);
        p_Draw->_IdxWritePtr[5] = (ImDrawIdx)(idx2 + 0);
        p_Draw->_IdxWritePtr += 6;
    }

    for (int i = 0; i < p_PointCount; ++i)
    {
        const ImU32 col = p_PointColours[i];
        p_Draw->_VtxWritePtr[0].pos = edges[size_t(i) * 2 + 0];
        p_Draw->_VtxWritePtr[0].uv  = uv0;
        p_Draw->_VtxWritePtr[0].col = col;
        p_Draw->_VtxWritePtr[1].pos = edges[size_t(i) * 2 + 1];
        p_Draw->_VtxWritePtr[1].uv  = uv1;
        p_Draw->_VtxWritePtr[1].col = col;
        p_Draw->_VtxWritePtr += 2;
    }
    p_Draw->_VtxCurrentIdx += unsigned(p_PointCount) * 2;
}

void DrawVectorscopeEnhanced(ImDrawList* p_Draw, const ImRect& p_Rect,
                             const VectorscopeState& p_St, const ScopeFrame& p_Frame,
                             float p_RangeLo, float p_RangeHi, EnhancedRunCache& p_Cache)
{
    EnhancedTimer profileThisPass;

    // The walk below runs only when something it reads has changed - see
    // EnhancedRunCache. The luma weights it also reads come with the frame,
    // and a new frame is a new trace generation.
    if (!p_Cache.Matches(p_St.traceCache.generation, p_Rect, p_RangeLo, p_RangeHi,
                         p_St.gain, p_St.colorize, 0))
    {
        p_Cache.Begin(p_St.traceCache.generation, p_Rect, p_RangeLo, p_RangeHi,
                      p_St.gain, p_St.colorize, 0);

        const float   alpha     = std::clamp(p_St.gain / 6.0f * 0.028f, 0.004f, 1.0f);
        const uint8_t alpha8    = uint8_t(alpha * 255.0f);
        const ImU32   plainWhite = IM_COL32(235, 240, 245, alpha8);

        // Chroma distance beyond which two neighbouring samples are not connected.
        // A fraction of the visible span rather than an absolute, so it stays
        // meaningful at 2x zoom.
        const float breakDistance = (p_RangeHi - p_RangeLo) * 0.25f;

        std::vector<ImVec2> run;
        run.reserve(kWaveformColumns);

        // Colorize tints the beam by each sample's own colour instead of drawing it
        // white. Unlike the density trace - which can only tint by the *bin's* Cb/Cr
        // position, because a bin has no single source pixel - the line trace still
        // has the sample that produced each point, so this is the real colour rather
        // than a hue-wheel lookup.
        std::vector<ImU32> runColours;
        if (p_St.colorize) runColours.reserve(kWaveformColumns);

        auto flush = [&]()
        {
            if (p_St.colorize)
                p_Cache.AddRun(run.data(), runColours.data(), int(run.size()));
            else
                p_Cache.AddRun(run.data(), int(run.size()), plainWhite);
            run.clear();
            runColours.clear();
        };

        for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
        {
            run.clear();

            float prevCb = 0.0f, prevCr = 0.0f;
            bool  havePrev = false;

            for (uint32_t col = 0; col < kWaveformColumns; ++col)
            {
                const float r = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneR)];
                const float g = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneG)];
                const float b = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneB)];

                // NaN (see BuildMaskedWaveformTrace) means a masked column with
                // nothing measured in it - breaks the run exactly like an
                // out-of-range sample does below, rather than feeding a NaN
                // through CbCrOfRgb/ChromaToPlot where the range check's
                // comparisons would silently be false and let it through.
                if (std::isnan(r))
                {
                    flush();
                    havePrev = false;
                    continue;
                }

                float cb = 0.0f, cr = 0.0f;
                CbCrOfRgb(r, g, b, p_Frame.lumaCoeff, cb, cr);

                if (havePrev)
                {
                    const float dx = cb - prevCb;
                    const float dy = cr - prevCr;
                    if (std::sqrt(dx * dx + dy * dy) > breakDistance)
                        flush();
                }

                float u = 0.0f, v = 0.0f;
                ChromaToPlot(cb, cr, p_RangeLo, p_RangeHi, u, v);

                // A point outside the visible span breaks the run rather than being
                // clamped: clamping would pin it to the rim and draw an arc that is
                // not in the data.
                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                {
                    flush();
                    havePrev = false;
                    continue;
                }

                run.push_back(ImVec2(p_Rect.Min.x + u * p_Rect.GetWidth(),
                                     p_Rect.Min.y + v * p_Rect.GetHeight()));

                if (p_St.colorize)
                {
                    // Normalised to the brightest channel, the same convention the
                    // density path uses: the position already carries saturation, so
                    // spending brightness on it again would say the same thing twice
                    // and leave dark pixels invisible.
                    runColours.push_back(BeamColour(r, g, b, alpha8));
                }

                prevCb   = cb;
                prevCr   = cr;
                havePrev = true;
            }

            flush();
        }
    }

    EmitEnhancedRuns(p_Draw, p_Cache);
}

// One vectorscope wheel, drawn into p_Cell.
//
// Factored out because 3-Stack draws three of them, and the graticule, targets,
// skin line and badge all have to be identical in each - a stack whose cells
// disagreed about where 100% saturation sits would be worse than no stack at all.
void DrawVectorscopeWheel(App& p_App, VectorscopeState& p_St, PanelCommon& p_Common,
                          ImDrawList* p_Draw, const ImRect& p_Cell,
                          uint32_t p_BandMask, const char* p_Label, int p_TextureSlot)
{
    const ImVec2 centre((p_Cell.Min.x + p_Cell.Max.x) * 0.5f,
                        (p_Cell.Min.y + p_Cell.Max.y) * 0.5f);
    const float  radius = p_Cell.GetWidth() * 0.5f;

    const float brightness = p_Common.graticuleBrightness;
    const float zoomFactor = p_St.zoom2x ? 2.0f : 1.0f;

    const float rangeLo = p_App.frame.chromaRangeLow  / zoomFactor;
    const float rangeHi = p_App.frame.chromaRangeHigh / zoomFactor;

    // While a mask is active, reads the same masked trace the Waveform's own
    // Enhanced Render does (App::maskedScope.waveformTrace) - one shared
    // buffer, since both scopes derive from the same per-scanline samples.
    const std::vector<float>* maskedTrace = p_App.maskedScopeValid ? &p_App.maskedScope.waveformTrace : nullptr;
    const bool enhancedReady =
        p_St.enhancedRender && p_App.haveFrame && !p_App.prefs.disableEnhancedRender &&
        PrepareEnhancedTraceCached(p_App.frame, p_St.signalPrefilter, p_St.smoothTrace,
                                   p_St.lowPassKernel, p_St.traceState, p_St.preparedTrace,
                                   maskedTrace, p_App.spatialMask, p_App.valueQualifier,
                                   p_St.traceCache);
    if (enhancedReady)
    {
        DrawVectorscopeEnhanced(p_Draw, p_Cell, p_St, p_App.frame, rangeLo, rangeHi,
                                p_St.enhancedRuns[p_TextureSlot]);
    }
    else if (p_App.haveFrame)
    {
        // A spatial mask restricts what this scope reads, same as it already
        // restricts what the preview-tier panels show - see EnsureMaskedScopeBins.
        const bool      masked       = p_App.maskedScopeValid;
        const uint32_t* vectorCounts = masked ? p_App.maskedScope.vectorscope.data() : nullptr;

        // One texture per cell. ImGui's rendering is deferred, so three cells
        // sharing a texture would all sample whatever was uploaded last - the
        // same trap the waveform parade hit. The cache key is per cell for the
        // same reason: each holds a different band mask's picture.
        Texture&                  tex   = p_St.textures[p_TextureSlot];
        VectorscopeState::Built&  built = p_St.built[p_TextureSlot];

        // Rebuilt only when the frame or an input changed - see DrawFalseColorPanel.
        const bool stale = built.frameIndex != p_App.frame.frameIndex
                        || built.bandMask   != p_BandMask
                        || built.gain       != p_St.gain
                        || built.colorize   != p_St.colorize
                        || built.zoom2x     != p_St.zoom2x
                        || built.masked     != masked
                        || !SpatialMaskEquals(built.mask, p_App.spatialMask)
                        || !ValueQualifierEquals(built.qualifier, p_App.valueQualifier);
        if (stale)
        {
            BuildVectorscopeImage(p_App.frame, p_BandMask, p_St.gain, p_St.colorize,
                                  zoomFactor, p_St.image, vectorCounts);
            if (!p_St.image.Empty())
                tex.UploadRGBA(p_St.image.pixels.data(), p_St.image.width, p_St.image.height);

            built.frameIndex = p_App.frame.frameIndex;
            built.bandMask   = p_BandMask;
            built.gain       = p_St.gain;
            built.colorize   = p_St.colorize;
            built.zoom2x     = p_St.zoom2x;
            built.masked     = masked;
            built.mask       = p_App.spatialMask;
            built.qualifier  = p_App.valueQualifier;
        }

        if (tex.Valid())
            p_Draw->AddImage(tex.ImGuiHandle(), p_Cell.Min, p_Cell.Max);

        if (p_App.maskedScopeValid && p_App.maskedScope.empty)
            DrawMaskEmptyNotice(p_Draw, p_Cell);
    }

    // Graticule on top of the trace, not under it: the targets are a reference the
    // eye aligns the trace against, and a trace that hides them defeats the point.
    //
    // Rings at 25/50/75/100% of legal saturation, brightest at 100%.
    const float legalFraction = 0.5f / rangeHi;

    for (int step = 1; step <= 4; ++step)
    {
        const float r = radius * legalFraction * (float(step) / 4.0f);
        if (r > radius) continue;

        p_Draw->AddCircle(centre, r,
                          GraticuleColor(step == 4 ? kColGraticule : kColGraticuleDim, brightness),
                          96, 1.0f);
    }

    // The plot edge, outside 100% - the chroma range carries headroom so genuinely
    // over-saturated pixels plot visibly rather than clamping onto the legal ring.
    p_Draw->AddCircle(centre, radius, GraticuleColor(kColGraticuleDim, brightness), 96, 1.0f);

    p_Draw->AddLine(ImVec2(centre.x, p_Cell.Min.y), ImVec2(centre.x, p_Cell.Max.y),
                    GraticuleColor(kColGraticuleDim, brightness));
    p_Draw->AddLine(ImVec2(p_Cell.Min.x, centre.y), ImVec2(p_Cell.Max.x, centre.y),
                    GraticuleColor(kColGraticuleDim, brightness));

    if (p_St.showSkinLine)
    {
        // One ray from the centre out to the graticule edge, not a full diameter:
        // the flesh-tone reference is a direction to check hue against, not a
        // symmetric axis.
        const float theta = kSkinToneAngleDeg * 3.14159265f / 180.0f;
        p_Draw->AddLine(centre,
                        ImVec2(centre.x + std::cos(theta) * radius,
                               centre.y - std::sin(theta) * radius),
                        GraticuleColor(kColSkinTone, brightness), 1.0f);
    }

    if (p_St.showTargets && p_App.haveFrame)
    {
        VectorTarget targets[6];
        VectorscopeTargets(p_App.frame, targets);

        const float   box   = p_St.targetSize;
        const uint8_t alpha = uint8_t(std::clamp(p_St.targetOpacity, 0.0f, 1.0f) * 255.0f);

        for (const VectorTarget& t : targets)
        {
            // Against the cell and the frame's TRUE chroma range, so 2x Zoom
            // magnifies the trace against a stationary target rather than carrying
            // the reference along with it.
            const ImVec2 at(p_Cell.Min.x + t.x * p_Cell.GetWidth(),
                            p_Cell.Min.y + t.y * p_Cell.GetHeight());

            p_Draw->AddRect(ImVec2(at.x - box, at.y - box), ImVec2(at.x + box, at.y + box),
                            IM_COL32(t.color.r, t.color.g, t.color.b, alpha), 0.0f, 0, 1.0f);
        }
    }

    // The band name, top-left of its own cell. Omitted for All, where there is no
    // band to name and the label would only be noise.
    if (p_Label)
        p_Draw->AddText(ImVec2(p_Cell.Min.x + 6.0f, p_Cell.Min.y + 4.0f),
                        GraticuleColor(kColAxisLabel, brightness), p_Label);

    if (p_St.zoom2x)
    {
        // A permanent reminder, because of what 2x does *not* move: the targets
        // stay at their true positions, so without this the trace reads as twice
        // as saturated as it is.
        const char* badge = "2x";
        const ImVec2 size = ImGui::CalcTextSize(badge);
        p_Draw->AddText(ImVec2(p_Cell.Max.x - size.x - 6.0f, p_Cell.Min.y + 4.0f),
                        GraticuleColor(kColAxisLabel, brightness), badge);
    }
}

void DrawVectorscopePanel(App& p_App, Panel& p_Panel)
{
    VectorscopeState& st     = p_Panel.vectorscope;
    PanelCommon&      common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    const bool stacked = (st.range == VectorRange::Stack);

    // The rectangle zoom input is measured against has to be the same one the
    // transform is applied to, or the cursor anchor solves for a rectangle that is
    // not on screen and the wheel drifts under the pointer. For a single wheel
    // that is the fitted square; for the stack it is the whole panel, since the
    // three cells fill it.
    const ImRect zoomBase = stacked ? panel : FitAspect(panel, 1.0f, 1.0f);

    common.zoom.HandleInput(zoomBase, common.zoomable);

    draw->PushClipRect(panel.Min, panel.Max, true);

    const ImRect content = common.zoom.Apply(zoomBase);

    if (stacked)
    {
        // Three wheels side by side, one per luma band. Same control as the single
        // bands rather than a separate checkbox, because it is the same question -
        // which band(s) to look at - and the two could never be combined anyway.
        const char*    labels[3] = { "Low", "Mid", "High" };
        const uint32_t bands[3]  = { kBandLow, kBandMid, kBandHigh };

        const float cellW = content.GetWidth() / 3.0f;

        for (int i = 0; i < 3; ++i)
        {
            const ImRect slot(ImVec2(content.Min.x + cellW * float(i), content.Min.y),
                              ImVec2(content.Min.x + cellW * float(i + 1), content.Max.y));

            DrawVectorscopeWheel(p_App, st, common, draw,
                                 FitAspect(slot, 1.0f, 1.0f),
                                 1u << bands[i], labels[i], i);
        }
    }
    else
    {
        uint32_t mask  = 0;
        const char* label = nullptr;

        switch (st.range)
        {
            case VectorRange::Low:  mask = 1u << kBandLow;  label = "Low";  break;
            case VectorRange::Mid:  mask = 1u << kBandMid;  label = "Mid";  break;
            case VectorRange::High: mask = 1u << kBandHigh; label = "High"; break;
            case VectorRange::All:
            default:
                mask = (1u << kBandLow) | (1u << kBandMid) | (1u << kBandHigh);
                break;
        }

        DrawVectorscopeWheel(p_App, st, common, draw, content, mask, label, 0);
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

// ---------------------------------------------------------------------------
// Chromaticity (CIE 1931 x,y)
// ---------------------------------------------------------------------------
//
// Base (binned density) rendering only - see ScopeImages.h's own comment on
// why Enhanced Render isn't offered here yet. Entirely app-side, derived from
// the preview the same way Color Cube is.

// Enhanced Render: an exact per-pixel chromaticity beam, tinted by each
// sample's own source colour - the same technique DrawVectorscopeEnhanced
// uses, reading BuildChromaticityTrace's samples instead of the shared
// waveform_trace mixin (see that function's own comment for why: CIE xy is
// a genuine division, not a linear function of RGB).
//
// Breaks the run on a large chroma jump between neighbouring samples, not
// just on an invalid one - the same fix Vectorscope/White Balance needed for
// the identical reason (an isolated saturated pixel against a neutral
// background otherwise draws a straight sunburst spike through colours that
// are not actually adjacent in the frame). The break distance here is a
// reasoned default (25% of the plot's diagonal span, the same fraction
// Vectorscope/White Balance settled on for their own units) rather than
// measured fresh against real footage the way theirs was - worth revisiting
// if a sunburst shows up on real content.
void DrawChromaticityEnhanced(ImDrawList* p_Draw, const ImRect& p_Content,
                              const std::vector<ChromaticitySample>& p_Trace,
                              float p_Gain, bool p_Colorize)
{
    EnhancedTimer profileThisPass;
    // Same 0.09-at-DEFAULT_GAIN convention every Enhanced Render trace uses:
    // gain is opacity here, since brightness comes from many faint lines
    // agreeing, not from scaling a density count.
    const float   alpha  = std::clamp(0.09f * (p_Gain / 20.0f), 0.015f, 1.0f);
    const uint8_t alpha8 = uint8_t(alpha * 255.0f);
    const ImU32   plainWhite = IM_COL32(235, 240, 245, alpha8);

    const float xSpan = kChromaticityPlotXHi - kChromaticityPlotXLo;
    const float ySpan = kChromaticityPlotYHi - kChromaticityPlotYLo;
    const float breakDistance = std::sqrt(xSpan * xSpan + ySpan * ySpan) * 0.25f;

    std::vector<ImVec2> run;
    std::vector<ImU32>  runColours;
    run.reserve(kWaveformColumns);
    if (p_Colorize) runColours.reserve(kWaveformColumns);

    auto flush = [&]()
    {
        if (run.size() >= 2)
        {
            if (p_Colorize)
            {
                AddColouredPolyline(p_Draw, run.data(), runColours.data(), int(run.size()));
            }
            else
            {
                g_EnhancedProfile.segments += uint64_t(run.size()) - 1;
                g_EnhancedProfile.vertices += uint64_t(run.size()) * 2;   // shared between segments
                EmitTimer emitTimer;
                p_Draw->AddPolyline(run.data(), int(run.size()), plainWhite, ImDrawFlags_None, 1.0f);
            }
        }
        run.clear();
        runColours.clear();
    };

    for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
    {
        run.clear();
        runColours.clear();
        float prevX = 0.0f, prevY = 0.0f;
        bool  havePrev = false;

        for (uint32_t col = 0; col < kWaveformColumns; ++col)
        {
            const ChromaticitySample& s = p_Trace[size_t(row) * size_t(kWaveformColumns) + col];
            if (!s.valid)
            {
                flush();
                havePrev = false;
                continue;
            }

            if (havePrev)
            {
                const float dx = s.x - prevX, dy = s.y - prevY;
                if (std::sqrt(dx * dx + dy * dy) > breakDistance) flush();
            }

            const float tx = (s.x - kChromaticityPlotXLo) / xSpan;
            const float ty = (s.y - kChromaticityPlotYLo) / ySpan;

            // Outside the visible span breaks the run rather than clamping -
            // clamping would pin it to the edge and draw a line that is not
            // in the data, same reasoning the vectorscope's own edge case.
            if (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f)
            {
                flush();
                havePrev = false;
                continue;
            }

            run.push_back(ImVec2(p_Content.Min.x + tx * p_Content.GetWidth(),
                                 p_Content.Max.y - ty * p_Content.GetHeight()));
            if (p_Colorize)
                runColours.push_back(IM_COL32(s.rgb.r, s.rgb.g, s.rgb.b, alpha8));

            prevX = s.x; prevY = s.y; havePrev = true;
        }

        flush();
    }
}

void DrawChromaticityPanel(App& p_App, Panel& p_Panel)
{
    ChromaticityState& st     = p_Panel.chromaticity;
    PanelCommon&       common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    const float xSpan = kChromaticityPlotXHi - kChromaticityPlotXLo;
    const float ySpan = kChromaticityPlotYHi - kChromaticityPlotYLo;
    const ImRect zoomBase = FitAspect(panel, xSpan, ySpan);
    common.zoom.HandleInput(zoomBase, common.zoomable);

    draw->PushClipRect(panel.Min, panel.Max, true);
    const ImRect content = common.zoom.Apply(zoomBase);

    // x increases rightward, y increases *upward* - point_of()'s own mapping,
    // matched here (content.Max.y, not Min.y, is where y = lo lands).
    auto pointOf = [&](float p_X, float p_Y) {
        const float tx = (p_X - kChromaticityPlotXLo) / xSpan;
        const float ty = (p_Y - kChromaticityPlotYLo) / ySpan;
        return ImVec2(content.Min.x + tx * content.GetWidth(),
                      content.Max.y - ty * content.GetHeight());
    };

    const float brightness = common.graticuleBrightness;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        bool drewEnhanced = false;

        if (st.enhancedRender && !p_App.prefs.disableEnhancedRender)
        {
            const bool frameChanged     = st.traceFrame != p_App.frame.frameIndex;
            const bool maskChanged      = !SpatialMaskEquals(st.traceForMask, p_App.spatialMask);
            const bool qualifierChanged = !ValueQualifierEquals(st.traceForQualifier, p_App.valueQualifier);

            if (frameChanged || maskChanged || qualifierChanged)
            {
                BuildChromaticityTrace(p_App.frame, &p_App.spatialMask, &p_App.valueQualifier, st.trace);
                st.traceHasAnyValid = std::any_of(st.trace.begin(), st.trace.end(),
                                                  [](const ChromaticitySample& s) { return s.valid; });
                st.traceFrame         = p_App.frame.frameIndex;
                st.traceForMask       = p_App.spatialMask;
                st.traceForQualifier  = p_App.valueQualifier;
            }

            if (st.traceHasAnyValid)
            {
                DrawChromaticityEnhanced(draw, content, st.trace, st.gain, st.colorize);
                drewEnhanced = true;
            }
        }

        if (!drewEnhanced)
        {
            if (st.analysisFrame != p_App.frame.frameIndex)
            {
                BuildChromaticityAnalysis(p_App.frame, st.analysis);
                st.analysisFrame = p_App.frame.frameIndex;
            }

            BuildChromaticityImage(st.analysis, st.gain, st.colorize, st.image);
            if (!st.image.Empty())
            {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
                draw->AddImage(st.texture.ImGuiHandle(), content.Min, content.Max);
            }

            if (st.analysis.empty)
            {
                const char* msg = "waiting for video";
                const ImVec2 size = ImGui::CalcTextSize(msg);
                draw->AddText(ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f),
                             kColMuted, msg);
            }
        }
    }
    else
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f),
                     kColMuted, msg);
    }

    // Graticule on top of the trace, not under it - see the vectorscope's own
    // reasoning: it is the reference the eye aligns the trace against.
    if (st.showLocus)
    {
        const std::vector<Vec2>& locus = ChromaticitySpectralLocus();
        const ImU32 col = GraticuleColor(kColGraticule, brightness);
        for (size_t i = 0; i < locus.size(); ++i)
        {
            const ImVec2 a = pointOf(locus[i].x, locus[i].y);
            const ImVec2 b = pointOf(locus[(i + 1) % locus.size()].x, locus[(i + 1) % locus.size()].y);
            draw->AddLine(a, b, col, 1.0f);   // closes via the line of purples
        }
    }

    if (st.showBlackbody)
    {
        // Open curve, not closed like the spectral locus - a temperature
        // trajectory has a start and an end, not a boundary to seal.
        const std::vector<Vec2>& bb = ChromaticityBlackbodyLocus();
        const ImU32 col = GraticuleColor(IM_COL32(255, 150, 60, 235), brightness);
        for (size_t i = 0; i + 1 < bb.size(); ++i)
            draw->AddLine(pointOf(bb[i].x, bb[i].y), pointOf(bb[i + 1].x, bb[i + 1].y), col, 1.0f);
    }

    for (const GamutTriangle& gamut : kGamutTriangles)
    {
        const ImU32 tint = GraticuleColor(IM_COL32(gamut.tint.r, gamut.tint.g, gamut.tint.b, 220), brightness);
        ImVec2 pts[3];
        for (int i = 0; i < 3; ++i) pts[i] = pointOf(gamut.primaries[i].x, gamut.primaries[i].y);
        for (int i = 0; i < 3; ++i) draw->AddLine(pts[i], pts[(i + 1) % 3], tint, 1.0f);
    }

    {
        const ImVec2 white = pointOf(kGamutTriangles[0].white.x, kGamutTriangles[0].white.y);
        draw->AddCircle(white, 3.0f, GraticuleColor(kColGraticule, brightness), 16, 1.0f);
    }

    if (p_App.haveFrame && !st.analysis.empty)
    {
        // Fraction of this frame's (valid) pixels each reference gamut can't
        // reproduce - the quantitative half of "is this deliverable", next to
        // the visual scatter/triangle overlay.
        char lines[3][32];
        for (int g = 0; g < 3; ++g)
            std::snprintf(lines[g], sizeof(lines[g]), "%s: %.1f%% outside",
                         kGamutTriangles[g].name, st.analysis.fractionOutside[g] * 100.0f);

        const ImVec2 boxMin(content.Min.x + 6.0f, content.Min.y + 6.0f);
        const ImVec2 boxMax(boxMin.x + 170.0f, boxMin.y + 14.0f * 3 + 6.0f);
        draw->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 190));
        for (int g = 0; g < 3; ++g)
            draw->AddText(ImVec2(boxMin.x + 4.0f, boxMin.y + 14.0f * float(g) + 3.0f), kColMuted, lines[g]);
    }

    // Cursor readout, bottom-left - the exact (x, y) under the pointer.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(content.Min, content.Max))
    {
        const ImVec2 pos = ImGui::GetIO().MousePos;
        const float tx = (pos.x - content.Min.x) / content.GetWidth();
        const float ty = (content.Max.y - pos.y) / content.GetHeight();
        const float x = kChromaticityPlotXLo + tx * xSpan;
        const float y = kChromaticityPlotYLo + ty * ySpan;

        char text[64];
        std::snprintf(text, sizeof(text), "x %.4f   y %.4f", x, y);
        const ImVec2 boxMin(content.Min.x + 6.0f, content.Max.y - 20.0f);
        const ImVec2 boxMax(boxMin.x + 140.0f, boxMin.y + 14.0f);
        draw->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 190));
        draw->AddText(ImVec2(boxMin.x + 4.0f, boxMin.y + 1.0f), IM_COL32(255, 210, 90, 255), text);
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawChromaticitySettings(Panel& p_Panel)
{
    ChromaticityState& st     = p_Panel.chromaticity;
    PanelCommon&       common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Chromaticity Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##gain", &st.gain, 1.0f, 80.0f, 20.0f, "%.0f");

    SettingsRowLabel("Colorize");
    ImGui::Checkbox("##colorize", &st.colorize);

    SettingsRowLabel("Spectral Locus");
    ImGui::Checkbox("##locus", &st.showLocus);

    SettingsRowLabel("Blackbody Curve");
    ImGui::Checkbox("##blackbody", &st.showBlackbody);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The Planckian locus: the chromaticity trajectory of an\n"
                          "ideal blackbody radiator as its temperature changes\n"
                          "(1667K-25000K) - useful for reading off where a white\n"
                          "balance decision actually sits.");

    ImGui::Separator();
    SettingsRowLabel("Enhanced Render");
    ImGui::Checkbox("##enhanced", &st.enhancedRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Plots exact per-pixel chromaticity as a beam trace instead\n"
                          "of the binned density map. Unlike the density map, this mode\n"
                          "honours the spatial mask and Histogram/HSL qualifiers. Does\n"
                          "not offer Smooth Trace/Low-Pass Filter/Signal Pre-filter -\n"
                          "those blur raw RGB before this panel's xy conversion, which\n"
                          "is exactly the wrong-error-first mistake this whole panel\n"
                          "avoids by reading pixels directly instead of the shared\n"
                          "waveform trace.");

    DrawSharedSettings(common);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Sat vs Luma / Sat vs Hue
// ---------------------------------------------------------------------------
//
// Both DaVinci Resolve "Curves" style - see LumaVsSatState/HueVsSatState's
// own comments. Two separate implementations, not one shared function: Sat
// vs Luma reads the app's own existing Y-plane histogram directly (its
// background reaches genuine white, so the overlay needs a two-tone stroke
// for legibility across the whole range), Sat vs Hue runs its own per-pixel
// pass over a dimmed hue wheel (bright enough throughout that a plain white
// stroke already reads).

// A dark stroke under a light one, so a line/label stays legible over a
// background that spans genuine black to genuine white - same reasoning the
// rotate-cursor glyph's own two-tone stroke uses for sitting over arbitrary
// video.
void AddOutlinedPolyline(ImDrawList* p_Draw, const ImVec2* p_Points, int p_Count)
{
    p_Draw->AddPolyline(p_Points, p_Count, IM_COL32(0, 0, 0, 200), ImDrawFlags_None, 3.0f);
    p_Draw->AddPolyline(p_Points, p_Count, IM_COL32(255, 255, 255, 255), ImDrawFlags_None, 1.5f);
}

void AddOutlinedText(ImDrawList* p_Draw, ImVec2 p_Pos, const char* p_Text)
{
    const ImU32 outline = IM_COL32(0, 0, 0, 200);
    p_Draw->AddText(ImVec2(p_Pos.x - 1.0f, p_Pos.y), outline, p_Text);
    p_Draw->AddText(ImVec2(p_Pos.x + 1.0f, p_Pos.y), outline, p_Text);
    p_Draw->AddText(ImVec2(p_Pos.x, p_Pos.y - 1.0f), outline, p_Text);
    p_Draw->AddText(ImVec2(p_Pos.x, p_Pos.y + 1.0f), outline, p_Text);
    p_Draw->AddText(p_Pos, IM_COL32(255, 255, 255, 255), p_Text);
}

void DrawLumaVsSatPanel(App& p_App, Panel& p_Panel)
{
    LumaVsSatState& st     = p_Panel.lumaVsSat;
    PanelCommon&    common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    common.zoom.HandleInput(panel, common.zoomable);
    draw->PushClipRect(panel.Min, panel.Max, true);
    const ImRect content = common.zoom.Apply(panel);

    // Luma gradient background, built once (it never depends on frame data).
    if (!st.gradientBuilt)
    {
        Image gradient;
        BuildLumaGradientImage(360, gradient);
        st.gradientTexture.UploadRGBA(gradient.pixels.data(), gradient.width, gradient.height);
        st.gradientBuilt = true;
    }
    draw->AddImage(st.gradientTexture.ImGuiHandle(), content.Min, content.Max);

    if (p_App.haveFrame && p_App.frame.histogram.size() >= kHistogramCells)
    {
        // The Y-plane histogram every Histogram panel already reads -
        // masked when a spatial mask/qualifier is active, same as that
        // panel - not a fresh per-pixel pass over the preview.
        const bool      maskActive = p_App.maskedScopeValid;
        const uint32_t* histCounts = maskActive ? p_App.maskedScope.histogram.data()
                                                : p_App.frame.histogram.data();

        uint32_t peak = 0;
        for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
            peak = std::max(peak, histCounts[HistogramIndex(kPlaneY, bin)]);

        if (peak > 0)
        {
            const float logPeak = std::log1p(float(peak));
            std::vector<ImVec2> outline;
            outline.reserve(kHistogramBins);

            ImVec2 prevPoint(0.0f, 0.0f);
            bool   havePrev = false;

            for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
            {
                const uint32_t count = histCounts[HistogramIndex(kPlaneY, bin)];
                const float norm = st.logScale
                    ? (logPeak > 0.0f ? std::log1p(float(count)) / logPeak : 0.0f)
                    : float(count) / float(peak);

                const float x = content.Min.x + content.GetWidth() * float(bin) / float(kHistogramBins - 1);
                const float y = content.Max.y - norm * content.GetHeight();

                // Grey, not white: a translucent white fill would blow out
                // to solid white against the background's own bright end,
                // where a grey fill instead reads as a slight darkening -
                // legible envelope shading across the full luma range.
                if (havePrev)
                    draw->AddQuadFilled(prevPoint, ImVec2(x, y), ImVec2(x, content.Max.y),
                                        ImVec2(prevPoint.x, content.Max.y), IM_COL32(128, 128, 128, 90));

                prevPoint = ImVec2(x, y);
                havePrev  = true;
                outline.push_back(ImVec2(x, y));
            }

            if (outline.size() >= 2)
                AddOutlinedPolyline(draw, outline.data(), int(outline.size()));
        }

        if (maskActive && p_App.maskedScope.empty)
            DrawMaskEmptyNotice(draw, panel);
    }
    else
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        AddOutlinedText(draw, ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f), msg);
    }

    // Luma across the bottom, every 10 - plain 0-100, matching the
    // Histogram/old app's own IRE-free convention for this axis.
    for (int level = 0; level <= 100; level += 10)
    {
        const float x = content.Min.x + (float(level) / 100.0f) * content.GetWidth();
        draw->AddLine(ImVec2(x, content.Min.y), ImVec2(x, content.Max.y), IM_COL32(128, 128, 128, 100), 1.0f);
        if (level % 20 == 0)
        {
            char label[8];
            std::snprintf(label, sizeof(label), "%d", level);
            AddOutlinedText(draw, ImVec2(x + 2.0f, content.Max.y - 16.0f), label);
        }
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawLumaVsSatSettings(Panel& p_Panel)
{
    LumaVsSatState& st     = p_Panel.lumaVsSat;
    PanelCommon&    common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Luma vs Sat Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Log Scale");
    ImGui::Checkbox("##log", &st.logScale);

    DrawSharedSettings(common);
    ImGui::End();
}

// Hue vs Sat, DaVinci Resolve "Curves" style - see HueVsSatState's own
// comment.
// rather than a third mode bolted onto the shared one.
void DrawHueVsSatPanel(App& p_App, Panel& p_Panel)
{
    HueVsSatState& st     = p_Panel.hueVsSat;
    PanelCommon&   common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    common.zoom.HandleInput(panel, common.zoomable);
    draw->PushClipRect(panel.Min, panel.Max, true);
    const ImRect content = common.zoom.Apply(panel);

    // Hue-wheel background, built once (it never depends on frame data) and
    // stretched over the plot as a real texture rather than 360 individual
    // AddRectFilled calls, so it interpolates smoothly instead of banding.
    if (!st.gradientBuilt)
    {
        Image gradient;
        BuildHueGradientImage(360, gradient);
        st.gradientTexture.UploadRGBA(gradient.pixels.data(), gradient.width, gradient.height);
        st.gradientBuilt = true;
    }
    draw->AddImage(st.gradientTexture.ImGuiHandle(), content.Min, content.Max);

    const ImU32 kTextColor = IM_COL32(255, 255, 255, 235);

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        const bool frameChanged     = st.analysisFrame != p_App.frame.frameIndex;
        const bool maskChanged      = !SpatialMaskEquals(st.analysisForMask, p_App.spatialMask);
        const bool qualifierChanged = !ValueQualifierEquals(st.analysisForQualifier, p_App.valueQualifier);

        if (frameChanged || maskChanged || qualifierChanged)
        {
            BuildHueHistogramAnalysis(p_App.frame, &p_App.spatialMask, &p_App.valueQualifier, st.analysis);
            st.analysisFrame        = p_App.frame.frameIndex;
            st.analysisForMask      = p_App.spatialMask;
            st.analysisForQualifier = p_App.valueQualifier;
        }

        if (!st.analysis.empty && st.analysis.peak > 0)
        {
            // Same trapezoid-fill-between-consecutive-points technique
            // DrawHistogramLane uses: a solid area under a crisp top edge,
            // not per-bin bars (which stripe once the panel is wider than
            // bins x bar-width).
            const float logPeak = std::log1p(float(st.analysis.peak));
            const ImU32 fill = IM_COL32(255, 255, 255, 100);
            const ImU32 line = IM_COL32(255, 255, 255, 255);

            std::vector<ImVec2> outline;
            outline.reserve(kHueHistogramBins);
            ImVec2 prevPoint(0.0f, 0.0f);
            bool   havePrev = false;

            for (int bin = 0; bin < kHueHistogramBins; ++bin)
            {
                const uint32_t count = st.analysis.counts[bin];
                const float norm = st.logScale
                    ? (logPeak > 0.0f ? std::log1p(float(count)) / logPeak : 0.0f)
                    : float(count) / float(st.analysis.peak);

                const float x = content.Min.x + content.GetWidth() * float(bin) / float(kHueHistogramBins - 1);
                const float y = content.Max.y - norm * content.GetHeight();

                if (havePrev)
                    draw->AddQuadFilled(prevPoint, ImVec2(x, y), ImVec2(x, content.Max.y),
                                        ImVec2(prevPoint.x, content.Max.y), fill);

                prevPoint = ImVec2(x, y);
                havePrev  = true;
                outline.push_back(ImVec2(x, y));
            }

            if (outline.size() >= 2)
                draw->AddPolyline(outline.data(), int(outline.size()), line, ImDrawFlags_None, 1.5f);
        }
        else if (st.analysis.empty)
        {
            const char* msg = "waiting for video";
            const ImVec2 size = ImGui::CalcTextSize(msg);
            draw->AddText(ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f),
                         kTextColor, msg);
        }
    }
    else
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f),
                     kTextColor, msg);
    }

    // Landmark hues across the bottom, same order the vectorscope's own
    // colour bars use - lines only, no labels (the hue wheel background
    // already says which colour each line sits at).
    static const float kLandmarkAngles[] = { 0.0f, 60.0f, 120.0f, 180.0f, 240.0f, 300.0f };
    for (float angle : kLandmarkAngles)
    {
        const float x = content.Min.x + (angle / 360.0f) * content.GetWidth();
        draw->AddLine(ImVec2(x, content.Min.y), ImVec2(x, content.Max.y), IM_COL32(255, 255, 255, 70), 1.0f);
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawHueVsSatSettings(Panel& p_Panel)
{
    HueVsSatState& st     = p_Panel.hueVsSat;
    PanelCommon&   common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Hue vs Sat Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Log Scale");
    ImGui::Checkbox("##log", &st.logScale);

    DrawSharedSettings(common);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Source (live video preview) + spatial mask editing
// ---------------------------------------------------------------------------
//
// Ctrl-drag draws a fresh mask (Ellipse/Rectangle/Gradient, chosen in this
// panel's Settings), Shift-drag draws one already inverted (garbage-matte:
// everywhere *except* the shape counts as "in"). Once a mask exists, drag its
// body to move it, a corner to resize (or the two endpoints for Gradient), or
// the handle above it to rotate. Double-click outside it to clear. Ported
// from VideoPanel's mouse handlers in the old app - same gestures, same
// handle set, same screen<->image-fraction mapping via the panel's own fit
// rect (so the mask tracks the image through zoom/pan).

// Screen-space (handle, position) pairs for the current mask - two endpoints
// for Gradient, four corners + a rotation handle for Ellipse/Rectangle.
// Returns how many of p_OutHandle/p_OutPos (each sized 5) were filled.
int SpatialMaskHandlePoints(const SpatialMask& p_Mask, const ImRect& p_Fit,
                            MaskHandle p_OutHandle[5], ImVec2 p_OutPos[5])
{
    if (!p_Mask.hasGeometry) return 0;

    auto toScreen = [&](float p_Fx, float p_Fy) {
        return ImVec2(p_Fit.Min.x + p_Fx * p_Fit.GetWidth(), p_Fit.Min.y + p_Fy * p_Fit.GetHeight());
    };

    if (p_Mask.shape == MaskShape::Gradient)
    {
        p_OutHandle[0] = MaskHandle::Start; p_OutPos[0] = toScreen(p_Mask.x0, p_Mask.y0);
        p_OutHandle[1] = MaskHandle::End;   p_OutPos[1] = toScreen(p_Mask.x1, p_Mask.y1);
        return 2;
    }

    const ImVec2 c  = toScreen((p_Mask.x0 + p_Mask.x1) * 0.5f, (p_Mask.y0 + p_Mask.y1) * 0.5f);
    const ImVec2 p0 = toScreen(p_Mask.x0, p_Mask.y0);
    const ImVec2 p1 = toScreen(p_Mask.x1, p_Mask.y1);
    const float halfW = std::fabs(p1.x - p0.x) * 0.5f;
    const float halfH = std::fabs(p1.y - p0.y) * 0.5f;
    const float cosA = std::cos(p_Mask.angle), sinA = std::sin(p_Mask.angle);
    auto rot = [&](float p_Sx, float p_Sy) {
        return ImVec2(c.x + p_Sx * cosA - p_Sy * sinA, c.y + p_Sx * sinA + p_Sy * cosA);
    };

    p_OutHandle[0] = MaskHandle::X0Y0;   p_OutPos[0] = rot(-halfW, -halfH);
    p_OutHandle[1] = MaskHandle::X1Y0;   p_OutPos[1] = rot(halfW, -halfH);
    p_OutHandle[2] = MaskHandle::X0Y1;   p_OutPos[2] = rot(-halfW, halfH);
    p_OutHandle[3] = MaskHandle::X1Y1;   p_OutPos[3] = rot(halfW, halfH);
    p_OutHandle[4] = MaskHandle::Rotate; p_OutPos[4] = rot(0.0f, -halfH - 25.0f);
    return 5;
}

MaskHandle SpatialMaskHandleAt(const SpatialMask& p_Mask, const ImRect& p_Fit, ImVec2 p_Pos)
{
    MaskHandle handles[5]; ImVec2 pts[5];
    const int n = SpatialMaskHandlePoints(p_Mask, p_Fit, handles, pts);
    const float r = 10.0f;
    for (int i = 0; i < n; ++i)
        if (std::fabs(p_Pos.x - pts[i].x) <= r && std::fabs(p_Pos.y - pts[i].y) <= r)
            return handles[i];
    return MaskHandle::None;
}

// Whether p_Pos falls inside the mask (Ellipse/Rectangle) or close enough to
// the line (Gradient) to grab-and-move it. Deliberately ignores `invert` -
// grabbing the shape to move it works the same whether it currently reads as
// "in" or "out".
bool SpatialMaskBodyHit(const SpatialMask& p_Mask, const ImRect& p_Fit, ImVec2 p_Pos)
{
    if (!p_Mask.hasGeometry) return false;

    if (p_Mask.shape == MaskShape::Gradient)
    {
        const ImVec2 p0(p_Fit.Min.x + p_Mask.x0 * p_Fit.GetWidth(), p_Fit.Min.y + p_Mask.y0 * p_Fit.GetHeight());
        const ImVec2 p1(p_Fit.Min.x + p_Mask.x1 * p_Fit.GetWidth(), p_Fit.Min.y + p_Mask.y1 * p_Fit.GetHeight());
        const float dx = p1.x - p0.x, dy = p1.y - p0.y;
        const float lenSq = dx * dx + dy * dy;
        if (lenSq < 1e-6f) return false;
        const float t = std::clamp(((p_Pos.x - p0.x) * dx + (p_Pos.y - p0.y) * dy) / lenSq, 0.0f, 1.0f);
        const ImVec2 nearest(p0.x + t * dx, p0.y + t * dy);
        return (std::fabs(p_Pos.x - nearest.x) + std::fabs(p_Pos.y - nearest.y)) <= 15.0f;
    }

    if (p_Fit.GetWidth() <= 0.0f || p_Fit.GetHeight() <= 0.0f) return false;
    const float fx = std::clamp((p_Pos.x - p_Fit.Min.x) / p_Fit.GetWidth(),  0.0f, 1.0f);
    const float fy = std::clamp((p_Pos.y - p_Fit.Min.y) / p_Fit.GetHeight(), 0.0f, 1.0f);
    const float aspect = p_Fit.GetWidth() / std::max(p_Fit.GetHeight(), 1.0f);

    const float cx = (p_Mask.x0 + p_Mask.x1) * 0.5f, cy = (p_Mask.y0 + p_Mask.y1) * 0.5f;
    const float c = std::cos(-p_Mask.angle), s = std::sin(-p_Mask.angle);
    const float dx = (fx - cx) * aspect, dy = fy - cy;
    const float rotX = dx * c - dy * s, rotY = dx * s + dy * c;
    const float rx = std::max(std::fabs(p_Mask.x1 - p_Mask.x0) * 0.5f, 1e-6f);
    const float ry = std::max(std::fabs(p_Mask.y1 - p_Mask.y0) * 0.5f, 1e-6f);

    if (p_Mask.shape == MaskShape::Rectangle)
        return std::fabs(rotX) <= rx * aspect && std::fabs(rotY) <= ry;
    return (rotX / (rx * aspect)) * (rotX / (rx * aspect)) + (rotY / ry) * (rotY / ry) <= 1.0f;
}

// Orders a mask's stored corners so x0<=x1 and y0<=y1. Everything that draws
// or applies a mask goes through midpoints and abs() and so cannot tell the
// difference - the handles can, since they are identified by where they sit
// on screen. Drag a mask out upward/leftward and the stored (x0,y0) is its
// bottom-right corner; grabbing the top-left handle next would then write the
// top-left position into the bottom-right pair and the shape would collapse
// on the very first adjustment. Gradient is left alone - its two points are a
// direction, not a bounding box, and swapping them would reverse the falloff.
void NormaliseSpatialMask(SpatialMask& p_Mask)
{
    if (!p_Mask.hasGeometry || p_Mask.shape == MaskShape::Gradient) return;
    const float x0 = p_Mask.x0, y0 = p_Mask.y0, x1 = p_Mask.x1, y1 = p_Mask.y1;
    p_Mask.x0 = std::min(x0, x1); p_Mask.y0 = std::min(y0, y1);
    p_Mask.x1 = std::max(x0, x1); p_Mask.y1 = std::max(y0, y1);
}

// A partial ring with an arrowhead at one end, for the rotate handle - reads
// as "spin this" at a glance, which neither GLFW's own standard cursor set
// (Hand is a pointing-finger link cursor, ResizeAll four straight arrows) nor
// ImGui's (the same list under the hood) does. Hand-drawn into a pixel buffer
// and set directly through GLFW rather than ImGui::SetMouseCursor, which only
// knows how to ask GLFW for a cursor from that standard set - outlined by
// drawing every shape slightly larger in black first, then true size in
// white on top.
GLFWcursor* RotateCursor()
{
    static GLFWcursor* cursor = nullptr;
    if (cursor) return cursor;

    constexpr int kSize = 24;
    std::vector<unsigned char> px(size_t(kSize) * kSize * 4, 0);

    auto setPixel = [&](int x, int y, unsigned char c)
    {
        if (x < 0 || x >= kSize || y < 0 || y >= kSize) return;
        unsigned char* p = &px[(size_t(y) * kSize + x) * 4];
        p[0] = c; p[1] = c; p[2] = c; p[3] = 255;
    };

    // A filled disk, used both as the ring's own stroke thickness (stamped
    // along the arc below) and stray-pixel-free enough for the arrowhead's
    // corners.
    auto fillDisk = [&](int cx, int cy, float radius, unsigned char c)
    {
        const int r = int(radius) + 1;
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (float(x * x + y * y) <= radius * radius)
                    setPixel(cx + x, cy + y, c);
    };

    // A triangular arrowhead, apex at (p_TipX, p_TipY) pointing along the unit
    // vector (p_DirX, p_DirY) - filled by testing which side of each of the
    // triangle's three edges a pixel falls on, cheap and exact at this size.
    auto fillArrowhead = [&](float p_TipX, float p_TipY, float p_DirX, float p_DirY,
                             float p_Length, float p_HalfWidth, unsigned char c)
    {
        const float backX = p_TipX - p_DirX * p_Length, backY = p_TipY - p_DirY * p_Length;
        const float perpX = -p_DirY, perpY = p_DirX;
        const float leftX  = backX + perpX * p_HalfWidth, leftY  = backY + perpY * p_HalfWidth;
        const float rightX = backX - perpX * p_HalfWidth, rightY = backY - perpY * p_HalfWidth;

        auto side = [](float px, float py, float ax, float ay, float bx, float by)
        {
            return (px - bx) * (ay - by) - (ax - bx) * (py - by);
        };

        const float minX = std::min({ p_TipX, leftX, rightX }) - 1.0f;
        const float maxX = std::max({ p_TipX, leftX, rightX }) + 1.0f;
        const float minY = std::min({ p_TipY, leftY, rightY }) - 1.0f;
        const float maxY = std::max({ p_TipY, leftY, rightY }) + 1.0f;

        for (int y = int(minY); y <= int(maxY); ++y)
            for (int x = int(minX); x <= int(maxX); ++x)
            {
                const float fx = float(x) + 0.5f, fy = float(y) + 0.5f;
                const float d1 = side(fx, fy, p_TipX, p_TipY, leftX, leftY);
                const float d2 = side(fx, fy, leftX, leftY, rightX, rightY);
                const float d3 = side(fx, fy, rightX, rightY, p_TipX, p_TipY);
                const bool hasNeg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
                const bool hasPos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
                if (!(hasNeg && hasPos)) setPixel(x, y, c);
            }
    };

    constexpr float kCx = 12.0f, kCy = 12.0f, kRadius = 8.0f;
    constexpr float kStartDeg = -220.0f, kEndDeg = 20.0f;   // about 240 degrees of arc
    constexpr float kPi = 3.14159265358979323846f;

    // Stamped along the arc rather than traced pixel-by-pixel, same reasoning
    // as GrabCursor's rects/circle: cheap, and every step's disk overlaps the
    // last one enough at this radius/step size to leave no gaps.
    auto drawArc = [&](float p_Thickness, float p_ArrowLen, float p_ArrowWidth, unsigned char c)
    {
        for (float deg = kStartDeg; deg <= kEndDeg; deg += 4.0f)
        {
            const float rad = deg * kPi / 180.0f;
            fillDisk(int(kCx + std::cos(rad) * kRadius), int(kCy + std::sin(rad) * kRadius), p_Thickness, c);
        }

        // Arrowhead at the arc's end, pointing tangentially - the direction
        // rotation would continue in, not radially outward.
        const float endRad = kEndDeg * kPi / 180.0f;
        const float tipX = kCx + std::cos(endRad) * kRadius;
        const float tipY = kCy + std::sin(endRad) * kRadius;
        const float dirX = -std::sin(endRad), dirY = std::cos(endRad);
        fillArrowhead(tipX, tipY, dirX, dirY, p_ArrowLen, p_ArrowWidth, c);
    };

    drawArc(2.2f, 6.0f, 4.0f, 0);     // outline pass: fatter, drawn first
    drawArc(1.4f, 5.0f, 3.0f, 255);   // true-size white pass on top

    GLFWimage image{ kSize, kSize, px.data() };
    cursor = glfwCreateCursor(&image, 12, 12);
    return cursor;
}

void UpdateSpatialMaskInput(App& p_App, const ImRect& p_Fit, int p_PanelId)
{
    ImGuiIO& io = ImGui::GetIO();
    MaskInputState& in = p_App.maskInput;
    SpatialMask& mask = p_App.spatialMask;
    const ImVec2 pos = io.MousePos;
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    auto imageFraction = [&](ImVec2 p_Pos, float& p_Fx, float& p_Fy) -> bool {
        if (p_Fit.GetWidth() <= 0.0f || p_Fit.GetHeight() <= 0.0f) return false;
        p_Fx = std::clamp((p_Pos.x - p_Fit.Min.x) / p_Fit.GetWidth(),  0.0f, 1.0f);
        p_Fy = std::clamp((p_Pos.y - p_Fit.Min.y) / p_Fit.GetHeight(), 0.0f, 1.0f);
        return true;
    };
    auto toScreen = [&](float p_Fx, float p_Fy) {
        return ImVec2(p_Fit.Min.x + p_Fx * p_Fit.GetWidth(), p_Fit.Min.y + p_Fy * p_Fit.GetHeight());
    };

    // --- Press: start a new mask, grab a handle, or grab the body to move ---
    if (hovered && in.editing == MaskHandle::None && !in.drawingNew &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const bool ctrl  = io.KeyCtrl;
        const bool shift = io.KeyShift;

        if (!(ctrl || shift) && mask.hasGeometry)
        {
            const MaskHandle handle = SpatialMaskHandleAt(mask, p_Fit, pos);
            if (handle != MaskHandle::None)
            {
                in.editing = handle;
                in.editStartGeometry = mask;
                in.owningPanelId = p_PanelId;
            }
            else if (SpatialMaskBodyHit(mask, p_Fit, pos))
            {
                in.editing = MaskHandle::Move;
                in.editStartGeometry = mask;
                in.owningPanelId = p_PanelId;
                imageFraction(pos, in.editAnchorFx, in.editAnchorFy);
            }
        }
        else if (ctrl || shift)
        {
            float fx, fy;
            if (imageFraction(pos, fx, fy))
            {
                in.drawingNew = true;
                in.owningPanelId = p_PanelId;
                in.startFx = fx; in.startFy = fy;
                in.moved = false;
                mask.hasGeometry = true;
                mask.x0 = mask.x1 = fx;
                mask.y0 = mask.y1 = fy;
                mask.angle = 0.0f;
                // Ctrl always starts a normal mask, Shift always an inverted
                // one, regardless of what the mask before this one was.
                mask.invert = shift;
            }
        }
    }

    // --- Drag: editing an existing mask's move/resize/rotate ---
    if (in.editing != MaskHandle::None && in.owningPanelId == p_PanelId)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const SpatialMask& start = in.editStartGeometry;
            SpatialMask next = start;

            if (in.editing == MaskHandle::Move)
            {
                float fx, fy;
                if (imageFraction(pos, fx, fy))
                {
                    const float dx = fx - in.editAnchorFx, dy = fy - in.editAnchorFy;
                    next.x0 = start.x0 + dx; next.y0 = start.y0 + dy;
                    next.x1 = start.x1 + dx; next.y1 = start.y1 + dy;
                }
            }
            else if (in.editing == MaskHandle::Rotate)
            {
                const ImVec2 c = toScreen((start.x0 + start.x1) * 0.5f, (start.y0 + start.y1) * 0.5f);
                next.angle = std::atan2(pos.y - c.y, pos.x - c.x) + 3.14159265358979323846f * 0.5f;
            }
            else
            {
                // Resize: unrotate the mouse position about the mask's own centre
                // first, then map that back to an image fraction - corners are
                // stored in the shape's own unrotated local frame.
                const ImVec2 c = toScreen((start.x0 + start.x1) * 0.5f, (start.y0 + start.y1) * 0.5f);
                const float msx = pos.x - c.x, msy = pos.y - c.y;
                const float cosA = std::cos(-start.angle), sinA = std::sin(-start.angle);
                const float ux = msx * cosA - msy * sinA, uy = msx * sinA + msy * cosA;
                float fx, fy;
                if (imageFraction(ImVec2(c.x + ux, c.y + uy), fx, fy))
                {
                    switch (in.editing)
                    {
                        case MaskHandle::Start: case MaskHandle::X0Y0: next.x0 = fx; next.y0 = fy; break;
                        case MaskHandle::End:                          next.x1 = fx; next.y1 = fy; break;
                        case MaskHandle::X1Y0:                         next.x1 = fx; next.y0 = fy; break;
                        case MaskHandle::X0Y1:                         next.x0 = fx; next.y1 = fy; break;
                        case MaskHandle::X1Y1:                         next.x1 = fx; next.y1 = fy; break;
                        default: break;
                    }
                }
            }

            next.x0 = std::clamp(next.x0, 0.0f, 1.0f); next.y0 = std::clamp(next.y0, 0.0f, 1.0f);
            next.x1 = std::clamp(next.x1, 0.0f, 1.0f); next.y1 = std::clamp(next.y1, 0.0f, 1.0f);
            mask = next;
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            in.editing = MaskHandle::None;
            in.owningPanelId = -1;
            // Settled on release rather than mid-drag, so the handle under the
            // mouse keeps its identity for as long as the button is held.
            NormaliseSpatialMask(mask);
        }
    }

    // --- Drag: drawing a brand new mask ---
    if (in.drawingNew && in.owningPanelId == p_PanelId)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float fx, fy;
            if (imageFraction(pos, fx, fy))
            {
                if (fx != in.startFx || fy != in.startFy) in.moved = true;
                mask.x0 = in.startFx; mask.y0 = in.startFy;
                mask.x1 = fx; mask.y1 = fy;
            }
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            in.drawingNew = false;
            in.owningPanelId = -1;
            if (!in.moved)
                // A plain Ctrl/Shift-click, not a drag - clears rather than
                // leaves a degenerate zero-size mask.
                mask.hasGeometry = false;
            NormaliseSpatialMask(mask);
        }
    }

    // Double-click outside an existing mask clears it - a quicker way out than
    // Settings -> Clear Mask.
    if (hovered && mask.hasGeometry && in.editing == MaskHandle::None && !in.drawingNew &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        SpatialMaskHandleAt(mask, p_Fit, pos) == MaskHandle::None &&
        !SpatialMaskBodyHit(mask, p_Fit, pos))
    {
        mask.hasGeometry = false;
    }

    if (!hovered) return;

    MaskHandle underMouse = in.editing;
    if (underMouse == MaskHandle::None && !in.drawingNew)
        underMouse = SpatialMaskHandleAt(mask, p_Fit, pos);

    switch (underMouse)
    {
        case MaskHandle::Rotate:                          glfwSetCursor(glfwGetCurrentContext(), RotateCursor()); break;
        case MaskHandle::X0Y0: case MaskHandle::X1Y1:      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
        case MaskHandle::X1Y0: case MaskHandle::X0Y1:      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
        case MaskHandle::Move: case MaskHandle::Start: case MaskHandle::End:
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            break;
        default:
            if (in.editing == MaskHandle::None && !in.drawingNew && SpatialMaskBodyHit(mask, p_Fit, pos))
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            break;
    }
}

void DrawSpatialMaskOverlay(ImDrawList* p_Draw, const SpatialMask& p_Mask, const ImRect& p_Fit)
{
    if (!p_Mask.hasGeometry) return;

    const ImU32 lineCol = IM_COL32(255, 210, 90, 220);
    const ImU32 fillCol = IM_COL32(255, 210, 90, 160);

    auto toScreen = [&](float p_Fx, float p_Fy) {
        return ImVec2(p_Fit.Min.x + p_Fx * p_Fit.GetWidth(), p_Fit.Min.y + p_Fy * p_Fit.GetHeight());
    };

    if (p_Mask.shape == MaskShape::Gradient)
    {
        const ImVec2 p0 = toScreen(p_Mask.x0, p_Mask.y0);
        const ImVec2 p1 = toScreen(p_Mask.x1, p_Mask.y1);
        p_Draw->AddLine(p0, p1, lineCol, 2.0f);
        p_Draw->AddCircleFilled(p0, 5.0f, fillCol);
        p_Draw->AddCircleFilled(p1, 5.0f, fillCol);
        p_Draw->AddCircle(p0, 5.0f, lineCol);
        p_Draw->AddCircle(p1, 5.0f, lineCol);
        return;
    }

    const ImVec2 c  = toScreen((p_Mask.x0 + p_Mask.x1) * 0.5f, (p_Mask.y0 + p_Mask.y1) * 0.5f);
    const ImVec2 p0 = toScreen(p_Mask.x0, p_Mask.y0);
    const ImVec2 p1 = toScreen(p_Mask.x1, p_Mask.y1);
    const float halfW = std::fabs(p1.x - p0.x) * 0.5f;
    const float halfH = std::fabs(p1.y - p0.y) * 0.5f;
    const float cosA = std::cos(p_Mask.angle), sinA = std::sin(p_Mask.angle);
    auto rot = [&](float p_Sx, float p_Sy) {
        return ImVec2(c.x + p_Sx * cosA - p_Sy * sinA, c.y + p_Sx * sinA + p_Sy * cosA);
    };

    if (p_Mask.shape == MaskShape::Rectangle)
    {
        ImVec2 pts[4] = { rot(-halfW, -halfH), rot(halfW, -halfH), rot(halfW, halfH), rot(-halfW, halfH) };
        p_Draw->AddPolyline(pts, 4, lineCol, ImDrawFlags_Closed, 1.5f);
    }
    else
    {
        constexpr int kSegments = 64;
        ImVec2 pts[kSegments];
        for (int i = 0; i < kSegments; ++i)
        {
            const float t = float(i) / float(kSegments) * 2.0f * 3.14159265358979323846f;
            pts[i] = rot(halfW * std::cos(t), halfH * std::sin(t));
        }
        p_Draw->AddPolyline(pts, kSegments, lineCol, ImDrawFlags_Closed, 1.5f);
    }

    MaskHandle handles[5]; ImVec2 pos[5];
    const int n = SpatialMaskHandlePoints(p_Mask, p_Fit, handles, pos);
    for (int i = 0; i < n; ++i)
    {
        if (handles[i] == MaskHandle::Rotate)
        {
            p_Draw->AddLine(rot(0.0f, -halfH), pos[i], lineCol, 1.5f);
            p_Draw->AddCircle(pos[i], 6.0f, lineCol, 16, 1.5f);
        }
        else
        {
            p_Draw->AddRectFilled(ImVec2(pos[i].x - 5, pos[i].y - 5), ImVec2(pos[i].x + 5, pos[i].y + 5), fillCol);
            p_Draw->AddRect(ImVec2(pos[i].x - 5, pos[i].y - 5), ImVec2(pos[i].x + 5, pos[i].y + 5), lineCol);
        }
    }
}

constexpr float kPhi    = 1.6180339887f;
constexpr float kInvPhi = 1.0f / kPhi;   // ~0.618

// The golden spiral, drawn as the nested rectangles it is built from plus a
// quarter arc through each one.
//
// Built in unit space and mirrored into place rather than constructed four
// ways: the four corner options are the same spiral flipped in x, in y, or
// in both, so there is one construction to get right instead of four. (A
// mirrored golden spiral is the usual thing these overlays offer - it winds
// the other way round, which is the point of being able to pick a corner.)
//
// Each step cuts the current rectangle at 1/phi along an axis, alternating
// right/down/left/up, draws a quarter arc across the cut-off cell, and
// recurses into what is left. The cut is 1/phi of the *current* rectangle
// rather than a true square, so the construction survives a frame that is
// not itself a golden rectangle - 16:9 included - by stretching with it.
// Two steps scale both axes by 1/phi, so it stays self-similar either way.
// The arcs are elliptical for the same reason: on a stretched frame a
// circular one would not meet the next cell's corner.
static void DrawFibonacciSpiral(ImDrawList* p_Draw, const ImRect& p_Fit,
                                SpiralCorner p_Corner, ImU32 p_LineCol, ImU32 p_ArcCol)
{
    const bool flipX = (p_Corner == SpiralCorner::BottomLeft || p_Corner == SpiralCorner::TopLeft);
    const bool flipY = (p_Corner == SpiralCorner::TopRight   || p_Corner == SpiralCorner::TopLeft);

    auto toScreen = [&](float u, float v) {
        return ImVec2(p_Fit.Min.x + (flipX ? 1.0f - u : u) * p_Fit.GetWidth(),
                      p_Fit.Min.y + (flipY ? 1.0f - v : v) * p_Fit.GetHeight());
    };

    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;

    // Ten steps puts the last cell well under a pixel on any real panel;
    // beyond that the arcs just pile up on the same texel.
    constexpr int kSteps       = 10;
    constexpr int kArcSegments = 24;

    for (int step = 0; step < kSteps; ++step)
    {
        const float cw = w * kInvPhi;
        const float ch = h * kInvPhi;

        float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;
        ImVec2 cutA, cutB;
        int phase = step % 4;

        switch (phase)
        {
            case 0:   // cell on the left, remainder to the right
                cx = x + cw; cy = y + h; rx = cw; ry = h;
                cutA = toScreen(x + cw, y); cutB = toScreen(x + cw, y + h);
                break;
            case 1:   // cell on top, remainder below
                cx = x; cy = y + ch; rx = w; ry = ch;
                cutA = toScreen(x, y + ch); cutB = toScreen(x + w, y + ch);
                break;
            case 2:   // cell on the right, remainder to the left
                cx = x + w - cw; cy = y; rx = cw; ry = h;
                cutA = toScreen(x + w - cw, y); cutB = toScreen(x + w - cw, y + h);
                break;
            default:  // cell on the bottom, remainder above
                cx = x + w; cy = y + h - ch; rx = w; ry = ch;
                cutA = toScreen(x, y + h - ch); cutB = toScreen(x + w, y + h - ch);
                break;
        }

        p_Draw->AddLine(cutA, cutB, p_LineCol, 1.0f);

        for (int s = 0; s <= kArcSegments; ++s)
        {
            const float t = (float(s) / float(kArcSegments)) * 1.5707963f;   // 0 -> pi/2
            const float ct = std::cos(t), stt = std::sin(t);

            float u = 0.0f, v = 0.0f;
            switch (phase)
            {
                case 0:  u = cx - rx * ct;  v = cy - ry * stt; break;
                case 1:  u = cx + rx * stt; v = cy - ry * ct;  break;
                case 2:  u = cx + rx * ct;  v = cy + ry * stt; break;
                default: u = cx - rx * stt; v = cy + ry * ct;  break;
            }
            p_Draw->PathLineTo(toScreen(u, v));
        }
        p_Draw->PathStroke(p_ArcCol, ImDrawFlags_None, 1.5f);

        switch (phase)
        {
            case 0:  x += cw; w -= cw; break;
            case 1:  y += ch; h -= ch; break;
            case 2:  w -= cw;          break;
            default: h -= ch;          break;
        }
    }
}

// Framing guides over the fitted image (not the whole panel, so they track
// the picture rather than the letterbox around it). Ported from the old
// app's _draw_composition_grid, Fibonacci aside.
static void DrawCompositionGrid(ImDrawList* p_Draw, const ImRect& p_Fit, const SourceState& p_St)
{
    if (p_St.grid == CompositionGrid::None) return;
    if (p_Fit.GetWidth() <= 0.0f || p_Fit.GetHeight() <= 0.0f) return;

    const ImU32 kGridCol = IM_COL32(255, 255, 255, 160);
    const float w = p_Fit.GetWidth(), h = p_Fit.GetHeight();

    auto divisions = [&](std::initializer_list<float> p_Fracs) {
        for (float t : p_Fracs)
        {
            p_Draw->AddLine(ImVec2(p_Fit.Min.x + t * w, p_Fit.Min.y),
                            ImVec2(p_Fit.Min.x + t * w, p_Fit.Max.y), kGridCol, 1.0f);
            p_Draw->AddLine(ImVec2(p_Fit.Min.x, p_Fit.Min.y + t * h),
                            ImVec2(p_Fit.Max.x, p_Fit.Min.y + t * h), kGridCol, 1.0f);
        }
    };

    switch (p_St.grid)
    {
        case CompositionGrid::Thirds:
            divisions({ 1.0f / 3.0f, 2.0f / 3.0f });
            break;

        case CompositionGrid::GoldenRatio:
            // The same construction as Rule of Thirds with the 1/3 split
            // replaced by the golden one (~0.382 / 0.618).
            divisions({ 1.0f - kInvPhi, kInvPhi });
            break;

        case CompositionGrid::CenterCross:
            divisions({ 0.5f });
            break;

        case CompositionGrid::Crop185:
        case CompositionGrid::Crop239:
        {
            const float target = (p_St.grid == CompositionGrid::Crop185) ? 1.85f : 2.39f;
            ImRect crop = p_Fit;
            if (w / h > target)
            {
                const float cropW = h * target;                  // wider than the crop - bars at the sides
                crop.Min.x = p_Fit.Min.x + (w - cropW) * 0.5f;
                crop.Max.x = crop.Min.x + cropW;
            }
            else
            {
                const float cropH = w / target;                  // taller - bars top and bottom
                crop.Min.y = p_Fit.Min.y + (h - cropH) * 0.5f;
                crop.Max.y = crop.Min.y + cropH;
            }

            // The excluded strips are shaded, not merely outlined, so the
            // crop reads at a glance instead of having to be inferred from
            // one thin rectangle over a busy picture.
            const ImU32 shade = IM_COL32(0, 0, 0, 140);
            if (crop.Min.x > p_Fit.Min.x)
            {
                p_Draw->AddRectFilled(p_Fit.Min, ImVec2(crop.Min.x, p_Fit.Max.y), shade);
                p_Draw->AddRectFilled(ImVec2(crop.Max.x, p_Fit.Min.y), p_Fit.Max, shade);
            }
            if (crop.Min.y > p_Fit.Min.y)
            {
                p_Draw->AddRectFilled(p_Fit.Min, ImVec2(p_Fit.Max.x, crop.Min.y), shade);
                p_Draw->AddRectFilled(ImVec2(p_Fit.Min.x, crop.Max.y), p_Fit.Max, shade);
            }
            p_Draw->AddRect(crop.Min, crop.Max, IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
            break;
        }

        case CompositionGrid::Fibonacci:
            DrawFibonacciSpiral(p_Draw, p_Fit, p_St.spiralCorner,
                                IM_COL32(255, 255, 255, 110), kGridCol);
            break;

        default:
            break;
    }
}

// One entry per SubtitleStyle, loaded once at startup (see main(), near
// ApplyDpiScale) from system fonts with real CJK coverage - DaVinci Resolve
// subtitle text is not guaranteed to be Latin script, and ImGui's own
// built-in default font (ProggyClean, a fixed ASCII-only bitmap) cannot
// render anything else.
//
// An entry is null when that face could not be loaded, which SubtitleFont()
// resolves rather than every call site.
static ImFont* g_SubtitleFonts[(int)SubtitleStyle::Count] = {};

// The face for p_Style, falling back to Regular and then to the ambient UI
// font. A machine whose font family has no true italic (several of the
// fallbacks below are regular/bold only) therefore still renders readable
// subtitles, just without the slant - which beats refusing to draw.
static ImFont* SubtitleFont(SubtitleStyle p_Style)
{
    if (ImFont* exact = g_SubtitleFonts[(int)p_Style])
        return exact;
    if (ImFont* regular = g_SubtitleFonts[(int)SubtitleStyle::Regular])
        return regular;
    return ImGui::GetFont();
}

// Horizontally centered burned-in subtitle text from DaVinci Resolve's
// current timeline (see SubtitleBridge), styled by the Subtitles section of
// Preferences. p_Fit is the Source panel's actual fitted image rect (not the
// full panel rect), so the overlay tracks the video itself rather than empty
// letterbox space around it - and every size below is a fraction of that
// rect, so the result holds its proportions at any panel size.
//
// Drawn with g_SubtitleFont (see main()'s startup, near ApplyDpiScale)
// rather than the ambient ImGui font: DaVinci Resolve subtitle text is not
// guaranteed to be Latin script, and ImGui's own built-in default font has
// no non-Latin glyphs at all - every CJK character rendered as "?" before
// this. Using ImFont::CalcTextSizeA/ImDrawList::AddText's font-argument
// overloads instead of the ambient ImGui::CalcTextSize()/AddText() keeps
// this swap local to just the overlay, rather than pushing/popping the
// global font stack (or changing the metrics every other panel's layout was
// tuned against).
static void DrawSubtitleOverlay(ImDrawList* p_Draw, const ImRect& p_Fit,
                                const std::string& p_Text, const Preferences& p_Prefs)
{
    if (p_Text.empty())
        return;

    std::vector<std::string> lines;
    size_t start = 0;
    for (;;)
    {
        const size_t nl = p_Text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(p_Text.substr(start));
            break;
        }
        lines.push_back(p_Text.substr(start, nl - start));
        start = nl + 1;
    }

    ImFont* font = SubtitleFont(p_Prefs.subtitleStyle);
    const float noMaxWidth = 1.0e9f;   // CalcTextSizeA's own early-out clip width, not a wrap limit

    // Floored so the text stays legible rather than collapsing to a smear in
    // a panel dragged down to a sliver.
    const float fontSize   = std::max(8.0f, p_Fit.GetHeight() * (p_Prefs.subtitleSizePct * 0.01f));
    const float lineHeight = fontSize * 1.25f;
    const float paddingX   = fontSize * 0.6f;
    const float paddingY   = fontSize * 0.3f;
    const float margin     = p_Fit.GetHeight() * (p_Prefs.subtitleMarginPct * 0.01f);

    float maxWidth = 0.0f;
    for (const std::string& line : lines)
        maxWidth = std::max(maxWidth, font->CalcTextSizeA(fontSize, noMaxWidth, 0.0f, line.c_str()).x);

    const float blockWidth  = std::max(0.0f, std::min(maxWidth + paddingX * 2.0f, p_Fit.GetWidth()));
    const float blockHeight = lineHeight * float(lines.size()) + paddingY * 2.0f;

    const float blockTop = (p_Prefs.subtitlePosition == SubtitlePosition::Top)
                             ? p_Fit.Min.y + margin
                             : p_Fit.Max.y - blockHeight - margin;

    const ImVec2 boxMin(p_Fit.GetCenter().x - blockWidth * 0.5f, blockTop);
    const ImVec2 boxMax(boxMin.x + blockWidth, boxMin.y + blockHeight);

    if (p_Prefs.subtitleBackdrop == SubtitleBackdrop::Box)
    {
        const ImU32 boxCol = ImGui::GetColorU32(ImVec4(p_Prefs.subtitleBoxColor[0],
                                                       p_Prefs.subtitleBoxColor[1],
                                                       p_Prefs.subtitleBoxColor[2],
                                                       p_Prefs.subtitleBoxOpacity * 0.01f));
        p_Draw->AddRectFilled(boxMin, boxMax, boxCol);
    }

    const ImU32 textCol = ImGui::GetColorU32(ImVec4(p_Prefs.subtitleTextColor[0],
                                                    p_Prefs.subtitleTextColor[1],
                                                    p_Prefs.subtitleTextColor[2], 1.0f));
    // Scaled off the text size rather than fixed pixels, so the shadow keeps
    // the same visual weight as the subtitle is sized up or down.
    const float shadowOffset = fontSize * (p_Prefs.subtitleShadowSize * 0.01f);

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const ImVec2 lineSize = font->CalcTextSizeA(fontSize, noMaxWidth, 0.0f, lines[i].c_str());
        const ImVec2 pos(boxMin.x + (blockWidth - lineSize.x) * 0.5f,
                         boxMin.y + paddingY + lineHeight * float(i));

        if (p_Prefs.subtitleShadow)
        {
            p_Draw->AddText(font, fontSize, ImVec2(pos.x + shadowOffset, pos.y + shadowOffset),
                            IM_COL32(0, 0, 0, 200), lines[i].c_str());
        }
        p_Draw->AddText(font, fontSize, pos, textCol, lines[i].c_str());
    }
}

void DrawSourcePanel(App& p_App, Panel& p_Panel)
{
    SourceState& st     = p_Panel.source;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    const bool haveSource = p_App.haveFrame && p_App.frame.HasPreview();

    // Sized from the incoming frame directly when one's available this tick,
    // or the last successfully uploaded texture otherwise (the same numbers
    // either way - video resolution does not change frame to frame) - not
    // from the texture alone, so this can run *before* rebuilding it below.
    //
    // HandleInput and Apply must share this same base rect, or the cursor-
    // anchor math in HandleInput solves for a rectangle Apply never draws
    // into - which is exactly wrong whenever the fitted image's aspect
    // differs from the panel's own (a tall, narrow scope in a wide panel,
    // say), and was the bug behind zooming not tracking the mouse.
    //
    // Mask input also has to run against this same fit, *before* the dimmed
    // preview is (re)built below rather than after it (the previous order):
    // the image this frame tints was otherwise always built from whatever
    // the mask was *before* this frame's own drag delta got applied, one
    // frame stale - correct again the instant a drag settles, but visibly
    // behind the overlay outline (drawn fresh from the already-updated mask
    // every frame) while it was still moving, worse the faster the drag.
    const float texW = haveSource ? float(p_App.frame.previewWidth)  : float(st.texture.Width());
    const float texH = haveSource ? float(p_App.frame.previewHeight) : float(st.texture.Height());

    ImRect fit;
    if (texW > 0.0f && texH > 0.0f)
    {
        const ImRect fitBase = FitAspect(avail, texW, texH);
        common.zoom.HandleInput(fitBase, common.zoomable);
        fit = common.zoom.Apply(fitBase);
        UpdateSpatialMaskInput(p_App, fit, p_Panel.id);
    }

    if (haveSource)
    {
        // Rebuilt (or re-uploaded) only when the frame or the tint inputs
        // changed - see DrawFalseColorPanel. The mask and qualifier are read
        // after UpdateSpatialMaskInput above, so a drag still lands in the
        // same frame it happens in.
        const bool dimmed = p_App.spatialMask.hasGeometry || p_App.valueQualifier.AnyActive();
        const bool stale  = st.builtFrameIndex != p_App.frame.frameIndex
                         || st.builtDimmed != dimmed
                         || (dimmed && (!SpatialMaskEquals(st.builtMask, p_App.spatialMask)
                                     || !ValueQualifierEquals(st.builtQualifier, p_App.valueQualifier)));
        if (stale)
        {
            if (dimmed)
            {
                BuildDimmedPreviewImage(p_App.frame, p_App.spatialMask, st.dimmedImage, &p_App.valueQualifier);
                if (!st.dimmedImage.Empty())
                    st.texture.UploadRGBA(st.dimmedImage.pixels.data(), st.dimmedImage.width, st.dimmedImage.height);
            }
            else
            {
                st.texture.UploadRGB(p_App.frame.preview.data(),
                                     int(p_App.frame.previewWidth),
                                     int(p_App.frame.previewHeight));
            }

            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtDimmed     = dimmed;
            st.builtMask       = p_App.spatialMask;
            st.builtQualifier  = p_App.valueQualifier;
        }
    }

    if (!st.texture.Valid())
    {
        // No preview has ever arrived. Almost always "Publish Video" left off on
        // the tap rather than anything broken, so say that instead of drawing an
        // empty box the user has to guess about.
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f,
                             avail.GetCenter().y - size.y * 0.5f),
                      kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
            p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);

    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);

    DrawSpatialMaskOverlay(draw, p_App.spatialMask, fit);
    DrawCompositionGrid(draw, fit, st);

    if (p_App.prefs.showSubtitles && p_App.input == InputSource::Resolve)
        DrawSubtitleOverlay(draw, fit, SubtitleBridgeGetText(p_App.frame.timelineTime), p_App.prefs);

    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

// ---------------------------------------------------------------------------
// Settings windows for the remaining panels
// ---------------------------------------------------------------------------
//
// Waveform is the one ported in full for now. These three carry the controls they
// already had plus the shared rows, so every panel answers its right-click menu
// the same way rather than one behaving differently from the rest.

void DrawHistogramSettings(Panel& p_Panel)
{
    HistogramState& p_St  = p_Panel.histogram;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    PositionSettingsWindow(common);

    if (!ImGui::Begin("Histogram Settings", &common.showSettings,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // Measured for the next open, so the placement above can keep the window
    // inside a panel too small to hold it at the default anchor.
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Channels");
    const char* channelModes[] = { "Luma", "Stacked", "YRGB" };
    int channelIndex = int(p_St.channels);
    if (ImGui::Combo("##channels", &channelIndex, channelModes, IM_ARRAYSIZE(channelModes)))
        p_St.channels = HistogramChannels(channelIndex);

    // Only YRGB has channels to pick between - Luma is one channel by definition
    // and Stacked is the three colour channels over each other.
    if (p_St.channels == HistogramChannels::YRGB)
    {
        SettingsRowLabel("  Show");
        ImGui::Checkbox("Y", &p_St.showY); ImGui::SameLine();
        ImGui::Checkbox("R", &p_St.showR); ImGui::SameLine();
        ImGui::Checkbox("G", &p_St.showG); ImGui::SameLine();
        ImGui::Checkbox("B", &p_St.showB);
    }

    SettingsRowLabel("Log Scale");
    ImGui::Checkbox("##log", &p_St.logScale);

    SettingsRowLabel("Cursor readout");
    ImGui::Checkbox("##cursor", &p_St.showCursor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hovering reports the exact value under the pointer -\n"
                          "IRE, the working-space float, and the 10-bit code.");

    bool anyQualifier = false;
    for (bool a : p_St.qualifyActive) anyQualifier |= a;
    if (anyQualifier)
    {
        ImGui::Separator();
        if (ImGui::Button("Clear Qualifiers"))
        {
            for (bool& a : p_St.qualifyActive) a = false;
        }
    }

    DrawSharedSettings(common);

    ImGui::End();
}

void DrawVectorscopeSettings(Panel& p_Panel)
{
    VectorscopeState& p_St  = p_Panel.vectorscope;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    PositionSettingsWindow(common);

    if (!ImGui::Begin("Vectorscope Settings", &common.showSettings,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // Range first: which luma band(s) the trace reflects is the most consequential
    // choice on this panel.
    SettingsRowLabel("Range");
    const char* ranges[] = { "All", "Low", "Mid", "High", "3-Stack" };
    int rangeIndex = int(p_St.range);
    if (ImGui::Combo("##range", &rangeIndex, ranges, IM_ARRAYSIZE(ranges)))
        p_St.range = VectorRange(rangeIndex);

    SettingsSliderFloat("Gain", &p_St.gain, 1.0f, 80.0f, 6.0f);

    SettingsRowLabel("2x Zoom");
    ImGui::Checkbox("##zoom2x", &p_St.zoom2x);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Halves the Cb/Cr span across the plot.\n"
                          "The target boxes stay at their true positions.");

    SettingsRowLabel("Colorize");
    ImGui::Checkbox("##colorize", &p_St.colorize);

    ImGui::SeparatorText("Reference");

    SettingsRowLabel("Targets");
    ImGui::Checkbox("##targets", &p_St.showTargets);

    if (p_St.showTargets)
    {
        SettingsSliderFloat("Target size", &p_St.targetSize, 3.0f, 16.0f, 9.0f);

        // Deliberately independent of the Graticule slider: that one dims the
        // rings and crosshair, this one only the target boxes, and either can go
        // to zero without hiding the other.
        float targetPercent = p_St.targetOpacity * 100.0f;
        if (SettingsSliderFloat("Target opacity", &targetPercent, 0.0f, 100.0f, 100.0f, "%.0f%%"))
            p_St.targetOpacity = targetPercent / 100.0f;
    }

    SettingsRowLabel("Skin tone line");
    ImGui::Checkbox("##skinline", &p_St.showSkinLine);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A flesh-tone reference ray at %.0f degrees.", kSkinToneAngleDeg);

    ImGui::SeparatorText("Enhanced Render");

    SettingsRowLabel("Enhanced Render");
    ImGui::Checkbox("##enhanced", &p_St.enhancedRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the beam from real scanlines instead of a density cloud.");

    if (p_St.enhancedRender)
    {
        SettingsRowLabel("Smooth Trace");
        if (ImGui::Checkbox("##smooth", &p_St.smoothTrace) && !p_St.smoothTrace)
            p_St.traceState.Reset();

        SettingsSliderFloat("Low-Pass Filter", &p_St.lowPassKernel, 0.0f, 20.0f, 0.0f);

        SettingsRowLabel("Signal Pre-filter");
        ImGui::Checkbox("##prefilter", &p_St.signalPrefilter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A generic bandwidth-limiting pass on the raw signal.\n"
                              "Not a verified EBU R103 implementation.");
    }

    // No bounding lines: they mark a level, and this scope has no level axis.
    DrawSharedSettings(common, /*p_WithBoundingLines=*/false);

    ImGui::End();
}




// ---------------------------------------------------------------------------
// White Balance ("Twin Peaks")
// ---------------------------------------------------------------------------

void DrawWhiteBalanceDiamond(ImDrawList* p_Draw, const ImRect& p_Fit, const ScopeFrame& p_Frame,
                             bool p_Upper, float p_Brightness)
{
    auto toScreen = [&](float p_Fx, float p_Fy) {
        return ImVec2(p_Fit.Min.x + p_Fx * p_Fit.GetWidth(), p_Fit.Min.y + p_Fy * p_Fit.GetHeight());
    };

    const DiamondGraticule g = WhiteBalanceGraticule(p_Frame, p_Upper);
    const ImVec2 top    = toScreen(g.topX, g.topY);
    const ImVec2 bottom = toScreen(g.bottomX, g.bottomY);
    const ImVec2 left   = toScreen(g.leftX, g.leftY);
    const ImVec2 right  = toScreen(g.rightX, g.rightY);

    ImVec2 pts[4] = { top, right, bottom, left };
    p_Draw->AddPolyline(pts, 4, GraticuleColor(kColGraticule, p_Brightness), ImDrawFlags_Closed, 1.0f);

    // diff=0 (the centre line, matching channels at every level) is the one
    // line this whole scope exists to check a trace against.
    p_Draw->AddLine(top, bottom, GraticuleColor(kColGratMajor, p_Brightness), 1.0f);
}

// Enhanced Render: the same beam-trace treatment as the Vectorscope's own
// (DrawVectorscopeEnhanced), mapped to (diff, level) in each diamond instead
// of (Cb, Cr) - exact for the same reason the density path's own per-pixel
// derivation is: level = (G+other)/2 and diff = (G-other)/2 are linear in
// R/G/B, so the trace's own column-bucket averaging commutes with them
// exactly. GB (upper) and GR (lower) are two independent traces of the same
// per-scanline R/G/B samples, each broken into runs on its own jump test -
// a jump in one diamond's (diff, level) does not imply one in the other's.
void DrawWhiteBalanceEnhanced(ImDrawList* p_Draw, const ImRect& p_Fit,
                              const WhiteBalanceState& p_St, const ScopeFrame& p_Frame,
                              EnhancedRunCache& p_Cache)
{
    EnhancedTimer profileThisPass;

    // The walk below runs only when something it reads has changed - see
    // EnhancedRunCache. The bin range is the frame's, keyed here explicitly.
    if (!p_Cache.Matches(p_St.traceCache.generation, p_Fit, p_Frame.binRangeLow, p_Frame.binRangeHigh,
                         p_St.gain, p_St.colorize, 0))
    {
        p_Cache.Begin(p_St.traceCache.generation, p_Fit, p_Frame.binRangeLow, p_Frame.binRangeHigh,
                      p_St.gain, p_St.colorize, 0);

        // Same "very low, order of magnitude below the density trace's own
        // gain feel" reasoning as Waveform's/Vectorscope's alpha, scaled against
        // this panel's own default Gain (1.0, not those two scopes' 8/6) rather
        // than borrowing their constant.
        const float   alpha  = std::clamp(p_St.gain * 0.028f, 0.004f, 1.0f);
        const uint8_t alpha8 = uint8_t(alpha * 255.0f);
        const ImU32   plainWhite = IM_COL32(235, 240, 245, alpha8);

        // Measured for this scope's own (level, diff) units, per the old app -
        // same shape of distribution as the Vectorscope's chroma break, a
        // different absolute span (1.16 here vs. the Vectorscope's 1.5), so its
        // own constant rather than one derived from the Vectorscope's.
        constexpr float kBreakDistance = 0.25f;

        std::vector<ImVec2> run;
        run.reserve(kWaveformColumns);
        std::vector<ImU32> runColours;
        if (p_St.colorize) runColours.reserve(kWaveformColumns);

        auto flush = [&]()
        {
            if (p_St.colorize)
                p_Cache.AddRun(run.data(), runColours.data(), int(run.size()));
            else
                p_Cache.AddRun(run.data(), int(run.size()), plainWhite);
            run.clear();
            runColours.clear();
        };

        for (int diamond = 0; diamond < 2; ++diamond)
        {
            const bool  upper = (diamond == 0);   // 0 = Green/Blue, 1 = Green/Red
            const float vMin  = upper ? 0.0f : 0.5f;
            const float vMax  = upper ? 0.5f : 1.0f;

            for (uint32_t row = 0; row < kWaveformTraceRows; ++row)
            {
                run.clear();
                runColours.clear();

                float prevLevel = 0.0f, prevDiff = 0.0f;
                bool  havePrev = false;

                for (uint32_t col = 0; col < kWaveformColumns; ++col)
                {
                    const float r = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneR)];
                    const float g = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneG)];
                    const float b = p_St.preparedTrace[WaveformTraceIndex(row, col, kPlaneB)];

                    // NaN (see BuildMaskedWaveformTrace) means a masked column
                    // with nothing measured in it - breaks the run exactly like
                    // an out-of-range sample does below, rather than feeding a
                    // NaN through TwinPeaksToPlot where the range check's
                    // comparisons would silently be false and let it through.
                    if (std::isnan(r))
                    {
                        flush();
                        havePrev = false;
                        continue;
                    }

                    const float other = upper ? b : r;
                    const float level = (g + other) * 0.5f;
                    const float diff  = (g - other) * 0.5f;

                    if (havePrev)
                    {
                        const float dx = diff - prevDiff;
                        const float dy = level - prevLevel;
                        if (std::sqrt(dx * dx + dy * dy) > kBreakDistance)
                            flush();
                    }

                    float u = 0.0f, v = 0.0f;
                    TwinPeaksToPlot(level, diff, p_Frame.binRangeLow, p_Frame.binRangeHigh, upper, u, v);

                    // Out of this diamond's own displayable extent breaks the run
                    // rather than clamping it - clamping would pin the point to
                    // the rim and draw an arc that is not in the data, the same
                    // reasoning the Vectorscope's own Enhanced Render uses. Bounded
                    // to this diamond's own half (not 0..1 generally) so an
                    // out-of-range sample cannot bleed into the other diamond's
                    // territory instead of just disappearing.
                    if (u < 0.0f || u > 1.0f || v < vMin || v > vMax)
                    {
                        flush();
                        havePrev = false;
                        continue;
                    }

                    run.push_back(ImVec2(p_Fit.Min.x + u * p_Fit.GetWidth(),
                                         p_Fit.Min.y + v * p_Fit.GetHeight()));

                    if (p_St.colorize)
                    {
                        runColours.push_back(BeamColour(r, g, b, alpha8));
                    }

                    prevLevel = level;
                    prevDiff  = diff;
                    havePrev  = true;
                }

                flush();
            }
        }
    }

    EmitEnhancedRuns(p_Draw, p_Cache);
}

void DrawWhiteBalancePanel(App& p_App, Panel& p_Panel)
{
    WhiteBalanceState& st     = p_Panel.whiteBalance;
    PanelCommon&       common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    // HandleInput and Apply must share the same base rect, or the cursor-anchor
    // math in HandleInput solves for a rectangle Apply never draws into - wrong
    // whenever the fitted image's aspect differs from the panel's own, which for
    // this scope's tall, narrow combined image (kTwinPeaksSize x *2) is the
    // common case, not the exception. This one is a wire-format constant, not
    // texture-dependent, so it can be computed up front.
    const ImRect fitBase = FitAspect(avail, float(kTwinPeaksSize), float(kTwinPeaksSize) * 2.0f);
    common.zoom.HandleInput(fitBase, common.zoomable);

    const bool haveSignal = p_App.haveFrame && !p_App.frame.twinPeaks.empty();

    if (!haveSignal)
    {
        const char* msg = "No signal yet.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    // A spatial mask restricts what this scope reads, same as it already
    // restricts what the preview-tier panels show - see EnsureMaskedScopeBins.
    const uint32_t* twinPeaksCounts = p_App.maskedScopeValid ? p_App.maskedScope.twinPeaks.data() : nullptr;
    const std::vector<float>* maskedTrace = p_App.maskedScopeValid ? &p_App.maskedScope.waveformTrace : nullptr;

    // Enhanced Render replaces the density image rather than overlaying it,
    // same as the Waveform's and Vectorscope's own.
    const bool enhancedReady =
        st.enhancedRender && !p_App.prefs.disableEnhancedRender &&
        PrepareEnhancedTraceCached(p_App.frame, st.signalPrefilter, st.smoothTrace,
                                   st.lowPassKernel, st.traceState, st.preparedTrace,
                                   maskedTrace, p_App.spatialMask, p_App.valueQualifier,
                                   st.traceCache);

    draw->PushClipRect(avail.Min, avail.Max, true);

    const ImRect fit = common.zoom.Apply(fitBase);

    if (enhancedReady)
    {
        DrawWhiteBalanceEnhanced(draw, fit, st, p_App.frame, st.enhancedRuns);
    }
    else
    {
        // Rebuilt only when the frame or an input changed - see DrawFalseColorPanel.
        const bool masked = p_App.maskedScopeValid;
        const bool stale  = st.builtFrameIndex != p_App.frame.frameIndex
                         || st.builtGain   != st.gain
                         || st.builtMasked != masked
                         || !SpatialMaskEquals(st.builtMask, p_App.spatialMask)
                         || !ValueQualifierEquals(st.builtQualifier, p_App.valueQualifier);
        if (stale)
        {
            BuildWhiteBalanceImage(p_App.frame, st.gain, st.image, twinPeaksCounts);
            if (!st.image.Empty())
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);

            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtGain       = st.gain;
            st.builtMasked     = masked;
            st.builtMask       = p_App.spatialMask;
            st.builtQualifier  = p_App.valueQualifier;
        }

        if (st.texture.Valid())
            draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    }

    draw->AddRect(fit.Min, fit.Max, kColBorder);

    DrawWhiteBalanceDiamond(draw, fit, p_App.frame, true,  common.graticuleBrightness);
    DrawWhiteBalanceDiamond(draw, fit, p_App.frame, false, common.graticuleBrightness);

    if (p_App.maskedScopeValid && p_App.maskedScope.empty)
        DrawMaskEmptyNotice(draw, fit);

    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawWhiteBalanceSettings(Panel& p_Panel)
{
    WhiteBalanceState& st     = p_Panel.whiteBalance;
    PanelCommon&        common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "White Balance Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##wbgain", &st.gain, 0.1f, 8.0f, 1.0f, "%.2fx");

    ImGui::SeparatorText("Enhanced Render");

    SettingsRowLabel("Enhanced Render");
    ImGui::Checkbox("##enhanced", &st.enhancedRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the beam from real scanlines instead of a density cloud.");

    if (st.enhancedRender)
    {
        SettingsRowLabel("Smooth Trace");
        if (ImGui::Checkbox("##smooth", &st.smoothTrace) && !st.smoothTrace)
            st.traceState.Reset();

        SettingsSliderFloat("Low-Pass Filter", &st.lowPassKernel, 0.0f, 20.0f, 0.0f);

        SettingsRowLabel("Signal Pre-filter");
        ImGui::Checkbox("##prefilter", &st.signalPrefilter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A generic bandwidth-limiting pass on the raw signal.\n"
                              "Not a verified EBU R103 implementation.");

        SettingsRowLabel("Colorize");
        ImGui::Checkbox("##colorize", &st.colorize);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tints the beam by each sample's own colour instead of white.");
    }

    DrawSharedSettings(common, false, true);
    ImGui::End();
}

// The 0-100% key: a luma ramp with each band painted in at the position it
// actually triggers, so the bar reads as "this color means this exposure"
// rather than as a legend to cross-reference. Positions come straight from
// the same band table the image is built from, so the key cannot drift out
// of step with the thresholds - retuning a band moves its stripe here by
// construction. Ported from FalseColorPanel.draw_overlay in the old app.
void DrawFalseColorScale(ImDrawList* p_Draw, const ImRect& p_Panel, FalseColorMode p_Mode, float p_Brightness)
{
    const float kMargin = 10.0f;
    const float kWidth  = 22.0f;

    ImRect bar;
    bar.Min = ImVec2(p_Panel.Min.x + kMargin, p_Panel.Min.y + kMargin + 14.0f);
    bar.Max = ImVec2(bar.Min.x + kWidth,
                     bar.Min.y + std::max(40.0f, p_Panel.GetHeight() - 2.0f * kMargin - 28.0f));

    // 0.0 at the bottom of the bar, 1.0 at the top - the same way up as the waveform.
    auto yOf = [&](float p_Fraction) {
        return bar.Max.y - std::clamp(p_Fraction, 0.0f, 1.0f) * bar.GetHeight();
    };

    p_Draw->AddRectFilled(ImVec2(bar.Min.x - 2, bar.Min.y - 2), ImVec2(bar.Max.x + 2, bar.Max.y + 2),
                          IM_COL32(0, 0, 0, 170));

    // Grey ramp first; the bands then overwrite their own slices of it, so the
    // gaps between bands still read as the exposures they pass through.
    p_Draw->AddRectFilledMultiColor(bar.Min, bar.Max,
                                    IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255),
                                    IM_COL32(0, 0, 0, 255),       IM_COL32(0, 0, 0, 255));

    int bandCount = 0;
    const FalseColorBand* bands = FalseColorBandsForMode(p_Mode, bandCount);
    for (int i = 0; i < bandCount; ++i)
    {
        const float top = yOf(bands[i].hi);
        const float bottom = std::max(top + 1.0f, yOf(bands[i].lo));
        p_Draw->AddRectFilled(ImVec2(bar.Min.x, top), ImVec2(bar.Max.x, bottom),
                              IM_COL32(bands[i].color.r, bands[i].color.g, bands[i].color.b, 255));
    }

    p_Draw->AddRect(bar.Min, bar.Max, GraticuleColor(kColGraticule, p_Brightness));

    // Percentage ticks down the right-hand edge. 0 and 100 are left to the
    // bar's own ends - labelling them would collide with the clip stripes,
    // which are the two the eye finds without help anyway.
    for (int percent = 10; percent < 100; percent += 10)
    {
        const float y = yOf(percent / 100.0f);
        p_Draw->AddLine(ImVec2(bar.Max.x + 1, y), ImVec2(bar.Max.x + 4, y),
                        GraticuleColor(kColGraticule, p_Brightness), 1.0f);

        char label[8];
        std::snprintf(label, sizeof(label), "%d", percent);
        p_Draw->AddText(ImVec2(bar.Max.x + 7, y - ImGui::GetFontSize() * 0.5f),
                        GraticuleColor(kColMuted, p_Brightness), label);
    }
}

void DrawFalseColorPanel(App& p_App, Panel& p_Panel)
{
    FalseColorState& st = p_Panel.falseColor;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        const bool stale = st.builtFrameIndex != p_App.frame.frameIndex
                        || st.builtMode != st.mode
                        || !SpatialMaskEquals(st.builtMask, p_App.spatialMask)
                        || !ValueQualifierEquals(st.builtQualifier, p_App.valueQualifier);
        if (stale)
        {
            int bandCount = 0;
            const FalseColorBand* bands = FalseColorBandsForMode(st.mode, bandCount);
            BuildFalseColorImage(p_App.frame, bands, bandCount, st.image, &p_App.spatialMask, &p_App.valueQualifier);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtMode       = st.mode;
            st.builtMask       = p_App.spatialMask;
            st.builtQualifier  = p_App.valueQualifier;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (st.showScale)
        DrawFalseColorScale(draw, avail, st.mode, common.graticuleBrightness);

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawSkinTonePanel(App& p_App, Panel& p_Panel)
{
    SkinToneState& st = p_Panel.skinTone;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        SkinToneParams params;
        params.model         = st.model;
        params.colorMode     = st.colorMode;
        params.tolerance     = st.tolerance;
        params.limitsEnabled = st.limitsEnabled;
        params.hueMin        = st.hueMin;
        params.hueMax        = st.hueMax;
        params.satMin        = st.satMin;
        params.satMax        = st.satMax;
        params.lumaMin       = st.lumaMin;
        params.lumaMax       = st.lumaMax;
        params.greyNonSkin   = st.greyNonSkin;
        params.solidColor      = Rgb8FromFloat3(st.solidColor);
        params.gradientColorA  = Rgb8FromFloat3(st.gradientColorA);
        params.gradientColorB  = Rgb8FromFloat3(st.gradientColorB);

        const bool stale = st.builtFrameIndex != p_App.frame.frameIndex
                        || !SkinToneParamsEquals(st.builtParams, params)
                        || !SpatialMaskEquals(st.builtMask, p_App.spatialMask)
                        || !ValueQualifierEquals(st.builtQualifier, p_App.valueQualifier);
        if (stale)
        {
            BuildSkinToneImage(p_App.frame, params, st.image, &p_App.spatialMask, &p_App.valueQualifier);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtParams     = params;
            st.builtMask       = p_App.spatialMask;
            st.builtQualifier  = p_App.valueQualifier;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawNoisePanel(App& p_App, Panel& p_Panel)
{
    NoiseState& st = p_Panel.noise;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        // Rebuilt only when the frame or a setting changed - see DrawFalseColorPanel.
        const bool stale = st.builtFrameIndex != p_App.frame.frameIndex
                        || st.builtMethod  != st.method
                        || st.builtChannel != st.channel
                        || st.builtGain    != st.gain;
        if (stale)
        {
            BuildNoiseImage(p_App.frame, (int)st.method, st.channel, st.gain, st.image);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtMethod     = st.method;
            st.builtChannel    = st.channel;
            st.builtGain       = st.gain;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}





void DrawFalseColorSettings(Panel& p_Panel)
{
    FalseColorState& st = p_Panel.falseColor;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;
    
    char name[64];
    std::snprintf(name, sizeof(name), "False Color Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Mode");
    if (ImGui::BeginCombo("##Mode", FalseColorModeName(st.mode)))
    {
        for (int i = 0; i < (int)FalseColorMode::Count; ++i)
        {
            const FalseColorMode mode = (FalseColorMode)i;
            if (ImGui::Selectable(FalseColorModeName(mode), st.mode == mode))
                st.mode = mode;
        }
        ImGui::EndCombo();
    }

    SettingsRowLabel("Exposure Scale");
    ImGui::Checkbox("##showscale", &st.showScale);

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

void DrawSkinToneSettings(Panel& p_Panel)
{
    SkinToneState& st = p_Panel.skinTone;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;
    
    char name[64];
    std::snprintf(name, sizeof(name), "Skin Tone Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Model");
    const char* modelNames[] = { "YCbCr", "HSL" };
    int modelIndex = (int)st.model;
    if (ImGui::Combo("##Model", &modelIndex, modelNames, IM_ARRAYSIZE(modelNames)))
    {
        st.model = (SkinToneModel)modelIndex;
        // The two models' reference angles are ~100 degrees apart (123 vs 25)
        // - re-centering the Hue Range on the new model's own default keeps a
        // first-time switch to Limits usable, rather than silently carrying
        // over a range that made sense for the old model and none at all for
        // the new one.
        const float center = (st.model == SkinToneModel::Hsl) ? kSkinToneAngleHslDeg : kSkinToneAngleDeg;
        st.hueMin = center - st.tolerance;
        st.hueMax = center + st.tolerance;
    }

    SettingsRowLabel("Color Mode");
    const char* colorModeNames[] = { "Default", "Solid Color", "Gradient" };
    int colorModeIndex = (int)st.colorMode;
    if (ImGui::Combo("##ColorMode", &colorModeIndex, colorModeNames, IM_ARRAYSIZE(colorModeNames)))
        st.colorMode = (SkinToneColorMode)colorModeIndex;

    if (st.colorMode == SkinToneColorMode::Solid)
    {
        SettingsRowLabel("Solid Color");
        ImGui::ColorEdit3("##solidcolor", st.solidColor, ImGuiColorEditFlags_NoInputs);
    }
    else if (st.colorMode == SkinToneColorMode::Gradient)
    {
        SettingsRowLabel("Gradient Color A");
        ImGui::ColorEdit3("##gradcolora", st.gradientColorA, ImGuiColorEditFlags_NoInputs);
        SettingsRowLabel("Gradient Color B");
        ImGui::ColorEdit3("##gradcolorb", st.gradientColorB, ImGuiColorEditFlags_NoInputs);
    }

    SettingsRowLabel("Grey Non-Skin Areas");
    ImGui::Checkbox("##greynonskin", &st.greyNonSkin);

    SettingsRowLabel("Tolerance");
    SettingsSliderFloat("##Tolerance", &st.tolerance, 2.0f, 45.0f, 22.0f, "%.1f deg");

    ImGui::SeparatorText("Saturation and Luminance Limits");

    SettingsRowLabel("Enable Limits");
    ImGui::Checkbox("##limitsenabled", &st.limitsEnabled);

    if (st.limitsEnabled)
    {
        // A plain min/max slider pair rather than the old app's draggable
        // gradient bar widget - same control surface (a wrap-aware hue
        // window), simpler to build and to read.
        SettingsRowLabel("Hue Min");
        SettingsSliderFloat("##huemin", &st.hueMin, 0.0f, 360.0f, kSkinToneAngleDeg - 22.0f, "%.0f deg");
        SettingsRowLabel("Hue Max");
        SettingsSliderFloat("##huemax", &st.hueMax, 0.0f, 360.0f, kSkinToneAngleDeg + 22.0f, "%.0f deg");

        SettingsRowLabel("Saturation Min");
        SettingsSliderFloat("##satmin", &st.satMin, 0.0f, 1.0f, kSkinMinSaturation, "%.2f");
        SettingsRowLabel("Saturation Max");
        SettingsSliderFloat("##satmax", &st.satMax, 0.0f, 1.0f, 1.0f, "%.2f");

        SettingsRowLabel("Luminance Min");
        SettingsSliderFloat("##lumamin", &st.lumaMin, 0.0f, 1.0f, kSkinMinLuma, "%.2f");
        SettingsRowLabel("Luminance Max");
        SettingsSliderFloat("##lumamax", &st.lumaMax, 0.0f, 1.0f, kSkinMaxLuma, "%.2f");
    }

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

void DrawTimecodeSettings(Panel& p_Panel)
{
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;
    
    char name[64];
    std::snprintf(name, sizeof(name), "Timecode Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    DrawSharedSettings(common, false, false);
    ImGui::End();
}

void DrawTimecodePanel(App& p_App, Panel& p_Panel)
{
    PanelCommon& common = p_Panel.common;

    // Every other panel sets panelRect here, which is what the right-click menu
    // and the Settings window placement both key off - skipping it (as this
    // panel used to) left both permanently broken: BeginPanelContextMenu tests
    // the mouse against whatever panelRect last held, which for a panel that
    // never sets it is the struct's zero-initialised default, a rect no click
    // can ever land inside.
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    // Resolve's comes from its scripting API (TimecodeBridge). Premiere's
    // needs no bridge: the frame carries its position in the sequence, and
    // ScopeTransmit hands over the rate, drop-frame and start timecode it is
    // shown against (ScopeControl.h). A screen capture has none.
    std::string text;
    if (p_App.input == InputSource::Resolve)
    {
        text = TimecodeBridgeGetText(p_App.frame.timelineTime);
    }
    else if (p_App.input == InputSource::Premiere)
    {
        const ControlBlock* block = p_App.premiereControl.Block();
        const uint32_t fps = block ? block->tcFps.load(std::memory_order_acquire) : 0;
        if (fps > 0 && p_App.haveFrame)
        {
            const long long frame = std::llround(p_App.frame.timelineTime) +
                                    block->tcStartFrame.load(std::memory_order_relaxed);
            text = FormatTimecode(frame, static_cast<int>(fps),
                                  block->tcDropFrame.load(std::memory_order_relaxed) != 0);
        }
        else
        {
            text = "--:--:--:--";
        }
    }
    else
    {
        text = "No timecode (Screen Capture)";
    }
    const ImVec2 base       = ImGui::GetCursorPos();
    const ImVec2 availSize  = ImGui::GetContentRegionAvail();

    // Scale to fit the panel instead of a fixed multiplier - a fixed scale
    // overflows into neighbouring docked panels the moment this one is
    // resized smaller than the text needs at that scale.
    const ImVec2 baseSize = ImGui::CalcTextSize(text.c_str());
    float scale = 4.0f;
    if (baseSize.x > 0.0f && baseSize.y > 0.0f && availSize.x > 0.0f && availSize.y > 0.0f)
    {
        const float margin = 0.85f; // a little breathing room, not edge-to-edge
        scale = std::min(availSize.x / baseSize.x, availSize.y / baseSize.y) * margin;

        // No upper cap: this panel is nothing but one string of text, so
        // there is no neighbouring content for it to grow into. It used to
        // top out at 4x, which read as the panel refusing to fill a large
        // section rather than as a deliberate limit - past a few dozen x the
        // font atlas's own fixed bake resolution will show as visible
        // blur/blockiness (ImGui stretches the same baked glyph bitmap
        // rather than re-rasterising it at the new size), but that is a
        // quality tradeoff to hit at a large monitor's actual extremes, not
        // a reason to stop scaling well before a normal panel size does.
        scale = std::max(scale, 0.5f);
    }

    ImGui::SetWindowFontScale(scale);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    // SetCursorPos is relative to the window's own corner, not the content
    // region - it does NOT already account for WindowPadding or the docked
    // tab bar the way the cursor position returned by GetCursorPos() does.
    // Centering must add the offset to that base, not use it as an absolute
    // position, or the text renders far enough above the real content area
    // to be entirely clipped in a short panel (found by bisecting a minimal
    // repro down to `SetCursorPos(ImVec2(0,0))` alone making text vanish).
    ImGui::SetCursorPos(ImVec2(base.x + (availSize.x - size.x) * 0.5f, base.y + (availSize.y - size.y) * 0.5f));
    ImGui::TextUnformatted(text.c_str());
    ImGui::SetWindowFontScale(1.0f);

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawQualifierSettings(Panel& p_Panel)
{
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Qualifier Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// Dedicated HSL qualifier tool - a Hue/Saturation/Luminance match range that,
// when enabled, folds into App::valueQualifier.hsl exactly like the
// Histogram's own per-channel qualifiers fold into its channel half - so it
// restricts the Source preview and every masked scope (Waveform, Histogram,
// Vectorscope, White Balance, False Color, Skin Tone) with no per-panel
// change needed there. A plain ImGui-widgets panel, not tied to the pixel
// tap's frame data at all - same reasoning TimecodePanel's own comment gives
// for drawing directly rather than through a texture/ScopePanel path.
//
// State lives on App::valueQualifier.hsl, not this panel - global, like
// spatialMask, so any number of Qualifier panels open at once just all show
// the current shared values with no sync-from-host step needed.
void DrawQualifierPanel(App& p_App, Panel& p_Panel)
{
    PanelCommon& common = p_Panel.common;
    HslQualifier& hsl = p_App.valueQualifier.hsl;

    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    ImGui::Checkbox("Affect Source Preview && Scopes", &hsl.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When on, this range restricts the Source preview and every\n"
                          "scope that already honours a mask (Waveform, Histogram,\n"
                          "Vectorscope, White Balance, False Color, Skin Tone) - the\n"
                          "same way the Histogram's own per-channel qualifiers do.");
    ImGui::SameLine();
    ImGui::Checkbox("Invert", &hsl.invert);

    ImGui::SeparatorText("Hue (H) Range");
    SettingsRowLabel("Min");
    SettingsSliderFloat("##huemin", &hsl.hueMin, 0.0f, 360.0f, 0.0f, "%.0f deg");
    SettingsRowLabel("Max");
    SettingsSliderFloat("##huemax", &hsl.hueMax, 0.0f, 360.0f, 360.0f, "%.0f deg");

    ImGui::SeparatorText("Saturation (S) Range");
    SettingsRowLabel("Min");
    SettingsSliderFloat("##satmin", &hsl.satMin, 0.0f, 1.0f, 0.0f, "%.2f");
    SettingsRowLabel("Max");
    SettingsSliderFloat("##satmax", &hsl.satMax, 0.0f, 1.0f, 1.0f, "%.2f");

    ImGui::SeparatorText("Luminance (L / IRE) Range");
    SettingsRowLabel("Min");
    SettingsSliderFloat("##lumamin", &hsl.lumaMin, 0.0f, 1.0f, 0.0f, "%.2f");
    SettingsRowLabel("Max");
    SettingsSliderFloat("##lumamax", &hsl.lumaMax, 0.0f, 1.0f, 1.0f, "%.2f");

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawAudioMeterSettings(Panel& p_Panel)
{
    AudioMeterState& st = p_Panel.audioMeter;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Audio Meter Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Peak Hold");
    ImGui::Checkbox("##PeakHold", &st.peakHold);

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// Levels are dBFS (0 = full scale). -60dB floor, not the levels' own -100dB
// floor: a meter that only ever moves in its top fifth for ordinary program
// audio is much less readable than one where -60 to 0 spans the full bar,
// and nothing in normal monitoring needs to distinguish -70 from -100 dB.
void AudioStatusMessage(const AudioMeterLevels& p_Levels, char* p_Buf, size_t p_BufSize);   // below

void DrawAudioMeterPanel(App& p_App, Panel& p_Panel)
{
    PanelCommon&     common = p_Panel.common;
    AudioMeterState& st     = p_Panel.audioMeter;

    const ImRect avail = PlotRect();
    ImDrawList*  draw  = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    const AudioMeterLevels levels = AudioMeterBridgeGetLevels();
    const double now = ImGui::GetTime();

    constexpr float kFloorDb  = -60.0f;
    constexpr float kCeilDb   = 0.0f;
    constexpr float kYellowDb = -10.0f;   // matches Resolve's own meter, not a broadcast standard
    constexpr float kRedDb    = -5.0f;

    auto dbToY = [&](float p_Db) -> float
    {
        const float t = std::clamp((p_Db - kFloorDb) / (kCeilDb - kFloorDb), 0.0f, 1.0f);
        return avail.Max.y - t * (avail.Max.y - avail.Min.y);
    };

    // A shared numbered scale down the left edge, the same way Resolve's own
    // meter has one - reserved before laying out the bars so they occupy
    // whatever's left rather than overlapping the labels.
    static constexpr float kLabelDb[] = { 0.0f, -5.0f, -10.0f, -15.0f, -20.0f, -30.0f, -40.0f, -50.0f, -60.0f };
    float labelColumnWidth = 0.0f;
    for (float db : kLabelDb)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%.0f", db);
        labelColumnWidth = std::max(labelColumnWidth, ImGui::CalcTextSize(buf).x);
    }
    const float labelMargin = labelColumnWidth + 6.0f;
    const float barsMinX    = avail.Min.x + labelMargin;

    for (float db : kLabelDb)
    {
        const float y = dbToY(db);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%.0f", db);
        const ImVec2 textSize = ImGui::CalcTextSize(buf);
        draw->AddText(ImVec2(avail.Min.x, y - textSize.y * 0.5f), kColAxisLabel, buf);
        draw->AddLine(ImVec2(barsMinX, y), ImVec2(avail.Max.x, y), IM_COL32(255, 255, 255, 30));
    }

    // Still draws one empty bar with nothing captured yet, rather than a
    // blank panel that looks broken until the first packet arrives.
    const int channels = std::max(levels.channelCount, 1);
    const float gap = 4.0f;
    const float barWidth = (avail.Max.x - barsMinX - gap * (channels + 1)) / float(channels);

    for (int c = 0; c < channels; ++c)
    {
        const bool haveChannel = c < levels.channelCount;
        const float rawDb = haveChannel ? levels.peakDb[c] : kFloorDb;

        // Peak-hold tracks the true, unsmoothed instantaneous reading - jumps
        // to a louder peak immediately, otherwise sits for a beat before
        // falling back at a fixed rate - so a brief transient still registers
        // even though the bar itself (below) no longer follows every raw
        // reading directly.
        if (rawDb >= st.heldPeakDb[c])
        {
            st.heldPeakDb[c]    = rawDb;
            st.heldPeakAtSec[c] = now;
        }
        else if (now - st.heldPeakAtSec[c] > 1.0)
        {
            constexpr float kDecayDbPerSec = 24.0f;
            st.heldPeakDb[c] = std::max(rawDb,
                st.heldPeakDb[c] - kDecayDbPerSec * ImGui::GetIO().DeltaTime);
        }

        // Ballistics for the bar itself: rises to a louder reading quickly,
        // falls back slowly, instead of snapping straight to whatever the
        // capture thread's latest ~10-20ms packet happened to read - real
        // audio varies far more buffer-to-buffer than a meter should read as
        // "the level," and that raw jumpiness is what showed up as flicker.
        constexpr float kAttackDbPerSec  = 200.0f;   // up: fast enough to feel instant
        constexpr float kReleaseDbPerSec = 20.0f;    // down: a deliberate fall, not a snap
        const float rate = (rawDb >= st.displayDb[c]) ? kAttackDbPerSec : kReleaseDbPerSec;
        const float maxStep = rate * ImGui::GetIO().DeltaTime;
        const float delta = rawDb - st.displayDb[c];
        st.displayDb[c] += std::clamp(delta, -maxStep, maxStep);

        const float instDb = st.displayDb[c];

        const float x0 = barsMinX + gap + c * (barWidth + gap);
        const float x1 = x0 + barWidth;

        draw->AddRectFilled(ImVec2(x0, avail.Min.y), ImVec2(x1, avail.Max.y), IM_COL32(30, 30, 30, 255));

        // Dim green/yellow/red zones painted across the *whole* scale, always
        // visible - the same way a hardware meter's own face is marked - so
        // the danger zones read at a glance even while the signal is nowhere
        // near them, not just once it's already too loud.
        draw->AddRectFilled(ImVec2(x0, dbToY(kYellowDb)), ImVec2(x1, avail.Max.y),      IM_COL32(30, 70, 40, 255));
        draw->AddRectFilled(ImVec2(x0, dbToY(kRedDb)),    ImVec2(x1, dbToY(kYellowDb)), IM_COL32(70, 65, 25, 255));
        draw->AddRectFilled(ImVec2(x0, avail.Min.y),      ImVec2(x1, dbToY(kRedDb)),    IM_COL32(75, 25, 25, 255));

        // The actual level lights up those same zones at full brightness, zone
        // by zone rather than one flat colour for the whole filled bar - green
        // from the floor to -10dB, then yellow to -5, then red above - so the
        // colour itself climbs with the level instead of jumping straight to
        // whatever colour the current instant happens to be in.
        const float greenTop = std::min(instDb, kYellowDb);
        draw->AddRectFilled(ImVec2(x0, dbToY(greenTop)), ImVec2(x1, avail.Max.y), IM_COL32(60, 200, 90, 255));

        if (instDb > kYellowDb)
        {
            const float yellowTop = std::min(instDb, kRedDb);
            draw->AddRectFilled(ImVec2(x0, dbToY(yellowTop)), ImVec2(x1, dbToY(kYellowDb)), IM_COL32(220, 200, 60, 255));
        }

        if (instDb > kRedDb)
            draw->AddRectFilled(ImVec2(x0, dbToY(instDb)), ImVec2(x1, dbToY(kRedDb)), IM_COL32(220, 60, 60, 255));

        if (st.peakHold && haveChannel)
        {
            const float holdY = dbToY(st.heldPeakDb[c]);
            draw->AddRectFilled(ImVec2(x0, holdY - 1.0f), ImVec2(x1, holdY + 1.0f), IM_COL32(255, 255, 255, 220));
        }

        // True Peak: a distinct cyan tick above the sample-peak reading -
        // see AudioMeterBridge.cpp's AccumulatePacket for exactly what this
        // does and doesn't claim (a real inter-sample estimate, not
        // certified ITU-R BS.1770-4 compliance). Drawn every frame at its
        // instantaneous value, not peak-held like the white tick above -
        // a true-peak overage is usually momentary but the app is already
        // redrawing every video frame, so a hold isn't needed to catch it.
        if (haveChannel && levels.truePeakDb[c] > levels.peakDb[c] + 0.05f)
        {
            const float tpY = dbToY(levels.truePeakDb[c]);
            draw->AddRectFilled(ImVec2(x0, tpY - 1.0f), ImVec2(x1, tpY + 1.0f), IM_COL32(80, 220, 255, 230));
        }
    }

    if (!levels.deviceOk)
    {
        // Distinct messages per stage, not one flat "no audio" - "Resolve
        // isn't running" and "found it, but couldn't tap its audio" and
        // "tapped it, just nothing playing" are very different things to see
        // while chasing down why the meter reads silence.
        char msg[128];
        AudioStatusMessage(levels, msg, sizeof(msg));

        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f),
                      kColMuted, msg);
    }

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

// Shared "why is this silent" message for the Goniometer/Spectrum Analyzer -
// same status switch DrawAudioMeterPanel's own inline version has, factored
// out here rather than duplicated a second and third time.
//
// Names whichever host the bridge is tapping (AudioMeterLevels::hostName).
void AudioStatusMessage(const AudioMeterLevels& p_Levels, char* p_Buf, size_t p_BufSize)
{
    const char* host = p_Levels.hostName ? p_Levels.hostName : "the host";
#ifdef __APPLE__
    // macOS: lastHresult is an OSStatus, and Core Audio's are mostly
    // four-character codes ('!obj', 'nope') - shown as such, since that is
    // the form Apple's headers and every forum thread about them use.
    char code[24];
    {
        const uint32_t v = static_cast<uint32_t>(p_Levels.lastHresult);
        const char c[4] = { char(v >> 24), char(v >> 16), char(v >> 8), char(v) };
        bool printable = true;
        for (char ch : c) printable = printable && ch >= 0x20 && ch < 0x7F;
        if (printable)
            std::snprintf(code, sizeof(code), "'%c%c%c%c'", c[0], c[1], c[2], c[3]);
        else
            std::snprintf(code, sizeof(code), "%ld", p_Levels.lastHresult);
    }
    switch (p_Levels.status)
    {
        case AudioMeterStatus::ResolveNotFound:
            std::snprintf(p_Buf, p_BufSize, "%s isn't running.", host);
            break;
        case AudioMeterStatus::ActivationCallFailed:
            std::snprintf(p_Buf, p_BufSize, "Couldn't tap %s's audio (%s).", host, code);
            break;
        case AudioMeterStatus::ActivationTimedOut:   // Windows only - never set on macOS
            std::snprintf(p_Buf, p_BufSize, "Timed out waiting to tap %s's audio.", host);
            break;
        case AudioMeterStatus::ActivationResultFailed:
            std::snprintf(p_Buf, p_BufSize, "Couldn't set up capture of %s's audio (%s).", host, code);
            break;
        case AudioMeterStatus::StreamInitFailed:
            std::snprintf(p_Buf, p_BufSize, "%s's audio stream failed to start (%s).", host, code);
            break;
        case AudioMeterStatus::PermissionDenied:
            // Three short lines: the panels can be narrow, and this one has
            // to be read in full to be any use.
            std::snprintf(p_Buf, p_BufSize, "Audio recording is off for Scope Deck. Allow it in\n"
                                            "System Settings > Privacy & Security >\n"
                                            "Screen & System Audio Recording.");
            break;
        case AudioMeterStatus::HostHasNoAudio:
            std::snprintf(p_Buf, p_BufSize, "%s hasn't opened any audio yet.", host);
            break;
        case AudioMeterStatus::Capturing:
        default:
            std::snprintf(p_Buf, p_BufSize, "Tapped into %s - waiting for audio.", host);
            break;
    }
#else
    const unsigned long hr = static_cast<unsigned long>(p_Levels.lastHresult);
    switch (p_Levels.status)
    {
        case AudioMeterStatus::ResolveNotFound:
            std::snprintf(p_Buf, p_BufSize, "%s isn't running.", host);
            break;
        case AudioMeterStatus::ActivationCallFailed:
            std::snprintf(p_Buf, p_BufSize, "Couldn't request %s's audio (0x%08lX).", host, hr);
            break;
        case AudioMeterStatus::ActivationTimedOut:
            std::snprintf(p_Buf, p_BufSize, "Timed out waiting to tap %s's audio.", host);
            break;
        case AudioMeterStatus::ActivationResultFailed:
            std::snprintf(p_Buf, p_BufSize, "%s refused the audio tap (0x%08lX).", host, hr);
            break;
        case AudioMeterStatus::StreamInitFailed:
            std::snprintf(p_Buf, p_BufSize, "%s's audio stream failed to start (0x%08lX).", host, hr);
            break;
        case AudioMeterStatus::Capturing:
        default:
            std::snprintf(p_Buf, p_BufSize, "Tapped into %s - waiting for audio.", host);
            break;
    }
#endif
}

// ---------------------------------------------------------------------------
// Goniometer
// ---------------------------------------------------------------------------
//
// Classic diamond orientation: mid (L+R) up the screen, side (L-R) across it
// - mono/correlated content draws a vertical line, fully out-of-phase draws
// a horizontal one, the same convention every hardware/software goniometer
// uses. Reads AudioMeterBridge's raw ring buffer directly - the level
// meter's own peak/RMS have already thrown the waveform away by the time
// this panel would see them.
void DrawGoniometerPanel(App& p_App, Panel& p_Panel)
{
    GoniometerState& st     = p_Panel.goniometer;
    PanelCommon&     common = p_Panel.common;

    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    // Square, centred - the diamond graticule only means what it should
    // when X and Y share one scale.
    const ImRect plot = FitAspect(avail, 1.0f, 1.0f);
    const ImVec2 center = plot.GetCenter();
    const float  radius = plot.GetWidth() * 0.5f;

    draw->PushClipRect(avail.Min, avail.Max, true);

    const ImU32 gridCol = IM_COL32(255, 255, 255, 50);
    draw->AddLine(ImVec2(center.x, plot.Min.y), ImVec2(center.x, plot.Max.y), gridCol, 1.0f);
    draw->AddLine(ImVec2(plot.Min.x, center.y), ImVec2(plot.Max.x, center.y), gridCol, 1.0f);
    draw->AddLine(plot.Min, plot.Max, gridCol, 1.0f);
    draw->AddLine(ImVec2(plot.Min.x, plot.Max.y), ImVec2(plot.Max.x, plot.Min.y), gridCol, 1.0f);
    draw->AddCircle(center, radius, IM_COL32(255, 255, 255, 70), 64, 1.0f);
    draw->AddText(ImVec2(center.x - 4.0f, plot.Min.y + 4.0f), kColAxisLabel, "M");
    draw->AddText(ImVec2(plot.Max.x - 12.0f, center.y - 7.0f), kColAxisLabel, "S");

    const AudioMeterLevels levels = AudioMeterBridgeGetLevels();

    if (levels.deviceOk)
    {
        const AudioRingSnapshot ring = AudioMeterBridgeGetRingSnapshot();
        constexpr int kMaxFrames = 2048;
        std::vector<float> buf(size_t(kMaxFrames) * AudioRingSnapshot::kChannels);
        const int count = ring.CopyLastFrames(buf.data(), kMaxFrames);

        constexpr float kScale = 0.70710678f;   // 1/sqrt(2)

        if (st.enhancedRender)
        {
            // Enhanced Render: one continuous translucent polyline through
            // the actual sample path, not independent dots - the same
            // additively-blended-beam technique DrawVectorscopeEnhanced
            // uses, so overlapping segments naturally brighten wherever the
            // trace revisits the same region (each is its own alpha-blended
            // draw, and ImGui doesn't merge overlapping geometry - repeated
            // coverage really does accumulate). Consecutive audio samples
            // are always physically continuous - a real jump between them
            // would be an audible click - so unlike a video scanline's
            // colour there is no "jump break" case to guard against here;
            // every connection drawn is real signal, not a spurious link
            // between unrelated points.
            if (count >= 2)
            {
                std::vector<ImVec2> points;
                points.reserve(size_t(count));
                for (int i = 0; i < count; ++i)
                {
                    const float l = buf[size_t(i) * 2 + 0];
                    const float r = buf[size_t(i) * 2 + 1];
                    const float side = (l - r) * kScale * st.gain;
                    const float mid  = (l + r) * kScale * st.gain;
                    points.push_back(ImVec2(center.x + side * radius, center.y - mid * radius));
                }
                draw->AddPolyline(points.data(), int(points.size()), IM_COL32(120, 220, 160, 45),
                                  ImDrawFlags_None, 1.3f);
            }
        }
        else
        {
            // Default: one dot per sample - Density sizes them, same
            // control/range Color Cube's own point-cloud panel uses.
            const float pointRadius = float(st.density) * 0.5f + 0.5f;
            const ImU32 dotCol = IM_COL32(120, 220, 160, 130);
            for (int i = 0; i < count; ++i)
            {
                const float l = buf[size_t(i) * 2 + 0];
                const float r = buf[size_t(i) * 2 + 1];
                const float side = (l - r) * kScale * st.gain;
                const float mid  = (l + r) * kScale * st.gain;
                draw->AddCircleFilled(ImVec2(center.x + side * radius, center.y - mid * radius),
                                      pointRadius, dotCol);
            }
        }
    }
    else
    {
        char msg[128];
        AudioStatusMessage(levels, msg, sizeof(msg));
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawGoniometerSettings(Panel& p_Panel)
{
    GoniometerState& st     = p_Panel.goniometer;
    PanelCommon&     common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Goniometer Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##gain", &st.gain, 0.25f, 4.0f, 1.0f, "%.2f");

    SettingsRowLabel("Density");
    SettingsSliderInt("##density", &st.density, 1, 6, 2);

    SettingsRowLabel("Enhanced Render");
    ImGui::Checkbox("##enhanced", &st.enhancedRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws one connected beam trace through the actual\n"
                          "sample path instead of a dot per sample.");

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Spectrum Analyzer
// ---------------------------------------------------------------------------
//
// 20Hz-20kHz, log-scaled - low-end rumble, 50/60Hz ground-loop hum, and
// high-frequency hiss each have a natural home on this axis a level meter
// or goniometer can't show. No zoom/pan: a fixed frequency axis is the
// entire point of a spectrum analyzer.
void DrawSpectrumAnalyzerPanel(App& p_App, Panel& p_Panel)
{
    SpectrumAnalyzerState& st     = p_Panel.spectrumAnalyzer;
    PanelCommon&           common = p_Panel.common;

    const ImRect content = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, content);
    common.panelRect = content;

    draw->PushClipRect(content.Min, content.Max, true);

    constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f;
    auto freqToX = [&](float f) {
        const float t = std::log10(f / kMinFreq) / std::log10(kMaxFreq / kMinFreq);
        return content.Min.x + std::clamp(t, 0.0f, 1.0f) * content.GetWidth();
    };

    const AudioMeterLevels levels = AudioMeterBridgeGetLevels();

    if (levels.deviceOk)
    {
        const AudioRingSnapshot ring = AudioMeterBridgeGetRingSnapshot();
        constexpr int n = SpectrumAnalyzerState::kFftSize;
        std::vector<float> raw(size_t(n) * AudioRingSnapshot::kChannels);
        const int count = ring.CopyLastFrames(raw.data(), n);

        if (count == n)
        {
            std::vector<float> mono(n);
            for (int i = 0; i < n; ++i)
                mono[i] = (raw[size_t(i) * 2 + 0] + raw[size_t(i) * 2 + 1]) * 0.5f;

            std::vector<float> spectrumDb;
            ComputeSpectrumDb(mono, spectrumDb);

            if (st.displayDb.size() != spectrumDb.size())
                st.displayDb.assign(spectrumDb.size(), st.minDb);

            // Same ballistics reasoning the Audio Meter's peak/RMS bars use:
            // a raw per-frame FFT redraw is too jittery to read, so this
            // rises fast and falls back at a fixed rate instead of snapping.
            // Attack close to the Audio Meter's own 200dB/sec (near-instant)
            // rather than 40 - the original rate visibly lagged behind real
            // transients, reading as smoothed-out rather than responsive.
            constexpr float kAttackDbPerSec  = 160.0f;
            constexpr float kReleaseDbPerSec = 30.0f;
            const float dt = ImGui::GetIO().DeltaTime;
            for (size_t i = 0; i < spectrumDb.size(); ++i)
            {
                const float rate = (spectrumDb[i] >= st.displayDb[i]) ? kAttackDbPerSec : kReleaseDbPerSec;
                const float maxStep = rate * dt;
                const float delta = spectrumDb[i] - st.displayDb[i];
                st.displayDb[i] += std::clamp(delta, -maxStep, maxStep);
            }

            std::vector<ImVec2> outline;
            outline.reserve(st.displayDb.size());
            ImVec2 prevPoint(0.0f, 0.0f);
            bool   havePrev = false;

            const float sampleRate = float(ring.sampleRate);
            for (size_t i = 1; i < st.displayDb.size(); ++i)   // skip bin 0 (DC)
            {
                const float freq = float(i) * sampleRate / float(n);
                if (freq < kMinFreq) continue;
                if (freq > kMaxFreq) break;

                const float x = freqToX(freq);
                const float t = std::clamp((st.displayDb[i] - st.minDb) / (0.0f - st.minDb), 0.0f, 1.0f);
                const float y = content.Max.y - t * content.GetHeight();

                if (havePrev)
                    draw->AddQuadFilled(prevPoint, ImVec2(x, y), ImVec2(x, content.Max.y),
                                        ImVec2(prevPoint.x, content.Max.y), IM_COL32(90, 200, 255, 60));
                prevPoint = ImVec2(x, y);
                havePrev  = true;
                outline.push_back(ImVec2(x, y));
            }
            if (outline.size() >= 2)
                draw->AddPolyline(outline.data(), int(outline.size()), IM_COL32(120, 220, 255, 255),
                                  ImDrawFlags_None, 1.5f);
        }
    }
    else
    {
        char msg[128];
        AudioStatusMessage(levels, msg, sizeof(msg));
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(content.GetCenter().x - size.x * 0.5f, content.GetCenter().y - size.y * 0.5f),
                     kColMuted, msg);
    }

    // Frequency gridlines, log-spaced landmarks.
    static const float kFreqLabels[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float f : kFreqLabels)
    {
        const float x = freqToX(f);
        draw->AddLine(ImVec2(x, content.Min.y), ImVec2(x, content.Max.y), IM_COL32(255, 255, 255, 35), 1.0f);
        char label[8];
        if (f >= 1000.0f) std::snprintf(label, sizeof(label), "%gk", f / 1000.0f);
        else               std::snprintf(label, sizeof(label), "%g", f);
        draw->AddText(ImVec2(x + 2.0f, content.Max.y - 16.0f), kColAxisLabel, label);
    }

    // 50/60Hz ground-loop hum landmarks - the specific thing this panel
    // exists to catch, called out rather than left to blend into the grid.
    for (float f : { 50.0f, 60.0f })
    {
        const float x = freqToX(f);
        draw->AddLine(ImVec2(x, content.Min.y), ImVec2(x, content.Max.y), IM_COL32(255, 200, 60, 90), 1.0f);
    }

    // dB gridlines.
    for (float db = 0.0f; db >= st.minDb; db -= 20.0f)
    {
        const float t = (db - st.minDb) / (0.0f - st.minDb);
        const float y = content.Max.y - t * content.GetHeight();
        draw->AddLine(ImVec2(content.Min.x, y), ImVec2(content.Max.x, y), IM_COL32(255, 255, 255, 25), 1.0f);
        char label[8];
        std::snprintf(label, sizeof(label), "%.0f", db);
        draw->AddText(ImVec2(content.Min.x + 2.0f, y - 14.0f), kColAxisLabel, label);
    }

    draw->PopClipRect();

    if (DrawPanelCloseButton(content, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawSpectrumAnalyzerSettings(Panel& p_Panel)
{
    SpectrumAnalyzerState& st     = p_Panel.spectrumAnalyzer;
    PanelCommon&           common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Spectrum Analyzer Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Floor (dB)");
    SettingsSliderFloat("##floor", &st.minDb, -120.0f, -40.0f, -80.0f, "%.0f");

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// Radial Spectrum removed 2026-09-19 at Trevor's request after three outer-
// ring redesigns in one session didn't land (a drawn wave, then drifting
// particles, then transient-triggered ripple rings - the last of which he
// "sort of liked" but asked to shelve rather than keep tuning right now).
// Full last-working code archived outside the repository for a future revival.
// ---------------------------------------------------------------------------
// Color Cube
// ---------------------------------------------------------------------------
//
// A genuine 3D point cloud with real camera orbit - not a fixed-angle
// projection - ported from the old app's own design (its own docstring:
// "a plain CPU rasteriser, same spirit as the rest of this app's trace
// rendering"). Redesigned for this rendering paradigm rather than a literal
// port, in two ways:
//   - Points are drawn as ordinary ImDrawList primitives (AddCircleFilled),
//     not rasterised into a CPU pixel buffer and blitted as a texture - the
//     old app needed that trick because QPainter has no cheap way to stamp a
//     few thousand individual points; ImDrawList is GPU-batched and does not
//     need it, so skipping it is simpler *and* almost certainly faster, not
//     a compromise.
//   - The point subsample is evenly strided rather than truly random (see
//     BuildColorCubePoints' own comment) - cheap regardless of preview
//     resolution instead of scaling with it.
// Everything else - the 6000-point cap, the frame-seeded determinism, the
// three colour spaces sharing one camera scale, orbit/pan/zoom/reset, Spread
// and Density - matches the old app's own design.

// Rotates a normalised (-1..1 per axis) point by the camera's yaw/pitch and
// projects it orthographically to screen space. Returns the rotated depth
// too (not used for the 2D position, an orthographic projection does not
// need it - only for back-to-front draw order).
void ColorCubeProject(const ColorCubeState& p_St, const ImVec2& p_Center, float p_ScalePx,
                      float p_X, float p_Y, float p_Z, ImVec2& p_Out, float& p_OutDepth)
{
    float x2, y2, z2;
    Mat3Apply(p_St.rotation, p_X, p_Y, p_Z, x2, y2, z2);

    p_OutDepth = z2;
    const float scale = p_ScalePx * p_St.zoom;
    p_Out = ImVec2(p_Center.x + x2 * scale + p_St.panX,
                   p_Center.y - y2 * scale + p_St.panY);
}

// The true black/white points for the current colour space, run through the
// same per-space normalisation BuildColorCubePoints uses - not always the
// cube's own (-1,-1,-1)/(1,1,1) corners. RGB and XYZ both happen to land
// exactly there (each axis is independently scaled 0..1 -> -1..1 against
// that space's own white point), but Lab's neutral axis is L alone at
// a*=b*=0, i.e. (-1,0,0) to (1,0,0) - nowhere near the cube's diagonal.
void ColorCubeNeutralAxis(ColorCubeSpace p_Space, float p_Out[2][3])
{
    if (p_Space == ColorCubeSpace::Lab)
    {
        p_Out[0][0] = -1.0f; p_Out[0][1] = 0.0f; p_Out[0][2] = 0.0f;
        p_Out[1][0] =  1.0f; p_Out[1][1] = 0.0f; p_Out[1][2] = 0.0f;
    }
    else
    {
        p_Out[0][0] = p_Out[0][1] = p_Out[0][2] = -1.0f;
        p_Out[1][0] = p_Out[1][1] = p_Out[1][2] =  1.0f;
    }
}

void DrawColorCubePanel(App& p_App, Panel& p_Panel)
{
    PanelCommon&    common = p_Panel.common;
    ColorCubeState& st     = p_Panel.colorCube;

    const ImRect avail = PlotRect();
    ImDrawList*  draw  = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    // Re-sampled only when the underlying video frame, the colour space, or
    // the mask/qualifier the points are filtered through actually changes -
    // re-converting a few thousand points is real work, and every panel needs
    // the same set for both drawing and (eventually) picking, not a fresh
    // sample per draw call.
    if (st.pointsFrameIndex != p_App.frame.frameIndex || st.pointsSpace != st.space
        || !SpatialMaskEquals(st.pointsMask, p_App.spatialMask)
        || !ValueQualifierEquals(st.pointsQualifier, p_App.valueQualifier))
    {
        BuildColorCubePoints(p_App.frame, st.space, st.points,
                             &p_App.spatialMask, &p_App.valueQualifier);
        st.pointsFrameIndex = p_App.frame.frameIndex;
        st.pointsSpace      = st.space;
        st.pointsMask       = p_App.spatialMask;
        st.pointsQualifier  = p_App.valueQualifier;
    }

    // Orbit left/right (plain middle-drag), orbit up/down (Alt+middle-drag),
    // pan (Shift+middle-drag) and zoom (wheel) - the same middle-button
    // convention common 3D DCC tools use, so panning doesn't fight orbiting
    // the way sharing one plain-drag gesture would. Each modifier claims the
    // drag outright rather than adding to it, so the three never overlap.
    // "Z" resets the view. All gated on this panel being hovered, so two
    // Color Cube panels' cameras never fight over the same drag the way the
    // spatial mask's single shared instance once did - this state is
    // per-panel already, nothing to share in the first place.
    const bool hovered = ImGui::IsWindowHovered();
    ImGuiIO&   io      = ImGui::GetIO();

    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    {
        if (io.KeyShift)
        {
            st.panX += io.MouseDelta.x;
            st.panY += io.MouseDelta.y;
        }
        else
        {
            // Each drag adjusts one of the two turntable angles and the
            // matrix is rebuilt from both (see ColorCubeBuildRotation) -
            // never folded into the previous orientation, which is what
            // would let roll accumulate and tip the cube off its plane.
            constexpr float kDegPerPixel = 0.5f;
            const float xSign = st.invertX ? -1.0f : 1.0f;
            const float ySign = st.invertY ? -1.0f : 1.0f;

            // One axis at a time: a plain middle-drag only ever yaws (about
            // screen-vertical), and Alt+middle-drag only ever pitches (about
            // screen-horizontal). A free two-axis drag makes it easy to
            // knock the cube off an orientation that was deliberately set,
            // since no real hand moves in a perfectly straight line;
            // separating them means a horizontal sweep cannot disturb the
            // elevation and vice versa.
            const bool  pitching = io.KeyAlt;
            const float dYaw   = pitching ? 0.0f : io.MouseDelta.x * kDegPerPixel * xSign;
            const float dPitch = pitching ? io.MouseDelta.y * kDegPerPixel * ySign : 0.0f;

            if (dYaw != 0.0f || dPitch != 0.0f)
            {
                st.azimuthDeg   = std::fmod(st.azimuthDeg + dYaw, 360.0f);
                st.elevationDeg = std::clamp(st.elevationDeg + dPitch, -90.0f, 90.0f);
                ColorCubeBuildRotation(st.azimuthDeg, st.elevationDeg, st.rotation);
            }
        }
    }

    if (hovered)
    {
        if (io.MouseWheel != 0.0f)
            st.zoom = std::clamp(st.zoom * std::pow(1.15f, io.MouseWheel),
                                 kColorCubeMinZoom, kColorCubeMaxZoom);

        if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            st.azimuthDeg   = kColorCubeDefaultAzimuth;
            st.elevationDeg = kColorCubeDefaultElevation;
            ColorCubeBuildRotation(st.azimuthDeg, st.elevationDeg, st.rotation);
            st.zoom = kColorCubeDefaultZoom; st.panX = 0.0f; st.panY = 0.0f;
        }
    }

    const ImVec2 center   = avail.GetCenter();
    const float  scalePx  = std::min(avail.GetWidth(), avail.GetHeight()) * 0.5f * 0.8f;
    const ImU32  wireCol  = GraticuleColor(kColGraticule, common.graticuleBrightness);

    auto project = [&](float x, float y, float z, ImVec2& outPos, float& outDepth)
    {
        ColorCubeProject(st, center, scalePx, x, y, z, outPos, outDepth);
    };

    draw->PushClipRect(avail.Min, avail.Max, true);

    // Wireframe cube: 8 corners at every +-1 combination, an edge between
    // any two that differ in exactly one axis.
    ImVec2 cornerPos[8];
    for (int i = 0; i < 8; ++i)
    {
        float depth;
        project((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f,
               cornerPos[i], depth);
    }
    for (int i = 0; i < 8; ++i)
        for (int j = i + 1; j < 8; ++j)
        {
            const int diff = i ^ j;
            if (diff == 1 || diff == 2 || diff == 4)
                draw->AddLine(cornerPos[i], cornerPos[j], wireCol, 1.0f);
        }

    // Spread stretches each sampled point's perpendicular distance off the
    // neutral axis - real footage hugs that axis as a thin sliver at
    // Spread=1 (accurate, but not very legible), so this is purely a
    // legibility aid, applied at draw time rather than baked into the
    // sampled points so changing it never needs a re-sample.
    float neutral[2][3];
    ColorCubeNeutralAxis(st.space, neutral);
    const float axisX = neutral[1][0] - neutral[0][0];
    const float axisY = neutral[1][1] - neutral[0][1];
    const float axisZ = neutral[1][2] - neutral[0][2];
    const float axisLenSq = std::max(axisX * axisX + axisY * axisY + axisZ * axisZ, 1e-6f);

    // Back-to-front: farthest points painted first so nearer ones overwrite,
    // matching the old app's own depth-sorted rasterisation.
    std::vector<std::pair<float, size_t>> order(st.points.size());
    for (size_t i = 0; i < st.points.size(); ++i)
    {
        const ColorCubePoint& pt = st.points[i];
        ImVec2 pos; float depth;
        project(pt.x, pt.y, pt.z, pos, depth);
        order[i] = { depth, i };
    }
    std::sort(order.begin(), order.end(),
             [](const auto& a, const auto& b) { return a.first < b.first; });

    const float pointRadius = float(st.density) * 0.5f + 0.5f;
    for (const auto& [depth, idx] : order)
    {
        const ColorCubePoint& pt = st.points[idx];

        // Project onto the neutral axis, then stretch the remainder by
        // Spread - same effect as the old app's own perpendicular-only scale.
        const float dx = pt.x - neutral[0][0], dy = pt.y - neutral[0][1], dz = pt.z - neutral[0][2];
        const float t  = (dx * axisX + dy * axisY + dz * axisZ) / axisLenSq;
        const float onAxisX = neutral[0][0] + t * axisX;
        const float onAxisY = neutral[0][1] + t * axisY;
        const float onAxisZ = neutral[0][2] + t * axisZ;
        const float spread  = float(st.spread);
        const float sx = onAxisX + (pt.x - onAxisX) * spread;
        const float sy = onAxisY + (pt.y - onAxisY) * spread;
        const float sz = onAxisZ + (pt.z - onAxisZ) * spread;

        ImVec2 pos; float unusedDepth;
        project(sx, sy, sz, pos, unusedDepth);
        draw->AddCircleFilled(pos, pointRadius, IM_COL32(pt.colour.r, pt.colour.g, pt.colour.b, 255));
    }

    // Neutral axis, dashed, with endpoint labels - drawn after the points so
    // it reads as a reference line over the cloud, not under it.
    {
        ImVec2 blackPos, whitePos; float unusedDepth;
        project(neutral[0][0], neutral[0][1], neutral[0][2], blackPos, unusedDepth);
        project(neutral[1][0], neutral[1][1], neutral[1][2], whitePos, unusedDepth);
        AddDashedLine(draw, blackPos, whitePos, wireCol, 4.0f, 3.0f, 1.0f);
        draw->AddText(ImVec2(blackPos.x + 4.0f, blackPos.y + 4.0f), kColAxisLabel, "Black");
        draw->AddText(ImVec2(whitePos.x + 4.0f, whitePos.y - 14.0f), kColAxisLabel, "White");
    }

    // The three coordinate axes, running along the cube's own edges out of
    // the black corner (-1,-1,-1) rather than out of the centre - coloured
    // and labelled per the current colour space. Anchoring them at a corner
    // instead of the middle keeps them out of the point cloud, which sits
    // mostly near the centre and along the neutral axis.
    {
        static const char* kAxisNamesRGB[3] = { "R", "G", "B" };
        static const char* kAxisNamesXYZ[3] = { "X", "Y", "Z" };
        static const char* kAxisNamesLab[3] = { "L", "a", "b" };
        const char* const* axisNames = (st.space == ColorCubeSpace::XYZ) ? kAxisNamesXYZ
                                       : (st.space == ColorCubeSpace::Lab) ? kAxisNamesLab
                                                                            : kAxisNamesRGB;
        static const ImU32 kAxisColours[3] = {
            IM_COL32(230, 90, 90, 255), IM_COL32(90, 220, 110, 255), IM_COL32(100, 140, 240, 255)
        };

        ImVec2 cornerScreenPos; float unusedDepth;
        project(-1.0f, -1.0f, -1.0f, cornerScreenPos, unusedDepth);

        const float axisEnds[3][3] = { {1,-1,-1}, {-1,1,-1}, {-1,-1,1} };
        for (int a = 0; a < 3; ++a)
        {
            ImVec2 endPos;
            project(axisEnds[a][0], axisEnds[a][1], axisEnds[a][2], endPos, unusedDepth);
            draw->AddLine(cornerScreenPos, endPos, kAxisColours[a], 1.5f);
            draw->AddText(ImVec2(endPos.x + 3.0f, endPos.y - 6.0f), kAxisColours[a], axisNames[a]);
        }
    }

    draw->PopClipRect();

    char spaceLabel[32];
    std::snprintf(spaceLabel, sizeof(spaceLabel), "Color space: %s", ColorCubeSpaceName(st.space));
    draw->AddText(ImVec2(avail.Min.x + 4.0f, avail.Max.y - 18.0f), kColAxisLabel, spaceLabel);

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawColorCubeSettings(Panel& p_Panel)
{
    ColorCubeState& st     = p_Panel.colorCube;
    PanelCommon&    common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Color Cube Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Color Space");
    int spaceIndex = int(st.space);
    const char* spaceNames[] = { "RGB", "XYZ", "Lab" };
    if (ImGui::Combo("##colorspace", &spaceIndex, spaceNames, IM_ARRAYSIZE(spaceNames)))
        st.space = ColorCubeSpace(spaceIndex);

    SettingsRowLabel("Spread");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stretches the cloud away from the neutral (black-white)\n"
                          "axis for legibility - real footage hugs that axis as a thin\n"
                          "sliver at 1x, accurate but hard to read.");
    SettingsSliderInt("##spread", &st.spread, 1, 10, 1);

    SettingsRowLabel("Density");
    SettingsSliderInt("##density", &st.density, 1, 6, 2);

    SettingsRowLabel("Invert X Rotation");
    ImGui::Checkbox("##invertX", &st.invertX);

    SettingsRowLabel("Invert Y Rotation");
    ImGui::Checkbox("##invertY", &st.invertY);

    ImGui::TextDisabled("Middle-drag to orbit left/right, Alt+middle-drag to");
    ImGui::TextDisabled("orbit up/down, Shift+middle-drag to pan, scroll to");
    ImGui::TextDisabled("zoom, Z to reset the view.");

    DrawSharedSettings(common, false, true);
    ImGui::End();
}

void DrawNoiseSettings(Panel& p_Panel)
{
    NoiseState& st = p_Panel.noise;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;
    
    char name[64];
    std::snprintf(name, sizeof(name), "Noise Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Method");
    int methodInt = (int)st.method;
    if (ImGui::Combo("##Method", &methodInt, "Highpass\0Channel\0Differenced\0")) {
        st.method = (NoiseMethod)methodInt;
        st.dirty = true;
    }

    SettingsRowLabel("Channel");
    if (ImGui::Combo("##Channel", &st.channel, "Red\0Green\0Blue\0Luma\0")) st.dirty = true;
    
    SettingsRowLabel("Gain");
    if (SettingsSliderFloat("##Gain", &st.gain, 1.0f, 64.0f, 8.0f, "%.1f")) st.dirty = true;

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// Focus Peaking, Banding Analyzer and A/B Difference are all preview-tier
// panels shaped exactly like Noise above: a CPU build function into st.image,
// uploaded to st.texture, drawn fit-to-panel. Kept as separate functions
// rather than folded into one template - each has its own settings and the
// duplication is a handful of lines, well under the cost of a shared
// abstraction three call sites don't otherwise ask for.

void DrawFocusPeakingPanel(App& p_App, Panel& p_Panel)
{
    FocusPeakingState& st = p_Panel.focusPeaking;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        // Rebuilt only when the frame or a setting changed - see DrawFalseColorPanel.
        const bool stale = st.builtFrameIndex  != p_App.frame.frameIndex
                        || st.builtThreshold   != st.threshold
                        || st.builtGrayscaleBg != st.grayscaleBg
                        || st.builtColor       != st.color;
        if (stale)
        {
            BuildFocusPeakingImage(p_App.frame, st.threshold, st.grayscaleBg, PeakColorRgb(st.color), st.image);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex  = p_App.frame.frameIndex;
            st.builtThreshold   = st.threshold;
            st.builtGrayscaleBg = st.grayscaleBg;
            st.builtColor       = st.color;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawBandingAnalyzerPanel(App& p_App, Panel& p_Panel)
{
    BandingAnalyzerState& st = p_Panel.bandingAnalyzer;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        // Rebuilt only when the frame or the gain changed - see DrawFalseColorPanel.
        const bool stale = st.builtFrameIndex != p_App.frame.frameIndex
                        || st.builtGain != st.gain;
        if (stale)
        {
            BuildBandingAnalyzerImage(p_App.frame, st.gain, st.image);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex = p_App.frame.frameIndex;
            st.builtGain       = st.gain;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawDifferencePanel(App& p_App, Panel& p_Panel)
{
    DifferenceState& st = p_Panel.difference;
    PanelCommon& common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (p_App.haveFrame && p_App.frame.HasPreview())
    {
        // Rebuilt only when the frame, the gain or the still changed - see
        // DrawFalseColorPanel. A cleared still has generation 0, which differs
        // from any captured one, so clearing rebuilds too.
        const bool stale = st.builtFrameIndex      != p_App.frame.frameIndex
                        || st.builtGain            != st.gain
                        || st.builtStillGeneration != st.still.generation;
        if (stale)
        {
            BuildDifferenceImage(p_App.frame,
                                 st.still.Empty() ? nullptr : st.still.rgb.data(), st.still.width, st.still.height,
                                 st.gain, st.image);
            if (!st.image.Empty()) {
                st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
            }
            st.builtFrameIndex      = p_App.frame.frameIndex;
            st.builtGain            = st.gain;
            st.builtStillGeneration = st.still.generation;
        }
    }

    if (!st.texture.Valid())
    {
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);
    draw->PopClipRect();

    if (st.still.Empty())
    {
        const char* msg = "No reference still - showing live x 0.2.\nRight-click -> Settings -> Capture or Load one.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(fit.GetCenter().x - size.x * 0.5f, fit.Max.y - size.y - 6.0f), kColMuted, msg);
    }
    else if (st.still.width != int(p_App.frame.previewWidth) || st.still.height != int(p_App.frame.previewHeight))
    {
        // BuildDifferenceImage silently falls back to "no reference" the
        // moment the sizes disagree (see its own comment) - a loaded still
        // is under no obligation to match the live preview's resolution the
        // way a capture always does, so this is the one case worth telling
        // the user about rather than leaving them to notice the fallback
        // text disagrees with what they just loaded.
        const char* msg = "Reference still size doesn't match the live preview - showing live x 0.2.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(fit.GetCenter().x - size.x * 0.5f, fit.Max.y - size.y - 6.0f), kColMuted, msg);
    }

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawFocusPeakingSettings(Panel& p_Panel)
{
    FocusPeakingState& st = p_Panel.focusPeaking;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Focus Peaking Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Threshold");
    float thresholdPercent = st.threshold * 100.0f;
    if (SettingsSliderFloat("##Threshold", &thresholdPercent, 5.0f, 80.0f, 20.0f, "%.0f%%"))
        st.threshold = thresholdPercent / 100.0f;

    SettingsRowLabel("Grayscale BG");
    ImGui::Checkbox("##grayscalebg", &st.grayscaleBg);

    SettingsRowLabel("Peaking Color");
    if (ImGui::BeginCombo("##peakcolor", PeakColorName(st.color)))
    {
        for (int i = 0; i < 4; ++i)
        {
            const PeakColor c = (PeakColor)i;
            if (ImGui::Selectable(PeakColorName(c), st.color == c))
                st.color = c;
        }
        ImGui::EndCombo();
    }

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

void DrawBandingAnalyzerSettings(Panel& p_Panel)
{
    BandingAnalyzerState& st = p_Panel.bandingAnalyzer;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Banding Analyzer Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##Gain", &st.gain, 1.0f, 64.0f, 16.0f, "%.1f");

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// Capture/Load/Clear row for a StillImage, shared by A/B Difference and
// Compare's own settings so the two panels' buttons cannot drift apart -
// they are the same operation on a different panel's still.
//
// The still itself is a snapshot, not a live setting - same reasoning as
// the old app's ref_image, kept off the persisted field list.
void DrawStillCaptureRow(App& p_App, StillImage& p_Still)
{
    if (ImGui::Button("Capture from Source"))
    {
        if (p_App.haveFrame)
            CaptureStillFromFrame(p_App.frame, p_Still);
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Still..."))
    {
        const std::string path = OpenStillFileDialog();
        if (!path.empty())
            LoadStillFromFile(path.c_str(), p_Still);
    }

    if (!p_Still.Empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
            p_Still = StillImage{};
    }

    if (!p_Still.Empty())
        ImGui::TextDisabled("%s (%dx%d)", p_Still.label.c_str(), p_Still.width, p_Still.height);
}

void DrawDifferenceSettings(App& p_App, Panel& p_Panel)
{
    DifferenceState& st = p_Panel.difference;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "A/B Difference Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##Gain", &st.gain, 1.0f, 20.0f, 1.0f, "%.1f");

    DrawStillCaptureRow(p_App, st.still);

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------
//
// A still (captured from Source, or loaded from disk) with an optional scope
// overlay drawn over it - computed from the STILL's own pixels, not the live
// signal. To hold that shape against the current source, put an ordinary
// live Waveform/Vectorscope/Histogram panel next to this one and compare by
// eye; this panel deliberately does not try to show two signals in one.

// Rebuilds st.overlayBins from the still's own pixels, but only when the
// still has actually changed since the last build - a still never changes
// on its own, so re-running the per-pixel reduction every frame the way a
// live masked scope has to would be pure waste.
void EnsureCompareOverlayBins(CompareState& p_St)
{
    if (p_St.still.Empty())
    {
        p_St.overlayBinsGeneration = 0;
        return;
    }

    if (p_St.overlayBinsGeneration == p_St.still.generation) return;

    BuildScopeBinsFromRGB(p_St.still.rgb.data(), p_St.still.width, p_St.still.height,
                          p_St.still.lumaCoeff, nullptr, p_St.overlayBins);
    p_St.overlayBinsGeneration = p_St.still.generation;
}

// Drawn over the still's own image, inside the same p_Fit rect. Each overlay
// keeps its own natural aspect (FitAspect within p_Fit) rather than being
// stretched to the still's - a vectorscope squashed to a 16:9 frame reads as
// broken, not as an overlay, the same reasoning the live Vectorscope panel's
// own square wheel is fitted rather than stretched.
void DrawCompareOverlay(ImDrawList* p_Draw, const ImRect& p_Fit, App& p_App, CompareState& p_St)
{
    if (p_St.overlay == CompareOverlay::None || p_St.overlayBins.empty) return;

    const uint8_t alpha8 = uint8_t(std::clamp(p_St.overlayOpacity, 0.0f, 1.0f) * 255.0f);

    if (p_St.overlay == CompareOverlay::Waveform || p_St.overlay == CompareOverlay::Vectorscope)
    {
        // The picture is of the still's bins, so it only changes with the
        // still, the overlay choice, or the frame-level wheel geometry the
        // Vectorscope builder reads (chroma range, luma weights). Keyed on
        // those, not on the frame, so playback does not rebuild a static image.
        const ScopeFrame& f = p_App.frame;
        const bool stale = p_St.builtOverlay    != p_St.overlay
                        || p_St.builtGeneration != p_St.overlayBinsGeneration
                        || p_St.builtChromaLo   != f.chromaRangeLow
                        || p_St.builtChromaHi   != f.chromaRangeHigh
                        || p_St.builtLuma[0]    != f.lumaCoeff[0]
                        || p_St.builtLuma[1]    != f.lumaCoeff[1]
                        || p_St.builtLuma[2]    != f.lumaCoeff[2];
        if (stale)
        {
            Image img;
            if (p_St.overlay == CompareOverlay::Waveform)
            {
                static const uint32_t kPlanes[4] = { kPlaneY, kPlaneR, kPlaneG, kPlaneB };
                BuildWaveformComposite(f, kPlanes, 4, kDefaultGain, 0, int(kWaveformLevels), img,
                                       p_St.overlayBins.waveform.data(), p_St.overlayBins.referenceSamples);
            }
            else
            {
                const uint32_t bandMask = (1u << kBandLow) | (1u << kBandMid) | (1u << kBandHigh);
                BuildVectorscopeImage(f, bandMask, 6.0f, false, 1.0f, img,
                                      p_St.overlayBins.vectorscope.data());
            }
            if (!img.Empty())
                p_St.overlayTexture.UploadRGBA(img.pixels.data(), img.width, img.height);

            p_St.builtOverlay    = p_St.overlay;
            p_St.builtGeneration = p_St.overlayBinsGeneration;
            p_St.builtChromaLo   = f.chromaRangeLow;
            p_St.builtChromaHi   = f.chromaRangeHigh;
            p_St.builtLuma[0]    = f.lumaCoeff[0];
            p_St.builtLuma[1]    = f.lumaCoeff[1];
            p_St.builtLuma[2]    = f.lumaCoeff[2];
        }

        if (!p_St.overlayTexture.Valid()) return;

        const ImRect box = FitAspect(p_Fit, float(p_St.overlayTexture.Width()),
                                            float(p_St.overlayTexture.Height()));
        p_Draw->AddImage(p_St.overlayTexture.ImGuiHandle(), box.Min, box.Max,
                         ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, alpha8));
        p_Draw->AddRect(box.Min, box.Max, kColBorder);
    }
    else if (p_St.overlay == CompareOverlay::Histogram)
    {
        // Y/R/G/B stacked over each other, the same convention the live
        // Histogram panel's own Stacked mode uses for R/G/B - Y joins them
        // here because this is a compact "signal shape" HUD, not a lane
        // layout with room to tell four channels apart at a glance anyway.
        static const uint32_t kPlanes[4] = { kPlaneY, kPlaneR, kPlaneG, kPlaneB };

        uint32_t peak = 0;
        for (uint32_t plane : kPlanes)
            for (uint32_t bin = 0; bin < kHistogramBins; ++bin)
                peak = std::max(peak, p_St.overlayBins.histogram[HistogramIndex(plane, bin)]);

        const uint8_t fillAlpha = uint8_t(40.0f * p_St.overlayOpacity);
        for (uint32_t plane : kPlanes)
            DrawHistogramLane(p_Draw, p_Fit, p_Fit, p_App.frame, plane, peak, false, fillAlpha,
                              p_St.overlayBins.histogram.data());
    }
}

void DrawComparePanel(App& p_App, Panel& p_Panel)
{
    CompareState& st = p_Panel.compare;
    PanelCommon&  common = p_Panel.common;
    const ImRect avail = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, avail);
    common.panelRect = avail;

    if (st.still.Empty())
    {
        const char* msg = "No still loaded.\nRight-click -> Settings -> Capture or Load one.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(avail.GetCenter().x - size.x * 0.5f, avail.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(avail.Min, avail.Max, kColGraticule);
        if (DrawPanelCloseButton(avail, p_App.panels.size() > 1)) p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    EnsureCompareOverlayBins(st);

    if (!st.texture.Valid() || st.uploadedGeneration != st.still.generation
        || st.texture.Width() != st.still.width || st.texture.Height() != st.still.height)
    {
        st.image.Resize(st.still.width, st.still.height);
        const uint8_t* src = st.still.rgb.data();
        uint8_t*       dst = st.image.pixels.data();
        const int      pixelCount = st.still.width * st.still.height;
        for (int i = 0; i < pixelCount; ++i)
        {
            dst[i * 4 + 0] = src[i * 3 + 0];
            dst[i * 4 + 1] = src[i * 3 + 1];
            dst[i * 4 + 2] = src[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
        st.texture.UploadRGBA(st.image.pixels.data(), st.image.width, st.image.height);
        st.uploadedGeneration = st.still.generation;
    }

    draw->PushClipRect(avail.Min, avail.Max, true);
    // HandleInput and Apply must share the same base rect - see DrawSourcePanel's
    // own comment on this fix.
    const ImRect fitBase = FitAspect(avail, float(st.texture.Width()), float(st.texture.Height()));
    common.zoom.HandleInput(fitBase, common.zoomable);
    const ImRect fit = common.zoom.Apply(fitBase);
    draw->AddImage(st.texture.ImGuiHandle(), fit.Min, fit.Max);
    draw->AddRect(fit.Min, fit.Max, kColBorder);

    DrawCompareOverlay(draw, fit, p_App, st);

    draw->PopClipRect();

    if (DrawPanelCloseButton(avail, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawCompareSettings(App& p_App, Panel& p_Panel)
{
    CompareState& st = p_Panel.compare;
    PanelCommon&  common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Compare Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    DrawStillCaptureRow(p_App, st.still);

    ImGui::SeparatorText("Scope Overlay");

    SettingsRowLabel("Overlay");
    if (ImGui::BeginCombo("##overlay", CompareOverlayName(st.overlay)))
    {
        for (int i = 0; i < int(CompareOverlay::Count); ++i)
        {
            const CompareOverlay ov = CompareOverlay(i);
            if (ImGui::Selectable(CompareOverlayName(ov), st.overlay == ov))
                st.overlay = ov;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drawn from this still's own pixels, not the live signal.\n"
                          "Compare it against the current source with an ordinary\n"
                          "live scope panel placed next to this one.");

    if (st.overlay != CompareOverlay::None)
    {
        float opacityPercent = st.overlayOpacity * 100.0f;
        if (SettingsSliderFloat("Opacity", &opacityPercent, 0.0f, 100.0f, 85.0f, "%.0f%%"))
            st.overlayOpacity = opacityPercent / 100.0f;
    }

    DrawSharedSettings(common, false, false);
    ImGui::End();
}

// A six-point "grain size" curve, drawn directly rather than through an
// Image/Texture - the same choice DrawHistogramLane makes, and for the same
// reason: it is a handful of points, not a per-pixel image, so there is
// nothing a texture upload would buy here.
void DrawGrainAnalyzerPanel(App& p_App, Panel& p_Panel)
{
    GrainAnalyzerState& st = p_Panel.grainAnalyzer;
    PanelCommon& common = p_Panel.common;

    const ImRect panel = PlotRect();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawPanelBackground(draw, panel);
    common.panelRect = panel;

    common.zoom.HandleInput(panel, common.zoomable);

    if (!p_App.haveFrame || !p_App.frame.HasPreview())
    {
        // Unlike Waveform/Histogram, this reads the preview pixels, not the
        // always-present binned data - so "no frame yet" and "frame present
        // but Publish Video is off" are the same case here, same as the
        // other preview-tier panels (False Color, Skin Tone, Noise).
        const char* msg = "No video published.\nEnable \"Publish Video\" on the Scope Tap node.";
        const ImVec2 size = ImGui::CalcTextSize(msg);
        draw->AddText(ImVec2(panel.GetCenter().x - size.x * 0.5f, panel.GetCenter().y - size.y * 0.5f), kColMuted, msg);
        draw->AddRect(panel.Min, panel.Max, GraticuleColor(kColGraticule, common.graticuleBrightness));
        if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
            p_App.closePanelIds.push_back(p_Panel.id);
        return;
    }

    if (st.builtFrameIndex != p_App.frame.frameIndex)
    {
        BuildGrainAnalyzerProfile(p_App.frame, st.profile);
        st.builtFrameIndex = p_App.frame.frameIndex;
    }

    draw->PushClipRect(panel.Min, panel.Max, true);

    const ImRect  content     = common.zoom.Apply(panel);
    const float   brightness  = common.graticuleBrightness;
    const float   scaleHeight = ImGui::GetTextLineHeight() + 4.0f;
    const ImRect  plot(ImVec2(content.Min.x, panel.Min.y + scaleHeight), ImVec2(content.Max.x, panel.Max.y));

    // 0/50/100% reference lines, the same three-line convention the other
    // scopes' graticules use.
    for (float frac : { 0.0f, 0.5f, 1.0f })
    {
        const float y = plot.Max.y - frac * plot.GetHeight();
        draw->AddLine(ImVec2(plot.Min.x, y), ImVec2(plot.Max.x, y),
                      GraticuleColor(frac == 0.0f ? kColGratMajor : kColGratMinor, brightness), 1.0f);
    }

    // One vertical line + size label per plotted band, smallest grain on the
    // left - a size axis reads left-to-right the way a ruler does, which is
    // the more direct reading and does not depend on knowing which end of a
    // spatial-frequency plot means "fine".
    ImVec2 points[kGrainBandCount];
    for (int band = 0; band < kGrainBandCount; ++band)
    {
        const float t = float(band) / float(kGrainBandCount - 1);
        const float x = plot.Min.x + t * plot.GetWidth();
        const float y = plot.Max.y - std::clamp(st.profile[band] * st.gain, 0.0f, 1.0f) * plot.GetHeight();
        points[band] = ImVec2(x, y);

        draw->AddLine(ImVec2(x, plot.Min.y), ImVec2(x, plot.Max.y),
                      GraticuleColor(kColGratMinor, brightness), 1.0f);

        char label[8];
        std::snprintf(label, sizeof(label), "%dpx", kGrainRadii[band + 1]);
        const ImVec2 size = ImGui::CalcTextSize(label);
        const float labelX = ImClamp(x - size.x * 0.5f, panel.Min.x + 2.0f, panel.Max.x - size.x - 2.0f);
        draw->AddText(ImVec2(labelX, panel.Min.y), GraticuleColor(kColHistogramScale, brightness), label);
    }

    // Filled area under the curve, then the outline and its own points on
    // top - DrawHistogramLane's "solid fill, crisp edge" look, at nine points
    // instead of 256 bins.
    const ImU32 lineCol = IM_COL32(90, 205, 255, 255);
    const ImU32 fillCol = IM_COL32(90, 205, 255, 46);
    for (int band = 0; band + 1 < kGrainBandCount; ++band)
    {
        draw->AddQuadFilled(points[band], points[band + 1],
                            ImVec2(points[band + 1].x, plot.Max.y), ImVec2(points[band].x, plot.Max.y),
                            fillCol);
    }
    draw->AddPolyline(points, kGrainBandCount, lineCol, ImDrawFlags_None, 1.5f);
    for (const ImVec2& p : points)
        draw->AddCircleFilled(p, 2.5f, lineCol);

    draw->PopClipRect();
    draw->AddRect(panel.Min, panel.Max, GraticuleColor(kColGraticule, brightness));

    if (DrawPanelCloseButton(panel, p_App.panels.size() > 1))
        p_App.closePanelIds.push_back(p_Panel.id);
}

void DrawGrainAnalyzerSettings(Panel& p_Panel)
{
    GrainAnalyzerState& st = p_Panel.grainAnalyzer;
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    char name[64];
    std::snprintf(name, sizeof(name), "Grain Analyzer Settings###settings%d", p_Panel.id);
    PositionSettingsWindow(common);

    if (!ImGui::Begin(name, &common.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    SettingsRowLabel("Gain");
    SettingsSliderFloat("##Gain", &st.gain, 1.0f, 100.0f, 25.0f, "%.1fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raw grain energy is tiny compared to the plot's own scale -\n"
                          "this is what actually makes two clips' grain look different\n"
                          "rather than both settling to the same normalised shape.");

    DrawSharedSettings(common, false, true);
    ImGui::End();
}

void DrawSourceSettings(App& p_App, Panel& p_Panel)
{
    PanelCommon& common = p_Panel.common;
    if (!common.showSettings) return;

    PositionSettingsWindow(common);

    if (!ImGui::Begin("Source Settings", &common.showSettings,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // Measured for the next open, so the placement above can keep the window
    // inside a panel too small to hold it at the default anchor.
    common.settingsSize = ImGui::GetWindowSize();

    // Unconditional, not just on first appearance: clicking back on the panel
    // that opened this (adjusting a scope's own controls while its Settings
    // window is up, say) raises that panel's display order the same way any
    // click does, and submission order alone - drawing Settings after every
    // panel, which is *already* the order below - only wins the very first
    // frame after that. Display order only (not FocusWindow), so this never
    // steals keyboard/nav focus away from whatever the user is actually
    // typing into.
    // Skipped while this window owns an open popup (a Combo's dropdown list,
    // say): that popup is itself the front-most window the instant it opens,
    // and unconditionally re-raising its *parent* every following frame would
    // shove the popup behind it again immediately - which is exactly what
    // broke every Combo in every Settings window (Mask Shape included) the
    // frame after this fix first landed.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // The mask itself is app-global (see App::spatialMask's own comment), but
    // its shape and invert flag are edited here, the same as the old app put
    // them on VideoPanel's own settings even though the geometry lived on
    // MainWindow.
    SourceState& st = p_Panel.source;
    SettingsRowLabel("Composition Grid");
    const char* gridNames[] = { "None", "Rule of Thirds", "Golden Ratio", "Center Cross",
                                "1.85:1 Crop", "2.39:1 Crop", "Fibonacci Spiral" };
    int gridIndex = (int)st.grid;
    if (ImGui::Combo("##compgrid", &gridIndex, gridNames, IM_ARRAYSIZE(gridNames)))
        st.grid = (CompositionGrid)gridIndex;

    if (st.grid == CompositionGrid::Fibonacci)
    {
        SettingsRowLabel("Spiral Corner");
        const char* cornerNames[] = { "Bottom Right", "Bottom Left", "Top Right", "Top Left" };
        int cornerIndex = (int)st.spiralCorner;
        if (ImGui::Combo("##spiralcorner", &cornerIndex, cornerNames, IM_ARRAYSIZE(cornerNames)))
            st.spiralCorner = (SpiralCorner)cornerIndex;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Which corner the spiral coils into. The four are mirrors\n"
                              "of one another, so the spiral winds the opposite way in\n"
                              "the left-hand pair.");
    }

    ImGui::Separator();

    SpatialMask& mask = p_App.spatialMask;
    SettingsRowLabel("Mask Shape");
    const char* shapeNames[] = { "Ellipse", "Rectangle", "Gradient" };
    int shapeIndex = (int)mask.shape;
    if (ImGui::Combo("##maskshape", &shapeIndex, shapeNames, IM_ARRAYSIZE(shapeNames)))
        mask.shape = (MaskShape)shapeIndex;

    if (mask.hasGeometry)
    {
        SettingsRowLabel("Invert Mask");
        ImGui::Checkbox("##maskinvert", &mask.invert);

        if (ImGui::Button("Clear Mask"))
        {
            mask.hasGeometry = false;
            mask.invert = false;
        }
    }
    else
    {
        ImGui::TextDisabled("Ctrl-drag on the image to draw a mask");
        ImGui::TextDisabled("(Shift-drag for an inverted one).");
    }

    ImGui::SeparatorText("Display");

    // Bounding lines are a waveform/histogram idea and mean nothing over a video
    // image, so this panel gets the shared rows it can actually use rather than
    // all of them.
    float brightnessPercent = common.graticuleBrightness * 100.0f;
    if (SettingsSliderFloat("Graticule", &brightnessPercent, 0.0f, 100.0f, 100.0f, "%.0f%%"))
        common.graticuleBrightness = brightnessPercent / 100.0f;

    SettingsRowLabel("Zoomable");
    ImGui::Checkbox("##zoomable", &common.zoomable);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------
//
// The old app's General -> Preferences window. Options whose subsystem does not
// exist in this build yet are shown disabled with a tooltip saying what they are
// waiting on, rather than hidden: the set of preferences is part of knowing what
// the app does, and a silently missing one reads as a regression.


void DrawPreferences(Preferences& p_Prefs)
{
    if (!p_Prefs.show) return;

    // Pinned to the main viewport, same as every per-panel Settings window
    // (see PositionSettingsWindow's own comment) - without this, ImGui's own
    // multi-viewport logic is free to decide this window doesn't fully
    // overlap whatever it's drawn over and give it a native OS window of its
    // own, titlebar and all, which is exactly what it did before this fix.
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &p_Prefs.show,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // Same reasoning as every DrawXSettings function: display order only, so
    // this stays on top of the panels without stealing keyboard/nav focus,
    // and skipped while a Combo of its own has a dropdown open (see that
    // fix's own comment for why re-raising the parent every frame otherwise
    // breaks the dropdown the very next frame).
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::SeparatorText("Rendering");

    ImGui::Checkbox("Disable Enhanced Render", &p_Prefs.disableEnhancedRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every scope opens in Enhanced Render by default, which costs\n"
                          "several times what the standard trace does. Turn this on to\n"
                          "fall back to the standard trace everywhere, regardless of what\n"
                          "each panel's own Enhanced Render checkbox says - an easy switch\n"
                          "for a lower-power machine, without hunting down every panel.");

    ImGui::SeparatorText("Host App");

    ImGui::Checkbox("Space Brings Resolve / Premiere to Focus", &p_Prefs.spaceFocusesResolve);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Neither host lets this app drive its transport directly (see\n"
                          "ERRORS.md in the old Python tree for what was tried with\n"
                          "Resolve and why it didn't work). This switches to the window of\n"
                          "whichever host is the input instead, so your next Space press\n"
                          "lands there and actually plays/pauses it - one extra keystroke\n"
                          "instead of a round trip to the taskbar.");

    ImGui::SeparatorText("Layout");

    ImGui::Checkbox("Remember Layout and Settings on Close", &p_Prefs.rememberLayoutOnClose);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Panel arrangement, window size and every scope setting are\n"
                          "restored on next launch.\n"
#ifdef _WIN32
                          "Stored in %%LOCALAPPDATA%%\\ScopeDeck\\layout.ini");
#else
                          "Stored in ~/Library/Application Support/Scope Deck/layout.ini");
#endif

    ImGui::Checkbox("Solo Fills the Screen", &p_Prefs.soloFullScreen);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Soloing a panel makes its window fill the whole monitor it is\n"
                          "on - no menu bar, status bar, tab or close button, over the\n"
                          "taskbar. Press Esc, or right-click -> Solo, to come back; the\n"
                          "window returns exactly where it was.");

    ImGui::SeparatorText("Subtitles");

    ImGui::Checkbox("Show Subtitles (from DaVinci Resolve, if present)", &p_Prefs.showSubtitles);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Burns the current timeline's active subtitle track text\n"
                          "over the Source panel's image, approximating Resolve's own\n"
                          "subtitle render (font/styling are not reproduced). Polled\n"
                          "from Resolve via its scripting API - needs Resolve Studio\n"
                          "with External Scripting on and a project open (the free\n"
                          "edition has no scripting; the installer provides the Python).");

    // Why the overlay is empty, when the bridge knows. Before this the box
    // sat ticked over a blank overlay on the free edition, with nothing
    // anywhere to say that Resolve's scripting was the missing piece.
    if (p_Prefs.showSubtitles)
    {
        const std::string subtitleStatus = SubtitleBridgeGetStatus();
        if (!subtitleStatus.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", subtitleStatus.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Styling is meaningless with the overlay off, and leaving it live would
    // invite adjusting controls whose effect cannot be seen.
    ImGui::BeginDisabled(!p_Prefs.showSubtitles);

    SettingsRowLabel("Size");
    SettingsSliderFloat("##subsize", &p_Prefs.subtitleSizePct, 2.0f, 15.0f, 5.0f, "%.1f%% of height");

    SettingsRowLabel("Style");
    const char* subStyles[] = { "Regular", "Bold", "Italic", "Bold Italic" };
    int subStyleIndex = (int)p_Prefs.subtitleStyle;
    if (ImGui::Combo("##substyle", &subStyleIndex, subStyles, IM_ARRAYSIZE(subStyles)))
        p_Prefs.subtitleStyle = (SubtitleStyle)subStyleIndex;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Real font faces, not synthesized emphasis. A style falls back\n"
                          "to Regular if this machine's font family has no such face -\n"
                          "several of the fallback families carry no italic at all.");

    SettingsRowLabel("Position");
    const char* subPositions[] = { "Bottom", "Top" };
    int subPositionIndex = (int)p_Prefs.subtitlePosition;
    if (ImGui::Combo("##subpos", &subPositionIndex, subPositions, IM_ARRAYSIZE(subPositions)))
        p_Prefs.subtitlePosition = (SubtitlePosition)subPositionIndex;

    SettingsRowLabel("Edge Margin");
    SettingsSliderFloat("##submargin", &p_Prefs.subtitleMarginPct, 0.0f, 40.0f, 5.0f, "%.1f%% of height");

    SettingsRowLabel("Text Color");
    ImGui::ColorEdit3("##subtextcol", p_Prefs.subtitleTextColor, ImGuiColorEditFlags_NoInputs);

    SettingsRowLabel("Background");
    const char* subBackdrops[] = { "None", "Box" };
    int subBackdropIndex = (int)p_Prefs.subtitleBackdrop;
    if (ImGui::Combo("##subbackdrop", &subBackdropIndex, subBackdrops, IM_ARRAYSIZE(subBackdrops)))
        p_Prefs.subtitleBackdrop = (SubtitleBackdrop)subBackdropIndex;

    if (p_Prefs.subtitleBackdrop == SubtitleBackdrop::Box)
    {
        SettingsRowLabel("Background Color");
        ImGui::ColorEdit3("##subboxcol", p_Prefs.subtitleBoxColor, ImGuiColorEditFlags_NoInputs);

        SettingsRowLabel("Background Opacity");
        SettingsSliderFloat("##subboxopacity", &p_Prefs.subtitleBoxOpacity, 0.0f, 100.0f, 63.0f, "%.0f%%");
    }

    SettingsRowLabel("Drop Shadow");
    ImGui::Checkbox("##subshadow", &p_Prefs.subtitleShadow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Worth turning on with Background set to None - without\n"
                          "either, white text disappears over a bright shot.");

    if (p_Prefs.subtitleShadow)
    {
        SettingsRowLabel("Shadow Distance");
        SettingsSliderFloat("##subshadowsize", &p_Prefs.subtitleShadowSize, 1.0f, 25.0f, 8.0f, "%.0f%% of text");
    }

    ImGui::EndDisabled();

    ImGui::SeparatorText("Not yet available in this build");

    // Everything below is disabled on purpose. Each names the piece it is waiting
    // on, so it is clear these are unbuilt rather than broken.
    ImGui::BeginDisabled();

    ImGui::Checkbox("Persist Grid View Reference Images", &p_Prefs.persistGridViewImages);

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputInt("Playback Frame Delay", &p_Prefs.playbackFrameDelay);

    ImGui::EndDisabled();

    ImGui::TextDisabled("Grid View is not built yet.");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Input source
// ---------------------------------------------------------------------------

// Resolve or Premiere. p_Host is never ScreenCapture.
void UseHostInput(App& p_App, InputSource p_Host)
{
    p_App.capture.Stop();
    p_App.prefs.hostInput = (p_Host == InputSource::Premiere) ? 1 : 0;
    p_App.input = p_Host;

    if (p_App.readerName != HostShmName(p_Host))
    {
        // The other host's block: RenderOneFrame reopens on the right name
        // this frame. Nothing to show until it does - the last frame was the
        // other host's picture.
        p_App.reader.Close();
        p_App.readerName.clear();
        p_App.lastOpenAttempt = -1.0;
        p_App.haveFrame = false;
        return;
    }

    // Same host as before Screen Capture: re-read its current slot now rather
    // than holding the last captured frame until it next renders - with the
    // playhead parked that could be never.
    p_App.haveFrame = p_App.reader.IsOpen() && p_App.reader.ReadLatest(p_App.frame, false);
}

void UseScreenCapture(App& p_App, const ScreenRegion& p_Region)
{
    if (!p_App.capture.Start(p_Region)) return;
    p_App.captureRegion = p_Region;
    p_App.input = InputSource::ScreenCapture;
    p_App.haveFrame = false;   // the first capture lands within a frame or two
}

void RequestRegionPick(App& p_App)
{
    // Three frames: the one being built still has the menu open, and DWM
    // shows what was presented a frame late.
    p_App.regionPickCountdown = 3;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void DrawStatusBar(App& p_App)
{
    const ScopeReader& r = p_App.reader;

    ImU32 colour = kColMuted;
    std::string text;

    if (p_App.input == InputSource::ScreenCapture)
    {
        ScreenCaptureStats stats;
        p_App.capture.Stats(stats);
        const ScreenRegion& region = p_App.capture.Region();

        char buf[256];
        if (!stats.error.empty())
        {
            std::snprintf(buf, sizeof(buf), "Screen Capture: %s", stats.error.c_str());
            colour = kColError;
        }
        else if (stats.frames == 0)
        {
            std::snprintf(buf, sizeof(buf), "Screen Capture starting...");
            colour = kColWaiting;
        }
        else
        {
            // grab and scopes are the capture thread's own costs - they set
            // its rate, not the UI's (the fps at the right).
            std::snprintf(buf, sizeof(buf),
                          "Screen Capture  |  %dx%d at (%d, %d)  |  %.0f fps  |  grab %.1f ms, scopes %.1f ms",
                          region.width, region.height, region.x, region.y,
                          stats.fps, stats.msGrab, stats.msScopes);
            colour = kColLive;
        }
        text = buf;
    }
    else switch (r.Status())
    {
        case ReaderStatus::Closed:
            colour = kColMuted;
            text = (p_App.input == InputSource::Premiere)
                ? "Waiting for Premiere Pro - turn on Scope Deck in Preferences > Playback > Video Device."
                : "Waiting for Scope Tap - start Resolve and add the tap to a node.";
            break;

        case ReaderStatus::NoPublisher:
            colour = kColWaiting;
            text = (p_App.input == InputSource::Premiere)
                ? "Connected, no frames yet - play or scrub a sequence in Premiere Pro."
                : "Connected, no frames yet - add Scope Tap to a node on the Color page.";
            break;

        case ReaderStatus::VersionMismatch:
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "Wire format mismatch: shared memory is v%u, this app reads v%u. "
#ifdef _WIN32
                          "The %s in Program Files is out of date.",
#else
                          "The installed %s is out of date.",
#endif
                          r.ShmVersion(), ScopeReader::ExpectedVersion(),
                          (p_App.input == InputSource::Premiere) ? "ScopeTransmit.prm" : "ScopeTap.ofx");
            colour = kColError;
            text = buf;
            break;
        }

        // Stale and Live share the same "<status> | WxH ColorSpace | frame N"
        // layout - only the leading word and colour change, so parking the
        // playhead doesn't also blank out the resolution/space/frame reading
        // that was on screen a moment ago. Stale only fires once a frame has
        // already arrived at least once (Closed/NoPublisher cover "never
        // has"), so p_App.frame is never stale garbage here.
        case ReaderStatus::Stale:
        case ReaderStatus::Live:
        {
            // frameIndex is a monotonic publish counter (see its own comment
            // in ScopeTypes.h) - it counts how many times the tap has
            // rendered, not where the playhead is, so it never goes
            // backwards even when scrubbing does. timelineTime is the OFX
            // effect time for this render call instead - genuinely the
            // timeline position, which is what this line is meant to show.
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%s  |  %ux%u  %s  |  frame %lld",
                          (r.Status() == ReaderStatus::Live) ? "Live" : "Idle",
                          p_App.frame.width, p_App.frame.height,
                          p_App.frame.SpaceName(),
                          (long long)std::llround(p_App.frame.timelineTime));
            colour = (r.Status() == ReaderStatus::Live) ? kColLive : kColMuted;
            text = buf;
            break;
        }
    }

    if (g_ProfileEnabled && g_EnhancedProfileLast.passes > 0)
    {
        char prof[160];
        std::snprintf(prof, sizeof(prof),
                      "  |  enhanced %.1f ms (walk %.1f / emit %.1f)  %llu seg  %llu vtx  x%d%s",
                      g_EnhancedProfileLast.ms,
                      g_EnhancedProfileLast.ms - g_EnhancedProfileLast.msEmit,
                      g_EnhancedProfileLast.msEmit,
                      static_cast<unsigned long long>(g_EnhancedProfileLast.segments),
                      static_cast<unsigned long long>(g_EnhancedProfileLast.vertices),
                      g_EnhancedProfileLast.passes,
                      g_EnhancedProfileLast.fallbacks > 0 ? "  FALLBACK" : "");
        text += prof;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colour));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
    ImGui::TextDisabled("%.0f fps", ImGui::GetIO().Framerate);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Rebuilds the whole style for a given DPI scale.
//
// Sizes and padding are ours to scale: ImGui's io.ConfigDpiScaleFonts handles
// fonts and explicitly does *not* touch metrics ("This will scale fonts but _NOT_
// scale sizes/padding for now").
//
// The style is reset to defaults and re-themed before scaling rather than scaled
// in place, because ScaleAllSizes multiplies what is already there - calling it
// twice compounds, so a window dragged 100% -> 150% -> 100% would end up at 1.5x
// forever rather than back where it started.
void ApplyDpiScale(float p_Scale)
{
    ImGuiStyle& style = ImGui::GetStyle();

    style = ImGuiStyle();
    ApplyTheme();
    style.ScaleAllSizes(p_Scale);

    // With viewports on, a panel dragged out becomes a real OS window - which is
    // what "Open in New Window" was in the old app. Those windows must not be
    // rounded or translucent differently from the docked ones, or they read as a
    // different app. Re-applied here because the reset above clears it.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
}

// ---------------------------------------------------------------------------
// Panel management
// ---------------------------------------------------------------------------

void ClearCentralNode(ImGuiID p_DockspaceId);   // defined with the layout helpers below

Panel* FindPanel(App& p_App, int p_Id)
{
    for (Panel& panel : p_App.panels)
        if (panel.id == p_Id) return &panel;
    return nullptr;
}

Panel& AddPanel(App& p_App, PanelKind p_Kind)
{
    Panel panel;
    panel.id   = p_App.nextPanelId++;
    panel.kind = p_Kind;
    p_App.panels.push_back(std::move(panel));
    return p_App.panels.back();
}

// The right-click menu, composed here rather than in PanelCommon because Content,
// Add Section and Solo all need the panel list.
void DrawPanelMenu(App& p_App, Panel& p_Panel)
{
    if (!BeginPanelContextMenu(p_Panel.common, p_Panel.common.panelRect))
        return;

    ImGui::TextDisabled("%s", PanelKindName(p_Panel.kind));
    ImGui::Separator();

    if (ImGui::MenuItem("Settings..."))
    {
        p_Panel.common.showSettings        = true;
        p_Panel.common.settingsPlaceFrames = 2;
    }

    ImGui::Separator();

    // Content: switch what this section shows, in place. The section keeps its
    // position in the dock tree, which is the whole point - it is the same piece
    // of screen, showing something else.
    if (ImGui::BeginMenu("Content"))
    {
        for (int k = 0; k < int(PanelKind::Count); ++k)
        {
            const PanelKind kind = PanelKind(k);
            if (ImGui::MenuItem(PanelKindName(kind), nullptr, p_Panel.kind == kind))
                p_Panel.kind = kind;
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Add Section"))
    {
        struct Entry { const char* label; AddDirection dir; };
        const Entry entries[4] = {
            { "Left",  AddDirection::Left  },
            { "Right", AddDirection::Right },
            { "Above", AddDirection::Above },
            { "Below", AddDirection::Below },
        };

        for (const Entry& e : entries)
        {
            if (!ImGui::MenuItem(e.label)) continue;

            // Recorded, not done here: splitting a dock node while inside the
            // window it is about to move is not something ImGui supports, and the
            // panel list must not grow while it is being iterated.
            p_App.pendingAdd.active = true;
            p_App.pendingAdd.fromId = p_Panel.id;
            p_App.pendingAdd.dir    = e.dir;
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    const bool soloed = (p_App.soloPanelId == p_Panel.id);
    if (ImGui::MenuItem("Solo", nullptr, soloed))
        p_App.soloPanelId = soloed ? -1 : p_Panel.id;

    ImGui::Separator();

    // Closing the last panel would leave a deck with no way to add one back, so
    // it is refused rather than allowed to strand the user.
    const bool canClose = p_App.panels.size() > 1;
    if (ImGui::MenuItem("Close Section", nullptr, false, canClose))
        p_App.closePanelIds.push_back(p_Panel.id);

    // Undocks into its own OS window - what dragging the tab out already does
    // (multi-viewport is on unconditionally, see main()'s ConfigFlags), just
    // reachable without a drag. Queued rather than applied immediately
    // (DockContextQueueUndockWindow, not the Process* variant) for the same
    // reason Add Section/Close are deferred to ApplyPendingAdd/Close: a dock
    // structural change is not safe from inside the window it changes, mid-
    // submission - this gets processed at the start of next frame instead,
    // the same path ImGui's own drag-to-undock gesture uses.
    char winName[64];
    p_Panel.MakeWindowName(winName, sizeof(winName));
    ImGuiWindow* panelWindow = ImGui::FindWindowByName(winName);
    const bool canUndock = panelWindow && panelWindow->DockNode != nullptr;
    if (ImGui::MenuItem("Open in New Window", nullptr, false, canUndock))
        ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), panelWindow);

    EndPanelContextMenu(p_Panel.common);
}

// Carries out an Add Section request recorded during the frame.
void ApplyPendingAdd(App& p_App, ImGuiID p_DockspaceId)
{
    if (!p_App.pendingAdd.active) return;
    p_App.pendingAdd.active = false;

    Panel* from = FindPanel(p_App, p_App.pendingAdd.fromId);
    if (!from) return;

    char fromName[64];
    from->MakeWindowName(fromName, sizeof(fromName));

    ImGuiWindow* fromWindow = ImGui::FindWindowByName(fromName);
    if (!fromWindow) return;

    // Split the node the source panel is docked in, not the dockspace root: "Add
    // Section Right" means right *of this section*, which is only the right of the
    // window when the section already spans it.
    ImGuiID targetNode = fromWindow->DockId;

    // finishRoot names whichever hierarchy DockBuilderFinish has to walk to
    // actually place the new window - normally that is the main dockspace,
    // but a genuinely standalone window (DockId == 0: popped out via "Open in
    // New Window", or just dragged off and never re-merged) has no node of
    // its own to split at all, and used to fall back to p_DockspaceId here -
    // which put the new section back in the *main* window instead of growing
    // the popped-out one, the opposite of what "Add Section" on it should do.
    // Fixed the same way ImGui's own docking demo bootstraps a fresh
    // dockspace: give the standalone window a plain (non-DockSpace) node of
    // its own first - "floating nodes... carry its own window" is exactly
    // this case, per DockBuilderDockWindow's own comment - which stays in
    // the same OS window/viewport it already is, then split *that*.
    ImGuiID finishRoot = p_DockspaceId;
    if (targetNode == 0)
    {
        targetNode = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodePos(targetNode, fromWindow->Pos);
        ImGui::DockBuilderSetNodeSize(targetNode, fromWindow->Size);
        ImGui::DockBuilderDockWindow(fromName, targetNode);
        finishRoot = targetNode;
    }

    ImGuiDir dir = ImGuiDir_Right;
    switch (p_App.pendingAdd.dir)
    {
        case AddDirection::Left:  dir = ImGuiDir_Left;  break;
        case AddDirection::Right: dir = ImGuiDir_Right; break;
        case AddDirection::Above: dir = ImGuiDir_Up;    break;
        case AddDirection::Below: dir = ImGuiDir_Down;  break;
    }

    ImGuiID newNode = 0, remainder = 0;
    ImGui::DockBuilderSplitNode(targetNode, dir, 0.5f, &newNode, &remainder);

    // The new section starts as a copy of the one it was added from - the same
    // content and the same settings. Adding a section next to a Waveform you have
    // set up is nearly always the first half of comparing two of them.
    Panel& added = AddPanel(p_App, from->kind);
    added.common.showSettings        = false;
    added.common.settingsPlaceFrames = 0;

    char addedName[64];
    added.MakeWindowName(addedName, sizeof(addedName));

    ImGui::DockBuilderDockWindow(addedName, newNode);
    ImGui::DockBuilderFinish(finishRoot);

    // Splitting a standalone window's freshly built node turns it from a
    // single window owning its platform viewport directly into a node with
    // two children, which needs a shared hidden "host" window to carry that
    // viewport instead - created as part of the DockBuilderFinish/next-frame
    // docking churn just queued above. That host window doesn't necessarily
    // come up front of the main OS window, which is what made the popped-out
    // window drop behind it the instant Add Section was used on it. Forcing
    // focus back onto the window the user was just adding a section to is
    // what actually raises it again: ImGui syncs the OS window front-most
    // (Platform_SetWindowFocus) whenever the focused window's viewport
    // changes over the course of a frame.
    if (finishRoot != p_DockspaceId)
        ImGui::FocusWindow(fromWindow);

    // Only meaningful for the main dockspace's own central node - a no-op
    // for a standalone window's freshly built node, which has none.
    ClearCentralNode(p_DockspaceId);
}

void ApplyPendingClose(App& p_App)
{
    if (p_App.closePanelIds.empty()) return;

    // Drains every id queued this frame, not just one: the native OS "X" on a
    // popped-out window fires PlatformRequestClose for every panel sharing that
    // viewport at once (see the panel window loop below), and a popped-out
    // window grown with Add Section can hold more than one.
    std::vector<int> ids;
    ids.swap(p_App.closePanelIds);

    for (int id : ids)
    {
        if (p_App.panels.size() <= 1) break;

        if (p_App.soloPanelId == id) p_App.soloPanelId = -1;

        for (size_t i = 0; i < p_App.panels.size(); ++i)
        {
            if (p_App.panels[i].id != id) continue;
            p_App.panels.erase(p_App.panels.begin() + long(i));
            break;
        }
    }
}

// Drops the dockspace's central node, so panels keep their proportions when the
// window is resized or maximised.
//
// This is ImGui's size-allocation policy, not our layout code. In
// DockNodeTreeUpdatePosSize, a split whose child contains the central node takes
// branch 3: the *other* child keeps its absolute SizeRef in pixels and the
// central node absorbs every pixel of growth. Only branch 4 - reached when
// neither child has a central node child - distributes by the ratio of the two
// SizeRefs, which is the behaviour a scope grid wants.
//
// Symptom without this: maximising left Source and Histogram pinned at their
// windowed pixel width while the Vectorscope (the node that happened to end up
// central) swallowed the whole screen.
//
// A dockspace has no need for a central node here - every region holds a real
// panel, and none of them is "the document area" the central node exists for.
void ClearCentralNode(ImGuiID p_DockspaceId)
{
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(p_DockspaceId);
    if (!root) return;

    if (ImGuiDockNode* central = root->CentralNode)
    {
        central->SetLocalFlags(central->LocalFlags & ~ImGuiDockNodeFlags_CentralNode);
        root->CentralNode = nullptr;
    }
}

// The default arrangement, built once. After that ImGui's own .ini owns it, which
// is also what gives "remember layout on close" for free.
void BuildDefaultLayout(App& p_App, ImGuiID p_DockspaceId)
{
    ImGui::DockBuilderRemoveNode(p_DockspaceId);
    ImGui::DockBuilderAddNode(p_DockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(p_DockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID left = 0, right = 0;
    ImGui::DockBuilderSplitNode(p_DockspaceId, ImGuiDir_Left, 0.5f, &left, &right);

    ImGuiID leftTop = 0, leftBottom = 0;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.5f, &leftTop, &leftBottom);

    ImGuiID rightTop = 0, rightBottom = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.5f, &rightTop, &rightBottom);

    // Docked by the panels' own window names, so the default layout follows the
    // list rather than a hardcoded set of four titles.
    const ImGuiID nodes[4] = { leftTop, rightTop, leftBottom, rightBottom };

    for (size_t i = 0; i < p_App.panels.size(); ++i)
    {
        char name[64];
        p_App.panels[i].MakeWindowName(name, sizeof(name));
        ImGui::DockBuilderDockWindow(name, nodes[i % 4]);
    }

    ImGui::DockBuilderFinish(p_DockspaceId);

    ClearCentralNode(p_DockspaceId);
}

// The name-entry popup for "Save Current Layout as Preset..." and the confirm
// popup for "Delete Preset", both queued from the General menu rather than
// acted on directly - a MenuItem click happens deep inside BeginMenu/EndMenu,
// and OpenPopup from there works, but it reads simpler to keep every one of
// this menu's side effects flowing through the same request/apply split the
// rest of the frame already uses.
void DrawLayoutPresetDialogs(App& p_App)
{
    if (p_App.showSavePresetDialog)
    {
        ImGui::OpenPopup("Save Layout Preset");
        p_App.showSavePresetDialog = false;
    }

    if (ImGui::BeginPopupModal("Save Layout Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        const bool enterPressed = ImGui::InputText("Preset name", p_App.savePresetNameBuf,
                                                    sizeof(p_App.savePresetNameBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);

        const bool canSave = p_App.savePresetNameBuf[0] != '\0';

        ImGui::BeginDisabled(!canSave);
        const bool savePressed = ImGui::Button("Save");
        ImGui::EndDisabled();

        ImGui::SameLine();
        const bool cancelPressed = ImGui::Button("Cancel");

        if (canSave && (savePressed || enterPressed))
        {
            SaveLayoutPreset(p_App.savePresetNameBuf);
            ImGui::CloseCurrentPopup();
        }
        else if (cancelPressed)
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!p_App.confirmDeletePreset.empty())
        ImGui::OpenPopup("Delete Preset?");

    if (ImGui::BeginPopupModal("Delete Preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete layout preset '%s'?", p_App.confirmDeletePreset.c_str());
        ImGui::Separator();

        if (ImGui::Button("Delete"))
        {
            DeleteLayoutPreset(p_App.confirmDeletePreset);
            p_App.confirmDeletePreset.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            p_App.confirmDeletePreset.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void DrawFrame(App& p_App)
{
    // Reset the Enhanced Render measurement for this frame - see EnhancedProfile.
    g_EnhancedProfileLast = g_EnhancedProfile;
    g_EnhancedProfile = EnhancedProfile{};
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // A full-screen Solo in the main window drops the menu bar and the status
    // line, leaving the dockspace - and so the one soloed panel - the whole
    // window. BeginMenuBar below simply returns false without the flag.
    const bool mainFullScreen = p_App.fullScreen.window && p_App.fullScreen.isMain;

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    if (!mainFullScreen) hostFlags |= ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##ScopeDeckHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("General"))
        {
            if (ImGui::MenuItem("Preferences...")) p_App.prefs.show = true;

            ImGui::Separator();

            if (ImGui::MenuItem("Reset Layout"))
            {
                p_App.layoutBuilt   = false;
                p_App.forceDefaultLayout = true;
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("Layout Presets"))
            {
                if (ImGui::MenuItem("Save Current Layout as Preset..."))
                {
                    p_App.savePresetNameBuf[0] = '\0';
                    p_App.showSavePresetDialog = true;
                }

                const std::vector<std::string> presets = ListLayoutPresets();

                if (!presets.empty())
                {
                    ImGui::Separator();
                    for (const std::string& name : presets)
                        if (ImGui::MenuItem(name.c_str()))
                            p_App.pendingLoadPreset = name;

                    ImGui::Separator();
                    if (ImGui::BeginMenu("Delete Preset"))
                    {
                        for (const std::string& name : presets)
                            if (ImGui::MenuItem(name.c_str()))
                                p_App.confirmDeletePreset = name;
                        ImGui::EndMenu();
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Quit"))
                glfwSetWindowShouldClose(glfwGetCurrentContext(), 1);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Input"))
        {
#if defined(_WIN32) || defined(__APPLE__)
            const bool canCapture = true;
#else
            const bool canCapture = false;   // GDI / ScreenCaptureKit only, see ScreenCapture.h
#endif
            const bool capturing = p_App.input == InputSource::ScreenCapture;
            const bool resolve   = p_App.input == InputSource::Resolve;
            const bool premiere  = p_App.input == InputSource::Premiere;

            if (ImGui::MenuItem("DaVinci Resolve (OFX)", nullptr, resolve) && !resolve)
                UseHostInput(p_App, InputSource::Resolve);

            if (ImGui::MenuItem("Adobe Premiere Pro", nullptr, premiere) && !premiere)
                UseHostInput(p_App, InputSource::Premiere);

            // Scope Tap keeps these as node parameters in Resolve; Premiere's
            // plugin has nowhere a user would find them, so they live here and
            // reach it through ScopeControl.h. Available whichever input is
            // selected, so they can be set before switching.
            if (ImGui::BeginMenu("Premiere Pro Options"))
            {
                ImGui::SeparatorText("Preview Scale");
                ImGui::TextDisabled("The picture sent to this app - the scopes\n"
                                    "always measure the frame Premiere renders.");
                struct ScaleOption { const char* label; int pct; };
                static const ScaleOption kScales[] = {
                    { "Auto (full size up to 1920 wide)", 0 },
                    { "100%", 100 }, { "75%", 75 }, { "50%", 50 }, { "25%", 25 },
                };
                for (const ScaleOption& option : kScales)
                    if (ImGui::MenuItem(option.label, nullptr, p_App.prefs.premierePreviewScale == option.pct))
                        p_App.prefs.premierePreviewScale = option.pct;

                ImGui::SeparatorText("Row Step");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderInt("##prRowStep", &p_App.prefs.premiereRowStep, 1, 16);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Analyse every Nth row. 1 reads every pixel; higher is faster\n"
                                      "but the scopes see fewer rows. Same as Scope Tap's Row Step.");

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Screen Capture", nullptr, capturing, canCapture) && !capturing)
            {
                // This session's region again if there is one; a first use
                // has nothing to capture until one is picked.
                if (p_App.captureRegion.IsValid())
                    UseScreenCapture(p_App, p_App.captureRegion);
                else
                    RequestRegionPick(p_App);
            }

            if (ImGui::MenuItem("Select Capture Region...", nullptr, false, canCapture))
                RequestRegionPick(p_App);

            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    DrawLayoutPresetDialogs(p_App);

    // Footer first, so the dockspace can claim exactly the rest and the status
    // line never gets pushed off the bottom by a tall panel.
    const float footerHeight = mainFullScreen ? 0.0f : ImGui::GetTextLineHeightWithSpacing() + 8.0f;

    const ImGuiID dockspaceId = ImGui::GetID("ScopeDeckDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -footerHeight), ImGuiDockNodeFlags_None);

    if (!p_App.layoutBuilt)
    {
        // Only build the default arrangement when there is nothing to restore.
        //
        // This is what "Remember Layout on Close" was missing: BuildDefaultLayout
        // starts with DockBuilderRemoveNode, so calling it unconditionally on
        // every launch tore down the layout ImGui had just loaded from the .ini
        // and rebuilt the default on top of it. The layout was being saved
        // correctly the whole time - it was overwritten a frame after being read.
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
        const bool restored = (node != nullptr && node->IsSplitNode());

        if (!restored || p_App.forceDefaultLayout)
            BuildDefaultLayout(p_App, dockspaceId);

        p_App.layoutBuilt        = true;
        p_App.forceDefaultLayout = false;
    }

    // Every frame, not just at build time: a layout restored from the .ini can
    // carry a saved central node, and re-docking a panel by hand can create a new
    // one. Either would quietly restore the absolute-size behaviour. It is two
    // pointer checks when there is nothing to clear.
    ClearCentralNode(dockspaceId);

    if (!mainFullScreen)
    {
        ImGui::Separator();
        DrawStatusBar(p_App);
    }

    ImGui::End();

    RebuildValueQualifier(p_App);
    EnsureMaskedScopeBins(p_App);

    // Solo is implemented by simply not submitting the other panels. ImGui hands a
    // dock node entirely to its one visible child and leaves the split ratios
    // alone, so turning Solo off restores the arrangement exactly - no saving and
    // reapplying a layout, and nothing to get out of step.
    for (Panel& panel : p_App.panels)
    {
        if (p_App.soloPanelId >= 0 && panel.id != p_App.soloPanelId) continue;

        char name[64];
        panel.MakeWindowName(name, sizeof(name));

        // NoTitleBar unconditionally: irrelevant while docked (the dock tab
        // bar is what's shown there regardless of this flag), and once
        // floating in its own OS window - "Open in New Window" or a plain
        // drag-out, both the same since ViewportsEnable is always on here -
        // it is what stops ImGui drawing a second titlebar under the native
        // OS one now that ConfigViewportsNoDecoration is off. This panel
        // never relied on ImGui's own titlebar for anything (its own
        // right-click menu and close button are drawn over the content
        // area, not the titlebar), so nothing here depended on having one.
        //
        // Timecode also skips scrollbars - a single centered readout has no
        // reason to scroll.
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
        if (panel.kind == PanelKind::Timecode)
            flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        // A real p_open, not nullptr: ImGui only acts on the native window's
        // own close button (PlatformRequestClose, e.g. clicking the OS X or
        // Alt-F4) when p_open is non-null - passing nullptr, as this did
        // before, is exactly why that close button did nothing. Routed into
        // the same closePanelId ApplyPendingClose already handles for the
        // in-canvas close button, so both close the same way and the "can't
        // close the last panel" guard there covers this too.
        // Full-screen Solo: no dock tab above the panel, no padding or border
        // around it, and no close button over it - nothing but the scope.
        // The tab bar comes back by itself on un-solo: a node re-derives its
        // flags from its windows' classes every frame.
        const bool bare = p_App.fullScreen.window && panel.id == p_App.soloPanelId;
        if (bare)
        {
            ImGuiWindowClass bareClass;
            bareClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
            ImGui::SetNextWindowClass(&bareClass);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        }

        bool panelOpen = true;
        ImGui::Begin(name, &panelOpen, flags);
        if (bare) ImGui::PopStyleVar(2);
        g_HidePanelChrome = bare;
        if (!panelOpen)
            p_App.closePanelIds.push_back(panel.id);

        // Recorded every frame so this panel's Settings window - drawn later,
        // in its own pass - knows which viewport to pin itself to (see
        // PositionSettingsWindow), whether that's the main window or this
        // panel's own popped-out one.
        panel.common.panelViewportId = ImGui::GetWindowViewport()->ID;

        switch (panel.kind)
        {
            case PanelKind::Source:      DrawSourcePanel(p_App, panel);      break;
            case PanelKind::Waveform:    DrawWaveformPanel(p_App, panel);    break;
            case PanelKind::Histogram:   DrawHistogramPanel(p_App, panel);   break;
            case PanelKind::Vectorscope: DrawVectorscopePanel(p_App, panel); break;
            case PanelKind::FalseColor:  DrawFalseColorPanel(p_App, panel);  break;
            case PanelKind::SkinTone:    DrawSkinTonePanel(p_App, panel);    break;
            case PanelKind::Noise:       DrawNoisePanel(p_App, panel);       break;
            case PanelKind::Timecode:    DrawTimecodePanel(p_App, panel);    break;
            case PanelKind::WhiteBalance: DrawWhiteBalancePanel(p_App, panel); break;
            case PanelKind::FocusPeaking:    DrawFocusPeakingPanel(p_App, panel);    break;
            case PanelKind::BandingAnalyzer: DrawBandingAnalyzerPanel(p_App, panel); break;
            case PanelKind::Difference:      DrawDifferencePanel(p_App, panel);      break;
            case PanelKind::GrainAnalyzer:   DrawGrainAnalyzerPanel(p_App, panel);   break;
            case PanelKind::Compare:         DrawComparePanel(p_App, panel);         break;
            case PanelKind::AudioMeter:      DrawAudioMeterPanel(p_App, panel);      break;
            case PanelKind::ColorCube:       DrawColorCubePanel(p_App, panel);       break;
            case PanelKind::Qualifier:       DrawQualifierPanel(p_App, panel);       break;
            case PanelKind::Chromaticity:    DrawChromaticityPanel(p_App, panel);    break;
            case PanelKind::LumaVsSat:       DrawLumaVsSatPanel(p_App, panel);       break;
            case PanelKind::HueVsSat:        DrawHueVsSatPanel(p_App, panel);        break;
            case PanelKind::Goniometer:      DrawGoniometerPanel(p_App, panel);      break;
            case PanelKind::SpectrumAnalyzer: DrawSpectrumAnalyzerPanel(p_App, panel); break;
            default: break;
        }
        DrawPanelMenu(p_App, panel);
        ImGui::End();
        g_HidePanelChrome = false;
    }

    // Settings windows last, so they float above the panels that opened them.
    for (Panel& panel : p_App.panels)
    {
        switch (panel.kind)
        {
            case PanelKind::Source:      DrawSourceSettings(p_App, panel);      break;
            case PanelKind::Waveform:    DrawWaveformSettings(panel);    break;
            case PanelKind::Histogram:   DrawHistogramSettings(panel);   break;
            case PanelKind::Vectorscope: DrawVectorscopeSettings(panel); break;
            case PanelKind::FalseColor:  DrawFalseColorSettings(panel);  break;
            case PanelKind::SkinTone:    DrawSkinToneSettings(panel);    break;
            case PanelKind::Noise:       DrawNoiseSettings(panel);       break;
            case PanelKind::Timecode:    DrawTimecodeSettings(panel);    break;
            case PanelKind::WhiteBalance: DrawWhiteBalanceSettings(panel); break;
            case PanelKind::FocusPeaking:    DrawFocusPeakingSettings(panel);       break;
            case PanelKind::BandingAnalyzer: DrawBandingAnalyzerSettings(panel);    break;
            case PanelKind::Difference:      DrawDifferenceSettings(p_App, panel);  break;
            case PanelKind::GrainAnalyzer:   DrawGrainAnalyzerSettings(panel);      break;
            case PanelKind::Compare:         DrawCompareSettings(p_App, panel);     break;
            case PanelKind::AudioMeter:      DrawAudioMeterSettings(panel);         break;
            case PanelKind::ColorCube:       DrawColorCubeSettings(panel);          break;
            case PanelKind::Qualifier:       DrawQualifierSettings(panel);          break;
            case PanelKind::Chromaticity:    DrawChromaticitySettings(panel);       break;
            case PanelKind::LumaVsSat:       DrawLumaVsSatSettings(panel);          break;
            case PanelKind::HueVsSat:        DrawHueVsSatSettings(panel);           break;
            case PanelKind::Goniometer:      DrawGoniometerSettings(panel);          break;
            case PanelKind::SpectrumAnalyzer: DrawSpectrumAnalyzerSettings(panel);   break;
            default: break;
        }
    }

    DrawPreferences(p_App.prefs);

    // Structural changes happen here, after every panel has been submitted: a dock
    // node cannot be split from inside the window it holds, and the list must not
    // grow or shrink while it is being iterated.
    ApplyPendingAdd(p_App, dockspaceId);
    ApplyPendingClose(p_App);
    ApplyPendingLoadPreset(p_App);
}

void GlfwErrorCallback(int p_Error, const char* p_Description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", p_Error, p_Description);
}

#ifdef _WIN32
// Space Brings Resolve to Focus (Preferences). Resolve's scripting API has no
// play/pause - there is no clean way for this app to drive its transport
// directly (see ERRORS.md in the old Python tree: a key posted to Resolve's
// window while it isn't the active one is dropped by Qt's focus routing, and
// SendInput after forcing it foreground was itself blocked, most likely by
// UIPI since Resolve typically runs at a different integrity level). This
// does only the part that actually works: switching which window is in
// front, so the user's own next Space press lands on Resolve instead of here.
// Premiere gets the same treatment when it is the input.
//
// Window enumeration is safe here, unlike on AudioMeterBridge's thread: this
// runs on the UI thread, which answers its own windows' messages directly.

struct FindResolveWindowData
{
    HWND        found = nullptr;
    const char* exe   = "Resolve.exe";
};

BOOL CALLBACK FindResolveWindowProc(HWND p_Hwnd, LPARAM p_LParam)
{
    FindResolveWindowData& data = *reinterpret_cast<FindResolveWindowData*>(p_LParam);

    if (!IsWindowVisible(p_Hwnd)) return TRUE;

    // Skips Resolve's own tool/popup windows (which carry no title), leaving
    // only its main frame - and skips matching by title at all, which
    // false-positived on an unrelated Explorer window the one time this was
    // tried before (see ERRORS.md).
    if (GetWindowTextLengthW(p_Hwnd) == 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(p_Hwnd, &pid);
    if (pid == 0) return TRUE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;

    char imagePath[MAX_PATH] = {};
    DWORD imagePathLen = MAX_PATH;
    const bool ok = QueryFullProcessImageNameA(process, 0, imagePath, &imagePathLen);
    CloseHandle(process);
    if (!ok) return TRUE;

    const std::string path(imagePath);
    const size_t slash = path.find_last_of("\\/");
    const std::string exeName = (slash == std::string::npos) ? path : path.substr(slash + 1);

    if (_stricmp(exeName.c_str(), data.exe) == 0)
    {
        data.found = p_Hwnd;
        return FALSE;   // stop enumerating - found it
    }
    return TRUE;
}

void FocusHostWindow(InputSource p_Host)
{
    FindResolveWindowData data;
    if (p_Host == InputSource::Premiere) data.exe = "Adobe Premiere Pro.exe";
    EnumWindows(FindResolveWindowProc, reinterpret_cast<LPARAM>(&data));
    if (!data.found) return;

    if (IsIconic(data.found))
        ShowWindow(data.found, SW_RESTORE);

    SetForegroundWindow(data.found);
}
#elif defined(__APPLE__)
// The same job through NSRunningApplication, by bundle id - no window
// enumeration needed, Cocoa knows which app is which. See MacPlatform.mm.
void FocusHostWindow(InputSource p_Host)
{
    mac::ActivateApp(p_Host == InputSource::Premiere ? mac::kPremiereBundleId : mac::kResolveBundleId);
}
#endif

// ---------------------------------------------------------------------------
// Solo Fills the Screen
// ---------------------------------------------------------------------------
//
// Works on whichever OS window the soloed panel is in - the main window, or a
// popped-out one - because ImGui follows its platform windows: moving or
// resizing a viewport's GLFW window reaches ImGui through the backend's
// position and size callbacks, the same way a drag by the user does. Borderless
// and covering the whole monitor rather than GLFW's exclusive full screen: no
// video mode change, and alt-tab behaves like any other window.

// A window pointer is only safe to touch while it still exists: hiding the
// other panels can destroy a popped-out viewport's window outright.
bool IsLiveWindow(GLFWwindow* p_Window, GLFWwindow* p_Main)
{
    if (!p_Window) return false;
    if (p_Window == p_Main) return true;
    for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports)
        if (vp->PlatformHandle == p_Window) return true;
    return false;
}

// The geometry half of the switch, done while the window is faded out - see
// UpdateSoloFullScreen for the fade around it.
#ifdef _WIN32

// Windows' own minimize/maximize/restore animations. Off for the switch; see
// EnterBorderless.
void SetWindowTransitions(GLFWwindow* p_Window, bool p_Enabled)
{
    HWND hwnd = glfwGetWin32Window(p_Window);
    if (!hwnd) return;
    const BOOL disabled = p_Enabled ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
}

// The standard borderless toggle: strip the caption and frame and cover the
// monitor in ONE SetWindowPos, and come back by restoring the saved style and
// WINDOWPLACEMENT. A maximized window stays maximized throughout - it is never
// restored to be resized and re-maximized afterwards, which is what played
// Windows' maximize animation on the way back and made leaving full screen
// look like the window being un-minimized.
bool EnterBorderless(App::FullScreenSolo& p_Fs, GLFWwindow* p_Window)
{
    HWND hwnd = glfwGetWin32Window(p_Window);
    if (!hwnd) return false;

    p_Fs.placement = WINDOWPLACEMENT{};
    p_Fs.placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &p_Fs.placement)) return false;
    p_Fs.style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const RECT& r = monitor.rcMonitor;   // the whole monitor, not its work area: over the taskbar

    SetWindowTransitions(p_Window, false);
    SetWindowLongPtrW(hwnd, GWL_STYLE, p_Fs.style & ~(WS_CAPTION | WS_THICKFRAME));

    // One row taller than the monitor. A window exactly covering it can be
    // promoted by the GPU driver to a full-screen present - NVIDIA's OpenGL
    // driver does this - which blanks the display for a moment like a mode
    // change. One row past the edge still covers the monitor (so the taskbar
    // still stays behind it) without matching it exactly.
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, (r.bottom - r.top) + 1,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    p_Fs.window = p_Window;
    return true;
}

void LeaveBorderless(App::FullScreenSolo& p_Fs)
{
    HWND hwnd = glfwGetWin32Window(p_Fs.window);
    if (!hwnd) return;
    SetWindowTransitions(p_Fs.window, false);
    SetWindowLongPtrW(hwnd, GWL_STYLE, p_Fs.style);
    SetWindowPlacement(hwnd, &p_Fs.placement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

#else

void SetWindowTransitions(GLFWwindow*, bool) {}

// No platform window API to lean on: GLFW's own calls, monitor by the
// window's centre.
bool EnterBorderless(App::FullScreenSolo& p_Fs, GLFWwindow* p_Window)
{
    p_Fs.decorated = glfwGetWindowAttrib(p_Window, GLFW_DECORATED) == GLFW_TRUE;
    p_Fs.maximized = glfwGetWindowAttrib(p_Window, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (p_Fs.maximized) glfwRestoreWindow(p_Window);
    glfwGetWindowPos(p_Window, &p_Fs.x, &p_Fs.y);
    glfwGetWindowSize(p_Window, &p_Fs.width, &p_Fs.height);

    const int cx = p_Fs.x + p_Fs.width / 2, cy = p_Fs.y + p_Fs.height / 2;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* target = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; ++i)
    {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (mode && cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height) { target = monitors[i]; break; }
    }
    int mx = 0, my = 0;
    glfwGetMonitorPos(target, &mx, &my);
    const GLFWvidmode* mode = glfwGetVideoMode(target);
    if (!mode) return false;

    glfwSetWindowAttrib(p_Window, GLFW_DECORATED, GLFW_FALSE);
    glfwSetWindowPos(p_Window, mx, my);
    glfwSetWindowSize(p_Window, mode->width, mode->height);
    p_Fs.window = p_Window;
    return true;
}

void LeaveBorderless(App::FullScreenSolo& p_Fs)
{
    glfwSetWindowAttrib(p_Fs.window, GLFW_DECORATED, p_Fs.decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwSetWindowMonitor(p_Fs.window, nullptr, p_Fs.x, p_Fs.y, p_Fs.width, p_Fs.height, GLFW_DONT_CARE);
    if (p_Fs.maximized) glfwMaximizeWindow(p_Fs.window);
}

#endif

// Runs between frames. Each switch is a short cross-fade rather than a cut:
//
//   FadeOut  the window as it is now, ~0.1 s
//   Hold     switched while invisible, and two frames drawn at the new size
//   FadeIn   the window as it now is, ~0.15 s
//
// The hold is what hides the black: a window resized before anything is
// drawn into it shows black until the next frame lands. Faded out, nobody
// sees it - what fades in is already the new picture.
void UpdateSoloFullScreen(App& p_App, GLFWwindow* p_Main)
{
    constexpr double kFadeOutSeconds = 0.09;
    constexpr double kFadeInSeconds  = 0.15;

    // Which window should be full screen: the soloed panel's, as of the last
    // frame it was drawn in.
    GLFWwindow* want = nullptr;
    if (p_App.prefs.soloFullScreen && p_App.soloPanelId >= 0)
    {
        if (Panel* panel = FindPanel(p_App, p_App.soloPanelId))
            if (ImGuiViewport* vp = ImGui::FindViewportByID(panel->common.panelViewportId))
                want = static_cast<GLFWwindow*>(vp->PlatformHandle);
    }

    using Phase = App::FullScreenSolo::Phase;
    App::FullScreenSolo& fs = p_App.fullScreen;
    const double now = NowSeconds();

    // A popped-out window can be destroyed mid-way (its panel re-docked):
    // forget it rather than touch it.
    if (fs.window && !IsLiveWindow(fs.window, p_Main)) { fs.window = nullptr; fs.isMain = false; }
    if (fs.fading && !IsLiveWindow(fs.fading, p_Main)) fs.fading = nullptr;

    const auto ease = [](double t) { t = std::clamp(t, 0.0, 1.0); return float(t * t * (3.0 - 2.0 * t)); };

    switch (fs.phase)
    {
        case Phase::Idle:
            if (want == fs.window) return;
            fs.fading     = fs.window ? fs.window : want;   // what the user is looking at changing
            fs.phase      = Phase::FadeOut;
            fs.phaseStart = now;
            return;

        case Phase::FadeOut:
        {
            const double t = (now - fs.phaseStart) / kFadeOutSeconds;
            if (fs.fading) glfwSetWindowOpacity(fs.fading, 1.0f - ease(t));
            if (t < 1.0) return;

            // Invisible: switch, unless Solo was toggled straight back during
            // the fade, in which case there is nothing to change.
            GLFWwindow* previous = fs.window;
            if (want != fs.window)
            {
                if (fs.window) LeaveBorderless(fs);
                fs.window = nullptr;
                fs.isMain = false;
                if (want && EnterBorderless(fs, want)) fs.isMain = (want == p_Main);
            }

            // Fade back in on whichever window now shows the change - the
            // full-screen one, or the one just put back. If that is not the
            // one faded out (moving straight between two windows), the other
            // is simply made visible again.
            GLFWwindow* shown = fs.window ? fs.window : previous;
            if (fs.fading && fs.fading != shown) glfwSetWindowOpacity(fs.fading, 1.0f);
            if (previous && previous != shown) SetWindowTransitions(previous, true);
            fs.fading = shown;
            if (fs.fading) glfwSetWindowOpacity(fs.fading, 0.0f);

            fs.phase      = Phase::Hold;
            fs.holdFrames = 2;
            return;
        }

        case Phase::Hold:
            if (--fs.holdFrames > 0) return;
            fs.phase      = Phase::FadeIn;
            fs.phaseStart = now;
            return;

        case Phase::FadeIn:
        {
            const double t = (now - fs.phaseStart) / kFadeInSeconds;
            if (fs.fading) glfwSetWindowOpacity(fs.fading, ease(t));
            if (t < 1.0) return;

            if (fs.fading)
            {
                glfwSetWindowOpacity(fs.fading, 1.0f);   // also drops the layered style fading needs
                SetWindowTransitions(fs.fading, true);
            }
            fs.fading = nullptr;
            fs.phase  = Phase::Idle;
            return;
        }
    }
}

// Bundles what a single frame needs so it can be called from both the main
// loop and the refresh callback below (see RenderOneFrame).
struct RenderContext
{
    App*  app            = nullptr;
    float forcedDpiScale = 0.0f;
    float appliedDpiScale = 1.0f;
#ifdef __APPLE__
    bool  mainWindowPlaced = false;   // see PlaceMainWindow
#endif
};

#ifdef __APPLE__
// The main window on macOS: put where it was last time, once layout.ini has
// been read, and its position recorded every frame after that so the next
// launch can do the same.
//
// Why macOS remembers the window and Windows launches maximized: on a
// one-display Mac a maximized Scope Deck covers DaVinci Resolve completely,
// and macOS then treats the fully hidden Resolve as idle (App Nap) - its
// playback free-runs with no sound until any part of its window shows
// again. Measured on a MacBook with Resolve 21: the moment Resolve's
// viewer is partly visible, playback is normal. So the Mac build opens at
// three quarters of the display, where Resolve's viewer can stay in view
// beside it, and then keeps whatever arrangement the user settles on.
//
// Runs after ImGui::NewFrame, because the first NewFrame is what reads the
// .ini - the window is created hidden in main() and shown here, so it never
// appears at the default size and then jumps.
bool MainWindowRectOnScreen(const App::MainWindowGeometry& p_G)
{
    const int cx = p_G.x + p_G.width / 2, cy = p_G.y + p_G.height / 2;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i)
    {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) return true;
    }
    return false;
}

void PlaceMainWindow(App& p_App, GLFWwindow* p_Window, RenderContext& p_Ctx)
{
    App::MainWindowGeometry& g = p_App.mainWindow;

    if (!p_Ctx.mainWindowPlaced)
    {
        p_Ctx.mainWindowPlaced = true;
        // A saved rectangle whose centre is on no current display (an
        // external monitor since unplugged) is ignored; the default placement
        // from main() stands.
        if (g.saved && g.width >= 320 && g.height >= 240 && MainWindowRectOnScreen(g))
        {
            glfwSetWindowPos(p_Window, g.x, g.y);
            glfwSetWindowSize(p_Window, g.width, g.height);
            if (g.maximized) glfwMaximizeWindow(p_Window);
        }
        glfwShowWindow(p_Window);
        return;
    }

    // Not while Solo Fills the Screen has the window borderless, or while
    // it is minimised: neither is a size worth coming back to.
    if (p_App.fullScreen.window == p_Window || p_App.fullScreen.phase != App::FullScreenSolo::Phase::Idle) return;
    if (glfwGetWindowAttrib(p_Window, GLFW_ICONIFIED) == GLFW_TRUE) return;

    const bool maximized = glfwGetWindowAttrib(p_Window, GLFW_MAXIMIZED) == GLFW_TRUE;
    g.maximized = maximized;
    if (!maximized)
    {
        glfwGetWindowPos(p_Window, &g.x, &g.y);
        glfwGetWindowSize(p_Window, &g.width, &g.height);
    }
    g.saved = true;   // MarkSettingsDirtyIfChanged notices any change and schedules the save
}
#endif

// The whole body of a frame, pulled out of main()'s loop so it can also run
// from glfwSetWindowRefreshCallback. On Windows, dragging a window's edge
// enters Win32's own modal SC_SIZE message loop inside glfwPollEvents() -
// the app's while loop does not get control back until the mouse is
// released, so without this the framebuffer just sits at its pre-drag
// content while the OS-drawn window border grows around it, live only once
// you let go. GLFW's refresh callback fires synchronously from inside that
// same modal loop on every intermediate size, which is exactly what makes
// the resize track the cursor instead of jumping once at the end.
#ifdef __APPLE__
// Hold the frame rate to the display's. glfwSwapInterval(1) does not do it on
// this platform: measured on an M1 Air's 60 Hz panel, the app ran at 84 fps
// and about 70% of a core redrawing an unchanged picture - the same sort of
// waste the Windows path cured with DwmFlush (see g_GlVsync). There is no
// compositor fence to wait on here, so the wait is a clock: each frame is
// due one refresh interval after the last, and a frame that finishes early
// sleeps until then. A frame that runs long resets the schedule rather than
// trying to catch up, so a slow frame costs one frame and not a burst.
void PaceToDisplay()
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_Next = Clock::now();

    int hz = 0;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) hz = mode->refreshRate;
    if (hz <= 0) hz = 60;
    const auto interval = std::chrono::nanoseconds(1000000000LL / hz);

    const Clock::time_point now = Clock::now();
    if (now < s_Next) std::this_thread::sleep_until(s_Next);
    else              s_Next = now;
    s_Next += interval;
}
#endif

void RenderOneFrame(GLFWwindow* p_Window, RenderContext& p_Ctx)
{
    App& app = *p_Ctx.app;
    ImGuiIO& io = ImGui::GetIO();

    bool anyTimecode = false;
    for (const Panel& panel : app.panels) {
        if (panel.kind == PanelKind::Timecode) {
            anyTimecode = true;
            break;
        }
    }
    // Only worth polling Resolve for subtitle cues while the preference is
    // on AND there's a Source panel open to actually burn them onto - an
    // idle app with the checkbox checked shouldn't keep shelling out for
    // nothing.
    bool anySource = false;
    for (const Panel& panel : app.panels) {
        if (panel.kind == PanelKind::Source) {
            anySource = true;
            break;
        }
    }
    // Both read Resolve's timeline, which means nothing for a screen capture:
    // its timelineTime is a capture counter, not a timeline position.
    const bool fromResolve = app.input == InputSource::Resolve;
    SubtitleBridgeSetActive(app.prefs.showSubtitles && anySource && fromResolve);

    TimecodeBridgeSetActive(anyTimecode && fromResolve);
    if (app.haveFrame)
        TimecodeBridgeNotifyFrame(app.frame.timelineTime);

    // One capture stream feeds the Audio Meter, Goniometer and Spectrum
    // Analyzer panels alike - active if any is open.
    bool anyAudioCapture = false;
    for (const Panel& panel : app.panels) {
        if (panel.kind == PanelKind::AudioMeter || panel.kind == PanelKind::Goniometer
            || panel.kind == PanelKind::SpectrumAnalyzer) {
            anyAudioCapture = true;
            break;
        }
    }
    AudioMeterBridgeSetActive(anyAudioCapture);

    // The host's audio, whichever host that is - kept on it through Screen
    // Capture too, which has no audio of its own.
    AudioMeterBridgeSetTarget(HostFromPrefs(app.prefs.hostInput) == InputSource::Premiere
                                  ? AudioTarget::Premiere : AudioTarget::Resolve);

    // GLFW already does the window-side work: its WM_DPICHANGED handler
    // resizes the window to the rect Windows suggests and updates the content
    // scale. All that is left is to rebuild the style metrics to match.
    //
    // Done here, between frames, because ApplyDpiScale() replaces the whole
    // ImGuiStyle - doing that mid-frame would change padding under widgets
    // that had already been laid out against the old values.
    {
        float scaleX = 1.0f;
#ifndef __APPLE__
        float scaleY = 1.0f;
        glfwGetWindowContentScale(p_Window, &scaleX, &scaleY);
#else
        // On macOS the content scale (2.0 on a Retina display) is already
        // spent on the framebuffer: GLFW sizes windows in points, ImGui lays
        // out in points and its GLFW backend reports a DPI scale of 1.0 for
        // every viewport (see ImGui_ImplGlfw_GetContentScaleForWindow), with
        // the framebuffer scale making the result sharp. Scaling the style by
        // it as well would double every size a second time.
        (void)p_Window;
#endif

        if (p_Ctx.forcedDpiScale > 0.0f) scaleX = p_Ctx.forcedDpiScale;

        if (scaleX > 0.0f && std::fabs(scaleX - p_Ctx.appliedDpiScale) > 0.01f)
        {
            ApplyDpiScale(scaleX);
            p_Ctx.appliedDpiScale = scaleX;
        }

        // Normally io.ConfigDpiScaleFonts keeps FontScaleDpi in step with the
        // viewport's own scale, which is what makes a panel dragged to another
        // monitor rasterise for that monitor. Under --dpi there is no real
        // viewport change to react to, and ImGui rewrites FontScaleDpi from the
        // true monitor scale on every SetCurrentViewport - so the override has
        // to be re-applied each frame or the metrics would scale and the text
        // would not.
        if (p_Ctx.forcedDpiScale > 0.0f)
            ImGui::GetStyle().FontScaleDpi = p_Ctx.forcedDpiScale;
    }

    // The host follows prefs.hostInput, which the .ini can change on the first
    // frame - after the reader was already opened on the default.
    const InputSource host = HostFromPrefs(app.prefs.hostInput);
    if (app.input != InputSource::ScreenCapture && app.input != host)
        UseHostInput(app, host);

    // Retry the mapping on a slow timer rather than every frame: the host is
    // usually started after the app, and OpenFileMapping on a name that does
    // not exist is a syscall we would otherwise make 60 times a second.
    const char* wantedBlock = HostShmName(host);
    if (app.reader.IsOpen() && app.readerName != wantedBlock)
    {
        app.reader.Close();
        app.readerName.clear();
        app.lastOpenAttempt = -1.0;
    }
    // GetFrameCount() is 0 until the first NewFrame, which is when the .ini -
    // and with it the saved host - is read. See main().
    const bool settingsLoaded = ImGui::GetFrameCount() > 0;

    // Premiere's Preview Scale and Row Step, written every frame rather than
    // on change: two atomic stores, and a plugin that attaches later - or a
    // block recreated after both sides closed it - is never left on stale
    // values. Created only once the saved values are loaded, for the same
    // reason as the reader below.
    if (settingsLoaded)
    {
        if (!app.premiereControl.Block())
            app.premiereControl.Create(kControlNamePremiere);
        if (ControlBlock* block = app.premiereControl.Block())
        {
            block->previewScalePct.store(static_cast<uint32_t>(std::max(app.prefs.premierePreviewScale, 0)),
                                         std::memory_order_relaxed);
            block->rowStep.store(static_cast<uint32_t>(std::clamp(app.prefs.premiereRowStep, 1, 16)),
                                 std::memory_order_relaxed);
        }
    }
    if (!app.reader.IsOpen() && settingsLoaded)
    {
        const double now = NowSeconds();
        if (now - app.lastOpenAttempt > 1.0)
        {
            app.lastOpenAttempt = now;
            if (app.reader.Open(wantedBlock))
                app.readerName = wantedBlock;
        }
    }

    // Only pay for the trace rows while something is drawing them. Asked every
    // frame rather than on the toggle, so a second Enhanced Render panel later
    // cannot switch it off underneath the first. One line per caller of
    // PrepareEnhancedTraceCached - a panel missing here silently draws its
    // density image instead, which is how White Balance went unnoticed.
    bool wantTrace = false;
    for (const Panel& panel : app.panels)
    {
        if (panel.kind == PanelKind::Waveform     && panel.waveform.enhancedRender)     wantTrace = true;
        if (panel.kind == PanelKind::Vectorscope  && panel.vectorscope.enhancedRender)  wantTrace = true;
        if (panel.kind == PanelKind::WhiteBalance && panel.whiteBalance.enhancedRender) wantTrace = true;
    }
    app.reader.SetWantWaveformTrace(wantTrace);

    // Between frames, never inside one: the picker runs its own message loop
    // and would otherwise freeze a half-built ImGui frame.
    if (app.regionPickCountdown > 0 && --app.regionPickCountdown == 0)
    {
        ScreenRegion region;
        if (PickScreenRegion(region))
            UseScreenCapture(app, region);
    }

    // Between frames too: it moves and resizes OS windows, which ImGui then
    // picks up at the next NewFrame.
    UpdateSoloFullScreen(app, p_Window);

    if (app.input == InputSource::ScreenCapture)
    {
        if (app.capture.TakeLatest(app.frame))
            app.haveFrame = true;
    }
    else if (app.reader.ReadLatest(app.frame))
    {
        app.haveFrame = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

#ifdef __APPLE__
    PlaceMainWindow(app, p_Window, p_Ctx);
#endif

    // Esc leaves a full-screen Solo: with no menu bar or tab there is nothing
    // else on screen to click, and the right-click menu is easy to miss.
    // Checked after NewFrame for the same reason as Space below.
    if (app.fullScreen.window && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        app.soloPanelId = -1;

#if defined(_WIN32) || defined(__APPLE__)
    // Checked here, once per frame right after NewFrame settles this frame's
    // input edge-detection - not earlier, or IsKeyPressed would still be
    // reading last frame's state. WantTextInput (not WantCaptureKeyboard, a
    // much broader flag) is the one guard that actually matters: this should
    // still fire while hovering an ordinary widget, only not steal the space
    // bar out from under someone actively typing.
    if (app.prefs.spaceFocusesResolve && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        FocusHostWindow(HostFromPrefs(app.prefs.hostInput));
    }
#endif

    DrawFrame(app);

    // Panels change settings through ordinary widgets, so nothing marks the
    // file dirty on its own.
    MarkSettingsDirtyIfChanged();

    ImGui::Render();

    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(p_Window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(kColBackground);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
    glfwSwapBuffers(p_Window);
#ifdef _WIN32
    // Pace on the compositor's vblank rather than in the driver - see g_GlVsync.
    if (!g_GlVsync) DwmFlush();
#elif defined(__APPLE__)
    PaceToDisplay();
#endif
}

void WindowRefreshCallback(GLFWwindow* p_Window)
{
    if (auto* ctx = static_cast<RenderContext*>(glfwGetWindowUserPointer(p_Window)))
        RenderOneFrame(p_Window, *ctx);
}

} // namespace

int main(int argc, char** argv)
{
    // --dpi=<scale> overrides the monitor's reported scale.
    //
    // DPI-change handling is otherwise untestable on a machine whose displays are
    // all at the same scale - the common case, and exactly the case where this
    // code rots unnoticed. A flag rather than an environment variable because it
    // survives however the app is launched.
    float       forcedDpiScale = 0.0f;
    const char* settingsOverride = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--dpi=", 6) == 0)
        {
            const float value = float(std::atof(argv[i] + 6));
            if (value > 0.1f && value < 8.0f) forcedDpiScale = value;
            continue;
        }

        // --settings=<path> puts layout and panel settings in a file of the
        // caller's choosing instead of the user's own.
        //
        // This exists because testing must never touch a real user's settings.
        // Launching the app to check a change rewrites layout.ini with whatever
        // that build's defaults are, and clearing the file to test a fresh start
        // throws the user's setup away for good. A separate file makes that
        // mistake impossible rather than something to remember not to do.
        if (std::strncmp(argv[i], "--settings=", 11) == 0)
        {
            settingsOverride = argv[i] + 11;
            continue;
        }
    }

#ifdef _WIN32
    // Declare per-monitor DPI awareness before anything creates a window.
    //
    // Without it Windows treats the process as 96 DPI and bitmap-scales the whole
    // window: on a 150% display the app believes it has more pixels than it is
    // given, so panels overflow the frame and every glyph is resampled to mush.
    // A scope deck is read for fine detail against a graticule, so a blurred
    // trace is not a cosmetic problem - it is the product failing at its job.
    //
    // Loaded dynamically rather than linked: SetProcessDpiAwarenessContext needs
    // Windows 10 1703, and a missing export should degrade to the old behaviour
    // rather than refuse to start.
    if (HMODULE user32 = ::LoadLibraryA("user32.dll"))
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (auto setCtx = reinterpret_cast<SetCtxFn>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
        {
            // -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, spelled as the
            // literal the SDK macro expands to so this needs no extra header.
            setCtx(reinterpret_cast<HANDLE>(-4));
        }
        else if (auto setAware = reinterpret_cast<BOOL(WINAPI*)()>(
                     ::GetProcAddress(user32, "SetProcessDPIAware")))
        {
            setAware();
        }
    }
#endif

#ifndef _WIN32
    // A Python worker that has just died turns the next write to its stdin
    // into SIGPIPE, which would kill the app outright. Ignored, the write
    // fails with EPIPE and the bridge relaunches the worker (PosixProcess.cpp).
    std::signal(SIGPIPE, SIG_IGN);
#endif

    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
    {
        std::fprintf(stderr, "Failed to initialise GLFW.\n");
        return 1;
    }

    // GL 3.3 core. Not because phase 1 needs it - it only uses GL 1.1 textures -
    // but because the shaders being ported from the old app are 3.3 core GLSL,
    // and asking for the context now means the first shader does not also have to
    // change how the window is created. macOS needs the forward-compatible core
    // profile hints to give anything above 2.1 at all.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    int initialWidth = 1600, initialHeight = 900;
#ifdef __APPLE__
    // Not maximized on macOS, and hidden until PlaceMainWindow has read the
    // remembered position - see that function for the Resolve/App Nap
    // reason. First launch: three quarters of the main display, centred.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    int workX = 0, workY = 0, workW = 0, workH = 0;
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &workX, &workY, &workW, &workH);
    if (workW > 0 && workH > 0)
    {
        initialWidth  = workW * 3 / 4;
        initialHeight = workH * 3 / 4;
    }
#else
    // Maximized, not exclusive fullscreen: still a real bordered OS window (so
    // the taskbar, Alt-Tab and "Open in New Window" popouts all behave exactly
    // as they already do), just starting at the size someone would otherwise
    // reach by clicking the maximize button once on launch.
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(initialWidth, initialHeight, "Scope Deck", nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "Failed to create a GL 3.3 core window.\n");
        glfwTerminate();
        return 1;
    }
#ifdef __APPLE__
    if (workW > 0 && workH > 0)
        glfwSetWindowPos(window, workX + (workW - initialWidth) / 2, workY + (workH - initialHeight) / 2);
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(g_GlVsync ? 1 : 0);   // see g_GlVsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // ImGui's own default (true) leaves a secondary viewport borderless, with
    // nothing but ImGui's own small in-canvas titlebar for chrome - a real OS
    // window, but one that does not read as one: no native titlebar, no
    // minimise/maximise, none of what "Open in New Window" is supposed to
    // deliver. Off, so "Open in New Window" (and dragging a panel out) hands
    // it genuine OS decoration instead - the panel window's own Begin() call
    // adds ImGuiWindowFlags_NoTitleBar so ImGui does not also draw a second,
    // redundant titlebar underneath the native one.
    io.ConfigViewportsNoDecoration = false;

    // ImGui's own default (false) re-merges a floating window back into the
    // main viewport's rendering the moment it overlaps the main window on
    // screen - which a just-undocked window always does, since it starts
    // out exactly where it was docked. That "merge" is not a real separate
    // OS window at all, just a headerless floating pane drawn as part of the
    // main one - which is what "Open in New Window" actually produced before
    // this: it looked like it undocked, then silently became a broken,
    // header-less panel still inside the same window. True unconditionally,
    // so an undocked window is always its own genuine OS window regardless
    // of where it happens to sit - drag it onto another monitor from there,
    // same as any other window.
    io.ConfigViewportsNoAutoMerge = true;

    // Fonts follow the monitor automatically, per viewport - so a panel dragged
    // out onto a second display with a different DPI is rasterised for *that*
    // monitor rather than scaled up from the main one. ImGui 1.92+ rasterises on
    // demand, so this does not mean rebuilding an atlas, and it is why no font is
    // pre-scaled at startup here.
    //
    // It covers fonts only. Sizes and padding are handled by ApplyDpiScale().
    io.ConfigDpiScaleFonts = true;
    // Layout file in the user's app-data directory, not the working directory.
    //
    // ImGui's default is a bare relative filename, which means the saved layout
    // silently depends on where the app was launched from - double-clicking the
    // exe, running it from the project root, and running it from build/ would each
    // get a different one, and a build.ps1 -Clean would delete it. This is the
    // panel arrangement the user set up; it should not be a casualty of how they
    // started the app.
    static std::string iniPath = [settingsOverride]() -> std::string
    {
        if (settingsOverride && *settingsOverride) return std::string(settingsOverride);

#ifdef _WIN32
        const char* base = std::getenv("LOCALAPPDATA");
        if (base && *base)
        {
            const std::string dir = std::string(base) + "\\ScopeDeck";
            ::CreateDirectoryA(dir.c_str(), nullptr);   // harmless if it exists
            return dir + "\\layout.ini";
        }
#elif defined(__APPLE__)
        return mac::AppSupportDir() + "/layout.ini";
#else
        const char* home = std::getenv("HOME");
        if (home && *home)
            return std::string(home) + "/.config/scopedeck_layout.ini";
#endif
        return "scopedeck_layout.ini";   // last resort, old behaviour
    }();

    io.IniFilename = iniPath.c_str();

    ApplyDpiScale(1.0f);

#if defined(_WIN32) || defined(__APPLE__)
    // The atlas's very first added font becomes ImGui's implicit default for
    // every window that doesn't push its own - which is every panel in this
    // app. Adding it explicitly here, before the CJK font below, keeps that
    // default the same built-in font the rest of the UI has always been
    // sized and laid out against; without this, loading the CJK font first
    // would have silently made *it* (at its own, much larger, 24px size)
    // the whole app's font, not just the subtitle overlay's.
    io.Fonts->AddFontDefault();

    // Families are tried in order and the first whose Regular face loads
    // wins. Meiryo leads because it is a base Windows system font present on
    // every locale since Vista (not gated behind an optional East Asian
    // language pack the way some others can be) and, unusually, ships true
    // Bold, Italic and Bold Italic faces for CJK text - verified by reading
    // the face names out of the collections themselves rather than assumed:
    //
    //   meiryo.ttc  -> 0 Meiryo,      1 Meiryo Italic
    //   meiryob.ttc -> 0 Meiryo Bold, 1 Meiryo Bold Italic
    //
    // The fallbacks have no italic faces at all, so they point their italic
    // slots at the upright face; SubtitleFont() degrades the rest.
    //
    // Loaded with GetGlyphRangesJapanese() so a classic static font atlas
    // build still bakes the needed glyphs; ImGui's own dynamic/on-demand
    // atlas (see io.ConfigDpiScaleFonts's comment above) would work without
    // it, but there is no reason to depend on that when the ranges are free.
    {
        struct SubtitleFace { const char* path; ImU32 faceNo; };
        struct SubtitleFamily { SubtitleFace faces[(int)SubtitleStyle::Count]; };

        static const SubtitleFamily kFamilies[] = {
#ifdef __APPLE__
            // Hiragino Sans is the system Japanese font on every Mac since
            // 10.11: W3 is the regular weight, W6 the bold, both single-face
            // collections at these fixed paths (the file names are Japanese,
            // stored here as UTF-8). No italic faces exist, so the italic
            // slots point at the upright faces; SubtitleFont() degrades the
            // rest. Hiragino Sans GB (Simplified Chinese, covers kana too)
            // and AppleGothic (Korean) follow as fallbacks.
            {{ { "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc", 0 },
               { "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W6.ttc", 0 },
               { "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc", 0 },
               { "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W6.ttc", 0 } }},
            {{ { "/System/Library/Fonts/Hiragino Sans GB.ttc", 0 },
               { "/System/Library/Fonts/Hiragino Sans GB.ttc", 1 },
               { "/System/Library/Fonts/Hiragino Sans GB.ttc", 0 },
               { "/System/Library/Fonts/Hiragino Sans GB.ttc", 1 } }},
            {{ { "/System/Library/Fonts/Supplemental/AppleGothic.ttf", 0 },
               { "/System/Library/Fonts/Supplemental/AppleGothic.ttf", 0 },
               { "/System/Library/Fonts/Supplemental/AppleGothic.ttf", 0 },
               { "/System/Library/Fonts/Supplemental/AppleGothic.ttf", 0 } }},
#else
            {{ { "C:\\Windows\\Fonts\\meiryo.ttc",  0 },
               { "C:\\Windows\\Fonts\\meiryob.ttc", 0 },
               { "C:\\Windows\\Fonts\\meiryo.ttc",  1 },
               { "C:\\Windows\\Fonts\\meiryob.ttc", 1 } }},
            {{ { "C:\\Windows\\Fonts\\YuGothR.ttc", 0 },
               { "C:\\Windows\\Fonts\\YuGothB.ttc", 0 },
               { "C:\\Windows\\Fonts\\YuGothR.ttc", 0 },
               { "C:\\Windows\\Fonts\\YuGothB.ttc", 0 } }},
            {{ { "C:\\Windows\\Fonts\\NotoSansJP-Regular.otf", 0 },
               { "C:\\Windows\\Fonts\\NotoSansJP-Bold.otf",    0 },
               { "C:\\Windows\\Fonts\\NotoSansJP-Regular.otf", 0 },
               { "C:\\Windows\\Fonts\\NotoSansJP-Bold.otf",    0 } }},
            {{ { "C:\\Windows\\Fonts\\msgothic.ttc", 0 },
               { "C:\\Windows\\Fonts\\msgothic.ttc", 0 },
               { "C:\\Windows\\Fonts\\msgothic.ttc", 0 },
               { "C:\\Windows\\Fonts\\msgothic.ttc", 0 } }},
#endif
        };

        for (const SubtitleFamily& family : kFamilies)
        {
            for (int style = 0; style < (int)SubtitleStyle::Count; ++style)
            {
                ImFontConfig cfg;
                cfg.FontNo = family.faces[style].faceNo;
                g_SubtitleFonts[style] = io.Fonts->AddFontFromFileTTF(
                    family.faces[style].path, 24.0f, &cfg,
                    io.Fonts->GetGlyphRangesJapanese());
            }

            // Judged on Regular alone: a family missing only its bold or
            // italic is still the right family to use, and SubtitleFont()
            // already falls back per style.
            if (g_SubtitleFonts[(int)SubtitleStyle::Regular]) break;
        }
    }
#endif

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;

    // The default deck. Restored settings may replace this before the first frame
    // (see the settings handler), which is why it is seeded before the handler is
    // installed rather than after.
    AddPanel(app, PanelKind::Source);
    AddPanel(app, PanelKind::Waveform);
    AddPanel(app, PanelKind::Histogram);
    AddPanel(app, PanelKind::Vectorscope);

    // Before the first NewFrame: that is when ImGui reads the .ini, and the
    // handler has to be registered to receive those lines.
    InstallSettingsHandler(app);

    // Not opened here: the saved host (prefs.hostInput) is only known once the
    // .ini has been read, during the first NewFrame. Opening before that read
    // the default host's block and showed its picture - Resolve's, for someone
    // whose saved input is Premiere - until the other block's first frame
    // replaced it. RenderOneFrame opens it from the second frame on.

    TimecodeBridgeStart();
    SubtitleBridgeStart();
    AudioMeterBridgeStart();

    RenderContext renderCtx;
    renderCtx.app            = &app;
    renderCtx.forcedDpiScale = forcedDpiScale;
    glfwSetWindowUserPointer(window, &renderCtx);
    glfwSetWindowRefreshCallback(window, WindowRefreshCallback);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        // Cmd+Q, Quit from the Dock and logout arrive as a close request on
        // every window at once, the popped-out panels' own windows included.
        // One more frame after that and ImGui reports each of those panels
        // closed, the deck drops them, and the layout saved below has lost
        // them. Checked again here so that frame is never drawn. The menu's
        // Quit and the title bar's close button set it on the main window
        // alone and never had the problem.
        if (glfwWindowShouldClose(window)) break;
        RenderOneFrame(window, renderCtx);
    }

    // Honour "Remember Layout on Close". ImGui writes the .ini on a timer while
    // the app runs, so switching the preference off cannot just skip a final save
    // - the file is already there and has to go.
    if (!app.prefs.rememberLayoutOnClose)
    {
        io.IniFilename = nullptr;      // stop ImGui rewriting it during shutdown
        std::remove(iniPath.c_str());
    }
    else
    {
        ImGui::SaveIniSettingsToDisk(iniPath.c_str());
    }

    // Gone from the screen now, layout already saved: the bridge threads can
    // take a moment to wind down (a Python worker being told to exit), and a
    // window that sits there unresponsive meanwhile reads as a crash.
    glfwHideWindow(window);
    ImGui::DestroyPlatformWindows();   // popped-out panels
    app.capture.Stop();

    TimecodeBridgeStop();
    SubtitleBridgeStop();
    AudioMeterBridgeStop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}



