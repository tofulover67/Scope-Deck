// macOS half of Screen Capture: the region picker overlay and the
// ScreenCaptureKit stream. Objective-C++ lives only in ScreenCaptureMac.mm;
// this header is plain C++ so ScreenCapture.cpp stays plain C++ on every
// platform, the same split MacPlatform.h makes for main.cpp.
//
// ScreenCapture.cpp owns the worker thread and the ScopeEngine; this side
// only gets pixels off the screen. See ScreenCapture.h for what a
// ScreenRegion means on macOS (global points for the origin, pixels for the
// size).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace scopedeck
{

struct ScreenRegion;

namespace mac
{

// The overlay picker behind scopedeck::PickScreenRegion. Main thread only.
bool PickScreenRegion(ScreenRegion& p_Out);

// Fits p_In to one display - the one holding its centre - and sizes it in
// that display's pixels as the display is now (its scale may have changed
// since the region was picked). False if nothing of the region is left on
// any display. Plain CoreGraphics, so safe off the main thread too.
bool FitRegionToDisplay(const ScreenRegion& p_In, ScreenRegion& p_Out, uint32_t& p_DisplayId);

// One captured frame, mapped for reading until ScreenStream::Release.
struct ScreenFrame
{
    const uint8_t* bgra      = nullptr;   // BGRA8, top row first
    int            width     = 0;
    int            height    = 0;
    size_t         stride    = 0;         // bytes between rows
    double         msGrab    = 0.0;       // arrived -> readable here
    void*          buffer    = nullptr;   // the retained CVPixelBuffer
};

// A ScreenCaptureKit stream of one region of one display. Frames are handed
// over newest-only: one arriving before the previous was taken replaces it,
// so a slow consumer drops frames instead of queueing them. Used from one
// thread (ScreenCaptureSource's worker); the stream's own callbacks run on a
// private queue.
class ScreenStream
{
public:
    enum class Wait { Frame, Timeout, Failed };

    ScreenStream();
    ~ScreenStream();

    ScreenStream(const ScreenStream&) = delete;
    ScreenStream& operator=(const ScreenStream&) = delete;

    // p_Region must already be fitted to p_DisplayId (FitRegionToDisplay).
    // Blocks until the stream is running, it fails (p_Error says why, in
    // words for the status bar), or p_Cancel is set (false, p_Error empty).
    bool Start(uint32_t p_DisplayId, const ScreenRegion& p_Region,
               const std::atomic<bool>& p_Cancel, std::string& p_Error);

    // Waits up to p_TimeoutMs for a frame newer than the last one taken.
    // ScreenCaptureKit sends nothing while the region's content is still, so
    // Timeout is routine. Failed means the stream has stopped for good;
    // Error() says why. A Frame must be given back with Release.
    Wait WaitFrame(int p_TimeoutMs, ScreenFrame& p_Out);
    void Release(ScreenFrame& p_Frame);

    std::string Error() const;

    void Stop();

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace mac
} // namespace scopedeck
