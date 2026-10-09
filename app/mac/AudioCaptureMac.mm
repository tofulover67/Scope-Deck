// macOS half of the Audio Meter bridge - see AudioCaptureMac.h.
//
// A Core Audio process tap (macOS 14.2+), the same mechanism Audio Hijack and
// OBS's macOS app capture use: the tap is a virtual input stream carrying a
// stereo mixdown of the chosen processes' output, and the only way to read
// it is to put it inside an aggregate device and run an IOProc on that. So
// one open "stream" here is three Core Audio objects - tap, aggregate
// device, IOProc - built in that order and destroyed in reverse.
//
// Both the tap and the aggregate are private to this process, which is also
// what keeps a crash from leaking them: Core Audio drops a private object
// with the process that made it, so nothing is left behind in Audio MIDI
// Setup even if Close() never runs.

#include "AudioCaptureMac.h"

#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>

#include <dlfcn.h>
#include <libproc.h>
#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace scopedeck
{
namespace mac
{

// The tap's audio is gated by its own TCC service ("System Audio Recording
// Only" in System Settings), and a tap created without it does not fail -
// it just delivers silence forever, which on its own reads exactly like
// "Resolve is paused". Apple offers no public call to ask, so this asks TCC
// itself, looked up at run time so a macOS without the symbol just answers
// Unknown rather than failing to launch. The same check the open-source
// AudioCap tap sample makes.
AudioCapturePermission QueryAudioCapturePermission()
{
    using PreflightFn = int (*)(CFStringRef, CFDictionaryRef);
    static const PreflightFn preflight = []() -> PreflightFn
    {
        void* tcc = dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC", RTLD_LAZY);
        return tcc ? reinterpret_cast<PreflightFn>(dlsym(tcc, "TCCAccessPreflight")) : nullptr;
    }();
    if (!preflight) return AudioCapturePermission::Unknown;

    // 0 = allowed, 1 = denied, anything else = not decided yet.
    switch (preflight(CFSTR("kTCCServiceAudioCapture"), nullptr))
    {
        case 0:  return AudioCapturePermission::Granted;
        case 1:  return AudioCapturePermission::Denied;
        default: return AudioCapturePermission::Unknown;
    }
}

bool ProcessAlive(int p_Pid)
{
    if (p_Pid <= 0) return false;
    // Signal 0 delivers nothing, it only checks the pid exists. EPERM means
    // it exists but belongs to someone else - still alive.
    return ::kill(pid_t(p_Pid), 0) == 0 || errno == EPERM;
}

template <typename T>
static OSStatus GetProperty(AudioObjectID p_Object, AudioObjectPropertySelector p_Selector, T& p_Out,
                            AudioObjectPropertyScope p_Scope = kAudioObjectPropertyScopeGlobal,
                            UInt32 p_QualifierSize = 0, const void* p_Qualifier = nullptr)
{
    const AudioObjectPropertyAddress address = { p_Selector, p_Scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(p_Object, &address, p_QualifierSize, p_Qualifier, &size, &p_Out);
}

static std::string CFStringToStd(CFStringRef p_String)
{
    if (!p_String) return std::string();
    char buf[512];
    if (CFStringGetCString(p_String, buf, sizeof(buf), kCFStringEncodingUTF8)) return std::string(buf);
    return std::string();
}

// The host's Core Audio process objects, sorted (so two lists compare with
// ==). Core Audio only has an object for a process once it has touched audio
// at all, so an empty list with the host running means "hasn't opened any
// audio yet", not "not running".
//
// Helpers are included the way Windows' INCLUDE_TARGET_PROCESS_TREE does:
// still the host's own audio, whichever of its processes plays it. A helper
// is anything whose bundle id extends the host's (com.example.App.helper)
// or whose parent is the host.
static std::vector<AudioObjectID> HostProcessObjects(int p_Pid, const char* p_BundleId)
{
    std::vector<AudioObjectID> result;

    const pid_t hostPid = pid_t(p_Pid);
    AudioObjectID mainObject = kAudioObjectUnknown;
    if (GetProperty(kAudioObjectSystemObject, kAudioHardwarePropertyTranslatePIDToProcessObject, mainObject,
                    kAudioObjectPropertyScopeGlobal, sizeof(hostPid), &hostPid) == noErr
        && mainObject != kAudioObjectUnknown)
    {
        result.push_back(mainObject);
    }

    const AudioObjectPropertyAddress listAddress = {
        kAudioHardwarePropertyProcessObjectList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size) != noErr)
        return result;
    std::vector<AudioObjectID> all(size / sizeof(AudioObjectID));
    if (all.empty()
        || AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size, all.data()) != noErr)
        return result;
    all.resize(size / sizeof(AudioObjectID));

    const std::string hostBundle = p_BundleId ? p_BundleId : "";
    for (AudioObjectID object : all)
    {
        if (object == mainObject) continue;

        pid_t pid = 0;
        if (GetProperty(object, kAudioProcessPropertyPID, pid) != noErr || pid <= 0) continue;

        bool helper = false;
        if (!hostBundle.empty())
        {
            CFStringRef bundle = nullptr;
            if (GetProperty(object, kAudioProcessPropertyBundleID, bundle) == noErr && bundle)
            {
                const std::string id = CFStringToStd(bundle);
                CFRelease(bundle);
                helper = id.size() > hostBundle.size() && id.compare(0, hostBundle.size(), hostBundle) == 0
                      && id[hostBundle.size()] == '.';
            }
        }
        if (!helper)
        {
            struct proc_bsdinfo info = {};
            helper = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == int(sizeof(info))
                  && pid_t(info.pbi_ppid) == hostPid;
        }
        if (helper) result.push_back(object);
    }

    std::sort(result.begin(), result.end());
    return result;
}

struct ProcessAudioTap::Impl
{
    // Large enough for any IO buffer Core Audio hands an aggregate in
    // practice (they run 512 frames by default, 4096 at most from the HAL
    // UI); a bigger one is simply delivered in pieces.
    static constexpr UInt32 kScratchFrames = 8192;

    AudioObjectID       tap       = kAudioObjectUnknown;
    AudioObjectID       aggregate = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProc    = nullptr;

    int                        pid = 0;
    std::string                bundleId;
    std::vector<AudioObjectID> processes;
    Float64                    tapRate = 0.0;   // the tap's format rate when it was opened

    AudioTapCallback callback = nullptr;
    void*            user     = nullptr;
    float            scratch[kScratchFrames * 2];

    // Runs on Core Audio's I/O thread. The stereo mixdown tap is float32, but
    // whether the aggregate presents it as one interleaved stereo buffer or
    // two mono ones is the aggregate's business, so both are handled (and a
    // mono buffer is played to both sides) rather than assumed.
    void Deliver(const AudioBufferList* p_Input)
    {
        if (!p_Input || p_Input->mNumberBuffers == 0 || !callback) return;

        const AudioBuffer& first = p_Input->mBuffers[0];
        if (!first.mData || first.mNumberChannels == 0) return;

        const float* left   = static_cast<const float*>(first.mData);
        const float* right  = left;
        UInt32       stride = first.mNumberChannels;
        UInt32       frames = first.mDataByteSize / UInt32(sizeof(float) * stride);
        if (stride >= 2)
        {
            right = left + 1;
        }
        else if (p_Input->mNumberBuffers >= 2 && p_Input->mBuffers[1].mData
                 && p_Input->mBuffers[1].mNumberChannels == 1)
        {
            right  = static_cast<const float*>(p_Input->mBuffers[1].mData);
            frames = std::min(frames, UInt32(p_Input->mBuffers[1].mDataByteSize / sizeof(float)));
        }

        for (UInt32 done = 0; done < frames; )
        {
            const UInt32 chunk = std::min(frames - done, kScratchFrames);
            for (UInt32 f = 0; f < chunk; ++f)
            {
                const size_t src = size_t(done + f) * stride;
                scratch[f * 2]     = left[src];
                scratch[f * 2 + 1] = right[src];
            }
            callback(scratch, chunk, user);
            done += chunk;
        }
    }
};

ProcessAudioTap::ProcessAudioTap() : m_Impl(new Impl()) {}

ProcessAudioTap::~ProcessAudioTap()
{
    Close();
    delete m_Impl;
}

bool ProcessAudioTap::IsOpen() const
{
    return m_Impl->ioProc != nullptr;
}

bool ProcessAudioTap::IsStale() const
{
    if (!IsOpen()) return false;
    if (HostProcessObjects(m_Impl->pid, m_Impl->bundleId.c_str()) != m_Impl->processes) return true;

    AudioStreamBasicDescription format = {};
    return GetProperty(m_Impl->tap, kAudioTapPropertyFormat, format) == noErr
        && format.mSampleRate != m_Impl->tapRate;
}

bool ProcessAudioTap::Open(int p_Pid, const char* p_BundleId, AudioTapCallback p_Callback, void* p_User,
                           AudioMeterStatus& p_OutStatus, long& p_OutOsStatus, int& p_OutSampleRate)
{
    Close();
    p_OutOsStatus   = 0;
    p_OutSampleRate = 0;

    if (!ProcessAlive(p_Pid))
    {
        p_OutStatus = AudioMeterStatus::ResolveNotFound;
        return false;
    }

    // Asked first: a denied tap is not refused, it is made and then only
    // ever hears silence (see QueryAudioCapturePermission), which the panel
    // would show as "Resolve is paused" instead of saying what to fix.
    if (QueryAudioCapturePermission() == AudioCapturePermission::Denied)
    {
        p_OutStatus = AudioMeterStatus::PermissionDenied;
        return false;
    }

    Impl& s = *m_Impl;
    s.pid       = p_Pid;
    s.bundleId  = p_BundleId ? p_BundleId : "";
    s.processes = HostProcessObjects(p_Pid, p_BundleId);
    s.callback  = p_Callback;
    s.user      = p_User;
    if (s.processes.empty())
    {
        p_OutStatus = AudioMeterStatus::HostHasNoAudio;
        return false;
    }

    @autoreleasepool
    {
        // --- The tap ---------------------------------------------------------
        NSMutableArray<NSNumber*>* processList = [NSMutableArray arrayWithCapacity:s.processes.size()];
        for (AudioObjectID object : s.processes)
            [processList addObject:@(object)];

        CATapDescription* description = [[CATapDescription alloc] initStereoMixdownOfProcesses:processList];
        description.name         = @"Scope Deck Audio Meter";
        description.privateTap   = YES;          // nobody else sees (or can read) it
        description.muteBehavior = CATapUnmuted; // the colourist still hears Resolve
        NSString* tapUid = [description.UUID.UUIDString copy];

        // On macOS 14.4+ the first tap this app ever makes is what puts up
        // the "System Audio Recording Only" prompt.
        OSStatus err = AudioHardwareCreateProcessTap(description, &s.tap);
#if !__has_feature(objc_arc)
        [description release];
        [tapUid autorelease];
#endif
        if (err != noErr || s.tap == kAudioObjectUnknown)
        {
            s.tap = kAudioObjectUnknown;
            p_OutStatus   = AudioMeterStatus::ActivationCallFailed;
            p_OutOsStatus = long(err != noErr ? err : kAudioHardwareUnspecifiedError);
            Close();
            return false;
        }

        // The stereo mixdown is float32 at the rate of the device the host
        // plays through - 44.1k, 48k, 96k, whatever the colourist's output
        // is set to. The meter math does not care; the Spectrum Analyzer's
        // frequency axis does, so the real rate is handed back to the bridge.
        AudioStreamBasicDescription format = {};
        err = GetProperty(s.tap, kAudioTapPropertyFormat, format);
        if (err != noErr || format.mFormatID != kAudioFormatLinearPCM
            || !(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32)
        {
            p_OutStatus   = AudioMeterStatus::ActivationCallFailed;
            p_OutOsStatus = long(err != noErr ? err : kAudioDeviceUnsupportedFormatError);
            Close();
            return false;
        }
        s.tapRate = format.mSampleRate;

        // --- The aggregate device ---------------------------------------------
        // Tap only, no real sub-device: the tap brings its own clock, and a
        // hardware sub-device would put that device's own inputs (if it has
        // any) in front of the tap's channels in the IOProc's buffer list.
        NSString* aggregateUid = [NSUUID UUID].UUIDString;
        NSDictionary* composition = @{
            @kAudioAggregateDeviceNameKey:         @"Scope Deck Audio Meter",
            @kAudioAggregateDeviceUIDKey:          aggregateUid,
            @kAudioAggregateDeviceIsPrivateKey:    @YES,
            @kAudioAggregateDeviceIsStackedKey:    @NO,
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceTapListKey: @[ @{
                @kAudioSubTapUIDKey:               tapUid,
                @kAudioSubTapDriftCompensationKey: @YES,
            } ],
        };
        err = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)composition, &s.aggregate);
        if (err != noErr || s.aggregate == kAudioObjectUnknown)
        {
            s.aggregate = kAudioObjectUnknown;
            p_OutStatus   = AudioMeterStatus::ActivationResultFailed;
            p_OutOsStatus = long(err != noErr ? err : kAudioHardwareUnspecifiedError);
            Close();
            return false;
        }

        // What the IOProc's frames actually run at. Normally the tap's own
        // rate; the aggregate's word wins if the two ever disagree, since
        // its clock is the one that delivers the buffers.
        Float64 rate = format.mSampleRate;
        Float64 aggregateRate = 0.0;
        if (GetProperty(s.aggregate, kAudioDevicePropertyNominalSampleRate, aggregateRate) == noErr
            && aggregateRate > 0.0)
        {
            rate = aggregateRate;
        }
        p_OutSampleRate = int(rate + 0.5);

        // --- The IOProc ---------------------------------------------------------
        // No dispatch queue: the block runs directly on the device's own I/O
        // thread, the same as a classic IOProc.
        Impl* impl = &s;
        err = AudioDeviceCreateIOProcIDWithBlock(&s.ioProc, s.aggregate, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* p_InputData, const AudioTimeStamp*,
              AudioBufferList*, const AudioTimeStamp*)
            {
                impl->Deliver(p_InputData);
            });
        if (err != noErr || !s.ioProc)
        {
            s.ioProc = nullptr;
            p_OutStatus   = AudioMeterStatus::StreamInitFailed;
            p_OutOsStatus = long(err != noErr ? err : kAudioHardwareUnspecifiedError);
            Close();
            return false;
        }

        err = AudioDeviceStart(s.aggregate, s.ioProc);
        if (err != noErr)
        {
            p_OutStatus   = AudioMeterStatus::StreamInitFailed;
            p_OutOsStatus = long(err);
            Close();
            return false;
        }
    }

    p_OutStatus = AudioMeterStatus::Capturing;
    return true;
}

void ProcessAudioTap::Close()
{
    Impl& s = *m_Impl;

    // AudioDeviceStop from outside the I/O thread waits for an IOProc call
    // already in flight, so once both of these return the block (and the
    // callback it calls) is done for good.
    if (s.ioProc)
    {
        AudioDeviceStop(s.aggregate, s.ioProc);
        AudioDeviceDestroyIOProcID(s.aggregate, s.ioProc);
        s.ioProc = nullptr;
    }
    if (s.aggregate != kAudioObjectUnknown)
    {
        AudioHardwareDestroyAggregateDevice(s.aggregate);
        s.aggregate = kAudioObjectUnknown;
    }
    if (s.tap != kAudioObjectUnknown)
    {
        AudioHardwareDestroyProcessTap(s.tap);
        s.tap = kAudioObjectUnknown;
    }
    s.processes.clear();
    s.tapRate  = 0.0;
    s.callback = nullptr;
    s.user     = nullptr;
}

} // namespace mac
} // namespace scopedeck
