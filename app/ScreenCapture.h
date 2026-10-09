// Screen Capture input: a region of the desktop instead of Resolve's tap.
//
// The captured pixels go through the same ScopeEngine the tap runs, and the
// result is handed over as an ordinary ScopeFrame - so every panel behaves
// identically whichever input is active, the same guarantee the old app's
// ScreenCaptureSource made by reusing analyse_rgb_frame(). Nothing goes
// through shared memory: the engine runs in this process, and the frame is
// filled with exactly the copies ScopePublisher::Publish and
// ScopeReader::ReadSlot would have made.
//
// Windows grabs with GDI (BitBlt) on a 60 Hz timer; macOS streams with
// ScreenCaptureKit (app/mac/ScreenCaptureMac.mm), which needs the Screen
// Recording permission. Elsewhere Start() fails and PickScreenRegion()
// returns false.

#pragma once

#include <cstdint>
#include <string>

namespace scopedeck
{

struct ScopeFrame;

// Windows: virtual-desktop coordinates in physical pixels - GLFW makes this
// process per-monitor DPI aware, so these are real pixels on every monitor. A
// region may span monitors.
//
// macOS: there is no global pixel space - each display has its own backing
// scale, and a 2x display next to a 1x one shares one coordinate space only in
// points. So x/y are global CoreGraphics points (origin at the top-left of the
// main display, y down, the same space CGDisplayBounds uses), while
// width/height are the pixels actually captured, which is what the status bar
// reports and what the scopes see. scale is the display's pixels per point
// when the region was picked, so the region's size in points is
// width/scale x height/scale. A region always lies on one display: the picker
// keeps the drag on the display it started on, and Start() clamps a region
// to the display holding its centre.
struct ScreenRegion
{
    int   x = 0;
    int   y = 0;
    int   width  = 0;
    int   height = 0;
    float scale  = 1.0f;   // macOS only; Windows is always in pixels

    bool IsValid() const { return width > 0 && height > 0 && scale > 0.0f; }
};

// Lets the user drag out a rectangle over every monitor, like a snipping
// tool: dimmed everywhere but the selection. Modal: returns once the drag
// ends (true) or on Esc / right-click / switching away (false). Must be called
// between frames, never while an ImGui frame is being built - it runs its own
// event loop.
//
// Windows dims a screenshot taken before the overlay appears. macOS dims a
// translucent overlay and leaves the selection see-through instead, so
// picking a region doesn't itself need the Screen Recording permission.
bool PickScreenRegion(ScreenRegion& p_Out);

struct ScreenCaptureStats
{
    double      fps        = 0.0;   // frames handed over per second, recent average
    double      msGrab     = 0.0;   // getting the region's pixels: Windows, the
                                    // BitBlt; macOS, from ScreenCaptureKit
                                    // handing the frame over to it being
                                    // readable (the copy itself is off-process)
    double      msScopes   = 0.0;   // ScopeEngine::Analyse + BuildPreview
    uint64_t    frames     = 0;
    std::string error;              // empty while capturing normally
};

class ScreenCaptureSource
{
public:
    ScreenCaptureSource();
    ~ScreenCaptureSource();

    ScreenCaptureSource(const ScreenCaptureSource&) = delete;
    ScreenCaptureSource& operator=(const ScreenCaptureSource&) = delete;

    // Starts capturing p_Region on a background thread, replacing any capture
    // already running. False only for a region there is nothing to capture
    // in; anything that goes wrong later (on macOS, a missing Screen
    // Recording permission) arrives as Stats().error while running.
    bool Start(const ScreenRegion& p_Region);
    void Stop();
    bool IsRunning() const;

    // The region being captured: p_Region as given on Windows; on macOS,
    // clamped to the display holding its centre and sized for that display.
    const ScreenRegion& Region() const;

    // Hands over the newest frame if one arrived since the last call. Swaps
    // buffers with p_Out rather than copying, so the caller's previous frame
    // is recycled as the next one's storage.
    bool TakeLatest(ScopeFrame& p_Out);

    void Stats(ScreenCaptureStats& p_Out) const;

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace scopedeck
