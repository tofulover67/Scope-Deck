// macOS half of the Audio Meter bridge: a Core Audio process tap on the host
// app's own output (DaVinci Resolve's, normally), delivered to the C++ side
// as interleaved stereo float. Objective-C++ lives only in AudioCaptureMac.mm;
// this header is plain C++ so AudioMeterBridge.cpp stays plain C++ too.
//
// The Windows counterpart is LoopbackStream inside AudioMeterBridge.cpp
// (WASAPI per-process loopback). Same shape on purpose: one tap at a time,
// opened when something needs it, torn down completely on any doubt and
// reopened fresh rather than nursed back to health.

#pragma once

#include "../AudioMeterBridge.h"

#include <cstdint>

namespace scopedeck
{
namespace mac
{

// Called on Core Audio's own I/O thread, once per buffer, with p_Frames
// interleaved stereo float frames (L R L R ...). Must not block for long:
// the device's next buffer is waiting on it.
using AudioTapCallback = void (*)(const float* p_Interleaved, uint32_t p_Frames, void* p_User);

// Whether the user has let this app record other apps' audio (System
// Settings > Privacy & Security > Screen & System Audio Recording, the
// "System Audio Recording Only" list). Unknown covers both "not asked yet"
// and "can't tell on this macOS".
enum class AudioCapturePermission { Granted, Denied, Unknown };
AudioCapturePermission QueryAudioCapturePermission();

// True while the process with this pid still exists.
bool ProcessAlive(int p_Pid);

class ProcessAudioTap
{
public:
    ProcessAudioTap();
    ~ProcessAudioTap();
    ProcessAudioTap(const ProcessAudioTap&) = delete;
    ProcessAudioTap& operator=(const ProcessAudioTap&) = delete;

    // Taps p_Pid plus any of its helper processes that Core Audio knows
    // about (same bundle id prefix, or a direct child). p_BundleId may be
    // null. p_OutStatus/p_OutOsStatus are filled on every path, success or
    // failure, exactly like LoopbackStream::Open on Windows; on success
    // p_OutSampleRate is the rate the callback's frames actually run at.
    bool Open(int p_Pid, const char* p_BundleId, AudioTapCallback p_Callback, void* p_User,
              AudioMeterStatus& p_OutStatus, long& p_OutOsStatus, int& p_OutSampleRate);

    // Stops I/O and destroys the aggregate device and the tap, in that
    // order. Safe to call when nothing is open. Once it returns the callback
    // will not be called again.
    void Close();

    bool IsOpen() const;

    // True when the open tap no longer describes what the host is doing,
    // though nothing has failed: the host's set of Core Audio process
    // objects changed (a helper process started or stopped making sound -
    // a tap's process list is fixed at creation), or the tap's sample rate
    // moved (the output device was switched or re-clocked under it). The
    // caller reopens. Cheap enough to call once a second.
    bool IsStale() const;

private:
    struct Impl;
    Impl* m_Impl;
};

} // namespace mac
} // namespace scopedeck
