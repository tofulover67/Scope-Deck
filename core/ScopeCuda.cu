// CUDA implementation of the scope reduction. See ScopeCuda.h for what this is
// and who calls it.
//
// The governing rule: this must agree with core/ScopeCore.cpp bit-for-bit, and
// bit-exactness is not a bonus - it is the acceptance criterion the conformance
// harness enforces over 960 cases. Three places where the natural GPU spelling
// silently disagrees with the CPU, all of them found by writing the harness
// before writing this:
//
//   1. Column assignment. The CPU assigns pixel x to the largest column c with
//      floor(c*w/cols) <= x, via a boundary table. floor(x*cols/w) looks
//      equivalent and is not - at UHD it moves 256 of 3840 pixels into a
//      different column. The spike used the wrong one. This uploads a per-x
//      column table built by the same arithmetic the CPU uses.
//   2. Cb/Cr scaling. The CPU precomputes 0.5f/(1-Kb) and 0.5f/(1-Kr) once and
//      multiplies. Dividing per-pixel, or using rounded literals like 0.5389f
//      the way the spike did, rounds differently. The weights also vary across
//      eight colour spaces, so no literal is defensible.
//   3. Fused multiply-add. nvcc contracts a*b+c by default; the luma dot
//      product then lands boundary values in different bins. Built with
//      -fmad=false (see CMakeLists.txt), and the harness refuses to run a
//      contracted build.
//
// The histogram is derived from the waveform by summing over columns, exactly
// as ScopeEngine::Analyse does. That is not a shortcut: both are exact integer
// sums of the same events, and deriving it removes 4 of what would otherwise be
// 11 atomics per pixel.

#include "ScopeCuda.h"

#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace scopedeck
{

namespace
{

// --- bin arithmetic, mirroring ScopeCore.cpp's anonymous-namespace copies -----

__constant__ float kBinScaleDev;
__constant__ float kChromaBinScaleDev;
__constant__ float kTwinDiffBinScaleDev;

__device__ __forceinline__ uint32_t BinOfDev(float p_Value, uint32_t p_BinCount)
{
    const float t = (p_Value - kBinRangeLow) * kBinScaleDev;
    if (!(t > 0.0f)) return 0;                      // also catches NaN
    // +inf; see ScopeCore.cpp's BinOf
    if (t >= static_cast<float>(p_BinCount)) return p_BinCount - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= p_BinCount) ? (p_BinCount - 1) : bin;
}

__device__ __forceinline__ uint32_t ChromaBinOfDev(float p_Value)
{
    const float t = (p_Value - kChromaRangeLow) * kChromaBinScaleDev;
    if (!(t > 0.0f)) return 0;
    if (t >= static_cast<float>(kVectorscopeSize)) return kVectorscopeSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kVectorscopeSize) ? (kVectorscopeSize - 1) : bin;
}

__device__ __forceinline__ uint32_t TwinDiffBinOfDev(float p_Value)
{
    const float t = (p_Value - kTwinPeaksDiffRangeLow) * kTwinDiffBinScaleDev;
    if (!(t > 0.0f)) return 0;
    if (t >= static_cast<float>(kTwinPeaksSize)) return kTwinPeaksSize - 1;
    const uint32_t bin = static_cast<uint32_t>(t);
    return (bin >= kTwinPeaksSize) ? (kTwinPeaksSize - 1) : bin;
}

__device__ __forceinline__ uint64_t WaveformIndexDev(uint32_t p_Plane, uint32_t p_Column, uint32_t p_Level)
{
    return (static_cast<uint64_t>(p_Column) * kWaveformLevels + p_Level) * kPlaneCount + p_Plane;
}

// --- warp-aggregated atomic increment -----------------------------------------
//
// Mandatory, not an optimisation: §3 of the handoff measured plain atomics at
// 14.68 ms worst case, slower than the CPU path this replaces, and 1 frame in
// 178 of real footage hitting 16.76 ms. Aggregation costs nothing where there is
// nothing to aggregate, so it is unconditional.

__device__ __forceinline__ void AggregatedAdd(unsigned int* p_Addr)
{
#if __CUDA_ARCH__ >= 700
    const unsigned int active = __activemask();
    const unsigned int peers  = __match_any_sync(active, reinterpret_cast<unsigned long long>(p_Addr));
    unsigned int lanemaskLt;
    asm("mov.u32 %0, %%lanemask_lt;" : "=r"(lanemaskLt));
    if ((peers & lanemaskLt) == 0u)
        atomicAdd(p_Addr, static_cast<unsigned int>(__popc(peers)));
#else
    atomicAdd(p_Addr, 1u);
#endif
}

// --- float min/max with the CPU's exact NaN behaviour --------------------------
//
// ScopeCore uses bare `if (v < lo)` / `if (v > hi)`, so a NaN never becomes an
// extreme and never propagates. fminf/fmaxf do NOT behave this way for all
// inputs, and an atomicMin on reinterpreted ints gets signed zero and negatives
// wrong. A CAS loop with the same bare comparison is the only spelling that
// reproduces the CPU exactly, including "every pixel was NaN" leaving the
// initial +/-inf in place, which ScopeCore then reports verbatim.

__device__ __forceinline__ void AtomicMinFloat(float* p_Addr, float p_Val)
{
    if (!(p_Val == p_Val)) return;                  // NaN: the CPU's < is false too
    int* asInt = reinterpret_cast<int*>(p_Addr);
    int old = *asInt, assumed;
    do
    {
        assumed = old;
        if (!(p_Val < __int_as_float(assumed))) break;
        old = atomicCAS(asInt, assumed, __float_as_int(p_Val));
    } while (assumed != old);
}

__device__ __forceinline__ void AtomicMaxFloat(float* p_Addr, float p_Val)
{
    if (!(p_Val == p_Val)) return;
    int* asInt = reinterpret_cast<int*>(p_Addr);
    int old = *asInt, assumed;
    do
    {
        assumed = old;
        if (!(p_Val > __int_as_float(assumed))) break;
        old = atomicCAS(asInt, assumed, __float_as_int(p_Val));
    } while (assumed != old);
}

// --- kernels ------------------------------------------------------------------

struct LumaDev { float r, g, b, cbScale, crScale; };

// One thread per sampled pixel. p_RowStep selects rows exactly as the CPU does:
// row index = y * rowStep, covering 0, s, 2s, ... < height.
__global__ void ScatterKernel(const float* __restrict__ p_Pixels,
                              int p_Width, int p_SampledRows, int p_RowStep,
                              size_t p_RowFloats,
                              const int* __restrict__ p_ColOfX,
                              LumaDev p_Luma,
                              unsigned int* __restrict__ p_Waveform,
                              unsigned int* __restrict__ p_Vector,
                              unsigned int* __restrict__ p_TwinPeaks,
                              float* __restrict__ p_MinMax)
{
    const int x  = blockIdx.x * blockDim.x + threadIdx.x;
    const int ry = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p_Width || ry >= p_SampledRows) return;

    const int y = ry * p_RowStep;
    const float* px = p_Pixels + static_cast<size_t>(y) * p_RowFloats + static_cast<size_t>(x) * 4;
    const float r = px[0];
    const float g = px[1];
    const float b = px[2];

    AtomicMinFloat(&p_MinMax[0], r); AtomicMaxFloat(&p_MinMax[3], r);
    AtomicMinFloat(&p_MinMax[1], g); AtomicMaxFloat(&p_MinMax[4], g);
    AtomicMinFloat(&p_MinMax[2], b); AtomicMaxFloat(&p_MinMax[5], b);

    // Same operation order as ScopeCore's `lumaR * r + lumaG * g + lumaB * b`.
    // With -fmad=false this is bit-identical; with contraction on it is not.
    const float luma = p_Luma.r * r + p_Luma.g * g + p_Luma.b * b;

    const uint32_t col = static_cast<uint32_t>(p_ColOfX[x]);

    AggregatedAdd(&p_Waveform[WaveformIndexDev(kPlaneR, col, BinOfDev(r, kWaveformLevels))]);
    AggregatedAdd(&p_Waveform[WaveformIndexDev(kPlaneG, col, BinOfDev(g, kWaveformLevels))]);
    AggregatedAdd(&p_Waveform[WaveformIndexDev(kPlaneB, col, BinOfDev(b, kWaveformLevels))]);
    AggregatedAdd(&p_Waveform[WaveformIndexDev(kPlaneY, col, BinOfDev(luma, kWaveformLevels))]);

    const float cb = (b - luma) * p_Luma.cbScale;
    const float cr = (r - luma) * p_Luma.crScale;
    const uint32_t band = (luma < kVectorscopeLowMax) ? kBandLow
                        : ((luma >= kVectorscopeHighMin) ? kBandHigh : kBandMid);
    AggregatedAdd(&p_Vector[(static_cast<uint64_t>(band) * kVectorscopeSize
                             + ChromaBinOfDev(cr)) * kVectorscopeSize + ChromaBinOfDev(cb)]);

    const float levelGB = (g + b) * 0.5f;
    const float diffGB  = (g - b) * 0.5f;
    AggregatedAdd(&p_TwinPeaks[(static_cast<uint64_t>(kDiamondGreenBlue) * kTwinPeaksCells)
                               + static_cast<uint64_t>(BinOfDev(levelGB, kTwinPeaksSize)) * kTwinPeaksSize
                               + TwinDiffBinOfDev(diffGB)]);

    const float levelGR = (g + r) * 0.5f;
    const float diffGR  = (g - r) * 0.5f;
    AggregatedAdd(&p_TwinPeaks[(static_cast<uint64_t>(kDiamondGreenRed) * kTwinPeaksCells)
                               + static_cast<uint64_t>(BinOfDev(levelGR, kTwinPeaksSize)) * kTwinPeaksSize
                               + TwinDiffBinOfDev(diffGR)]);
}

