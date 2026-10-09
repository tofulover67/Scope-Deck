#include "AudioMeterBridge.h"

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>

// Resolve has no audio-capable OFX suite and no scripting call that exposes
// live sample data or meter values (checked against Resolve's own OpenFX SDK
// and Scripting README - see PORTING.md) - the same wall ERRORS.md already
// hit trying to drive Resolve's transport. This instead captures Resolve's
// own process output specifically, rather than the whole system's default
// output device, which would also pick up whatever else on the machine
// happens to be making noise:
//
// - Windows: per-process loopback activation (`ActivateAudioInterfaceAsync`
//   with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`) - the same mechanism
//   behind Windows 11's own per-app volume mixer and OBS's "audio output
//   capture" source.
// - macOS: a Core Audio process tap (macOS 14.2+), the same mechanism behind
//   Audio Hijack and OBS's macOS app capture - see app/mac/AudioCaptureMac.mm.
//
// Everything after the capture itself - the peak/RMS/true-peak reduction,
// the ring buffer, the staged status - is shared by both.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <propidl.h>
#include <combaseapi.h>
#elif defined(__APPLE__)
#include "mac/AudioCaptureMac.h"
#include "mac/MacPlatform.h"
#endif

namespace scopedeck
{

static std::mutex          g_AmMutex;
static AudioMeterLevels    g_AmLevels;
static AudioRingSnapshot   g_AmRing;
static std::atomic<bool>   g_AmActive{ false };
static std::atomic<bool>   g_AmQuit{ false };
static std::atomic<int>    g_AmTarget{ static_cast<int>(AudioTarget::Resolve) };
static std::thread         g_AmThread;

static const char* TargetHostName(int p_Target)
{
    return (p_Target == static_cast<int>(AudioTarget::Premiere)) ? "Premiere Pro" : "DaVinci Resolve";
}

void AudioMeterBridgeSetActive(bool p_Active)
{
    g_AmActive.store(p_Active, std::memory_order_relaxed);
}

void AudioMeterBridgeSetTarget(AudioTarget p_Target)
{
    g_AmTarget.store(static_cast<int>(p_Target), std::memory_order_relaxed);
}

AudioMeterLevels AudioMeterBridgeGetLevels()
{
    std::lock_guard<std::mutex> lock(g_AmMutex);
    return g_AmLevels;
}

AudioRingSnapshot AudioMeterBridgeGetRingSnapshot()
{
    std::lock_guard<std::mutex> lock(g_AmMutex);
    return g_AmRing;
}

static void PublishLevels(const AudioMeterLevels& p_Levels)
{
    std::lock_guard<std::mutex> lock(g_AmMutex);
    g_AmLevels = p_Levels;
    g_AmLevels.hostName = TargetHostName(g_AmTarget.load(std::memory_order_relaxed));
}

// Appends p_Frames interleaved frames to the ring buffer - called once per
// captured packet, silent or not (a silent packet still advances time, and
// writing real zeros for it is what keeps the Goniometer/Spectrum Analyzer
// from reading stale, no-longer-current samples as if they were live).
static void AppendRingSamples(const float* p_Interleaved, uint32_t p_Frames)
{
    std::lock_guard<std::mutex> lock(g_AmMutex);
    for (uint32_t f = 0; f < p_Frames; ++f)
    {
        const uint64_t dst = (g_AmRing.writeCount + f) % uint64_t(AudioRingSnapshot::kCapacity);
        for (int c = 0; c < AudioRingSnapshot::kChannels; ++c)
            g_AmRing.samples[size_t(dst) * AudioRingSnapshot::kChannels + c] =
                p_Interleaved[size_t(f) * AudioRingSnapshot::kChannels + c];
    }
    g_AmRing.writeCount += p_Frames;
}

static float LinearToDb(float p_Linear)
{
    constexpr float kFloor = 1e-5f;   // -100 dB
    return 20.0f * std::log10(std::max(p_Linear, kFloor));
}

// Catmull-Rom cubic through four consecutive samples, evaluated at t in
// [0,1] between p1 and p2 - the interpolation kernel True Peak below uses to
// estimate what the waveform actually does *between* samples.
static float CatmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t
                  + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                  + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Fixed float32/kChannels layout on both platforms (Windows asks WASAPI for
// it - see LoopbackStream::Open; macOS's stereo mixdown tap delivers it and
// AudioCaptureMac.mm interleaves it), so this needs no format-detection
// branching at all - unlike a whole-device capture, which has to cope with
// whatever format that device's own mix happens to be.
static void AccumulatePacket(const uint8_t* p_Data, uint32_t p_Frames, bool p_Silent, AudioMeterLevels& p_Out)
{
    constexpr int channels = AudioRingSnapshot::kChannels;
    p_Out.channelCount = channels;
    p_Out.deviceOk      = true;

    float  peak[AudioMeterLevels::kMaxChannels]     = {};
    float  truePeak[AudioMeterLevels::kMaxChannels] = {};
    double sumSquares[AudioMeterLevels::kMaxChannels] = {};

    if (!p_Silent && p_Frames > 0)
    {
        const float* samples = reinterpret_cast<const float*>(p_Data);

        for (uint32_t f = 0; f < p_Frames; ++f)
        {
            for (int c = 0; c < channels; ++c)
            {
                const float sample = samples[size_t(f) * channels + c];
                const float mag    = std::fabs(sample);
                if (mag > peak[c]) peak[c] = mag;
                sumSquares[c] += double(sample) * double(sample);
            }
        }

        // True Peak: a real 4x-oversampled inter-sample estimate (Catmull-Rom
        // interpolation between consecutive samples), NOT a certified
        // ITU-R BS.1770-4 measurement - that standard specifies exact
        // polyphase FIR filter coefficients this doesn't reproduce, the same
        // "not claimed as verified [standard] compliance" disclosure this
        // project already gives its EBU R103 Signal Pre-filter. What this
        // does catch, honestly: the actual problem True Peak exists for -
        // a waveform that peaks *between* samples (common after upstream
        // gain/EQ) reads safely under 0dBFS on a plain sample-peak meter and
        // then clips for real once a downstream lossy codec or D/A converter
        // reconstructs the continuous waveform. Edge-clamped per packet (no
        // continuity carried across the ~10-20ms packet boundary), so an
        // inter-sample peak landing exactly on a boundary is a rare, small
        // blind spot, not a systematic one.
        constexpr int kOversample = 4;
        auto sampleAt = [&](int f, int c) -> float {
            const int clamped = std::clamp(f, 0, int(p_Frames) - 1);
            return samples[size_t(clamped) * channels + c];
        };
        for (uint32_t f = 0; f + 1 < p_Frames; ++f)
        {
            for (int c = 0; c < channels; ++c)
            {
                const float p0 = sampleAt(int(f) - 1, c);
                const float p1 = sampleAt(int(f),     c);
                const float p2 = sampleAt(int(f) + 1, c);
                const float p3 = sampleAt(int(f) + 2, c);
                for (int k = 1; k < kOversample; ++k)   // k=0 is p1 itself, already in peak[] above
                {
                    const float t = float(k) / float(kOversample);
                    const float interp = std::fabs(CatmullRom(p0, p1, p2, p3, t));
                    if (interp > truePeak[c]) truePeak[c] = interp;
                }
            }
        }
    }

    for (int c = 0; c < channels; ++c)
    {
        const float rms = p_Frames > 0 ? float(std::sqrt(sumSquares[c] / double(p_Frames))) : 0.0f;
        p_Out.peakDb[c]     = LinearToDb(peak[c]);
        p_Out.rmsDb[c]      = LinearToDb(rms);
        // Never below the sample peak - a true peak is, by construction, at
        // least as high as the highest actual sample.
        p_Out.truePeakDb[c] = LinearToDb(std::max(peak[c], truePeak[c]));
    }
}

#ifdef _WIN32

// The target host's process id (Resolve's or Premiere's), found by exe name
// in the process list - matched by filename rather than any window title (a
// title match false-positived on an unrelated Explorer window the one time it
// was tried - see ERRORS.md in the old Python tree).
//
// Deliberately NOT window enumeration, which this used to share with
// main.cpp's FocusResolveWindow. That called GetWindowTextLengthW on every
// top-level window, including Scope Deck's own, and for a window in this
// process that is a message sent to the UI thread and waited on. At shutdown
// the UI thread stops pumping messages and then joins this thread: if this
// thread was mid-enumeration - it re-searches every second while Resolve is
// not running - each waited on the other forever, and closing the app hung
// until Windows killed it. A process snapshot sends no window messages at all.
static DWORD FindHostProcessId(int p_Target)
{
    const wchar_t* exe = (p_Target == static_cast<int>(AudioTarget::Premiere))
        ? L"Adobe Premiere Pro.exe" : L"Resolve.exe";

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    DWORD pid = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, exe) == 0)
        {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return pid;
}

// ActivateAudioInterfaceAsync is asynchronous by design (it is a real IPC
// round trip to the audio engine), so it needs somewhere to deliver its
// result - this thread just blocks on m_Done, which is the same thing a
// synchronous call would do, without actually being one.
//
// It also needs to hand this completion callback across an apartment/thread
// boundary to actually deliver that result, which a hand-rolled COM object
// with no marshaling support at all cannot be passed through - this is
// exactly the E_POINTER ActivateAudioInterfaceAsync's own synchronous return
// turned out to be (confirmed by comparing this class against Microsoft's
// own ApplicationLoopback sample line-by-line: its equivalent callback
// object derives from WRL's FtmBase - a free-threaded marshaler - which
// this class had nothing equivalent to). Fixed by aggregating the standard
// free-threaded marshaler (CoCreateFreeThreadedMarshaler) and delegating any
// QueryInterface this object doesn't handle itself to it, which is what
// lets COM marshal a raw pointer to this object directly instead of needing
// a real proxy/stub for a private interface that has none registered.
class ActivationCompletionHandler : public IActivateAudioInterfaceCompletionHandler
{
public:
    ActivationCompletionHandler()
    {
        CoCreateFreeThreadedMarshaler(
            static_cast<IActivateAudioInterfaceCompletionHandler*>(this), &m_Marshaler);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** p_Ppv) override
    {
        if (!p_Ppv) return E_POINTER;
        if (riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == __uuidof(IUnknown))
        {
            *p_Ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (m_Marshaler)
            return m_Marshaler->QueryInterface(riid, p_Ppv);
        *p_Ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_Ref); }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG count = InterlockedDecrement(&m_Ref);
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* p_Op) override
    {
        HRESULT hrActivate = E_FAIL;
        IUnknown* unknown = nullptr;
        p_Op->GetActivateResult(&hrActivate, &unknown);

        m_Result = hrActivate;
        if (SUCCEEDED(hrActivate) && unknown)
            unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&m_Client));
        if (unknown) unknown->Release();

        SetEvent(m_Done);
        return S_OK;
    }

    HRESULT        m_Result = E_FAIL;
    IAudioClient*  m_Client = nullptr;
    HANDLE         m_Done   = nullptr;

