// FFT and spectrum-magnitude helpers for the Spectrum Analyzer panel. Kept
// separate from ScopeImages.h/.cpp (video-oriented) and AudioMeterBridge
// (capture) - this is pure DSP over whatever samples a caller already has,
// no capture or rendering concerns of its own.
#pragma once

#include <complex>
#include <vector>

namespace scopedeck
{
    // In-place iterative radix-2 Cooley-Tukey FFT. p_Data.size() must be a
    // power of two (callers pick the window size, so this is enforced by
    // construction rather than checked here).
    void Fft(std::vector<std::complex<float>>& p_Data);

    // Hann-windowed magnitude spectrum (dBFS) of p_Samples (mono,
    // size a power of two - 2048/4096 are the usual choices for a visual
    // spectrum analyzer: enough frequency resolution to separate low-end
    // rumble from 50/60Hz hum, without a window so long it can't track a
    // signal that's still moving). p_OutDb is sized p_Samples.size()/2 - the
    // real-signal Nyquist-limited half of the transform; p_OutDb[i]
    // corresponds to i * sampleRate / p_Samples.size() Hz. Leaves p_OutDb
    // empty (not resized) if p_Samples' size isn't a power of two, so a
    // caller that got this wrong sees "no spectrum" rather than reading
    // stale/wrong bins.
    void ComputeSpectrumDb(const std::vector<float>& p_Samples, std::vector<float>& p_OutDb);
}