// The histogram is the waveform summed over columns - an identity, since both
// use the same bin count and range. One thread per (plane, level).
__global__ void HistogramKernel(const unsigned int* __restrict__ p_Waveform,
                                unsigned int* __restrict__ p_Histogram)
{
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= kHistogramBins * kPlaneCount) return;

    const uint32_t plane = idx / kHistogramBins;
    const uint32_t level = idx % kHistogramBins;

    unsigned int sum = 0;
    for (uint32_t col = 0; col < kWaveformColumns; ++col)
        sum += p_Waveform[WaveformIndexDev(plane, col, level)];

    p_Histogram[plane * kHistogramBins + level] = sum;
}

// One thread per (trace row, column). The inner sum walks the column's pixel
// range in ascending x, sequentially - NOT a parallel reduction. Float addition
// is not associative, so a tree sum would give a different, equally valid answer
// and fail the exact comparison. Buckets are 1-8 pixels wide, so there is
// nothing to gain from parallelising them anyway.
__global__ void TraceKernel(const float* __restrict__ p_Pixels,
                            int p_Height, size_t p_RowFloats,
                            const int* __restrict__ p_ColStart,
                            LumaDev p_Luma,
                            float* __restrict__ p_Trace)
{
    const uint32_t col = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t row = blockIdx.y * blockDim.y + threadIdx.y;
    if (col >= kWaveformColumns || row >= kWaveformTraceRows) return;

    const int srcY = (kWaveformTraceRows > 1)
        ? static_cast<int>((static_cast<uint64_t>(row) * static_cast<uint64_t>(p_Height - 1))
                           / (kWaveformTraceRows - 1))
        : 0;

    const float* rowPixels = p_Pixels + static_cast<size_t>(srcY) * p_RowFloats;
    const int x0 = p_ColStart[col];
    const int x1 = p_ColStart[col + 1];

    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    int counted = 0;
    for (int x = x0; x < x1; ++x)
    {
        const float* px = rowPixels + static_cast<size_t>(x) * 4;
        sumR += px[0];
        sumG += px[1];
        sumB += px[2];
        ++counted;
    }

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (counted > 0)
    {
        const float inv = 1.0f / static_cast<float>(counted);
        r = sumR * inv;
        g = sumG * inv;
        b = sumB * inv;
    }
    const float luma = p_Luma.r * r + p_Luma.g * g + p_Luma.b * b;

    const uint64_t base = (static_cast<uint64_t>(row) * kWaveformColumns + col) * kPlaneCount;
    p_Trace[base + kPlaneR] = r;
    p_Trace[base + kPlaneG] = g;
    p_Trace[base + kPlaneB] = b;
    p_Trace[base + kPlaneY] = luma;
}

// Centre pixel, untouched - the value that gets cross-checked against Resolve's
// own colour picker.
__global__ void ProbeKernel(const float* __restrict__ p_Pixels,
                            int p_Width, int p_Height, size_t p_RowFloats,
                            float* __restrict__ p_Probe)
{
    const float* px = p_Pixels + static_cast<size_t>(p_Height / 2) * p_RowFloats
                    + static_cast<size_t>(p_Width / 2) * 4;
    p_Probe[0] = px[0];
    p_Probe[1] = px[1];
    p_Probe[2] = px[2];
}

const char* Err(cudaError_t e) { return cudaGetErrorString(e); }

} // namespace

// -----------------------------------------------------------------------------

struct CudaReducer::Impl
{
    bool  available = false;
    char  error[256] = {};
    char  device[256] = {};

    unsigned int* waveform   = nullptr;
    unsigned int* histogram  = nullptr;
    unsigned int* vector     = nullptr;
    unsigned int* twinPeaks  = nullptr;
    float*        trace      = nullptr;
    float*        minMax     = nullptr;      // [minR,minG,minB,maxR,maxG,maxB]
    float*        probe      = nullptr;
    int*          colOfX     = nullptr;
    int*          colStart   = nullptr;
    float*        pixels     = nullptr;

    size_t pixelCapacity = 0;
    int    colCapacity   = 0;

    bool Fail(const char* where, cudaError_t e)
    {
        std::snprintf(error, sizeof(error), "%s: %s", where, Err(e));
        return false;
    }
};

CudaReducer::CudaReducer() : m_Impl(new Impl)
{
    Impl& impl = *m_Impl;

    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess || count == 0)
    {
        std::snprintf(impl.error, sizeof(impl.error), "no CUDA device: %s",
                      err == cudaSuccess ? "count is zero" : Err(err));
        return;
    }

    int device = 0;
    cudaGetDevice(&device);
    cudaDeviceProp prop;
    std::memset(&prop, 0, sizeof(prop));
    if (cudaGetDeviceProperties(&prop, device) == cudaSuccess)
    {
        int runtime = 0, driver = 0;
        cudaRuntimeGetVersion(&runtime);
        cudaDriverGetVersion(&driver);
        std::snprintf(impl.device, sizeof(impl.device),
                      "device %d '%s' sm_%d%d SMs=%d runtime=%d driver=%d",
                      device, prop.name, prop.major, prop.minor,
                      prop.multiProcessorCount, runtime, driver);
    }

    // The three bin scales are compile-time constants on the CPU side; upload
    // them once rather than recomputing per thread, so the device uses the
    // identical float value rather than one re-derived at a different precision.
    const float binScale   = static_cast<float>(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);
    const float chromaScale = static_cast<float>(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);
    const float twinScale  = static_cast<float>(kTwinPeaksSize - 1) /
                             (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);

    if ((err = cudaMemcpyToSymbol(kBinScaleDev, &binScale, sizeof(float))) != cudaSuccess)
    { impl.Fail("cudaMemcpyToSymbol binScale", err); return; }
    if ((err = cudaMemcpyToSymbol(kChromaBinScaleDev, &chromaScale, sizeof(float))) != cudaSuccess)
    { impl.Fail("cudaMemcpyToSymbol chromaScale", err); return; }
    if ((err = cudaMemcpyToSymbol(kTwinDiffBinScaleDev, &twinScale, sizeof(float))) != cudaSuccess)
    { impl.Fail("cudaMemcpyToSymbol twinScale", err); return; }

    struct Alloc { void** ptr; size_t bytes; const char* name; };
    const Alloc allocs[] = {
        { reinterpret_cast<void**>(&impl.waveform),  static_cast<size_t>(kWaveformCells) * sizeof(unsigned int), "waveform" },
        { reinterpret_cast<void**>(&impl.histogram), static_cast<size_t>(kHistogramCells) * sizeof(unsigned int), "histogram" },
        { reinterpret_cast<void**>(&impl.vector),    static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int), "vectorscope" },
        { reinterpret_cast<void**>(&impl.twinPeaks), static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int), "twinPeaks" },
        { reinterpret_cast<void**>(&impl.trace),     static_cast<size_t>(kWaveformTraceCells) * sizeof(float), "trace" },
        { reinterpret_cast<void**>(&impl.minMax),    6 * sizeof(float), "minMax" },
        { reinterpret_cast<void**>(&impl.probe),     3 * sizeof(float), "probe" },
    };
    for (const Alloc& a : allocs)
    {
        if ((err = cudaMalloc(a.ptr, a.bytes)) != cudaSuccess) { impl.Fail(a.name, err); return; }
    }

    impl.available = true;
}

CudaReducer::~CudaReducer()
{
    if (!m_Impl) return;
    Impl& impl = *m_Impl;
    cudaFree(impl.waveform);  cudaFree(impl.histogram); cudaFree(impl.vector);
    cudaFree(impl.twinPeaks); cudaFree(impl.trace);     cudaFree(impl.minMax);
    cudaFree(impl.probe);     cudaFree(impl.colOfX);    cudaFree(impl.colStart);
    cudaFree(impl.pixels);
    delete m_Impl;
}