private:
    ~ActivationCompletionHandler() { if (m_Marshaler) m_Marshaler->Release(); }

    LONG      m_Ref       = 1;
    IUnknown* m_Marshaler = nullptr;
};

// One open stream at a time, torn down whenever the bridge goes inactive or a
// call fails - reopened fresh next time something needs it, rather than
// trying to nurse a half-broken IAudioClient back to health. Resolve exiting
// (or not having started yet) is the ordinary case here, not an edge case -
// Open() re-resolves its process id every time rather than assuming a
// previous PID is still good.
struct LoopbackStream
{
    static constexpr int kChannels = 2;
    static_assert(kChannels == AudioRingSnapshot::kChannels, "AccumulatePacket and the ring assume this layout");

    IAudioClient*              audioClient   = nullptr;
    IAudioCaptureClient*       captureClient = nullptr;
    ActivationCompletionHandler* handler     = nullptr;

    // p_OutStatus/p_OutHr are filled on every path, success or failure - the
    // caller publishes them either way, so the panel can say exactly which
    // stage things are stuck at instead of one flat "no audio" message.
    bool Open(int p_Target, AudioMeterStatus& p_OutStatus, long& p_OutHr)
    {
        Close();
        p_OutHr = 0;

        const DWORD pid = FindHostProcessId(p_Target);
        if (pid == 0)
        {
            p_OutStatus = AudioMeterStatus::ResolveNotFound;
            return false;
        }

        AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
        activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activationParams.ProcessLoopbackParams.TargetProcessId = pid;
        // Includes Resolve's own child/helper processes, not just its main
        // one - it is still all Resolve's own audio, not anyone else's.
        activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT prop = {};
        prop.vt               = VT_BLOB;
        prop.blob.cbSize       = sizeof(activationParams);
        prop.blob.pBlobData    = reinterpret_cast<BYTE*>(&activationParams);

        handler = new ActivationCompletionHandler();
        handler->m_Done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!handler->m_Done)
        {
            p_OutStatus = AudioMeterStatus::ActivationCallFailed;
            p_OutHr = long(HRESULT_FROM_WIN32(GetLastError()));
            Close();
            return false;
        }

        IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
        const HRESULT activateHr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &prop, handler, &asyncOp);
        if (asyncOp) asyncOp->Release();
        if (FAILED(activateHr))
        {
            // The synchronous return of the activation call itself - distinct
            // from GetActivateResult below, which is the *asynchronous*
            // engine-side result delivered later through ActivateCompleted.
            // The two report failure through completely different paths, so
            // collapsing them into one status would hide which stage this
            // actually got stuck at.
            p_OutStatus = AudioMeterStatus::ActivationCallFailed;
            p_OutHr = long(activateHr);
            Close();
            return false;
        }

        // The activation itself is a local engine call, not real I/O - two
        // seconds is generous headroom, not a real-world timeout.
        if (WaitForSingleObject(handler->m_Done, 2000) != WAIT_OBJECT_0)
        {
            p_OutStatus = AudioMeterStatus::ActivationTimedOut;
            Close();
            return false;
        }
        if (FAILED(handler->m_Result) || !handler->m_Client)
        {
            p_OutStatus = AudioMeterStatus::ActivationResultFailed;
            p_OutHr = long(handler->m_Result);
            Close();
            return false;
        }

        audioClient = handler->m_Client;
        handler->m_Client = nullptr;   // ownership moved to audioClient

        // Process loopback has no real device behind it to ask GetMixFormat
        // of - the engine resamples/downmixes the target process's own audio
        // into whatever format Initialize is given here instead, so this
        // just picks one (the same canonical format Microsoft's own
        // ApplicationLoopback sample uses) rather than querying for one.
        WAVEFORMATEX format         = {};
        format.wFormatTag           = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels            = kChannels;
        format.nSamplesPerSec       = 48000;
        format.wBitsPerSample       = 32;
        format.nBlockAlign          = WORD(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec      = format.nSamplesPerSec * format.nBlockAlign;

        // 200ms buffer: generous for a meter (this is not a low-latency
        // monitoring path), and long enough that a busy system missing a
        // poll by tens of milliseconds still can't overrun it.
        constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;   // 100ns units
        HRESULT hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                              kBufferDuration, 0, &format, nullptr);
        if (FAILED(hr))
        {
            p_OutStatus = AudioMeterStatus::StreamInitFailed;
            p_OutHr = long(hr);
            Close();
            return false;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr))
        {
            p_OutStatus = AudioMeterStatus::StreamInitFailed;
            p_OutHr = long(hr);
            Close();
            return false;
        }

        hr = audioClient->Start();
        if (FAILED(hr))
        {
            p_OutStatus = AudioMeterStatus::StreamInitFailed;
            p_OutHr = long(hr);
            Close();
            return false;
        }

        p_OutStatus = AudioMeterStatus::Capturing;
        return true;
    }

    void Close()
    {
        if (audioClient)    audioClient->Stop();
        if (captureClient) { captureClient->Release(); captureClient = nullptr; }
        if (audioClient)   { audioClient->Release(); audioClient = nullptr; }
        if (handler)
        {
            if (handler->m_Client) handler->m_Client->Release();
            if (handler->m_Done)   CloseHandle(handler->m_Done);
            handler->Release();
            handler = nullptr;
        }
    }

    ~LoopbackStream() { Close(); }
};

