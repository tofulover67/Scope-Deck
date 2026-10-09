#include "AudioAnalysis.h"

#include <algorithm>
#include <cmath>

namespace scopedeck
{

void Fft(std::vector<std::complex<float>>& p_Data)
{
    const size_t n = p_Data.size();
    if (n <= 1) return;

    // Bit-reversal permutation - reorders p_Data in place so the iterative
    // butterfly pass below can work on contiguous pairs at every stage.
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(p_Data[i], p_Data[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * 3.14159265358979323846f / float(len);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));

        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<float> u = p_Data[i + k];
                const std::complex<float> v = p_Data[i + k + len / 2] * w;
                p_Data[i + k]           = u + v;
                p_Data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void ComputeSpectrumDb(const std::vector<float>& p_Samples, std::vector<float>& p_OutDb)
{
    const size_t n = p_Samples.size();
    p_OutDb.clear();
    if (n < 2 || (n & (n - 1)) != 0) return;   // not a power of two

    std::vector<std::complex<float>> buf(n);
    for (size_t i = 0; i < n; ++i)
    {
        // Hann window - low spectral leakage, the standard choice for a
        // visual spectrum analyzer (same reasoning most DAW/scope spectrum
        // views make).
        const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * float(i) / float(n - 1));
        buf[i] = std::complex<float>(p_Samples[i] * w, 0.0f);
    }

    Fft(buf);

    // A Hann window's coherent gain is 0.5 - compensate so a full-scale sine
    // reads back at ~0dB instead of reading ~6dB low across the board.
    const float norm = 2.0f / (float(n) * 0.5f);
    p_OutDb.resize(n / 2);
    for (size_t i = 0; i < n / 2; ++i)
    {
        const float mag = std::abs(buf[i]) * norm;
        constexpr float kFloor = 1e-6f;   // -120dB
        p_OutDb[i] = 20.0f * std::log10(std::max(mag, kFloor));
    }
}

} // namespace scopedeck