bool CudaReducer::Available() const { return m_Impl && m_Impl->available; }
const char* CudaReducer::Error() const { return m_Impl ? m_Impl->error : "no context"; }
const char* CudaReducer::DeviceDescription() const { return m_Impl ? m_Impl->device : ""; }

bool CudaReducer::Analyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out,
                          CudaReduceTiming* p_Timing)
{
    Impl& impl = *m_Impl;
    if (!impl.available) return false;

    p_Out.waveform.assign(static_cast<size_t>(kWaveformCells), 0u);
    p_Out.histogram.assign(static_cast<size_t>(kHistogramCells), 0u);
    p_Out.vectorscope.assign(static_cast<size_t>(kVectorscopeTotalCells), 0u);
    p_Out.twinPeaks.assign(static_cast<size_t>(kTwinPeaksTotalCells), 0u);
    p_Out.waveformTrace.assign(static_cast<size_t>(kWaveformTraceCells), 0.0f);
    p_Out.pixelsSampled = 0;
    p_Out.colorSpace = p_Params.colorSpace;
    p_Out.luma = p_Params.luma;
    for (int c = 0; c < 3; ++c) { p_Out.minRGB[c] = 0.0f; p_Out.maxRGB[c] = 0.0f; p_Out.probeRGB[c] = 0.0f; }

    const int width  = p_Frame.width;
    const int height = p_Frame.height;
    if (!p_Frame.pixels || width <= 0 || height <= 0) return true;

    int rowStep = p_Params.rowStep;
    if (rowStep < 1) rowStep = 1;

    cudaError_t err = cudaSuccess;

    // --- column boundary table, built by the CPU's own arithmetic -------------
    std::vector<int> colStart(static_cast<size_t>(kWaveformColumns) + 1);
    for (uint32_t col = 0; col <= kWaveformColumns; ++col)
    {
        const uint64_t x = (static_cast<uint64_t>(col) * static_cast<uint64_t>(width)) / kWaveformColumns;
        colStart[col] = static_cast<int>(x < static_cast<uint64_t>(width) ? x : static_cast<uint64_t>(width));
    }
    colStart[kWaveformColumns] = width;

    // Per-x column lookup, derived from that table rather than from a division.
    std::vector<int> colOfX(static_cast<size_t>(width));
    {
        uint32_t c = 0;
        for (int x = 0; x < width; ++x)
        {
            while (c + 1 <= kWaveformColumns && colStart[c + 1] <= x) ++c;
            colOfX[static_cast<size_t>(x)] = static_cast<int>(c);
        }
    }

    if (impl.colCapacity < width)
    {
        cudaFree(impl.colOfX); impl.colOfX = nullptr;
        if ((err = cudaMalloc(&impl.colOfX, static_cast<size_t>(width) * sizeof(int))) != cudaSuccess)
            return impl.Fail("cudaMalloc colOfX", err);
        impl.colCapacity = width;
    }
    if (!impl.colStart)
    {
        if ((err = cudaMalloc(&impl.colStart, (static_cast<size_t>(kWaveformColumns) + 1) * sizeof(int))) != cudaSuccess)
            return impl.Fail("cudaMalloc colStart", err);
    }
    if ((err = cudaMemcpy(impl.colOfX, colOfX.data(), colOfX.size() * sizeof(int), cudaMemcpyHostToDevice)) != cudaSuccess)
        return impl.Fail("H2D colOfX", err);
    if ((err = cudaMemcpy(impl.colStart, colStart.data(), colStart.size() * sizeof(int), cudaMemcpyHostToDevice)) != cudaSuccess)
        return impl.Fail("H2D colStart", err);

    // --- upload the frame ----------------------------------------------------
    //
    // Conformance-only: the tap's frame is already device memory. The upload is
    // a tightly packed copy regardless of the source's rowBytes, so a negative
    // or padded stride on the host side becomes a clean stride on the device.
    const size_t rowFloats = static_cast<size_t>(width) * 4;
    const size_t frameBytes = rowFloats * static_cast<size_t>(height) * sizeof(float);
    if (impl.pixelCapacity < frameBytes)
    {
        cudaFree(impl.pixels); impl.pixels = nullptr;
        if ((err = cudaMalloc(&impl.pixels, frameBytes)) != cudaSuccess)
            return impl.Fail("cudaMalloc pixels", err);
        impl.pixelCapacity = frameBytes;
    }
    // Timing events exist only when someone asked for them, so the conformance
    // run - which calls this 960 times and cares about answers, not speed -
    // pays nothing. Created per call rather than per reducer because that is
    // outside the span being measured and keeps the lifetime obvious.
    cudaEvent_t evUpload0 = nullptr, evUpload1 = nullptr, evKernels1 = nullptr;
    if (p_Timing)
    {
        *p_Timing = CudaReduceTiming{};
        if (cudaEventCreate(&evUpload0) != cudaSuccess ||
            cudaEventCreate(&evUpload1) != cudaSuccess ||
            cudaEventCreate(&evKernels1) != cudaSuccess)
        {
            cudaGetLastError();
            if (evUpload0)  { cudaEventDestroy(evUpload0);  evUpload0 = nullptr; }
            if (evUpload1)  { cudaEventDestroy(evUpload1);  evUpload1 = nullptr; }
            if (evKernels1) { cudaEventDestroy(evKernels1); evKernels1 = nullptr; }
        }
    }

    if (evUpload0) cudaEventRecord(evUpload0);
    for (int y = 0; y < height; ++y)
    {
        const float* srcRow = p_Frame.Row(y);
        if (!srcRow) continue;
        if ((err = cudaMemcpy(impl.pixels + static_cast<size_t>(y) * rowFloats, srcRow,
                              rowFloats * sizeof(float), cudaMemcpyHostToDevice)) != cudaSuccess)
            return impl.Fail("H2D pixels", err);
    }
    if (evUpload1) cudaEventRecord(evUpload1);

    // --- clear ---------------------------------------------------------------
    cudaMemset(impl.waveform, 0, static_cast<size_t>(kWaveformCells) * sizeof(unsigned int));
    cudaMemset(impl.vector, 0, static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int));
    cudaMemset(impl.twinPeaks, 0, static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int));

    const float inf = std::numeric_limits<float>::infinity();
    const float seed[6] = { inf, inf, inf, -inf, -inf, -inf };
    if ((err = cudaMemcpy(impl.minMax, seed, sizeof(seed), cudaMemcpyHostToDevice)) != cudaSuccess)
        return impl.Fail("H2D minMax seed", err);

    // --- launch --------------------------------------------------------------
    const int sampledRows = (height - 1) / rowStep + 1;

    LumaDev luma;
    luma.r = p_Params.luma.r;
    luma.g = p_Params.luma.g;
    luma.b = p_Params.luma.b;
    luma.cbScale = 0.5f / (1.0f - p_Params.luma.b);
    luma.crScale = 0.5f / (1.0f - p_Params.luma.r);

    {
        const dim3 block(32, 8);
        const dim3 grid((width + block.x - 1) / block.x,
                        (sampledRows + block.y - 1) / block.y);
        ScatterKernel<<<grid, block>>>(impl.pixels, width, sampledRows, rowStep, rowFloats,
                                       impl.colOfX, luma,
                                       impl.waveform, impl.vector, impl.twinPeaks, impl.minMax);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("ScatterKernel", err);
    }
    {
        const int total = static_cast<int>(kHistogramBins * kPlaneCount);
        HistogramKernel<<<(total + 255) / 256, 256>>>(impl.waveform, impl.histogram);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("HistogramKernel", err);
    }
    {
        const dim3 block(32, 4);
        const dim3 grid((kWaveformColumns + block.x - 1) / block.x,
                        (kWaveformTraceRows + block.y - 1) / block.y);
        TraceKernel<<<grid, block>>>(impl.pixels, height, rowFloats, impl.colStart, luma, impl.trace);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("TraceKernel", err);
    }
    ProbeKernel<<<1, 1>>>(impl.pixels, width, height, rowFloats, impl.probe);
    if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("ProbeKernel", err);

    if (evKernels1) cudaEventRecord(evKernels1);

    // --- read back -----------------------------------------------------------
    if ((err = cudaMemcpy(p_Out.waveform.data(), impl.waveform,
                          static_cast<size_t>(kWaveformCells) * sizeof(unsigned int),
                          cudaMemcpyDeviceToHost)) != cudaSuccess) return impl.Fail("D2H waveform", err);
    if ((err = cudaMemcpy(p_Out.histogram.data(), impl.histogram,
                          static_cast<size_t>(kHistogramCells) * sizeof(unsigned int),
                          cudaMemcpyDeviceToHost)) != cudaSuccess) return impl.Fail("D2H histogram", err);
    if ((err = cudaMemcpy(p_Out.vectorscope.data(), impl.vector,
                          static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int),
                          cudaMemcpyDeviceToHost)) != cudaSuccess) return impl.Fail("D2H vectorscope", err);
    if ((err = cudaMemcpy(p_Out.twinPeaks.data(), impl.twinPeaks,
                          static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int),
                          cudaMemcpyDeviceToHost)) != cudaSuccess) return impl.Fail("D2H twinPeaks", err);
    if ((err = cudaMemcpy(p_Out.waveformTrace.data(), impl.trace,
                          static_cast<size_t>(kWaveformTraceCells) * sizeof(float),
                          cudaMemcpyDeviceToHost)) != cudaSuccess) return impl.Fail("D2H trace", err);

    float minMax[6] = {};
    if ((err = cudaMemcpy(minMax, impl.minMax, sizeof(minMax), cudaMemcpyDeviceToHost)) != cudaSuccess)
        return impl.Fail("D2H minMax", err);
    float probe[3] = {};
    if ((err = cudaMemcpy(probe, impl.probe, sizeof(probe), cudaMemcpyDeviceToHost)) != cudaSuccess)
        return impl.Fail("D2H probe", err);

    if ((err = cudaDeviceSynchronize()) != cudaSuccess) return impl.Fail("sync", err);

    // After the sync, so every event has completed and elapsed time is a read.
    if (evKernels1)
    {
        float msUp = 0.0f, msKern = 0.0f;
        const bool ok = cudaEventElapsedTime(&msUp, evUpload0, evUpload1) == cudaSuccess &&
                        cudaEventElapsedTime(&msKern, evUpload1, evKernels1) == cudaSuccess;
        if (!ok) cudaGetLastError();
        if (ok && p_Timing)
        {
            p_Timing->msUpload  = msUp;
            p_Timing->msKernels = msKern;
            p_Timing->valid     = true;
        }
    }
    if (evUpload0)  cudaEventDestroy(evUpload0);
    if (evUpload1)  cudaEventDestroy(evUpload1);
    if (evKernels1) cudaEventDestroy(evKernels1);

    // Analytic, and identical to the CPU's count: every sampled row contributes
    // exactly `width` pixels, since the column boundaries partition [0, width).
    p_Out.pixelsSampled = static_cast<uint64_t>(sampledRows) * static_cast<uint64_t>(width);

    const bool sawAny = (p_Out.pixelsSampled > 0);
    for (int c = 0; c < 3; ++c)
    {
        p_Out.minRGB[c] = sawAny ? minMax[c] : 0.0f;
        p_Out.maxRGB[c] = sawAny ? minMax[3 + c] : 0.0f;
        p_Out.probeRGB[c] = probe[c];
    }

    return true;
}

} // namespace scopedeck

