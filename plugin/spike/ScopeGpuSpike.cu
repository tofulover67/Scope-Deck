// CUDA implementation of the GPU spike. See ScopeGpuSpike.h for what this is for.

#include "ScopeGpuSpike.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace scopespike
{
namespace
{

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

__device__ __forceinline__ uint32_t BinOf(float p_Value, uint32_t p_Bins)
{
    const float t = (p_Value - kBinRangeLow) / (kBinRangeHigh - kBinRangeLow);
    int bin = static_cast<int>(t * static_cast<float>(p_Bins));
    if (bin < 0) bin = 0;
    if (bin >= static_cast<int>(p_Bins)) bin = static_cast<int>(p_Bins) - 1;
    return static_cast<uint32_t>(bin);
}

// Deliberately the same [column][level][plane] interleave the CPU path uses
// (core/ScopeTypes.h WaveformIndex). The layout is the whole point: it decides
// which addresses collide, and collisions are what this kernel exists to price.
__device__ __forceinline__ uint64_t WaveformIndex(uint32_t p_Plane, uint32_t p_Column, uint32_t p_Level)
{
    return (static_cast<uint64_t>(p_Column) * kWaveformLevels + p_Level) * kPlaneCount + p_Plane;
}

// Warp-aggregated increment. When several lanes of a warp target the same cell -
// which is the whole problem with a crushed or blown frame - __match_any_sync
// groups them, one elected lane issues a single atomicAdd of the group size, and
// 32 serialised memory transactions become one.
//
// __match_any_sync needs sm_70. The fatbin still ships sm_52 and sm_61, so those
// fall back to the plain path rather than failing to compile; Maxwell and Pascal
// simply do not have the instruction.
__device__ __forceinline__ void AggregatedAdd(unsigned int* p_Addr)
{
#if __CUDA_ARCH__ >= 700
    const unsigned int active = __activemask();
    const unsigned int peers  = __match_any_sync(active, reinterpret_cast<unsigned long long>(p_Addr));
    // The lowest-numbered peer leads. Derived from lanemask_lt rather than a lane
    // id, so this does not assume how warps are laid out across the block.
    unsigned int lanemaskLt;
    asm("mov.u32 %0, %%lanemask_lt;" : "=r"(lanemaskLt));
    if ((peers & lanemaskLt) == 0u)
        atomicAdd(p_Addr, static_cast<unsigned int>(__popc(peers)));
#else
    atomicAdd(p_Addr, 1u);
#endif
}

// Templated rather than branched so the plain path keeps its exact previous cost
// and the comparison stays honest.
template <bool Aggregated>
__device__ __forceinline__ void ScatterAdd(unsigned int* p_Addr)
{
    if (Aggregated) AggregatedAdd(p_Addr);
    else            atomicAdd(p_Addr, 1u);
}

template <bool Aggregated>
__global__ void ScatterKernel(const float* __restrict__ p_Src,
                              int p_Width, int p_Height, size_t p_RowFloats,
                              unsigned int* __restrict__ p_Waveform,
                              unsigned int* __restrict__ p_Vector,
                              int p_Content)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p_Width || y >= p_Height) return;

    const float* px = p_Src + static_cast<size_t>(y) * p_RowFloats + static_cast<size_t>(x) * 4;
    const float r = px[0], g = px[1], b = px[2];

    // Rec.709, matching LumaWeightsFor's default. The spike never varies it -
    // colour space changes the values, not the access pattern.
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

    uint32_t column, levelR, levelG, levelB, levelY;

    if (p_Content == kScatterWorstCase)
    {
        // Every thread onto one cell. This is not a pathological input, it is
        // the pathological *pattern* - a crushed or blown frame approaches it,
        // and it bounds how bad the atomics can get regardless of footage.
        column = 0; levelR = levelG = levelB = levelY = 0;
    }
    else if (p_Content == kScatterUniform)
    {
        // Cheap hash, spread across the whole table: the opposite bound.
        const uint32_t h = static_cast<uint32_t>(y) * 1664525u + static_cast<uint32_t>(x) * 1013904223u;
        column = h % kWaveformColumns;
        levelR = (h >> 8)  % kWaveformLevels;
        levelG = (h >> 12) % kWaveformLevels;
        levelB = (h >> 16) % kWaveformLevels;
        levelY = (h >> 20) % kWaveformLevels;
    }
    else
    {
        column = static_cast<uint32_t>(
            (static_cast<uint64_t>(x) * kWaveformColumns) / static_cast<uint64_t>(p_Width));
        if (column >= kWaveformColumns) column = kWaveformColumns - 1;
        levelR = BinOf(r, kWaveformLevels);
        levelG = BinOf(g, kWaveformLevels);
        levelB = BinOf(b, kWaveformLevels);
        levelY = BinOf(luma, kWaveformLevels);
    }

    ScatterAdd<Aggregated>(&p_Waveform[WaveformIndex(0, column, levelR)]);
    ScatterAdd<Aggregated>(&p_Waveform[WaveformIndex(1, column, levelG)]);
    ScatterAdd<Aggregated>(&p_Waveform[WaveformIndex(2, column, levelB)]);
    ScatterAdd<Aggregated>(&p_Waveform[WaveformIndex(3, column, levelY)]);

    // Vectorscope: a 256x256 table per luma band. Too big to privatise in shared
    // memory, so these are global atomics by necessity - the part of the real
    // kernel most likely to hurt, which is why the spike prices it.
    uint32_t cbBin, crBin, band;
    if (p_Content == kScatterWorstCase)
    {
        cbBin = 0; crBin = 0; band = 0;
    }
    else if (p_Content == kScatterUniform)
    {
        const uint32_t h = static_cast<uint32_t>(x) * 2654435761u + static_cast<uint32_t>(y) * 40503u;
        cbBin = h % kVectorscopeSize;
        crBin = (h >> 9) % kVectorscopeSize;
        band  = (h >> 18) % kBandCount;
    }
    else
    {
        const float cb = (b - luma) * 0.5389f;
        const float cr = (r - luma) * 0.6350f;
        const float span = kChromaRangeHigh - kChromaRangeLow;
        int cbi = static_cast<int>((cb - kChromaRangeLow) / span * kVectorscopeSize);
        int cri = static_cast<int>((cr - kChromaRangeLow) / span * kVectorscopeSize);
        cbi = cbi < 0 ? 0 : (cbi >= static_cast<int>(kVectorscopeSize) ? static_cast<int>(kVectorscopeSize) - 1 : cbi);
        cri = cri < 0 ? 0 : (cri >= static_cast<int>(kVectorscopeSize) ? static_cast<int>(kVectorscopeSize) - 1 : cri);
        cbBin = static_cast<uint32_t>(cbi);
        crBin = static_cast<uint32_t>(cri);
        band  = luma < 0.30f ? 0u : (luma >= 0.70f ? 2u : 1u);
    }

    ScatterAdd<Aggregated>(&p_Vector[(static_cast<uint64_t>(band) * kVectorscopeSize + crBin) * kVectorscopeSize + cbBin]);
}