static void CaptureThread()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return;   // nothing usable without COM - the bridge just reports silence forever

    LoopbackStream stream;
    bool opened = false;
    int  openedTarget = -1;   // whose process the open stream taps
    auto lastPacket = std::chrono::steady_clock::now();

    while (!g_AmQuit)
    {
        const bool active = g_AmActive.load(std::memory_order_relaxed);
        const int  target = g_AmTarget.load(std::memory_order_relaxed);

        // The input switched hosts: drop the old host's stream; the pass below
        // opens the new one's.
        if (opened && target != openedTarget)
        {
            stream.Close();
            opened = false;
        }

        if (!active)
        {
            if (opened) { stream.Close(); opened = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (!opened)
        {
            AudioMeterLevels openResult;
            long             openHr = 0;
            opened = stream.Open(target, openResult.status, openHr);
            openedTarget = target;
            openResult.lastHresult = openHr;
            PublishLevels(openResult);   // deviceOk stays false either way - true only once a real packet lands

            if (!opened)
            {
                // A second between retries, in steps, so closing the app is
                // not held up by however much of it is left.
                for (int i = 0; i < 10 && !g_AmQuit; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            lastPacket = std::chrono::steady_clock::now();
        }

        UINT32 packetFrames = 0;
        if (FAILED(stream.captureClient->GetNextPacketSize(&packetFrames)))
        {
            // Most likely Resolve exited - drop the stream and let the next
            // loop iteration look for it fresh rather than assume it'll come
            // back on the same activation.
            stream.Close();
            opened = false;
            continue;
        }

        if (packetFrames == 0)
        {
            // Nothing new - Resolve itself may still be running but silent.
            // Decay to silence after a beat rather than holding whatever
            // level was last measured indefinitely.
            const auto elapsed = std::chrono::steady_clock::now() - lastPacket;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 500)
            {
                AudioMeterLevels idle;
                idle.status = AudioMeterStatus::Capturing;   // stream is fine, just nothing arriving
                PublishLevels(idle);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        BYTE* data = nullptr;
        DWORD flags = 0;
        if (FAILED(stream.captureClient->GetBuffer(&data, &packetFrames, &flags, nullptr, nullptr)))
        {
            stream.Close();
            opened = false;
            continue;
        }

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

        AudioMeterLevels levels;
        levels.status = AudioMeterStatus::Capturing;
        AccumulatePacket(data, packetFrames, silent, levels);
        PublishLevels(levels);

        // Silent packets' own buffer contents are undefined per WASAPI (the
        // SILENT flag means "treat as silence", not "this happens to be
        // zeroed memory") - write real zeros for the ring buffer rather than
        // whatever garbage/stale data the engine handed back.
        if (silent)
        {
            static thread_local std::vector<float> zeros;
            zeros.assign(size_t(packetFrames) * AudioRingSnapshot::kChannels, 0.0f);
            AppendRingSamples(zeros.data(), packetFrames);
        }
        else
        {
            AppendRingSamples(reinterpret_cast<const float*>(data), packetFrames);
        }

        lastPacket = std::chrono::steady_clock::now();

        stream.captureClient->ReleaseBuffer(packetFrames);
    }

    if (opened) stream.Close();
    CoUninitialize();
}

#elif defined(__APPLE__)

// Bundle id of the host to tap. Premiere has no other support on macOS, so
// asking for it simply reports "isn't running" unless it happens to be.
static const char* TargetBundleId(int p_Target)
{
    return (p_Target == static_cast<int>(AudioTarget::Premiere)) ? mac::kPremiereBundleId : mac::kResolveBundleId;
}

// When the tap last delivered a buffer, in steady_clock nanoseconds - written
// on Core Audio's I/O thread, read by the supervisor below for the idle decay.
static std::atomic<int64_t> g_AmLastBufferNs{ 0 };

static int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The tap runs at whatever rate the host's output device does, unlike
// Windows' fixed 48 kHz. A change empties the ring rather than leave the
// Spectrum Analyzer reading old-rate samples against the new rate's
// frequency axis - CopyLastFrames' callers already cope with a ring that
// has not filled yet.
static void SetRingSampleRate(int p_SampleRate)
{
    if (p_SampleRate <= 0) return;
    std::lock_guard<std::mutex> lock(g_AmMutex);
    if (g_AmRing.sampleRate != p_SampleRate)
    {
        g_AmRing.sampleRate = p_SampleRate;
        g_AmRing.writeCount = 0;
    }
}

// The per-buffer half of what Windows' CaptureThread does per packet, run
// on Core Audio's I/O thread as each buffer arrives instead of polled for -
// the mutex-guarded publishes are short copies, fine for a meter. Digital
// silence is published like any other buffer, the way Windows publishes a
// SILENT packet: a tapped host that is paused reads as a meter at the floor,
// not as "waiting for audio".
static void OnTapFrames(const float* p_Interleaved, uint32_t p_Frames, void*)
{
    AudioMeterLevels levels;
    levels.status = AudioMeterStatus::Capturing;
    AccumulatePacket(reinterpret_cast<const uint8_t*>(p_Interleaved), p_Frames, false, levels);
    PublishLevels(levels);
    AppendRingSamples(p_Interleaved, p_Frames);
    g_AmLastBufferNs.store(SteadyNowNs(), std::memory_order_relaxed);
}

// The supervisor: opens, closes and retries the tap, and notices the host
// going away - the buffers themselves never pass through here (see
// OnTapFrames). Same states and the same once-a-second retry as Windows'
// CaptureThread, so the panel's status reads the same on both.
static void CaptureThread()
{
    mac::ProcessAudioTap tap;
    bool opened       = false;
    int  openedTarget = -1;   // whose process the open tap taps
    int  hostPid      = 0;
    auto lastWatch    = std::chrono::steady_clock::now();
    mac::AudioCapturePermission openedPermission = mac::AudioCapturePermission::Unknown;

    while (!g_AmQuit)
    {
        const bool active = g_AmActive.load(std::memory_order_relaxed);
        const int  target = g_AmTarget.load(std::memory_order_relaxed);

        // The input switched hosts: drop the old host's tap; the pass below
        // opens the new one's.
        if (opened && target != openedTarget)
        {
            tap.Close();
            opened = false;
        }

        if (!active)
        {
            if (opened) { tap.Close(); opened = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (!opened)
        {
            AudioMeterLevels openResult;
            long             openStatus = 0;
            int              sampleRate = 0;

            // Re-resolved every time, never remembered: a restarted host is
            // a new pid.
            const char* bundleId = TargetBundleId(target);
            hostPid = mac::RunningAppPid(bundleId);
            if (hostPid == 0)
            {
                openResult.status = AudioMeterStatus::ResolveNotFound;
            }
            else
            {
                openedPermission = mac::QueryAudioCapturePermission();
                opened = tap.Open(hostPid, bundleId, OnTapFrames, nullptr, openResult.status, openStatus, sampleRate);
            }
            openedTarget = target;
            openResult.lastHresult = openStatus;
            if (opened)
            {
                SetRingSampleRate(sampleRate);
                g_AmLastBufferNs.store(SteadyNowNs(), std::memory_order_relaxed);
            }
            PublishLevels(openResult);   // deviceOk stays false either way - true only once a real buffer lands

            if (!opened)
            {
                // A second between retries, in steps, so closing the app is
                // not held up by however much of it is left.
                for (int i = 0; i < 10 && !g_AmQuit; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            lastWatch = std::chrono::steady_clock::now();
        }

        // The host exited. Unlike WASAPI, a tap does not fail when its
        // process goes away - it just goes on delivering silence - so this
        // has to be noticed here rather than by an error. The next pass
        // looks for the host fresh.
        if (!mac::ProcessAlive(hostPid))
        {
            tap.Close();
            opened = false;
            continue;
        }

        // Once a second, the things that make the open tap stale without
        // making it fail: the host's helper processes or the output's sample
        // rate changed under it (see ProcessAudioTap::IsStale), or the user
        // changed the audio recording permission since the tap was made - a
        // tap made before the permission was granted stays silent. Either
        // way, start over; a denial is then reported by the reopen itself.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastWatch >= std::chrono::seconds(1))
        {
            lastWatch = now;
            if (tap.IsStale() || mac::QueryAudioCapturePermission() != openedPermission)
            {
                tap.Close();
                opened = false;
                continue;
            }
        }

        // Nothing new - the host itself may still be running but not
        // playing (a tap only starts delivering once its process first makes
        // sound). Decay to silence after a beat rather than holding whatever
        // level was last measured indefinitely.
        const int64_t sinceBufferNs = SteadyNowNs() - g_AmLastBufferNs.load(std::memory_order_relaxed);
        if (sinceBufferNs > int64_t(500) * 1000000)
        {
            AudioMeterLevels idle;
            idle.status = AudioMeterStatus::Capturing;   // tap is fine, just nothing arriving
            PublishLevels(idle);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (opened) tap.Close();
}

#endif // _WIN32 / __APPLE__

void AudioMeterBridgeStart()
{
#if defined(_WIN32) || defined(__APPLE__)
    g_AmQuit = false;
    g_AmThread = std::thread(CaptureThread);
#endif
}

void AudioMeterBridgeStop()
{
#if defined(_WIN32) || defined(__APPLE__)
    g_AmQuit = true;
    if (g_AmThread.joinable())
        g_AmThread.join();
#endif
}

} // namespace scopedeck