// =============================================================================
// GpuTap - the shipping path. See ScopeCuda.h for the measured reasoning behind
// every structural choice here.
// =============================================================================

#include "ScopeShm.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace scopedeck
{

namespace
{

// Preview downscale, matching ScopeEngine::BuildPreview's point sampling and its
// bottom-up flip. Deliberately not "better" filtering: a viewfinder that framed
// differently from the CPU path would be a second thing to explain.
__global__ void PreviewKernel(const float* __restrict__ p_Src,
                              int p_SrcWidth, int p_SrcHeight, size_t p_SrcRowFloats,
                              unsigned char* __restrict__ p_Dst,
                              int p_DstWidth, int p_DstHeight, size_t p_DstRowBytes)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p_DstWidth || y >= p_DstHeight) return;

    const int sx = static_cast<int>((static_cast<uint64_t>(x) * p_SrcWidth) / p_DstWidth);
    const int sy = p_SrcHeight - 1 - static_cast<int>((static_cast<uint64_t>(y) * p_SrcHeight) / p_DstHeight);

    const float* px = p_Src + static_cast<size_t>(sy) * p_SrcRowFloats + static_cast<size_t>(sx) * 4;
    unsigned char* out = p_Dst + static_cast<size_t>(y) * p_DstRowBytes + static_cast<size_t>(x) * 3;
    #pragma unroll
    for (int c = 0; c < 3; ++c)
    {
        float v = px[c];
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        out[c] = static_cast<unsigned char>(v * 255.0f + 0.5f);
    }
}

} // namespace

// -----------------------------------------------------------------------------
// PublishHub - one per process, however many plugin instances Resolve makes.
//
// Everything that is genuinely shared lives here: the mapping, its page-lock,
// the ticket counter, the in-flight ring and the thread that closes seqlocks.
// Per-instance state (device buffers, the publish stream) stays in GpuTap,
// because two instances really do reduce different frames at the same time.
//
// This is not tidiness. Before it existed, each instance owned a publisher and
// allocated tickets from `m_NextTicket` seeded off the shared write counter,
// which is a check-then-act across instances:
//
//   - Two instances could reserve the SAME ticket. ScopeShm.h called that
//     benign - "a lost frame for one of them rather than a torn read" - and
//     that was true while publishing was synchronous. It is not true now.
//     OpenSlot and CommitSlot are plain seq+1 increments with no parity
//     enforcement, so two interleaved opens take the sequence even -> odd ->
//     even: the slot advertises itself as READABLE while two DMAs are still
//     landing in it. A reader then passes the seqlock check on a torn frame.
//   - The GPU port widened that window from a synchronous memcpy to an async
//     D2H that outlives the render call, which is what turned a theoretical
//     race into a likely one.
//   - Per-instance rings also could not see each other, so N instances had
//     N x kSlotCount frames in flight over a ring of kSlotCount slots.
//
// Reserving the ticket and the ring entry under one lock fixes all three: a
// ticket is never handed out twice, and a slot is never reused while its own
// DMA is still in flight.
//
// That covered GPU instances only. Scope Tap's CPU-path instances each had a
// publisher of their own, and the GPU-off fallback published through this one
// with Publish(), and both took the committed counter as their ticket - so
// either could land on a slot a GPU copy was still filling. Reservation now
// lives in ScopeShm's PublishAuthority, shared by every publisher on the block
// in the process: the hub asks it for a slot nobody is writing, and commits
// only ever move the write counter forward, so a synchronous CPU publish may
// finish before an earlier GPU ticket without the counter going backwards
// (tools/ScopePublishContention.cpp).
//
// One registration instead of N also answers the multi-instance pinning
// question by removing it: no overlapping views at different addresses, and no
// teardown hazard from one instance unregistering while another still publishes.
struct PublishHub
{
    ScopePublisher publisher;
    bool   slotPinned = false;
    size_t blockBytes = 0;

    struct Pending
    {
        uint64_t    ticket = 0;
        cudaEvent_t start = nullptr;      // recorded on the host's stream
        cudaEvent_t kernels = nullptr;    // ... likewise
        cudaEvent_t publish = nullptr;    // ... on the owning instance's stream
        bool        inUse = false;
    };
    Pending pending[kSlotCount] = {};

    std::deque<int>         queue;
    mutable std::mutex      mutex;
    std::condition_variable cv;
    std::thread             worker;
    bool                    stopping = false;
    bool                    running = false;
    int                     refs = 0;

    uint64_t published = 0;
    uint64_t skipped = 0;

    double msDeviceTotal = 0.0;
    double msDeviceKernels = 0.0;
    double msDevicePublish = 0.0;
    bool   deviceTimingValid = false;

    // Reserves a ring entry and the ticket that goes with it, both under the
    // one lock. Returns -1 when every slot is still in flight, which skips the
    // frame rather than making a render thread wait.
    int Reserve(uint64_t& p_Ticket)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t i = 0; i < kSlotCount; ++i)
        {
            if (pending[i].inUse) continue;
            // Every block slot can be busy even with a ring entry free: CPU-path
            // instances publish into the same slots.
            if (!publisher.ReserveTicket(p_Ticket)) break;
            pending[i].ticket = p_Ticket;
            pending[i].inUse  = true;
            return static_cast<int>(i);
        }
        ++skipped;
        return -1;
    }

    // Undo a reservation that never got as far as being queued. Gives the block
    // slot back too, so call it exactly once per failed reservation - a second
    // call could free a slot another thread has reserved since.
    void Release(int p_Slot)
    {
        std::lock_guard<std::mutex> lock(mutex);
        publisher.AbandonSlot(pending[p_Slot].ticket);
        pending[p_Slot].inUse = false;
    }

    void Enqueue(int p_Slot)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(p_Slot);
        }
        cv.notify_one();
    }

    void Run()
    {
        for (;;)
        {
            int slot = -1;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) return;              // stopping, nothing left
                slot = queue.front();
                queue.pop_front();
            }

            // Outside the lock: this is the wait the render thread refuses to do.
            cudaEventSynchronize(pending[slot].publish);

            float msKernels = 0.0f, msTotal = 0.0f;
            const bool okK = cudaEventElapsedTime(&msKernels, pending[slot].start,
                                                  pending[slot].kernels) == cudaSuccess;
            const bool okT = cudaEventElapsedTime(&msTotal, pending[slot].start,
                                                  pending[slot].publish) == cudaSuccess;
            if (!okK || !okT) cudaGetLastError();

            {
                std::lock_guard<std::mutex> lock(mutex);
                publisher.CommitSlot(pending[slot].ticket);
                pending[slot].inUse = false;
                ++published;
                if (okK && okT)
                {
                    msDeviceKernels   = msKernels;
                    msDeviceTotal     = msTotal;
                    msDevicePublish   = msTotal - msKernels;
                    deviceTimingValid = true;
                }
            }
        }
    }
};