// The preview leg. Nearest-neighbour column/row pick and an 8-bit convert, to
// match BuildPreview's sampling exactly - the point is to prove the small D2H
// works, not to improve on the CPU version's filtering.
__global__ void PreviewKernel(const float* __restrict__ p_Src,
                              int p_SrcWidth, int p_SrcHeight, size_t p_SrcRowFloats,
                              unsigned char* __restrict__ p_Dst,
                              int p_DstWidth, int p_DstHeight, size_t p_DstRowBytes)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p_DstWidth || y >= p_DstHeight) return;

    const int sx = static_cast<int>((static_cast<uint64_t>(x) * p_SrcWidth) / p_DstWidth);
    // OFX images are bottom-up; row 0 is the top everywhere downstream.
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

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct SpikeContext::Impl
{
    unsigned int*  waveform      = nullptr;
    unsigned int*  vector        = nullptr;
    unsigned char* previewDevice = nullptr;
    unsigned char* previewHost   = nullptr;   // pinned, for an async D2H
    size_t         previewCapacity = 0;

    // The publish leg. binsDevice stands in for the full set of wire-format bin
    // arrays; only its size matters to a D2H measurement, never its contents.
    // slotHost is a real mapped file view, because the question is specifically
    // whether CUDA will pin *that* kind of memory, not heap.
    unsigned char* binsDevice    = nullptr;
    unsigned char* slotHost      = nullptr;
    bool           slotRegistered = false;
    int            registerState = 0;          // 1 pinned, 0 pageable, -1 failed
    char           registerNote[128];
#ifdef _WIN32
    HANDLE         slotMapping   = nullptr;
#endif

    // Two event sets, read one frame late so timing never forces a sync.
    static const int kRing = 2;
    cudaEvent_t evStart[kRing], evAfterCopy[kRing], evAfterScatter[kRing],
                evAfterPreview[kRing], evAfterPublish[kRing];
    bool        evValid[kRing];
    int         frame;
    bool        eventsCreated;

    Impl() : frame(0), eventsCreated(false)
    {
        std::memset(evStart, 0, sizeof(evStart));
        std::memset(evAfterCopy, 0, sizeof(evAfterCopy));
        std::memset(evAfterScatter, 0, sizeof(evAfterScatter));
        std::memset(evAfterPreview, 0, sizeof(evAfterPreview));
        std::memset(evAfterPublish, 0, sizeof(evAfterPublish));
        std::memset(evValid, 0, sizeof(evValid));
        registerNote[0] = 0;
    }

    // One slot's worth of shared memory, allocated the same way core/ScopeShm.cpp
    // allocates the real block - a named file mapping - under a deliberately
    // different name so it can never collide with a live tap's.
    bool EnsureSlot()
    {
        if (slotHost) return true;
#ifdef _WIN32
        slotMapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>(kSlotBytes >> 32),
            static_cast<DWORD>(kSlotBytes & 0xFFFFFFFFu),
            "Local\\ScopeDeck.GpuSpike.Slot");
        if (!slotMapping) return false;
        slotHost = static_cast<unsigned char*>(
            MapViewOfFile(slotMapping, FILE_MAP_ALL_ACCESS, 0, 0, kSlotBytes));
        if (!slotHost) { CloseHandle(slotMapping); slotMapping = nullptr; return false; }
#else
        slotHost = static_cast<unsigned char*>(std::malloc(kSlotBytes));
        if (!slotHost) return false;
#endif
        return true;
    }

    void ReleaseSlot()
    {
        if (slotHost && slotRegistered) { cudaHostUnregister(slotHost); slotRegistered = false; }
#ifdef _WIN32
        if (slotHost) UnmapViewOfFile(slotHost);
        if (slotMapping) CloseHandle(slotMapping);
        slotMapping = nullptr;
#else
        if (slotHost) std::free(slotHost);
#endif
        slotHost = nullptr;
    }
};

