#pragma once

#include <algorithm>
#include <cstdint>

namespace scopedeck
{
    // Which stage the capture thread is currently at - surfaced in the panel
    // instead of one flat "no audio" message, since "Resolve isn't running",
    // "found Resolve but couldn't tap its audio", and "tapped it, just
    // nothing playing right now" are three very different things to see
    // while chasing down why the meter reads silence.
    // Whose audio to tap: the host that is the app's input. Premiere plays
    // through its own process exactly as Resolve does, so the same
    // per-process loopback capture serves both - only the exe differs.
    enum class AudioTarget { Resolve, Premiere };

    enum class AudioMeterStatus
    {
        ResolveNotFound,        // the target host's process isn't running (named for the original target)
        ActivationCallFailed,   // ActivateAudioInterfaceAsync's own synchronous return failed
        ActivationTimedOut,     // ActivateCompleted never fired within the wait
        ActivationResultFailed, // ActivateCompleted fired, but GetActivateResult reported failure
        StreamInitFailed,       // activation succeeded but IAudioClient::Initialize/GetService/Start didn't
        Capturing,              // stream open and running - see AudioMeterLevels::deviceOk
                                // for whether a real (non-silent) packet has arrived yet
    };

    // A snapshot of the most recently captured Resolve audio levels. Peak and
    // RMS are dBFS (0 dB = full scale, more negative is quieter); channels at
    // or beyond channelCount are meaningless and should not be drawn.
    struct AudioMeterLevels
    {
        static constexpr int kMaxChannels = 8;

        AudioMeterStatus status      = AudioMeterStatus::ResolveNotFound;
        long             lastHresult = 0;   // meaningful for every *Failed status above
        const char*      hostName    = "DaVinci Resolve";   // who is being tapped, for the panels' messages

        bool  deviceOk     = false;
        int   channelCount = 0;
        float peakDb[kMaxChannels];
        float rmsDb[kMaxChannels];

        // Inter-sample ("true") peak - see AccumulatePacket's own comment in
        // AudioMeterBridge.cpp for exactly what this does and does not claim
        // (a real 4x-oversampled estimate, not certified ITU-R BS.1770-4
        // compliance). Never reads below peakDb for the same channel - a
        // true peak is, by construction, at least the sample peak.
        float truePeakDb[kMaxChannels];

        AudioMeterLevels()
        {
            for (int i = 0; i < kMaxChannels; ++i) { peakDb[i] = -100.0f; rmsDb[i] = -100.0f; truePeakDb[i] = -100.0f; }
        }
    };

    // A short rolling window of the raw interleaved samples the capture
    // thread actually receives, published alongside the reduced peak/RMS
    // levels above - the Goniometer and Spectrum Analyzer both need real
    // sample data, not a level that already threw the waveform away. Same
    // in-process, mutex-guarded "just copy the whole small thing" choice
    // AudioMeterLevels itself makes - a few hundred KB, read once a UI frame,
    // not a hot path worth a lock-free ring for.
    struct AudioRingSnapshot
    {
        static constexpr int kChannels = 2;      // matches LoopbackStream::kChannels
        static constexpr int kCapacity = 8192;   // ~170ms at 48kHz - enough for a 4096-sample FFT window with headroom

        float    samples[kCapacity * kChannels] = {};   // interleaved, circular - see CopyLastFrames
        uint64_t writeCount                     = 0;    // total frames ever written (not wrapped)
        int      sampleRate                     = 48000;

        // Unwraps the circular buffer into chronological (oldest-first)
        // order so callers never have to reason about the write position -
        // copies up to p_MaxFrames of the most recent frames into p_Out
        // (interleaved, must hold p_MaxFrames * kChannels floats). Returns
        // how many frames were actually available (<= p_MaxFrames) - callers
        // must check this rather than assuming a full window, since the
        // buffer may not have filled yet.
        int CopyLastFrames(float* p_Out, int p_MaxFrames) const
        {
            const uint64_t available = std::min<uint64_t>(writeCount, uint64_t(kCapacity));
            const int count = int(std::min<uint64_t>(available, uint64_t(std::max(p_MaxFrames, 0))));
            if (count == 0) return 0;

            const uint64_t startFrame = (writeCount - uint64_t(count)) % uint64_t(kCapacity);
            for (int i = 0; i < count; ++i)
            {
                const uint64_t srcFrame = (startFrame + uint64_t(i)) % uint64_t(kCapacity);
                for (int c = 0; c < kChannels; ++c)
                    p_Out[size_t(i) * kChannels + c] = samples[size_t(srcFrame) * kChannels + c];
            }
            return count;
        }
    };

    void AudioMeterBridgeStart();
    void AudioMeterBridgeStop();

    // Call every frame to tell the bridge whether any Audio Meter/Goniometer/
    // Spectrum Analyzer panel is open - the WASAPI loopback stream is only
    // opened while something actually needs it, same reasoning as
    // TimecodeBridgeSetActive. One stream feeds all three consumers.
    void AudioMeterBridgeSetActive(bool p_Active);

    // Call every frame with the app's host. A change drops the open stream and
    // taps the new host's process on the capture thread's next pass.
    void AudioMeterBridgeSetTarget(AudioTarget p_Target);

    AudioMeterLevels    AudioMeterBridgeGetLevels();
    AudioRingSnapshot   AudioMeterBridgeGetRingSnapshot();
}