// The singleton itself, plus the lock that serialises acquire/release. A
// function-local static would be destroyed at exit in an order we do not
// control relative to the CUDA runtime, so it is deliberately never destroyed.
std::mutex& HubLock()
{
    static std::mutex* lock = new std::mutex();
    return *lock;
}

PublishHub& Hub()
{
    static PublishHub* hub = new PublishHub();
    return *hub;
}

// Brings the hub up on the first instance and tears it down after the last.
// Returns false with p_Error filled when the GPU path cannot run at all.
bool HubAcquire(char* p_Error, size_t p_ErrorBytes, char* p_Note, size_t p_NoteBytes)
{
    std::lock_guard<std::mutex> lock(HubLock());
    PublishHub& hub = Hub();

    if (hub.running)
    {
        ++hub.refs;
        std::snprintf(p_Note, p_NoteBytes, "shared block pinned (%zu MB), %d instances",
                      hub.blockBytes / (1024 * 1024), hub.refs);
        return true;
    }

    if (!hub.publisher.IsRunning() && !hub.publisher.Start())
    {
        std::snprintf(p_Error, p_ErrorBytes, "shared block unavailable");
        return false;
    }

    void* block = hub.publisher.BlockData();
    hub.blockBytes = hub.publisher.BlockSize();
    if (!block || hub.blockBytes == 0)
    {
        std::snprintf(p_Error, p_ErrorBytes, "publisher block unavailable");
        return false;
    }

    cudaError_t err = cudaSuccess;
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        if ((err = cudaEventCreate(&hub.pending[i].start)) != cudaSuccess ||
            (err = cudaEventCreate(&hub.pending[i].kernels)) != cudaSuccess ||
            (err = cudaEventCreate(&hub.pending[i].publish)) != cudaSuccess)
        {
            std::snprintf(p_Error, p_ErrorBytes, "cudaEventCreate: %s", cudaGetErrorString(err));
            return false;
        }
        hub.pending[i].inUse = false;
    }

    // Page-lock the shared block, once for the process. Measured to work on a
    // mapped file view; if a driver refuses, the GPU path declines outright
    // rather than silently falling back to a staged copy that blocks the render
    // thread for 10 ms.
    const cudaError_t rerr = cudaHostRegister(block, hub.blockBytes, cudaHostRegisterDefault);
    if (rerr != cudaSuccess)
    {
        cudaGetLastError();
        std::snprintf(p_Error, p_ErrorBytes,
                      "cudaHostRegister refused the shared block: %s", cudaGetErrorString(rerr));
        return false;
    }
    hub.slotPinned = true;

    hub.stopping = false;
    hub.worker = std::thread([&hub] { hub.Run(); });
    hub.running = true;
    hub.refs = 1;

    std::snprintf(p_Note, p_NoteBytes, "shared block pinned once for the process (%zu MB)",
                  hub.blockBytes / (1024 * 1024));
    return true;
}

void HubRelease()
{
    std::lock_guard<std::mutex> lock(HubLock());
    PublishHub& hub = Hub();
    if (!hub.running) return;
    if (--hub.refs > 0) return;

    if (hub.worker.joinable())
    {
        {
            std::lock_guard<std::mutex> qlock(hub.mutex);
            hub.stopping = true;
        }
        hub.cv.notify_all();
        hub.worker.join();
    }

    if (hub.slotPinned && hub.publisher.BlockData())
    {
        cudaHostUnregister(hub.publisher.BlockData());
        hub.slotPinned = false;
    }
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        PublishHub::Pending& p = hub.pending[i];
        if (p.start)   { cudaEventDestroy(p.start);   p.start = nullptr; }
        if (p.kernels) { cudaEventDestroy(p.kernels); p.kernels = nullptr; }
        if (p.publish) { cudaEventDestroy(p.publish); p.publish = nullptr; }
        p.inUse = false;
    }
    hub.queue.clear();
    hub.running = false;
}

struct GpuTap::Impl
{
    bool running = false;
    char error[256] = {};
    char note[160] = {};

    // Per-instance device state. NOT shared via the hub: Resolve really does
    // reduce different frames in different instances at the same time, so these
    // buffers and this stream have to be one set per instance.
    //
    // And within an instance, one set per ring slot - not one set. The publish
    // copies read a frame's results on publishStream while the NEXT frame's
    // memsets and kernels already run on the host's stream; with a single set
    // they overwrote the results mid-copy, so a slot carried another frame's
    // scopes under its own timestamp (measured 2026-10-08: 192-197 of 300
    // slot reads wrong over back-to-back frames, no contrived timing needed;
    // tools/ScopePublishRace.cu is the regression test). A set
    // is indexed by the hub ring entry its frame reserved, and the hub frees
    // that entry only after cudaEventSynchronize on the frame's own publish
    // event - so a set is never written while a copy still reads it, on one
    // host stream or several. Ring entries are process-wide, so no two
    // in-flight frames of this instance can hold the same index.
    //
    // Cost: kSlotCount x ~5.3 MB of bins, plus kSlotCount previews, each grown
    // on demand to the largest size its slot has carried (25 MB at UHD/100%).
    struct DeviceSet
    {
        unsigned char* bins = nullptr;
        unsigned int*  waveform = nullptr;
        unsigned int*  histogram = nullptr;
        unsigned int*  vector = nullptr;
        unsigned int*  twinPeaks = nullptr;
        float*         trace = nullptr;
        float*         minMax = nullptr;
        float*         probe = nullptr;

        unsigned char* preview = nullptr;
        size_t         previewCapacity = 0;
    };
    DeviceSet sets[kSlotCount];

    int* colOfX = nullptr;
    int* colStart = nullptr;
    int  colWidth = 0;                 // frame width the tables were built for

    cudaStream_t publishStream = nullptr;

    double msEnqueueLast = 0.0;

    bool Fail(const char* p_Where, cudaError_t p_Err)
    {
        std::snprintf(error, sizeof(error), "%s: %s", p_Where, cudaGetErrorString(p_Err));
        return false;
    }
};

GpuTap::GpuTap() : m_Impl(new Impl) {}

GpuTap::~GpuTap()
{
    Stop();
    delete m_Impl;
}

bool GpuTap::IsRunning() const { return m_Impl && m_Impl->running; }
const char* GpuTap::Error() const { return m_Impl ? m_Impl->error : "no context"; }

void GpuTap::Stats(GpuTapStats& p_Out) const
{
    if (!m_Impl) return;
    PublishHub& hub = Hub();
    p_Out.msEnqueue = m_Impl->msEnqueueLast;
    std::snprintf(p_Out.note, sizeof(p_Out.note), "%s", m_Impl->note);

    // published/skipped are process-wide now, not per instance: there is one
    // ring and one worker, so a per-instance count would not mean anything.
    std::lock_guard<std::mutex> lock(hub.mutex);
    p_Out.published          = hub.published;
    p_Out.skipped            = hub.skipped;
    p_Out.slotPinned         = hub.slotPinned;
    p_Out.msDeviceTotal      = hub.msDeviceTotal;
    p_Out.msDeviceKernels    = hub.msDeviceKernels;
    p_Out.msDevicePublish    = hub.msDevicePublish;
    p_Out.deviceTimingValid  = hub.deviceTimingValid;
}

