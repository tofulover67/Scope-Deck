// The arguments and statistics a GPU tap backend takes and reports - shared by
// the CUDA backend (ScopeCuda.h, Windows) and the Metal backend (ScopeMetal.h,
// macOS) so plugin/ScopeTap.cpp drives either through one set of names. No
// vendor header is included here or by anything that includes this.

#pragma once

#include "ScopeCore.h"

#include <cstdint>

namespace scopedeck
{

struct GpuTapArgs
{
    const void* srcDevice = nullptr;    // kOfxImagePropData, source clip - a device
                                        // pointer under CUDA, an id<MTLBuffer> under Metal
    void*       dstDevice = nullptr;    // ... output clip
    size_t      srcRowBytes = 0;
    size_t      dstRowBytes = 0;
    int         width = 0;
    int         height = 0;
    void*       stream = nullptr;       // the host's cudaStream_t, or its id<MTLCommandQueue>

    ScopeParams params;
    double      timelineTime = 0.0;
    uint32_t    instanceId = 0;

    bool        publishPreview = true;
    float       previewScale = 0.5f;
};

struct GpuTapStats
{
    double msEnqueue = 0.0;     // wall time the render thread actually spent
    uint64_t published = 0;     // frames committed by the publisher thread
    uint64_t skipped = 0;       // frames dropped because the ring was full
    bool     slotPinned = false;   // CUDA: the block is page-locked; Metal: unified memory, always true
    char     note[160] = {};

    // Device-side time for the most recently COMPLETED frame, which is not the
    // frame the render thread just enqueued - measuring the current one would
    // mean waiting for it, which is the 9.57 ms mistake GPU_PORT_HANDOFF.md §3
    // priced. They come from the device's own timestamps, read on the publisher
    // thread once the work has landed.
    //
    // They exist because msEnqueue above is render-thread wall time and nothing
    // more: it says whether Resolve will drop frames, which is the ship gate,
    // but it is not what the GPU tap costs. Quoting it as such is the same
    // error as the fetch=/copy= conflation in §1 of GPU_PORT_HANDOFF.md.
    // msDeviceTotal spans the passthrough, every kernel and the publish leg.
    double msDeviceTotal = 0.0;
    double msDeviceKernels = 0.0;   // passthrough + scatter/histogram/trace/probe/preview
    double msDevicePublish = 0.0;   // CUDA: the D2H into the slot; Metal: the worker's memcpy into it
    bool   deviceTimingValid = false;
};

} // namespace scopedeck