SpikeContext::SpikeContext() : m_Impl(new Impl) {}

SpikeContext::~SpikeContext()
{
    if (m_Impl)
    {
        if (m_Impl->waveform)      cudaFree(m_Impl->waveform);
        if (m_Impl->vector)        cudaFree(m_Impl->vector);
        if (m_Impl->previewDevice) cudaFree(m_Impl->previewDevice);
        if (m_Impl->previewHost)   cudaFreeHost(m_Impl->previewHost);
        if (m_Impl->binsDevice)    cudaFree(m_Impl->binsDevice);
        m_Impl->ReleaseSlot();
        if (m_Impl->eventsCreated)
        {
            for (int i = 0; i < Impl::kRing; ++i)
            {
                cudaEventDestroy(m_Impl->evStart[i]);
                cudaEventDestroy(m_Impl->evAfterCopy[i]);
                cudaEventDestroy(m_Impl->evAfterScatter[i]);
                cudaEventDestroy(m_Impl->evAfterPreview[i]);
                cudaEventDestroy(m_Impl->evAfterPublish[i]);
            }
        }
        delete m_Impl;
    }
}

const char* SpikeContext::DeviceDescription()
{
    static char desc[256] = {};
    if (desc[0]) return desc;

    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess)
    {
        std::snprintf(desc, sizeof(desc), "no CUDA device");
        return desc;
    }
    cudaDeviceProp prop;
    std::memset(&prop, 0, sizeof(prop));
    if (cudaGetDeviceProperties(&prop, device) != cudaSuccess)
    {
        std::snprintf(desc, sizeof(desc), "device %d (properties unavailable)", device);
        return desc;
    }
    int runtime = 0, driver = 0;
    cudaRuntimeGetVersion(&runtime);
    cudaDriverGetVersion(&driver);
    std::snprintf(desc, sizeof(desc),
                  "device %d '%s' sm_%d%d  SMs=%d  runtime=%d driver=%d",
                  device, prop.name, prop.major, prop.minor,
                  prop.multiProcessorCount, runtime, driver);
    return desc;
}