bool GpuTap::Start()
{
    Impl& impl = *m_Impl;
    if (impl.running) return true;

    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess || count == 0)
    {
        std::snprintf(impl.error, sizeof(impl.error), "no CUDA device");
        return false;
    }

    const float binScale    = static_cast<float>(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);
    const float chromaScale = static_cast<float>(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);
    const float twinScale   = static_cast<float>(kTwinPeaksSize - 1) /
                              (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);
    if ((err = cudaMemcpyToSymbol(kBinScaleDev, &binScale, sizeof(float))) != cudaSuccess)
        return impl.Fail("binScale", err);
    if ((err = cudaMemcpyToSymbol(kChromaBinScaleDev, &chromaScale, sizeof(float))) != cudaSuccess)
        return impl.Fail("chromaScale", err);
    if ((err = cudaMemcpyToSymbol(kTwinDiffBinScaleDev, &twinScale, sizeof(float))) != cudaSuccess)
        return impl.Fail("twinScale", err);

    const size_t binBytes =
        static_cast<size_t>(kWaveformCells) * sizeof(unsigned int) +
        static_cast<size_t>(kHistogramCells) * sizeof(unsigned int) +
        static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int) +
        static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int) +
        static_cast<size_t>(kWaveformTraceCells) * sizeof(float);
    for (Impl::DeviceSet& set : impl.sets)
    {
        if ((err = cudaMalloc(&set.bins, binBytes)) != cudaSuccess) return impl.Fail("cudaMalloc bins", err);

        unsigned char* p = set.bins;
        set.waveform  = reinterpret_cast<unsigned int*>(p);
        p += static_cast<size_t>(kWaveformCells) * sizeof(unsigned int);
        set.histogram = reinterpret_cast<unsigned int*>(p);
        p += static_cast<size_t>(kHistogramCells) * sizeof(unsigned int);
        set.vector    = reinterpret_cast<unsigned int*>(p);
        p += static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int);
        set.twinPeaks = reinterpret_cast<unsigned int*>(p);
        p += static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int);
        set.trace     = reinterpret_cast<float*>(p);

        if ((err = cudaMalloc(&set.minMax, 6 * sizeof(float))) != cudaSuccess)
            return impl.Fail("cudaMalloc minMax", err);
        if ((err = cudaMalloc(&set.probe, 3 * sizeof(float))) != cudaSuccess)
            return impl.Fail("cudaMalloc probe", err);
    }
    if ((err = cudaMalloc(&impl.colStart, (static_cast<size_t>(kWaveformColumns) + 1) * sizeof(int))) != cudaSuccess)
        return impl.Fail("cudaMalloc colStart", err);

    if ((err = cudaStreamCreateWithFlags(&impl.publishStream, cudaStreamNonBlocking)) != cudaSuccess)
        return impl.Fail("cudaStreamCreate", err);

    // Everything shared - the mapping, its page-lock, the ticket counter, the
    // ring and the worker thread - comes up once for the process here.
    if (!HubAcquire(impl.error, sizeof(impl.error), impl.note, sizeof(impl.note)))
        return false;

    impl.running = true;
    return true;
}

void GpuTap::Stop()
{
    Impl& impl = *m_Impl;

    // Release the hub first: it joins the worker on the last instance out, and
    // the worker is the only other thread that touches the ring.
    if (impl.running) HubRelease();

    if (impl.publishStream) { cudaStreamDestroy(impl.publishStream); impl.publishStream = nullptr; }

    for (Impl::DeviceSet& set : impl.sets)
    {
        cudaFree(set.bins);    // waveform..trace are views into this one block
        cudaFree(set.minMax);
        cudaFree(set.probe);
        cudaFree(set.preview);
        set = Impl::DeviceSet();
    }
    cudaFree(impl.colOfX);        impl.colOfX = nullptr;
    cudaFree(impl.colStart);      impl.colStart = nullptr;
    impl.colWidth = 0;
    impl.running = false;
}

