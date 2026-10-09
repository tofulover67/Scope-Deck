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
// Windows-only for now (GDI capture); on other platforms Start() fails and
// PickScreenRegion() returns false.

#pragma once

#include <cstdint>
#include <string>

namespace scopedeck
{

struct ScopeFrame;

// Virtual-desktop coordinates in physical pixels - GLFW makes this process
// per-monitor DPI aware, so these are real pixels on every monitor. A region
// may span monitors.
struct ScreenRegion
{
    int x = 0;
    int y = 0;
    int width  = 0;
    int height = 0;

    bool IsValid() const { return width > 0 && height > 0; }
};

// Covers every monitor with a dimmed screenshot and lets the user drag out a
// rectangle, like a snipping tool. Modal: returns once the drag ends (true)
// or on Esc / right-click (false). Must be called between frames, never while
// an ImGui frame is being built - it runs its own message loop.
bool PickScreenRegion(ScreenRegion& p_Out);

struct ScreenCaptureStats
{
    double      fps        = 0.0;   // frames handed over per second, recent average
    double      msGrab     = 0.0;   // copying the region off the screen
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
    // already running.
    bool Start(const ScreenRegion& p_Region);
    void Stop();
    bool IsRunning() const;

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