namespace
{
bool Fail(SpikeTimings& p_Out, const char* p_Where, cudaError_t p_Err)
{
    p_Out.ok = false;
    std::snprintf(p_Out.error, sizeof(p_Out.error), "%s: %s", p_Where, cudaGetErrorString(p_Err));
    return false;
}
} // namespace

bool SpikeContext::Run(const SpikeArgs& p_Args, SpikeTimings& p_Out)
{
    std::memset(&p_Out, 0, sizeof(p_Out));
    p_Out.msPassthrough = p_Out.msScatter = p_Out.msPreview = p_Out.msPublish =
        p_Out.msTotalGpu = -1.0f;
    p_Out.pointerKindSrc = p_Out.pointerKindDst = -1;
    p_Out.deviceSrc = p_Out.deviceDst = -1;
    p_Out.publishRegistered = 0;
    p_Out.ok = true;

    Impl& impl = *m_Impl;
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(p_Args.stream);

    // --- what kind of memory did the host actually hand us? ------------------
    // The single most important thing this spike reports. "CudaEnabled" only
    // says the host believes it is passing device pointers.
    {
        cudaPointerAttributes attr;
        std::memset(&attr, 0, sizeof(attr));
        if (cudaPointerGetAttributes(&attr, p_Args.srcDevice) == cudaSuccess)
        {
            p_Out.pointerKindSrc = static_cast<int>(attr.type);
            p_Out.deviceSrc = attr.device;
        }
        cudaGetLastError();   // a non-device pointer sets an error; clear it
        std::memset(&attr, 0, sizeof(attr));
        if (cudaPointerGetAttributes(&attr, p_Args.dstDevice) == cudaSuccess)
        {
            p_Out.pointerKindDst = static_cast<int>(attr.type);
            p_Out.deviceDst = attr.device;
        }
        cudaGetLastError();
    }

    cudaMemGetInfo(&p_Out.vramFreeBytes, &p_Out.vramTotalBytes);

    // --- one-time allocation -------------------------------------------------
    const size_t waveformBytes = size_t(kWaveformColumns) * kWaveformLevels * kPlaneCount * sizeof(unsigned int);
    const size_t vectorBytes   = size_t(kVectorscopeSize) * kVectorscopeSize * kBandCount * sizeof(unsigned int);

    cudaError_t err = cudaSuccess;
    if (!impl.waveform)
    {
        if ((err = cudaMalloc(&impl.waveform, waveformBytes)) != cudaSuccess) return Fail(p_Out, "cudaMalloc waveform", err);
        if ((err = cudaMalloc(&impl.vector, vectorBytes)) != cudaSuccess)     return Fail(p_Out, "cudaMalloc vector", err);
    }
    if (!impl.eventsCreated)
    {
        for (int i = 0; i < Impl::kRing; ++i)
        {
            cudaEventCreate(&impl.evStart[i]);
            cudaEventCreate(&impl.evAfterCopy[i]);
            cudaEventCreate(&impl.evAfterScatter[i]);
            cudaEventCreate(&impl.evAfterPreview[i]);
            cudaEventCreate(&impl.evAfterPublish[i]);
        }
        impl.eventsCreated = true;
    }

    // --- publish leg allocation, once -----------------------------------------
    if (p_Args.work >= kWorkPlusPublish && p_Args.publishMode != kPublishOff)
    {
        if (!impl.binsDevice)
        {
            if ((err = cudaMalloc(&impl.binsDevice, kBinBytesTotal)) != cudaSuccess)
                return Fail(p_Out, "cudaMalloc bins", err);
            cudaMemsetAsync(impl.binsDevice, 0, kBinBytesTotal, stream);
        }
        if (!impl.slotHost && !impl.EnsureSlot())
        {
            p_Out.publishRegistered = -1;
            std::snprintf(p_Out.publishNote, sizeof(p_Out.publishNote),
                          "could not map a %zu-byte slot view", kSlotBytes);
        }

        // Pin the mapped view if asked. This is the open question: a file-mapping
        // view is not ordinary heap, and if the driver refuses it the real tap is
        // stuck with a staged, host-blocking copy. Attempted once - a failure is
        // reported and remembered, never retried per frame.
        if (impl.slotHost &&
            (p_Args.publishMode == kPublishPinned || p_Args.publishMode == kPublishPinnedSync) &&
            !impl.slotRegistered && impl.registerState == 0)
        {
            const cudaError_t rerr = cudaHostRegister(impl.slotHost, kSlotBytes,
                                                      cudaHostRegisterDefault);
            if (rerr == cudaSuccess)
            {
                impl.slotRegistered = true;
                impl.registerState = 1;
                std::snprintf(impl.registerNote, sizeof(impl.registerNote), "mapped view pinned");
            }
            else
            {
                impl.registerState = -1;
                std::snprintf(impl.registerNote, sizeof(impl.registerNote),
                              "cudaHostRegister refused the mapped view: %s",
                              cudaGetErrorString(rerr));
                cudaGetLastError();
            }
        }
        p_Out.publishRegistered = impl.slotRegistered ? 1 : impl.registerState;
        if (impl.registerNote[0])
            std::snprintf(p_Out.publishNote, sizeof(p_Out.publishNote), "%s", impl.registerNote);
    }

    const int previewWidth  = p_Args.previewScale > 0.0f ? int(p_Args.width  * p_Args.previewScale) : 0;
    const int previewHeight = p_Args.previewScale > 0.0f ? int(p_Args.height * p_Args.previewScale) : 0;
    const size_t previewRowBytes = size_t(previewWidth) * 3;
    const size_t previewBytes    = previewRowBytes * size_t(previewHeight);

    if (p_Args.work >= kWorkPlusPreview && previewBytes > impl.previewCapacity)
    {
        if (impl.previewDevice) cudaFree(impl.previewDevice);
        if (impl.previewHost)   cudaFreeHost(impl.previewHost);
        impl.previewDevice = 0; impl.previewHost = 0; impl.previewCapacity = 0;

        if ((err = cudaMalloc(&impl.previewDevice, previewBytes)) != cudaSuccess) return Fail(p_Out, "cudaMalloc preview", err);
        if ((err = cudaHostAlloc(reinterpret_cast<void**>(&impl.previewHost), previewBytes, cudaHostAllocDefault)) != cudaSuccess)
            return Fail(p_Out, "cudaHostAlloc preview", err);
        impl.previewCapacity = previewBytes;
    }

    // --- enqueue -------------------------------------------------------------
    const int slot = impl.frame % Impl::kRing;

    cudaEventRecord(impl.evStart[slot], stream);

    // The mandatory passthrough. On-card, so this is the copy that costs 10.5ms
    // of DRAM bandwidth on the CPU path and should cost almost nothing here -
    // the number this whole spike exists to compare against.
    err = cudaMemcpy2DAsync(p_Args.dstDevice, p_Args.dstRowBytes,
                            p_Args.srcDevice, p_Args.srcRowBytes,
                            size_t(p_Args.width) * 4 * sizeof(float), size_t(p_Args.height),
                            cudaMemcpyDeviceToDevice, stream);
    if (err != cudaSuccess) return Fail(p_Out, "cudaMemcpy2DAsync passthrough", err);
    cudaEventRecord(impl.evAfterCopy[slot], stream);

    if (p_Args.work >= kWorkPlusScatter)
    {
        cudaMemsetAsync(impl.waveform, 0, waveformBytes, stream);
        cudaMemsetAsync(impl.vector, 0, vectorBytes, stream);

        const dim3 block(32, 8);
        const dim3 grid((p_Args.width + block.x - 1) / block.x,
                        (p_Args.height + block.y - 1) / block.y);
        if (p_Args.atomicMode == kAtomicsAggregated)
        {
            ScatterKernel<true><<<grid, block, 0, stream>>>(
                static_cast<const float*>(p_Args.srcDevice),
                p_Args.width, p_Args.height, p_Args.srcRowBytes / sizeof(float),
                impl.waveform, impl.vector, p_Args.scatterContent);
        }
        else
        {
            ScatterKernel<false><<<grid, block, 0, stream>>>(
                static_cast<const float*>(p_Args.srcDevice),
                p_Args.width, p_Args.height, p_Args.srcRowBytes / sizeof(float),
                impl.waveform, impl.vector, p_Args.scatterContent);
        }

        if ((err = cudaGetLastError()) != cudaSuccess) return Fail(p_Out, "ScatterKernel launch", err);
    }
    cudaEventRecord(impl.evAfterScatter[slot], stream);

    if (p_Args.work >= kWorkPlusPreview && previewBytes > 0)
    {
        const dim3 block(16, 16);
        const dim3 grid((previewWidth + block.x - 1) / block.x,
                        (previewHeight + block.y - 1) / block.y);
        PreviewKernel<<<grid, block, 0, stream>>>(
            static_cast<const float*>(p_Args.srcDevice),
            p_Args.width, p_Args.height, p_Args.srcRowBytes / sizeof(float),
            impl.previewDevice, previewWidth, previewHeight, previewRowBytes);
        if ((err = cudaGetLastError()) != cudaSuccess) return Fail(p_Out, "PreviewKernel launch", err);

        // The leg that matters: a small D2H, not a readback of the float frame.
        err = cudaMemcpyAsync(impl.previewHost, impl.previewDevice, previewBytes,
                              cudaMemcpyDeviceToHost, stream);
        if (err != cudaSuccess) return Fail(p_Out, "cudaMemcpyAsync preview D2H", err);
    }
    cudaEventRecord(impl.evAfterPreview[slot], stream);

    // --- the publish leg ------------------------------------------------------
    //
    // Five separate copies into their wire-format offsets, not one fused copy:
    // the real publisher writes five distinct arrays, and per-copy launch
    // overhead is part of what is being priced. The preview goes to its own
    // offset afterwards, exactly as ScopePublisher::Publish lays it out.
    //
    // Nothing here is synchronised. A real tap does have to know the copies
    // landed before it advertises the slot through the seqlock - either by
    // waiting, or by advertising one frame late the way these event rings
    // already read one frame late. That choice is a design decision for the
    // port; this measures the transfer it would be waiting on.
    if (p_Args.work >= kWorkPlusPublish && p_Args.publishMode != kPublishOff &&
        impl.slotHost && impl.binsDevice)
    {
        size_t dstOffset = kSlotHeaderBytes;
        size_t srcOffset = 0;
        const size_t sizes[5] = { kWaveformBytes, kHistogramBytes, kVectorscopeBytes,
                                  kTwinPeaksBytes, kTraceBytes };
        for (int i = 0; i < 5; ++i)
        {
            err = cudaMemcpyAsync(impl.slotHost + dstOffset, impl.binsDevice + srcOffset,
                                  sizes[i], cudaMemcpyDeviceToHost, stream);
            if (err != cudaSuccess) return Fail(p_Out, "cudaMemcpyAsync bins D2H", err);
            dstOffset += sizes[i];
            srcOffset += sizes[i];
        }
        p_Out.publishBytes = kBinBytesTotal;

        if (p_Args.work >= kWorkPlusPreview && previewBytes > 0 && impl.previewDevice)
        {
            err = cudaMemcpyAsync(impl.slotHost + dstOffset, impl.previewDevice, previewBytes,
                                  cudaMemcpyDeviceToHost, stream);
            if (err != cudaSuccess) return Fail(p_Out, "cudaMemcpyAsync preview publish", err);
            p_Out.publishBytes += previewBytes;
        }
    }
    cudaEventRecord(impl.evAfterPublish[slot], stream);

    // The seqlock handshake, measured. A tap cannot advertise a slot until the
    // copies have actually landed, so either the render thread waits here or a
    // separate thread waits on its behalf. This prices the first option.
    //
    // Note what the wait necessarily includes: the stream is serialised, so
    // waiting for the publish event also waits for the passthrough copy and
    // every kernel ahead of it. That is not an artefact - it is the real cost
    // of the render thread being the one that publishes.
    //
    // cudaEventSynchronize on our own event, never cudaDeviceSynchronize, which
    // ofxGPURender.h forbids outright for a plugin handed a stream.
    if (p_Args.publishMode == kPublishPinnedSync && impl.slotHost)
    {
        const auto tWait = std::chrono::steady_clock::now();
        cudaEventSynchronize(impl.evAfterPublish[slot]);
        p_Out.msPublishWait = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tWait).count();
    }

    impl.evValid[slot] = true;

    // If the host did NOT give us a stream we are on the default stream and owe
    // it completion before returning (ofxGPURender.h is explicit about both
    // halves of this contract). With a stream, we return immediately and read
    // last frame's events below - never cudaDeviceSynchronize.
    if (!stream)
    {
        if ((err = cudaStreamSynchronize(0)) != cudaSuccess) return Fail(p_Out, "cudaStreamSynchronize", err);
    }

    // --- read the PREVIOUS frame's events, which are certainly done ----------
    const int prev = (impl.frame + 1) % Impl::kRing;
    if (impl.evValid[prev] && cudaEventQuery(impl.evAfterPublish[prev]) == cudaSuccess)
    {
        float ms = 0.0f;
        if (cudaEventElapsedTime(&ms, impl.evStart[prev], impl.evAfterCopy[prev]) == cudaSuccess) p_Out.msPassthrough = ms;
        if (cudaEventElapsedTime(&ms, impl.evAfterCopy[prev], impl.evAfterScatter[prev]) == cudaSuccess) p_Out.msScatter = ms;
        if (cudaEventElapsedTime(&ms, impl.evAfterScatter[prev], impl.evAfterPreview[prev]) == cudaSuccess) p_Out.msPreview = ms;
        if (cudaEventElapsedTime(&ms, impl.evAfterPreview[prev], impl.evAfterPublish[prev]) == cudaSuccess) p_Out.msPublish = ms;
        if (cudaEventElapsedTime(&ms, impl.evStart[prev], impl.evAfterPublish[prev]) == cudaSuccess) p_Out.msTotalGpu = ms;
    }
    cudaGetLastError();

    ++impl.frame;
    return true;
}

} // namespace scopespike