bool GpuTap::RenderFrame(const GpuTapArgs& p_Args)
{
    Impl& impl = *m_Impl;
    if (!impl.running) return false;

    const auto tEnter = std::chrono::steady_clock::now();
    cudaStream_t hostStream = reinterpret_cast<cudaStream_t>(p_Args.stream);
    cudaError_t err = cudaSuccess;

    const int width = p_Args.width, height = p_Args.height;
    if (width <= 0 || height <= 0) return false;

    int rowStep = p_Args.params.rowStep;
    if (rowStep < 1) rowStep = 1;

    // --- reserve a ring entry and its ticket, together -----------------------
    //
    // Both come from the process-wide hub under one lock, which is what stops
    // two instances claiming the same ticket or reusing a slot whose DMA is
    // still in flight. It happens before the passthrough so the slot's `start`
    // event can be recorded ahead of the copy and the device timing covers it.
    // A full ring skips: the frame still passes the image through below, only
    // its scope reading is dropped.
    PublishHub& hub = Hub();
    uint64_t ticket = 0;
    const int slot = hub.Reserve(ticket);

    // Every failure between here and Enqueue must give the ring entry back, or
    // the ring loses a slot permanently and the tap quietly degrades to two,
    // then one, then skipping every frame. There are a dozen such returns, so
    // it is a guard rather than a dozen chances to forget.
    struct SlotGuard
    {
        PublishHub* hub;
        int slot;
        bool armed;
        ~SlotGuard() { if (armed && slot >= 0) hub->Release(slot); }
    } slotGuard{ &hub, slot, true };

    // --- the mandatory passthrough, on the host's stream ---------------------
    if (slot >= 0) cudaEventRecord(hub.pending[slot].start, hostStream);
    err = cudaMemcpy2DAsync(p_Args.dstDevice, p_Args.dstRowBytes,
                            p_Args.srcDevice, p_Args.srcRowBytes,
                            static_cast<size_t>(width) * 4 * sizeof(float),
                            static_cast<size_t>(height),
                            cudaMemcpyDeviceToDevice, hostStream);
    if (err != cudaSuccess) return impl.Fail("passthrough", err);

    if (slot < 0)
    {
        impl.msEnqueueLast = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tEnter).count();
        return true;
    }

    // This frame's own result buffers, owned for as long as its ring entry is -
    // see Impl::DeviceSet. Every bin, extreme, probe and preview write below
    // goes here; only the read-only column tables stay instance-wide.
    Impl::DeviceSet& buf = impl.sets[slot];

    // --- column tables, rebuilt only when the frame size changes -------------
    if (impl.colWidth != width)
    {
        std::vector<int> colStart(static_cast<size_t>(kWaveformColumns) + 1);
        for (uint32_t col = 0; col <= kWaveformColumns; ++col)
        {
            const uint64_t x = (static_cast<uint64_t>(col) * static_cast<uint64_t>(width)) / kWaveformColumns;
            colStart[col] = static_cast<int>(x < static_cast<uint64_t>(width) ? x : static_cast<uint64_t>(width));
        }
        colStart[kWaveformColumns] = width;

        std::vector<int> colOfX(static_cast<size_t>(width));
        uint32_t c = 0;
        for (int x = 0; x < width; ++x)
        {
            while (c + 1 <= kWaveformColumns && colStart[c + 1] <= x) ++c;
            colOfX[static_cast<size_t>(x)] = static_cast<int>(c);
        }

        cudaFree(impl.colOfX); impl.colOfX = nullptr;
        if ((err = cudaMalloc(&impl.colOfX, static_cast<size_t>(width) * sizeof(int))) != cudaSuccess)
            return impl.Fail("cudaMalloc colOfX", err);
        if ((err = cudaMemcpyAsync(impl.colOfX, colOfX.data(), colOfX.size() * sizeof(int),
                                   cudaMemcpyHostToDevice, hostStream)) != cudaSuccess)
            return impl.Fail("H2D colOfX", err);
        if ((err = cudaMemcpyAsync(impl.colStart, colStart.data(), colStart.size() * sizeof(int),
                                   cudaMemcpyHostToDevice, hostStream)) != cudaSuccess)
            return impl.Fail("H2D colStart", err);
        // The only synchronise in the whole path, and it happens once per frame
        // size - never during steady playback. The vectors above are stack
        // memory the copies must finish reading before this function returns.
        if ((err = cudaStreamSynchronize(hostStream)) != cudaSuccess)
            return impl.Fail("table upload sync", err);
        impl.colWidth = width;
    }

    const size_t rowFloats = p_Args.srcRowBytes / sizeof(float);
    const int sampledRows = (height - 1) / rowStep + 1;

    LumaDev luma;
    luma.r = p_Args.params.luma.r;
    luma.g = p_Args.params.luma.g;
    luma.b = p_Args.params.luma.b;
    luma.cbScale = 0.5f / (1.0f - p_Args.params.luma.b);
    luma.crScale = 0.5f / (1.0f - p_Args.params.luma.r);

    cudaMemsetAsync(buf.waveform, 0, static_cast<size_t>(kWaveformCells) * sizeof(unsigned int), hostStream);
    cudaMemsetAsync(buf.vector, 0, static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int), hostStream);
    cudaMemsetAsync(buf.twinPeaks, 0, static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int), hostStream);

    // numeric_limits, not the INFINITY macro: under nvcc that expands to a
    // double literal that does not fit a float, which is six warnings and a
    // value nobody should have to squint at.
    static const float kInf = std::numeric_limits<float>::infinity();
    const float kSeed[6] = { kInf, kInf, kInf, -kInf, -kInf, -kInf };
    if ((err = cudaMemcpyAsync(buf.minMax, kSeed, sizeof(kSeed), cudaMemcpyHostToDevice, hostStream)) != cudaSuccess)
        return impl.Fail("seed minMax", err);

    {
        const dim3 block(32, 8);
        const dim3 grid((width + block.x - 1) / block.x, (sampledRows + block.y - 1) / block.y);
        ScatterKernel<<<grid, block, 0, hostStream>>>(
            static_cast<const float*>(p_Args.srcDevice), width, sampledRows, rowStep, rowFloats,
            impl.colOfX, luma, buf.waveform, buf.vector, buf.twinPeaks, buf.minMax);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("ScatterKernel", err);
    }
    {
        const int total = static_cast<int>(kHistogramBins * kPlaneCount);
        HistogramKernel<<<(total + 255) / 256, 256, 0, hostStream>>>(buf.waveform, buf.histogram);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("HistogramKernel", err);
    }
    {
        const dim3 block(32, 4);
        const dim3 grid((kWaveformColumns + block.x - 1) / block.x,
                        (kWaveformTraceRows + block.y - 1) / block.y);
        TraceKernel<<<grid, block, 0, hostStream>>>(
            static_cast<const float*>(p_Args.srcDevice), height, rowFloats, impl.colStart, luma, buf.trace);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("TraceKernel", err);
    }
    ProbeKernel<<<1, 1, 0, hostStream>>>(static_cast<const float*>(p_Args.srcDevice),
                                         width, height, rowFloats, buf.probe);
    if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("ProbeKernel", err);

    // --- preview -------------------------------------------------------------
    uint32_t previewW = 0, previewH = 0;
    size_t previewRowBytes = 0, previewBytes = 0;
    if (p_Args.publishPreview && p_Args.previewScale > 0.0f)
    {
        // The CPU path's own size rule, not a per-axis clamp of its own: that
        // squashed any source wider than the canvas, truncated where the CPU
        // rounds, and could reach 0, which fails the kernel launch below.
        PreviewSizeFor(width, height, p_Args.previewScale, previewW, previewH);
        previewRowBytes = static_cast<size_t>(previewW) * 3;
        previewBytes = previewRowBytes * previewH;

        if (previewBytes > buf.previewCapacity)
        {
            cudaFree(buf.preview); buf.preview = nullptr;
            if ((err = cudaMalloc(&buf.preview, previewBytes)) != cudaSuccess)
                return impl.Fail("cudaMalloc preview", err);
            buf.previewCapacity = previewBytes;
        }

        const dim3 block(16, 16);
        const dim3 grid((previewW + block.x - 1) / block.x, (previewH + block.y - 1) / block.y);
        PreviewKernel<<<grid, block, 0, hostStream>>>(
            static_cast<const float*>(p_Args.srcDevice), width, height, rowFloats,
            buf.preview, static_cast<int>(previewW), static_cast<int>(previewH), previewRowBytes);
        if ((err = cudaGetLastError()) != cudaSuccess) return impl.Fail("PreviewKernel", err);
    }

    // --- hand off to our own stream ------------------------------------------
    //
    // Everything above reads Resolve's source buffer and so had to run on
    // Resolve's stream. Everything below reads only our buffers, so it moves to
    // ours - which keeps the transfer off the stream Resolve is waiting on.
    if ((err = cudaEventRecord(hub.pending[slot].kernels, hostStream)) != cudaSuccess)
        return impl.Fail("record kernelsDone", err);
    if ((err = cudaStreamWaitEvent(impl.publishStream, hub.pending[slot].kernels, 0)) != cudaSuccess)
        return impl.Fail("stream wait", err);

    // The ticket was allocated with the ring entry, up at Reserve().
    char* slotBase = static_cast<char*>(hub.publisher.SlotAddress(ticket));
    if (!slotBase)
    {
        // slotGuard releases the reservation; releasing here as well would give
        // the block slot back twice.
        std::snprintf(impl.error, sizeof(impl.error), "slot address unavailable");
        return false;
    }

    hub.publisher.OpenSlot(ticket, p_Args.timelineTime,
                             static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                             p_Args.instanceId,
                             static_cast<uint64_t>(sampledRows) * static_cast<uint64_t>(width),
                             p_Args.params.colorSpace, p_Args.params.luma,
                             previewW, previewH, 0.0);

    struct Leg { void* dst; const void* src; size_t bytes; };
    const Leg legs[] = {
        { slotBase + kWaveformOffset,      buf.waveform,  static_cast<size_t>(kWaveformCells) * sizeof(unsigned int) },
        { slotBase + kHistogramOffset,     buf.histogram, static_cast<size_t>(kHistogramCells) * sizeof(unsigned int) },
        { slotBase + kVectorscopeOffset,   buf.vector,    static_cast<size_t>(kVectorscopeTotalCells) * sizeof(unsigned int) },
        { slotBase + kTwinPeaksOffset,     buf.twinPeaks, static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(unsigned int) },
        { slotBase + kWaveformTraceOffset, buf.trace,     static_cast<size_t>(kWaveformTraceCells) * sizeof(float) },
        // minRGB[3] and maxRGB[3] are adjacent in SlotHeader, and the device
        // buffer is laid out to match, so both extremes cross in one copy.
        { slotBase + ScopePublisher::MinMaxOffset(), buf.minMax, 6 * sizeof(float) },
        { slotBase + ScopePublisher::ProbeOffset(),  buf.probe,  3 * sizeof(float) },
    };
    for (const Leg& leg : legs)
    {
        if ((err = cudaMemcpyAsync(leg.dst, leg.src, leg.bytes, cudaMemcpyDeviceToHost,
                                   impl.publishStream)) != cudaSuccess)
            return impl.Fail("publish D2H", err);
    }
    if (previewBytes > 0)
    {
        if ((err = cudaMemcpyAsync(slotBase + kPreviewOffset, buf.preview, previewBytes,
                                   cudaMemcpyDeviceToHost, impl.publishStream)) != cudaSuccess)
            return impl.Fail("publish preview D2H", err);
    }

    if ((err = cudaEventRecord(hub.pending[slot].publish, impl.publishStream)) != cudaSuccess)
        return impl.Fail("record publishDone", err);

    // Queued, not committed: the worker closes the seqlock once the copies
    // land. Queue order is reservation order is ticket order, which is what
    // ScopeShm.h's "committed in ticket order" contract needs - and it now
    // holds across instances, not just within one.
    slotGuard.armed = false;
    hub.Enqueue(slot);

    impl.msEnqueueLast = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tEnter).count();
    return true;
}

} // namespace scopedeck

namespace scopedeck
{

void* CudaAllocHost(size_t p_Bytes)
{
    void* ptr = nullptr;
    if (cudaHostAlloc(&ptr, p_Bytes, cudaHostAllocDefault) != cudaSuccess)
    {
        cudaGetLastError();
        return nullptr;
    }
    return ptr;
}

void CudaFreeHost(void* p_Ptr)
{
    if (p_Ptr && cudaFreeHost(p_Ptr) != cudaSuccess) cudaGetLastError();
}

bool CudaDownload(void* p_DstHost, size_t p_DstRowBytes,
                  const void* p_SrcDevice, size_t p_SrcRowBytes,
                  int p_Width, int p_Height, void* p_Stream,
                  double* p_MsCopy)
{
    if (p_MsCopy) *p_MsCopy = 0.0;
    if (!p_DstHost || !p_SrcDevice || p_Width <= 0 || p_Height <= 0) return false;
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(p_Stream);

    // Events bracket the copy ITSELF, so the caller can tell a slow transfer
    // apart from a long wait for someone else's queued work.
    cudaEvent_t ev0 = nullptr, ev1 = nullptr;
    if (p_MsCopy)
    {
        if (cudaEventCreate(&ev0) != cudaSuccess || cudaEventCreate(&ev1) != cudaSuccess)
        {
            cudaGetLastError();
            if (ev0) { cudaEventDestroy(ev0); ev0 = nullptr; }
            if (ev1) { cudaEventDestroy(ev1); ev1 = nullptr; }
        }
    }

    if (ev0) cudaEventRecord(ev0, stream);
    cudaError_t err = cudaMemcpy2DAsync(
        p_DstHost, p_DstRowBytes, p_SrcDevice, p_SrcRowBytes,
        static_cast<size_t>(p_Width) * 4 * sizeof(float), static_cast<size_t>(p_Height),
        cudaMemcpyDeviceToHost, stream);
    if (err != cudaSuccess)
    {
        cudaGetLastError();
        if (ev0) cudaEventDestroy(ev0);
        if (ev1) cudaEventDestroy(ev1);
        return false;
    }
    if (ev1) cudaEventRecord(ev1, stream);

    // Enqueued on the HOST's stream, so it is correctly ordered after whatever
    // Resolve did to produce this frame - and then waited on, because the
    // caller is about to read the bytes on the CPU. Never
    // cudaDeviceSynchronize: ofxGPURender.h forbids it for a plugin that was
    // handed a stream.
    err = cudaStreamSynchronize(stream);

    if (ev1 && err == cudaSuccess)
    {
        float ms = 0.0f;
        if (cudaEventElapsedTime(&ms, ev0, ev1) == cudaSuccess) { if (p_MsCopy) *p_MsCopy = ms; }
        else cudaGetLastError();
    }
    if (ev0) cudaEventDestroy(ev0);
    if (ev1) cudaEventDestroy(ev1);

    if (err != cudaSuccess) { cudaGetLastError(); return false; }
    return true;
}

bool CudaPassthrough(void* p_Dst, size_t p_DstRowBytes,
                     const void* p_Src, size_t p_SrcRowBytes,
                     int p_Width, int p_Height, void* p_Stream)
{
    if (!p_Dst || !p_Src || p_Width <= 0 || p_Height <= 0) return false;
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(p_Stream);
    const cudaError_t err = cudaMemcpy2DAsync(
        p_Dst, p_DstRowBytes, p_Src, p_SrcRowBytes,
        static_cast<size_t>(p_Width) * 4 * sizeof(float), static_cast<size_t>(p_Height),
        cudaMemcpyDeviceToDevice, stream);
    if (err != cudaSuccess) { cudaGetLastError(); return false; }
    if (!stream) cudaStreamSynchronize(0);
    return true;
}

} // namespace scopedeck

// =============================================================================
// CpuFallbackTap - what "GPU Acceleration off" runs on a CUDA host.
//
// See ScopeCuda.h for the measurements. Short version: of the old synchronous
// 28.5 ms download, 22.5 ms was waiting for work Resolve had queued ahead of
// us and only 5.5 ms was transferring anything. Page-locking the destination
// changed nothing, because the wait was never about bandwidth.
// =============================================================================

namespace scopedeck
{

struct CpuFallbackTap::Impl
{
    bool running = false;
    char error[256] = {};

    float*  staging = nullptr;
    size_t  stagingBytes = 0;
    bool    pinned = false;

    cudaEvent_t copyStart = nullptr;
    cudaEvent_t copyDone = nullptr;

    // The frame currently in the staging buffer. Written by the render thread
    // before the worker is woken, read by the worker afterwards; `busy` is what
    // keeps those two from overlapping.
    ScopeParams params;
    double      timelineTime = 0.0;
    uint32_t    instanceId = 0;
    int         width = 0;
    int         height = 0;
    size_t      rowBytes = 0;
    bool        publishPreview = true;
    float       previewScale = 0.5f;

    ScopeEngine engine;
    ScopeResult result;

    mutable std::mutex      mutex;
    std::condition_variable cv;
    std::thread             worker;
    bool                    stopping = false;
    bool                    busy = false;        // a frame is in flight
    bool                    pending = false;     // ... and the worker has not taken it

    double   msEnqueueLast = 0.0;
    double   msCopyLast = 0.0;
    double   msBinLast = 0.0;
    uint64_t published = 0;
    uint64_t skipped = 0;

    bool EnsureStaging(size_t p_Bytes)
    {
        if (staging && stagingBytes >= p_Bytes) return true;
        ReleaseStaging();
        staging = static_cast<float*>(CudaAllocHost(p_Bytes));
        pinned  = (staging != nullptr);
        if (!staging) staging = static_cast<float*>(std::malloc(p_Bytes));
        stagingBytes = staging ? p_Bytes : 0;
        return staging != nullptr;
    }

    void ReleaseStaging()
    {
        if (!staging) return;
        if (pinned) CudaFreeHost(staging); else std::free(staging);
        staging = nullptr; stagingBytes = 0; pinned = false;
    }

    void Run()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || pending; });
                if (!pending) return;                     // stopping
                pending = false;
            }

            // Outside the lock: the wait the render thread refuses to do. It
            // covers the transfer AND whatever was queued ahead of it, which
            // measurement showed to be the larger half by four to one.
            cudaEventSynchronize(copyDone);

            float msCopy = 0.0f;
            if (cudaEventElapsedTime(&msCopy, copyStart, copyDone) != cudaSuccess)
                cudaGetLastError();

            FrameView view;
            view.pixels   = staging;
            view.width    = width;
            view.height   = height;
            view.rowBytes = static_cast<int>(rowBytes);

            engine.Analyse(view, params, result);

            if (publishPreview) engine.BuildPreview(view, previewScale, result.preview);
            else { result.preview.width = 0; result.preview.height = 0; }

            // Publish() reserves through the block's PublishAuthority, so it
            // cannot collide with a GPU copy still landing or with another
            // instance - no hub lock needed, and holding one here would stall
            // every GPU reservation and commit for the length of this copy.
            Hub().publisher.Publish(result, timelineTime,
                                    static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height), instanceId);

            {
                std::lock_guard<std::mutex> lock(mutex);
                msCopyLast = msCopy;
                msBinLast  = result.millis;
                ++published;
                busy = false;
            }
        }
    }
};

CpuFallbackTap::CpuFallbackTap() : m_Impl(new Impl) {}

CpuFallbackTap::~CpuFallbackTap()
{
    Stop();
    delete m_Impl;
}

bool CpuFallbackTap::IsRunning() const { return m_Impl && m_Impl->running; }
const char* CpuFallbackTap::Error() const { return m_Impl ? m_Impl->error : "no context"; }

void CpuFallbackTap::Stats(FallbackStats& p_Out) const
{
    if (!m_Impl) return;
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    p_Out.msEnqueue = m_Impl->msEnqueueLast;
    p_Out.msCopy    = m_Impl->msCopyLast;
    p_Out.msBin     = m_Impl->msBinLast;
    p_Out.published = m_Impl->published;
    p_Out.skipped   = m_Impl->skipped;
    p_Out.pinned    = m_Impl->pinned;
    p_Out.busy      = m_Impl->busy;
}

bool CpuFallbackTap::Start()
{
    Impl& impl = *m_Impl;
    if (impl.running) return true;

    cudaError_t err = cudaSuccess;
    if ((err = cudaEventCreate(&impl.copyStart)) != cudaSuccess ||
        (err = cudaEventCreate(&impl.copyDone)) != cudaSuccess)
    {
        std::snprintf(impl.error, sizeof(impl.error), "cudaEventCreate: %s", cudaGetErrorString(err));
        return false;
    }

    char note[160] = {};
    if (!HubAcquire(impl.error, sizeof(impl.error), note, sizeof(note))) return false;

    impl.stopping = false;
    impl.worker = std::thread([&impl] { impl.Run(); });
    impl.running = true;
    return true;
}

void CpuFallbackTap::Stop()
{
    Impl& impl = *m_Impl;
    if (impl.worker.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.stopping = true;
        }
        impl.cv.notify_all();
        impl.worker.join();
    }
    if (impl.running) HubRelease();

    if (impl.copyStart) { cudaEventDestroy(impl.copyStart); impl.copyStart = nullptr; }
    if (impl.copyDone)  { cudaEventDestroy(impl.copyDone);  impl.copyDone = nullptr; }
    impl.ReleaseStaging();
    impl.running = false;
}

bool CpuFallbackTap::Submit(const SubmitArgs& p_Args)
{
    Impl& impl = *m_Impl;
    if (!impl.running) return false;
    if (p_Args.width <= 0 || p_Args.height <= 0 || !p_Args.srcDevice) return false;

    const auto tEnter = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.busy)
        {
            // The worker still owns the staging buffer; overwriting it now
            // would tear the frame it is in the middle of reducing.
            ++impl.skipped;
            impl.msEnqueueLast = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tEnter).count();
            return false;
        }
        impl.busy = true;
    }

    const size_t rowBytes = static_cast<size_t>(p_Args.width) * 4 * sizeof(float);
    if (!impl.EnsureStaging(rowBytes * static_cast<size_t>(p_Args.height)))
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.busy = false;
        std::snprintf(impl.error, sizeof(impl.error), "no staging buffer");
        return false;
    }

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(p_Args.stream);
    cudaEventRecord(impl.copyStart, stream);
    const cudaError_t err = cudaMemcpy2DAsync(
        impl.staging, rowBytes, p_Args.srcDevice, p_Args.srcRowBytes,
        static_cast<size_t>(p_Args.width) * 4 * sizeof(float),
        static_cast<size_t>(p_Args.height), cudaMemcpyDeviceToHost, stream);
    if (err != cudaSuccess)
    {
        cudaGetLastError();
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.busy = false;
        std::snprintf(impl.error, sizeof(impl.error), "download: %s", cudaGetErrorString(err));
        return false;
    }
    cudaEventRecord(impl.copyDone, stream);

    impl.params         = p_Args.params;
    impl.timelineTime   = p_Args.timelineTime;
    impl.instanceId     = p_Args.instanceId;
    impl.width          = p_Args.width;
    impl.height         = p_Args.height;
    impl.rowBytes       = rowBytes;
    impl.publishPreview = p_Args.publishPreview;
    impl.previewScale   = p_Args.previewScale;

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.pending = true;
    }
    impl.cv.notify_one();

    impl.msEnqueueLast = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tEnter).count();
    return true;
}

} // namespace scopedeck
