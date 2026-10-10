// Metal implementation of the scope reduction. See ScopeMetal.h for what this
// is and who calls it, and ScopeCuda.cu for the CUDA original every structural
// decision here mirrors.
//
// The governing rule is the same: this must agree with core/ScopeCore.cpp
// bit-for-bit, and bit-exactness is the acceptance criterion the conformance
// harness enforces. The three places the natural GPU spelling disagrees with
// the CPU hold on Metal exactly as they did on CUDA:
//
//   1. Column assignment: a per-column boundary table built by the CPU's own
//      integer arithmetic, uploaded; never floor(x*cols/w).
//   2. Cb/Cr scaling: 0.5f/(1-Kb) and 0.5f/(1-Kr) computed once on the CPU and
//      passed in; never divided per pixel, never a literal.
//   3. Fused multiply-add: Metal contracts a*b+c into an FMA by default, and
//      fastMathEnabled = NO does NOT stop it - measured on an M1 with the
//      harness's own detector values (a*a - c came back 2^-24 instead of 0).
//      `#pragma METAL fp contract(off)` in the source is what stops it.
//
// The histogram is derived from the waveform by summing over columns, exactly
// as ScopeEngine::Analyse does.

#include "ScopeMetal.h"
#include "ScopeShm.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if !__has_feature(objc_arc)
#error "ScopeMetal.mm is written for ARC (see CMakeLists.txt: -fobjc-arc on this file)"
#endif

namespace scopedeck
{

namespace
{

// =============================================================================
// The kernels. One MSL source, compiled at run time by the Metal framework -
// which is also what lets this build with the Command Line Tools alone.
//
// Index arithmetic matches ScopeTypes.h's helpers:
//   waveform    (col * 256 + level) * 4 + plane
//   vectorscope (band * 256 + crBin) * 256 + cbBin
//   twin peaks  diamond * 65536 + levelBin * 256 + diffBin
//   trace       (row * 512 + col) * 4 + plane
// Every index fits in 32 bits, so no 64-bit integer arithmetic is needed.
// =============================================================================

const char* kMetalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

// Trap 3. Without this the luma dot product is fused and boundary values land
// in different bins from the CPU. fastMathEnabled = NO alone does not do it.
#pragma METAL fp contract(off)

constant uint kWaveformColumns  = 512;
constant uint kWaveformLevels   = 256;
constant uint kPlaneCount       = 4;
constant uint kVectorscopeSize  = 256;
constant uint kTwinPeaksSize    = 256;
constant uint kTwinPeaksCells   = 65536;
constant uint kWaveformTraceRows = 256;
constant uint kScatterThreads   = 256;   // threads per scatter threadgroup; 8 SIMD groups of 32

// Everything the kernels need that the CPU already computed. The range
// constants travel here rather than as MSL literals so the float the GPU uses
// is the very float ScopeTypes.h defines, not a re-rounded decimal of it.
struct Params
{
    uint  width;
    uint  height;
    uint  sampledRows;
    uint  rowStep;
    uint  rowFloats;      // floats from one source row to the next
    uint  bandRows;       // sampled rows per scatter threadgroup band
    uint  previewWidth;
    uint  previewHeight;
    float lumaR, lumaG, lumaB;
    float cbScale, crScale;
    float binScale, chromaScale, twinScale;
    float binRangeLow, chromaRangeLow, twinDiffRangeLow;
    float lowRangeMax, highRangeMin;
};

// --- bin arithmetic, mirroring ScopeCore.cpp's anonymous-namespace copies -----

inline uint binOf(float v, float low, float scale, uint count)
{
    const float t = (v - low) * scale;
    if (!(t > 0.0f)) return 0u;                       // also catches NaN
    if (t >= float(count)) return count - 1u;         // +inf; see ScopeCore.cpp's BinOf
    const uint bin = uint(t);
    return (bin >= count) ? (count - 1u) : bin;
}

// --- float min/max with the CPU's exact NaN behaviour --------------------------
//
// ScopeCore uses bare `if (v < lo)` / `if (v > hi)`, so a NaN never becomes an
// extreme and never propagates. min()/max() do not behave this way for all
// inputs. A CAS loop with the same bare comparison reproduces the CPU exactly,
// including "every pixel was NaN" leaving the initial +/-inf in place.

inline void atomicMinFloat(device atomic_uint* addr, float v)
{
    if (!(v == v)) return;
    uint old = atomic_load_explicit(addr, memory_order_relaxed);
    for (;;)
    {
        if (!(v < as_type<float>(old))) return;
        if (atomic_compare_exchange_weak_explicit(addr, &old, as_type<uint>(v),
                                                  memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

inline void atomicMaxFloat(device atomic_uint* addr, float v)
{
    if (!(v == v)) return;
    uint old = atomic_load_explicit(addr, memory_order_relaxed);
    for (;;)
    {
        if (!(v > as_type<float>(old))) return;
        if (atomic_compare_exchange_weak_explicit(addr, &old, as_type<uint>(v),
                                                  memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

// --- SIMD-group-aggregated atomic increment ------------------------------------
//
// The Metal spelling of CUDA's __match_any_sync aggregation, which §3 of the
// handoff measured as mandatory: plain atomics on flat content (every pixel in
// one cell) were slower than the CPU path - and on this M1 the same holds,
// 14 ms against 7 on a black frame. There is no match instruction, so this
// loops: ballot the lanes still pending, take the lowest as leader, broadcast
// its address, ballot who matches, and the leader adds the count. One
// iteration per DISTINCT address in the SIMD group.
//
// Bounded, unlike CUDA's: an unbounded loop costs 32 rounds on fully spread
// content, where nothing aggregates and plain atomics are uncontended anyway
// - measured 45 ms for the frame against 8 ms with plain atomics. After
// kAggregateRounds the lanes still pending add on their own. Flat content
// finishes in one round either way, which is the case that matters; two
// rounds measured as well as any other count on footage-like content (see
// GPU_PORT_HANDOFF.md's Metal section for the matrix). Must be called by
// every lane of the SIMD group together (uniform control flow); a lane with
// nothing to add passes valid = false.

constant uint kAggregateRounds = 2;

inline void aggregatedAdd(device atomic_uint* base, uint idx, bool valid, uint lane)
{
    bool done = !valid;
    for (uint round = 0; round < kAggregateRounds; ++round)
    {
        const simd_vote::vote_t pending = (simd_vote::vote_t)simd_ballot(!done);
        if (pending == 0) return;
        const uint leader = uint(ctz(pending));
        const uint leaderIdx = simd_shuffle(idx, ushort(leader));
        const bool match = !done && (idx == leaderIdx);
        const simd_vote::vote_t m = (simd_vote::vote_t)simd_ballot(match);
        if (lane == leader)
            atomic_fetch_add_explicit(base + idx, uint(popcount(m)), memory_order_relaxed);
        if (match) done = true;
    }
    if (!done) atomic_fetch_add_explicit(base + idx, 1u, memory_order_relaxed);
}

// The waveform's threadgroup atomics are NOT aggregated. The same ballot loop
// on them was measured and bought nothing on this GPU - threadgroup atomics
// are cheap enough that the cross-lane chain costs more than the contention it
// removes - so they are plain.

// --- scatter -------------------------------------------------------------------
//
// One threadgroup per (waveform column, band of sampled rows). That mapping is
// what makes the waveform cheap: the column's 256 levels x 4 planes are 4 KB,
// which fits threadgroup memory, so the four increments per pixel go there and
// the column is flushed to device memory once per threadgroup - into the
// waveform AND the histogram, which is the waveform summed over columns and so
// needs no gather kernel of its own. The vectorscope and twin peaks (768 KB
// and 512 KB) fit nowhere private, so they take the aggregated device atomics
// above.
//
// Thread t owns pixel column x0 + t % cw and every (256 / cw)-th row from
// t / cw: two integer divisions per THREAD, where the obvious p % cw / p / cw
// walk costs two per PIXEL and measured as a visible share of the frame on
// this GPU. Threads whose row start falls past the stride are idle. Every
// thread runs the same number of trips so every lane reaches every ballot; a
// trip past the band's end simply has nothing to add. rowStep selects rows
// exactly as the CPU does: y = ry * rowStep.

kernel void scatter(const device float4* pixels  [[buffer(0)]],
                    constant Params& P            [[buffer(1)]],
                    const device int* colStart    [[buffer(2)]],
                    device atomic_uint* waveform  [[buffer(3)]],
                    device atomic_uint* vectorscope [[buffer(4)]],
                    device atomic_uint* twinPeaks [[buffer(5)]],
                    device atomic_uint* minMax    [[buffer(6)]],   // minR,minG,minB,maxR,maxG,maxB as float bits
                    device atomic_uint* histogram [[buffer(7)]],
                    uint2 tgid   [[threadgroup_position_in_grid]],
                    uint  tid    [[thread_index_in_threadgroup]],
                    uint  lane   [[thread_index_in_simdgroup]],
                    uint  simdId [[simdgroup_index_in_threadgroup]])
{
    threadgroup atomic_uint tgWave[kWaveformLevels * kPlaneCount];
    threadgroup float tgMin[kScatterThreads / 32][3];
    threadgroup float tgMax[kScatterThreads / 32][3];

    for (uint i = tid; i < kWaveformLevels * kPlaneCount; i += kScatterThreads)
        atomic_store_explicit(&tgWave[i], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint col = tgid.x;
    const int  x0  = colStart[col];
    const int  x1  = colStart[col + 1];
    const uint cw  = (x1 > x0) ? uint(x1 - x0) : 0u;
    const uint r0  = tgid.y * P.bandRows;
    const uint r1  = min(r0 + P.bandRows, P.sampledRows);

    // This thread's pixel column and row stride; cw == 0 is an empty column
    // at tiny frame sizes, where nothing is sampled.
    const uint x      = (cw > 0u) ? uint(x0) + tid % cw : 0u;
    const uint ryBase = (cw > 0u) ? r0 + tid / cw : r1;
    const uint stride = (cw > 0u) ? kScatterThreads / cw : 1u;
    const bool owner  = (cw > 0u) && (tid / cw) < stride;
    const uint rowFloat4s = P.rowFloats / 4u;
    const uint trips = (cw > 0u) ? (r1 - r0 + stride - 1u) / stride : 0u;   // uniform

    const float inf = as_type<float>(0x7f800000u);
    float minR = inf, minG = inf, minB = inf;
    float maxR = -inf, maxG = -inf, maxB = -inf;

    // One row per trip. Loading several rows ahead was tried and measured no
    // better once the band count gave the GPU enough threadgroups to keep in
    // flight (see GPU_PORT_HANDOFF.md's Metal section), so the simple form
    // stays. The trip count is uniform across the threadgroup so every lane
    // reaches every ballot.
    for (uint trip = 0; trip < trips; ++trip)
    {
        float4 px[1];
        bool   valid[1];
        {
            const uint k = 0u;
            const uint ry = ryBase + trip * stride;
            valid[k] = owner && ry < r1;
            px[k] = valid[k] ? pixels[(ry * P.rowStep) * rowFloat4s + x] : float4(0.0f);
        }

        for (uint k = 0; k < 1u; ++k)
        {
            uint idxW0 = 0u, idxW1 = 0u, idxW2 = 0u, idxW3 = 0u;
            uint idxV = 0u, idxGB = 0u, idxGR = 0u;

            if (valid[k])
            {
                const float r = px[k].x;
                const float g = px[k].y;
                const float b = px[k].z;

                if (r < minR) minR = r;
                if (r > maxR) maxR = r;
                if (g < minG) minG = g;
                if (g > maxG) maxG = g;
                if (b < minB) minB = b;
                if (b > maxB) maxB = b;

                // Same operation order as ScopeCore's `lumaR * r + lumaG * g + lumaB * b`.
                const float luma = P.lumaR * r + P.lumaG * g + P.lumaB * b;
                idxW0 = binOf(r,    P.binRangeLow, P.binScale, kWaveformLevels) * kPlaneCount + 0u;
                idxW1 = binOf(g,    P.binRangeLow, P.binScale, kWaveformLevels) * kPlaneCount + 1u;
                idxW2 = binOf(b,    P.binRangeLow, P.binScale, kWaveformLevels) * kPlaneCount + 2u;
                idxW3 = binOf(luma, P.binRangeLow, P.binScale, kWaveformLevels) * kPlaneCount + 3u;

                const float cb = (b - luma) * P.cbScale;
                const float cr = (r - luma) * P.crScale;
                const uint band = (luma < P.lowRangeMax) ? 0u : ((luma >= P.highRangeMin) ? 2u : 1u);
                idxV = (band * kVectorscopeSize + binOf(cr, P.chromaRangeLow, P.chromaScale, kVectorscopeSize)) * kVectorscopeSize
                     + binOf(cb, P.chromaRangeLow, P.chromaScale, kVectorscopeSize);

                const float levelGB = (g + b) * 0.5f;
                const float diffGB  = (g - b) * 0.5f;
                idxGB = 0u * kTwinPeaksCells
                      + binOf(levelGB, P.binRangeLow, P.binScale, kTwinPeaksSize) * kTwinPeaksSize
                      + binOf(diffGB, P.twinDiffRangeLow, P.twinScale, kTwinPeaksSize);

                const float levelGR = (g + r) * 0.5f;
                const float diffGR  = (g - r) * 0.5f;
                idxGR = 1u * kTwinPeaksCells
                      + binOf(levelGR, P.binRangeLow, P.binScale, kTwinPeaksSize) * kTwinPeaksSize
                      + binOf(diffGR, P.twinDiffRangeLow, P.twinScale, kTwinPeaksSize);
            }

            if (valid[k])
            {
                atomic_fetch_add_explicit(&tgWave[idxW0], 1u, memory_order_relaxed);
                atomic_fetch_add_explicit(&tgWave[idxW1], 1u, memory_order_relaxed);
                atomic_fetch_add_explicit(&tgWave[idxW2], 1u, memory_order_relaxed);
                atomic_fetch_add_explicit(&tgWave[idxW3], 1u, memory_order_relaxed);
            }
            aggregatedAdd(vectorscope, idxV, valid[k], lane);
            aggregatedAdd(twinPeaks, idxGB, valid[k], lane);
            aggregatedAdd(twinPeaks, idxGR, valid[k], lane);
        }
    }

    // Extremes: butterfly across the SIMD group with the same bare comparison
    // (a NaN from another lane is never taken), then across SIMD groups, then
    // one CAS per threadgroup.
    for (uint off = 16u; off > 0u; off >>= 1u)
    {
        float o;
        o = simd_shuffle_xor(minR, ushort(off)); if (o < minR) minR = o;
        o = simd_shuffle_xor(minG, ushort(off)); if (o < minG) minG = o;
        o = simd_shuffle_xor(minB, ushort(off)); if (o < minB) minB = o;
        o = simd_shuffle_xor(maxR, ushort(off)); if (o > maxR) maxR = o;
        o = simd_shuffle_xor(maxG, ushort(off)); if (o > maxG) maxG = o;
        o = simd_shuffle_xor(maxB, ushort(off)); if (o > maxB) maxB = o;
    }
    if (lane == 0u)
    {
        tgMin[simdId][0] = minR; tgMin[simdId][1] = minG; tgMin[simdId][2] = minB;
        tgMax[simdId][0] = maxR; tgMax[simdId][1] = maxG; tgMax[simdId][2] = maxB;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0u)
    {
        float lo[3] = { inf, inf, inf }, hi[3] = { -inf, -inf, -inf };
        for (uint s = 0; s < kScatterThreads / 32; ++s)
            for (uint c = 0; c < 3; ++c)
            {
                if (tgMin[s][c] < lo[c]) lo[c] = tgMin[s][c];
                if (tgMax[s][c] > hi[c]) hi[c] = tgMax[s][c];
            }
        for (uint c = 0; c < 3; ++c)
        {
            atomicMinFloat(minMax + c, lo[c]);
            atomicMaxFloat(minMax + 3 + c, hi[c]);
        }
    }

    // Flush the column into the waveform, and the same counts into the
    // histogram - which is exactly the waveform summed over columns, so this
    // is the derivation ScopeEngine::Analyse does, just spread across the
    // threadgroups. Only cells this threadgroup touched: on flat content that
    // is a handful of the 1024.
    for (uint i = tid; i < kWaveformLevels * kPlaneCount; i += kScatterThreads)
    {
        const uint v = atomic_load_explicit(&tgWave[i], memory_order_relaxed);
        if (v != 0u)
        {
            atomic_fetch_add_explicit(waveform + col * (kWaveformLevels * kPlaneCount) + i, v, memory_order_relaxed);
            // tgWave is [level][plane]; the histogram is [plane][level].
            atomic_fetch_add_explicit(histogram + (i % kPlaneCount) * kWaveformLevels + i / kPlaneCount, v, memory_order_relaxed);
        }
    }
}

// One thread per (trace row, column). The inner sum walks the column's pixel
// range in ascending x, sequentially - NOT a parallel reduction. Float addition
// is not associative, so a tree sum would give a different, equally valid
// answer and fail the exact comparison.
kernel void trace(const device float* pixels [[buffer(0)]],
                  constant Params& P          [[buffer(1)]],
                  const device int* colStart  [[buffer(2)]],
                  device float* out           [[buffer(3)]],
                  uint2 id [[thread_position_in_grid]])
{
    const uint col = id.x;
    const uint row = id.y;
    if (col >= kWaveformColumns || row >= kWaveformTraceRows) return;

    const uint srcY = (row * (P.height - 1u)) / (kWaveformTraceRows - 1u);
    const device float* rowPixels = pixels + srcY * P.rowFloats;
    const int x0 = colStart[col];
    const int x1 = colStart[col + 1];

    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    int counted = 0;
    for (int x = x0; x < x1; ++x)
    {
        const device float* px = rowPixels + uint(x) * 4u;
        sumR += px[0];
        sumG += px[1];
        sumB += px[2];
        ++counted;
    }

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (counted > 0)
    {
        const float inv = 1.0f / float(counted);
        r = sumR * inv;
        g = sumG * inv;
        b = sumB * inv;
    }
    const float luma = P.lumaR * r + P.lumaG * g + P.lumaB * b;

    const uint base = (row * kWaveformColumns + col) * kPlaneCount;
    out[base + 0] = r;
    out[base + 1] = g;
    out[base + 2] = b;
    out[base + 3] = luma;
}

// Centre pixel, untouched - the value that gets cross-checked against Resolve's
// own colour picker.
kernel void probe(const device float* pixels [[buffer(0)]],
                  constant Params& P          [[buffer(1)]],
                  device float* out           [[buffer(2)]],
                  uint id [[thread_position_in_grid]])
{
    if (id != 0u) return;
    const device float* px = pixels + (P.height / 2u) * P.rowFloats + (P.width / 2u) * 4u;
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
}

// Preview downscale, matching ScopeEngine::BuildPreview exactly: the centre of
// each destination cell is point-sampled, clipped to [0,1], and rows are
// written bottom-up. Deliberately not "better" filtering: a viewfinder that
// framed differently from the CPU path would be a second thing to explain.
inline uchar toDisplay8(float v)
{
    if (!(v > 0.0f)) return uchar(0);
    if (v >= 1.0f) return uchar(255);
    return uchar(v * 255.0f + 0.5f);
}

kernel void preview(const device float* pixels [[buffer(0)]],
                    constant Params& P          [[buffer(1)]],
                    device uchar* out           [[buffer(2)]],
                    uint2 id [[thread_position_in_grid]])
{
    const uint dx = id.x, dy = id.y;
    if (dx >= P.previewWidth || dy >= P.previewHeight) return;

    const uint y0 = (dy * P.height) / P.previewHeight;
    const uint y1 = (dy + 1u == P.previewHeight) ? P.height : ((dy + 1u) * P.height) / P.previewHeight;
    const uint x0 = (dx * P.width) / P.previewWidth;
    const uint x1 = (dx + 1u == P.previewWidth) ? P.width : ((dx + 1u) * P.width) / P.previewWidth;
    const uint yMid = (y0 + y1) / 2u;
    const uint xMid = (x0 + x1) / 2u;

    const device float* px = pixels + yMid * P.rowFloats + xMid * 4u;
    device uchar* o = out + ((P.previewHeight - 1u - dy) * P.previewWidth + dx) * 3u;
    o[0] = toDisplay8(px[0]);
    o[1] = toDisplay8(px[1]);
    o[2] = toDisplay8(px[2]);
}
)MSL";

// Mirrors the MSL struct above, field for field.
struct Params
{
    uint32_t width, height, sampledRows, rowStep, rowFloats, bandRows, previewWidth, previewHeight;
    float lumaR, lumaG, lumaB;
    float cbScale, crScale;
    float binScale, chromaScale, twinScale;
    float binRangeLow, chromaRangeLow, twinDiffRangeLow;
    float lowRangeMax, highRangeMin;
};

constexpr uint32_t kScatterThreads = 256;

// The three bin scales, computed exactly as ScopeCore.cpp's constexpr copies.
constexpr float kBinScaleHost =
    static_cast<float>(kWaveformLevels - 1) / (kBinRangeHigh - kBinRangeLow);
constexpr float kChromaBinScaleHost =
    static_cast<float>(kVectorscopeSize - 1) / (kChromaRangeHigh - kChromaRangeLow);
constexpr float kTwinDiffBinScaleHost =
    static_cast<float>(kTwinPeaksSize - 1) / (kTwinPeaksDiffRangeHigh - kTwinPeaksDiffRangeLow);

void FillParams(Params& p, int p_Width, int p_Height, int p_RowStep, size_t p_RowFloats,
                const LumaWeights& p_Luma, uint32_t p_PreviewW, uint32_t p_PreviewH)
{
    const int sampledRows = (p_Height - 1) / p_RowStep + 1;
    p.width        = static_cast<uint32_t>(p_Width);
    p.height       = static_cast<uint32_t>(p_Height);
    p.sampledRows  = static_cast<uint32_t>(sampledRows);
    p.rowStep      = static_cast<uint32_t>(p_RowStep);
    p.rowFloats    = static_cast<uint32_t>(p_RowFloats);
    // Eight bands of rows per column for a frame of HD height or more: 4096
    // threadgroups for the GPU to keep in flight, at the cost of eight 4 KB
    // flushes per column instead of one. Measured on an M1 (UHD, atomics
    // removed, so the load path alone): 1 band 5.7 ms, 2 bands 5.3, 4 bands
    // 3.8, 8 bands 3.5, 16 bands 4.9 - past eight the flush costs more than
    // the parallelism returns. See the bench before changing it.
    p.bandRows     = static_cast<uint32_t>(sampledRows >= 1080 ? (sampledRows + 7) / 8 : sampledRows);
    if (p.bandRows == 0) p.bandRows = 1;
    p.previewWidth  = p_PreviewW;
    p.previewHeight = p_PreviewH;
    p.lumaR = p_Luma.r; p.lumaG = p_Luma.g; p.lumaB = p_Luma.b;
    p.cbScale = 0.5f / (1.0f - p_Luma.b);
    p.crScale = 0.5f / (1.0f - p_Luma.r);
    p.binScale = kBinScaleHost; p.chromaScale = kChromaBinScaleHost; p.twinScale = kTwinDiffBinScaleHost;
    p.binRangeLow = kBinRangeLow; p.chromaRangeLow = kChromaRangeLow; p.twinDiffRangeLow = kTwinPeaksDiffRangeLow;
    p.lowRangeMax = kVectorscopeLowMax; p.highRangeMin = kVectorscopeHighMin;
}

uint32_t BandCount(const Params& p)
{
    return p.sampledRows == 0 ? 0u : (p.sampledRows + p.bandRows - 1) / p.bandRows;
}

// Column boundary table, built by the CPU's own arithmetic (trap 1).
void BuildColumnTable(int p_Width, std::vector<int>& p_ColStart)
{
    p_ColStart.resize(static_cast<size_t>(kWaveformColumns) + 1);
    for (uint32_t col = 0; col <= kWaveformColumns; ++col)
    {
        const uint64_t x = (static_cast<uint64_t>(col) * static_cast<uint64_t>(p_Width)) / kWaveformColumns;
        p_ColStart[col] = static_cast<int>(x < static_cast<uint64_t>(p_Width) ? x : static_cast<uint64_t>(p_Width));
    }
    p_ColStart[kWaveformColumns] = p_Width;
}

// --- result buffer layout ------------------------------------------------------
//
// One shared buffer per frame in flight, laid out in slot order - waveform,
// histogram, vectorscope, twin peaks, trace - so the publish leg is one memcpy
// into the slot, then the extremes and probe behind them. Every region starts
// on a 256-byte boundary, which satisfies setBuffer:offset: on every Metal
// device.

constexpr size_t Align256(size_t v) { return (v + 255) & ~size_t(255); }

constexpr size_t kWaveformBytes   = static_cast<size_t>(kWaveformCells) * sizeof(uint32_t);
constexpr size_t kHistogramBytes  = static_cast<size_t>(kHistogramCells) * sizeof(uint32_t);
constexpr size_t kVectorBytes     = static_cast<size_t>(kVectorscopeTotalCells) * sizeof(uint32_t);
constexpr size_t kTwinBytes       = static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(uint32_t);
constexpr size_t kTraceBytes      = static_cast<size_t>(kWaveformTraceCells) * sizeof(float);
constexpr size_t kBinsBytes       = kWaveformBytes + kHistogramBytes + kVectorBytes + kTwinBytes + kTraceBytes;

constexpr size_t kOffWaveform  = 0;
constexpr size_t kOffHistogram = kOffWaveform + kWaveformBytes;
constexpr size_t kOffVector    = kOffHistogram + kHistogramBytes;
constexpr size_t kOffTwin      = kOffVector + kVectorBytes;
constexpr size_t kOffTrace     = kOffTwin + kTwinBytes;
constexpr size_t kOffMinMax    = Align256(kOffTrace + kTraceBytes);
constexpr size_t kOffProbe     = kOffMinMax + 256;
constexpr size_t kResultBytes  = kOffProbe + 256;

static_assert(kOffHistogram % 256 == 0 && kOffVector % 256 == 0 && kOffTwin % 256 == 0 && kOffTrace % 256 == 0,
              "result regions must be 256-byte aligned for setBuffer:offset:");
static_assert(kHistogramOffset - kWaveformOffset == kOffHistogram &&
              kVectorscopeOffset - kWaveformOffset == kOffVector &&
              kTwinPeaksOffset - kWaveformOffset == kOffTwin &&
              kWaveformTraceOffset - kWaveformOffset == kOffTrace &&
              kPreviewOffset - kWaveformOffset == kBinsBytes,
              "result buffer layout must match the slot layout so the publish is one memcpy");

// --- the compiled kernels, once per device -------------------------------------

struct Pipelines
{
    id<MTLLibrary>              library;
    id<MTLComputePipelineState> scatter;
    id<MTLComputePipelineState> trace;
    id<MTLComputePipelineState> probe;
    id<MTLComputePipelineState> preview;
    bool ok = false;
    char error[256] = {};
};

// Keyed by device pointer identity. Resolve hands every instance the same
// queue on the same device, so this compiles once per process in practice;
// the map is there so a second device could never be handed the first one's
// pipelines.
Pipelines* PipelinesFor(id<MTLDevice> p_Device)
{
    static std::mutex* s_Lock = new std::mutex();
    static auto* s_ByDevice = new std::unordered_map<void*, Pipelines*>();
    std::lock_guard<std::mutex> lock(*s_Lock);

    Pipelines*& entry = (*s_ByDevice)[(__bridge void*)p_Device];
    if (entry) return entry;
    entry = new Pipelines();

    @autoreleasepool
    {
        NSError* err = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        // Belt and braces with the pragma in the source: fast math also licenses
        // reassociation and NaN assumptions, neither of which the CPU made.
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [p_Device newLibraryWithSource:@(kMetalSource) options:options error:&err];
        if (!library)
        {
            std::snprintf(entry->error, sizeof(entry->error), "MSL compile: %s",
                          err ? err.localizedDescription.UTF8String : "unknown");
            return entry;
        }
        entry->library = library;

        struct Fn { const char* name; id<MTLComputePipelineState> __strong* out; };
        Fn fns[] = {
            { "scatter",   &entry->scatter },
            { "trace",     &entry->trace },
            { "probe",     &entry->probe },
            { "preview",   &entry->preview },
        };
        for (Fn& fn : fns)
        {
            id<MTLFunction> f = [library newFunctionWithName:@(fn.name)];
            if (!f)
            {
                std::snprintf(entry->error, sizeof(entry->error), "kernel '%s' missing", fn.name);
                return entry;
            }
            id<MTLComputePipelineState> ps = [p_Device newComputePipelineStateWithFunction:f error:&err];
            if (!ps)
            {
                std::snprintf(entry->error, sizeof(entry->error), "pipeline '%s': %s", fn.name,
                              err ? err.localizedDescription.UTF8String : "unknown");
                return entry;
            }
            *fn.out = ps;
        }
        if (entry->scatter.maxTotalThreadsPerThreadgroup < kScatterThreads ||
            entry->scatter.threadExecutionWidth != 32)
        {
            std::snprintf(entry->error, sizeof(entry->error),
                          "scatter needs 256 threads of SIMD width 32 (device offers %lu of %lu)",
                          (unsigned long)entry->scatter.maxTotalThreadsPerThreadgroup,
                          (unsigned long)entry->scatter.threadExecutionWidth);
            return entry;
        }
        entry->ok = true;
    }
    return entry;
}

// --- encoding the frame, shared by the reducer and the tap -----------------------
//
// Zeroes the bins, seeds the extremes, and encodes every kernel into one
// command buffer. The caller owns the command buffer's lifetime: the reducer
// waits on it, the tap hands it to the hub's worker.

struct FrameInputs
{
    id<MTLBuffer> pixels;        // the frame, interleaved RGBA float
    size_t        pixelsOffset = 0;
    id<MTLBuffer> colStart;      // kWaveformColumns + 1 ints
    id<MTLBuffer> results;       // kResultBytes, laid out as above
    id<MTLBuffer> preview;       // may be nil when no preview this frame
    Params        params;
};

void EncodeFrame(id<MTLCommandBuffer> p_Cb, const Pipelines& p_Pipes, const FrameInputs& p_In)
{
    // Only the regions the scatter adds into need zeroing - waveform,
    // histogram, vectorscope, twin peaks - and they are contiguous, so one fill
    // covers them; the trace is fully overwritten by its kernel.
    {
        id<MTLBlitCommandEncoder> blit = [p_Cb blitCommandEncoder];
        [blit fillBuffer:p_In.results range:NSMakeRange(kOffWaveform, kOffTrace - kOffWaveform) value:0];
        [blit endEncoding];
    }

    // Seeds for the extremes: +inf / -inf as the CPU starts from. Written by
    // the CPU into the shared buffer before commit; the fill above does not
    // reach this region.
    {
        static const float kInf = std::numeric_limits<float>::infinity();
        float* seed = reinterpret_cast<float*>(static_cast<char*>(p_In.results.contents) + kOffMinMax);
        seed[0] = kInf; seed[1] = kInf; seed[2] = kInf;
        seed[3] = -kInf; seed[4] = -kInf; seed[5] = -kInf;
    }

    id<MTLComputeCommandEncoder> enc = [p_Cb computeCommandEncoder];   // serial: dispatches run in order

    // Buffer index 1 is the parameter block in every kernel and nothing else
    // ever binds there - the first conformance run found the histogram's
    // output bound at 1, which left the trace and probe kernels reading their
    // luma weights and frame size out of histogram counts. Re-bound before
    // every dispatch anyway: it costs nothing and it cannot be undone by a
    // later kernel's bindings again.
    const auto bindParams = [&]() { [enc setBytes:&p_In.params length:sizeof(Params) atIndex:1]; };

    // scatter
    [enc setComputePipelineState:p_Pipes.scatter];
    bindParams();
    [enc setBuffer:p_In.pixels offset:p_In.pixelsOffset atIndex:0];
    [enc setBuffer:p_In.colStart offset:0 atIndex:2];
    [enc setBuffer:p_In.results offset:kOffWaveform atIndex:3];
    [enc setBuffer:p_In.results offset:kOffVector atIndex:4];
    [enc setBuffer:p_In.results offset:kOffTwin atIndex:5];
    [enc setBuffer:p_In.results offset:kOffMinMax atIndex:6];
    [enc setBuffer:p_In.results offset:kOffHistogram atIndex:7];
    const uint32_t bands = BandCount(p_In.params);
    if (bands > 0)
        [enc dispatchThreadgroups:MTLSizeMake(kWaveformColumns, bands, 1)
            threadsPerThreadgroup:MTLSizeMake(kScatterThreads, 1, 1)];

    // trace
    [enc setComputePipelineState:p_Pipes.trace];
    bindParams();
    [enc setBuffer:p_In.pixels offset:p_In.pixelsOffset atIndex:0];
    [enc setBuffer:p_In.colStart offset:0 atIndex:2];
    [enc setBuffer:p_In.results offset:kOffTrace atIndex:3];
    [enc dispatchThreads:MTLSizeMake(kWaveformColumns, kWaveformTraceRows, 1)
        threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];

    // probe
    [enc setComputePipelineState:p_Pipes.probe];
    bindParams();
    [enc setBuffer:p_In.pixels offset:p_In.pixelsOffset atIndex:0];
    [enc setBuffer:p_In.results offset:kOffProbe atIndex:2];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

    // preview
    if (p_In.preview && p_In.params.previewWidth > 0 && p_In.params.previewHeight > 0)
    {
        [enc setComputePipelineState:p_Pipes.preview];
        bindParams();
        [enc setBuffer:p_In.pixels offset:p_In.pixelsOffset atIndex:0];
        [enc setBuffer:p_In.preview offset:0 atIndex:2];
        [enc dispatchThreads:MTLSizeMake(p_In.params.previewWidth, p_In.params.previewHeight, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    }

    [enc endEncoding];
}

// Row-by-row blit between two buffers with their own strides; one blit when
// both are packed the same way, which is the usual case.
void EncodeCopyRows(id<MTLBlitCommandEncoder> p_Blit,
                    id<MTLBuffer> p_Dst, size_t p_DstRowBytes,
                    id<MTLBuffer> p_Src, size_t p_SrcRowBytes,
                    int p_Width, int p_Height)
{
    const size_t rowBytes = static_cast<size_t>(p_Width) * 4 * sizeof(float);
    if (p_SrcRowBytes == rowBytes && p_DstRowBytes == rowBytes)
    {
        [p_Blit copyFromBuffer:p_Src sourceOffset:0 toBuffer:p_Dst destinationOffset:0
                          size:rowBytes * static_cast<size_t>(p_Height)];
        return;
    }
    for (int y = 0; y < p_Height; ++y)
    {
        [p_Blit copyFromBuffer:p_Src sourceOffset:static_cast<size_t>(y) * p_SrcRowBytes
                      toBuffer:p_Dst destinationOffset:static_cast<size_t>(y) * p_DstRowBytes
                          size:rowBytes];
    }
}

double GpuMillis(id<MTLCommandBuffer> p_Cb)
{
    const double start = p_Cb.GPUStartTime, end = p_Cb.GPUEndTime;
    return (end > start) ? (end - start) * 1000.0 : 0.0;
}

double MillisSince(std::chrono::steady_clock::time_point p_T0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p_T0).count();
}

} // namespace

// =============================================================================
// MetalReducer - the conformance entry point.
// =============================================================================

struct MetalReducer::Impl
{
    bool available = false;
    char error[256] = {};
    char device[256] = {};

    id<MTLDevice>       dev;
    id<MTLCommandQueue> queue;
    Pipelines*          pipes = nullptr;

    id<MTLBuffer> pixels;     size_t pixelCapacity = 0;
    id<MTLBuffer> colStart;
    id<MTLBuffer> results;

    bool Fail(const char* p_Where, const char* p_What)
    {
        std::snprintf(error, sizeof(error), "%s: %s", p_Where, p_What);
        return false;
    }
};

MetalReducer::MetalReducer() : m_Impl(new Impl)
{
    Impl& impl = *m_Impl;
    @autoreleasepool
    {
        impl.dev = MTLCreateSystemDefaultDevice();
        if (!impl.dev)
        {
            std::snprintf(impl.error, sizeof(impl.error), "no Metal device");
            return;
        }
        std::snprintf(impl.device, sizeof(impl.device), "device '%s' unified=%d",
                      impl.dev.name.UTF8String, impl.dev.hasUnifiedMemory ? 1 : 0);

        impl.pipes = PipelinesFor(impl.dev);
        if (!impl.pipes->ok)
        {
            std::snprintf(impl.error, sizeof(impl.error), "%s", impl.pipes->error);
            return;
        }
        impl.queue    = [impl.dev newCommandQueue];
        impl.colStart = [impl.dev newBufferWithLength:(kWaveformColumns + 1) * sizeof(int)
                                              options:MTLResourceStorageModeShared];
        impl.results  = [impl.dev newBufferWithLength:kResultBytes options:MTLResourceStorageModeShared];
        if (!impl.queue || !impl.colStart || !impl.results)
        {
            std::snprintf(impl.error, sizeof(impl.error), "buffer allocation failed");
            return;
        }
        impl.available = true;
    }
}

MetalReducer::~MetalReducer()
{
    delete m_Impl;
}

bool MetalReducer::Available() const { return m_Impl && m_Impl->available; }
const char* MetalReducer::Error() const { return m_Impl ? m_Impl->error : "no context"; }
const char* MetalReducer::DeviceDescription() const { return m_Impl ? m_Impl->device : ""; }

bool MetalReducer::Analyse(const FrameView& p_Frame, const ScopeParams& p_Params, ScopeResult& p_Out,
                           MetalReduceTiming* p_Timing)
{
    Impl& impl = *m_Impl;
    if (!impl.available) return false;
    if (p_Timing) *p_Timing = MetalReduceTiming{};

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

    @autoreleasepool
    {
        // --- column boundary table ------------------------------------------
        std::vector<int> colStart;
        BuildColumnTable(width, colStart);
        std::memcpy(impl.colStart.contents, colStart.data(), colStart.size() * sizeof(int));

        // --- upload the frame -----------------------------------------------
        //
        // Conformance-only: the tap's frame is already a Metal buffer. Packed
        // tightly regardless of the source's rowBytes, so a negative or padded
        // host stride becomes a clean stride here.
        const size_t rowFloats  = static_cast<size_t>(width) * 4;
        const size_t frameBytes = rowFloats * static_cast<size_t>(height) * sizeof(float);
        if (impl.pixelCapacity < frameBytes)
        {
            impl.pixels = [impl.dev newBufferWithLength:frameBytes options:MTLResourceStorageModeShared];
            if (!impl.pixels) return impl.Fail("pixels", "allocation failed");
            impl.pixelCapacity = frameBytes;
        }
        const auto tUpload0 = std::chrono::steady_clock::now();
        {
            char* dst = static_cast<char*>(impl.pixels.contents);
            for (int y = 0; y < height; ++y)
            {
                const float* srcRow = p_Frame.Row(y);
                if (!srcRow) continue;
                std::memcpy(dst + static_cast<size_t>(y) * rowFloats * sizeof(float), srcRow, rowFloats * sizeof(float));
            }
        }
        const double msUpload = MillisSince(tUpload0);

        // --- encode and run -------------------------------------------------
        FrameInputs in;
        in.pixels   = impl.pixels;
        in.colStart = impl.colStart;
        in.results  = impl.results;
        in.preview  = nil;
        FillParams(in.params, width, height, rowStep, rowFloats, p_Params.luma, 0, 0);

        id<MTLCommandBuffer> cb = [impl.queue commandBuffer];
        if (!cb) return impl.Fail("commandBuffer", "unavailable");
        EncodeFrame(cb, *impl.pipes, in);
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
            return impl.Fail("command buffer", cb.error ? cb.error.localizedDescription.UTF8String : "failed");

        // --- read back (no copy on unified memory: the buffer is shared) -----
        const char* res = static_cast<const char*>(impl.results.contents);
        std::memcpy(p_Out.waveform.data(),      res + kOffWaveform,  kWaveformBytes);
        std::memcpy(p_Out.histogram.data(),     res + kOffHistogram, kHistogramBytes);
        std::memcpy(p_Out.vectorscope.data(),   res + kOffVector,    kVectorBytes);
        std::memcpy(p_Out.twinPeaks.data(),     res + kOffTwin,      kTwinBytes);
        std::memcpy(p_Out.waveformTrace.data(), res + kOffTrace,     kTraceBytes);
        float minMax[6], probe[3];
        std::memcpy(minMax, res + kOffMinMax, sizeof(minMax));
        std::memcpy(probe, res + kOffProbe, sizeof(probe));

        // Analytic, and identical to the CPU's count: every sampled row
        // contributes exactly `width` pixels.
        p_Out.pixelsSampled = static_cast<uint64_t>(in.params.sampledRows) * static_cast<uint64_t>(width);
        const bool sawAny = (p_Out.pixelsSampled > 0);
        for (int c = 0; c < 3; ++c)
        {
            p_Out.minRGB[c]   = sawAny ? minMax[c] : 0.0f;
            p_Out.maxRGB[c]   = sawAny ? minMax[3 + c] : 0.0f;
            p_Out.probeRGB[c] = probe[c];
        }

        if (p_Timing)
        {
            p_Timing->msUpload  = msUpload;
            p_Timing->msKernels = GpuMillis(cb);
            p_Timing->valid     = true;
        }
    }
    return true;
}

// =============================================================================
// MetalPassthrough
// =============================================================================

bool MetalPassthrough(void* p_Dst, size_t p_DstRowBytes,
                      const void* p_Src, size_t p_SrcRowBytes,
                      int p_Width, int p_Height, void* p_CommandQueue)
{
    if (!p_Dst || !p_Src || !p_CommandQueue || p_Width <= 0 || p_Height <= 0) return false;
    @autoreleasepool
    {
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)p_CommandQueue;
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)p_Src;
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)p_Dst;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!cb) return false;
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        EncodeCopyRows(blit, dst, p_DstRowBytes, src, p_SrcRowBytes, p_Width, p_Height);
        [blit endEncoding];
        [cb commit];
    }
    return true;
}

// =============================================================================
// PublishHub - one per process, however many plugin instances Resolve makes.
//
// The mapping, the ticket counter, the in-flight ring and the worker that
// closes seqlocks are one set per process, for the reasons ScopeCuda.cu's hub
// documents (two instances must never claim one ticket; a slot must never be
// reused while its frame is still landing). What differs from CUDA: there is
// no page-lock, and the publish leg is the worker's memcpy from the frame's
// shared result buffer into the slot, after the command buffer completes.
// =============================================================================

namespace
{

struct PublishHub
{
    ScopePublisher publisher;
    size_t blockBytes = 0;

    struct Pending
    {
        uint64_t             ticket = 0;
        id<MTLCommandBuffer> cb;            // the frame's work; waited on by the worker
        id<MTLBuffer>        results;       // where its bins are
        id<MTLBuffer>        preview;       // or nil
        size_t               previewBytes = 0;
        bool                 inUse = false;
    };
    Pending pending[kSlotCount];

    std::deque<int>         queue;
    mutable std::mutex      mutex;
    std::condition_variable cv;
    std::thread             worker;
    bool                    stopping = false;
    bool                    running = false;
    int                     refs = 0;

    uint64_t published = 0;
    uint64_t skipped = 0;
    uint64_t faulted = 0;      // command buffers that completed with an error

    double msDeviceTotal = 0.0;
    double msDeviceKernels = 0.0;
    double msDevicePublish = 0.0;
    bool   deviceTimingValid = false;

    int Reserve(uint64_t& p_Ticket)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t i = 0; i < kSlotCount; ++i)
        {
            if (pending[i].inUse) continue;
            if (!publisher.ReserveTicket(p_Ticket)) break;
            pending[i].ticket = p_Ticket;
            pending[i].inUse  = true;
            return static_cast<int>(i);
        }
        ++skipped;
        return -1;
    }

    void Release(int p_Slot)
    {
        std::lock_guard<std::mutex> lock(mutex);
        publisher.AbandonSlot(pending[p_Slot].ticket);
        pending[p_Slot].cb = nil;
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
                if (queue.empty()) return;
                slot = queue.front();
                queue.pop_front();
            }

            Pending& p = pending[slot];

            // Outside the lock: this is the wait the render thread refuses to do.
            @autoreleasepool
            {
                [p.cb waitUntilCompleted];
                const bool ok = (p.cb.status == MTLCommandBufferStatusCompleted);
                const double msGpu = ok ? GpuMillis(p.cb) : 0.0;

                double msPublish = 0.0;
                if (ok)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    char* slotBase = static_cast<char*>(publisher.SlotAddress(p.ticket));
                    const char* res = static_cast<const char*>(p.results.contents);
                    std::memcpy(slotBase + kWaveformOffset, res + kOffWaveform, kBinsBytes);
                    std::memcpy(slotBase + ScopePublisher::MinMaxOffset(), res + kOffMinMax, 6 * sizeof(float));
                    std::memcpy(slotBase + ScopePublisher::ProbeOffset(),  res + kOffProbe,  3 * sizeof(float));
                    if (p.preview && p.previewBytes > 0)
                        std::memcpy(slotBase + kPreviewOffset, p.preview.contents, p.previewBytes);
                    msPublish = MillisSince(t0);
                }

                std::lock_guard<std::mutex> lock(mutex);
                if (ok)
                {
                    publisher.CommitSlot(p.ticket);
                    ++published;
                    msDeviceKernels   = msGpu;
                    msDevicePublish   = msPublish;
                    msDeviceTotal     = msGpu + msPublish;
                    deviceTimingValid = true;
                }
                else
                {
                    // A faulted command buffer: the slot stays odd (unreadable)
                    // and goes back for reuse; the frame is simply never shown.
                    publisher.AbandonSlot(p.ticket);
                    ++faulted;
                }
                p.cb = nil;
                p.inUse = false;
            }
        }
    }
};

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

bool HubAcquire(char* p_Error, size_t p_ErrorBytes, char* p_Note, size_t p_NoteBytes)
{
    std::lock_guard<std::mutex> lock(HubLock());
    PublishHub& hub = Hub();

    if (hub.running)
    {
        ++hub.refs;
        std::snprintf(p_Note, p_NoteBytes, "unified memory, shared block %zu MB, %d instances",
                      hub.blockBytes / (1024 * 1024), hub.refs);
        return true;
    }

    if (!hub.publisher.IsRunning() && !hub.publisher.Start())
    {
        std::snprintf(p_Error, p_ErrorBytes, "shared block unavailable");
        return false;
    }
    hub.blockBytes = hub.publisher.BlockSize();
    if (!hub.publisher.BlockData() || hub.blockBytes == 0)
    {
        std::snprintf(p_Error, p_ErrorBytes, "publisher block unavailable");
        return false;
    }

    for (uint32_t i = 0; i < kSlotCount; ++i) hub.pending[i] = PublishHub::Pending();
    hub.stopping = false;
    hub.worker = std::thread([&hub] { hub.Run(); });
    hub.running = true;
    hub.refs = 1;

    std::snprintf(p_Note, p_NoteBytes, "unified memory, shared block %zu MB", hub.blockBytes / (1024 * 1024));
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
    for (uint32_t i = 0; i < kSlotCount; ++i) hub.pending[i] = PublishHub::Pending();
    hub.queue.clear();
    hub.running = false;
}

} // namespace

// =============================================================================
// MetalTap
// =============================================================================

struct MetalTap::Impl
{
    bool running = false;
    char error[256] = {};
    char note[160] = {};

    id<MTLDevice> dev;
    Pipelines*    pipes = nullptr;

    // One set per ring slot, not one set: a frame's results are read by the
    // worker's memcpy after its command buffer completes, while the next
    // frame's fill and kernels may already be queued. The hub frees a ring
    // entry only after that memcpy, so a set is never written while it is
    // still being read.
    struct ResultSet
    {
        id<MTLBuffer> results;
        id<MTLBuffer> preview;
        size_t        previewCapacity = 0;
    };
    ResultSet sets[kSlotCount];

    id<MTLBuffer> colStart;
    int           colWidth = 0;

    double msEnqueueLast = 0.0;

    bool Fail(const char* p_Where, const char* p_What)
    {
        std::snprintf(error, sizeof(error), "%s: %s", p_Where, p_What);
        return false;
    }
};

MetalTap::MetalTap() : m_Impl(new Impl) {}

MetalTap::~MetalTap()
{
    Stop();
    delete m_Impl;
}

bool MetalTap::IsRunning() const { return m_Impl && m_Impl->running; }
const char* MetalTap::Error() const { return m_Impl ? m_Impl->error : "no context"; }

void MetalTap::Stats(GpuTapStats& p_Out) const
{
    if (!m_Impl) return;
    PublishHub& hub = Hub();
    p_Out.msEnqueue = m_Impl->msEnqueueLast;
    std::snprintf(p_Out.note, sizeof(p_Out.note), "%s", m_Impl->note);

    std::lock_guard<std::mutex> lock(hub.mutex);
    p_Out.published          = hub.published;
    p_Out.skipped            = hub.skipped;
    p_Out.slotPinned         = true;   // unified memory: nothing to pin
    p_Out.msDeviceTotal      = hub.msDeviceTotal;
    p_Out.msDeviceKernels    = hub.msDeviceKernels;
    p_Out.msDevicePublish    = hub.msDevicePublish;
    p_Out.deviceTimingValid  = hub.deviceTimingValid;
}

bool MetalTap::Start()
{
    Impl& impl = *m_Impl;
    if (impl.running) return true;

    @autoreleasepool
    {
        // The device is whichever the host's queue is on, so this is resolved
        // per frame from the queue; the pipelines and buffers are created on
        // the system default device, which is the only one on Apple silicon.
        impl.dev = MTLCreateSystemDefaultDevice();
        if (!impl.dev) return impl.Fail("device", "no Metal device");

        impl.pipes = PipelinesFor(impl.dev);
        if (!impl.pipes->ok) return impl.Fail("kernels", impl.pipes->error);

        for (Impl::ResultSet& set : impl.sets)
        {
            set.results = [impl.dev newBufferWithLength:kResultBytes options:MTLResourceStorageModeShared];
            if (!set.results) return impl.Fail("results", "allocation failed");
        }
        impl.colStart = [impl.dev newBufferWithLength:(kWaveformColumns + 1) * sizeof(int)
                                              options:MTLResourceStorageModeShared];
        if (!impl.colStart) return impl.Fail("colStart", "allocation failed");
        impl.colWidth = 0;
    }

    if (!HubAcquire(impl.error, sizeof(impl.error), impl.note, sizeof(impl.note)))
        return false;

    impl.running = true;
    return true;
}

void MetalTap::Stop()
{
    Impl& impl = *m_Impl;
    if (impl.running) HubRelease();
    for (Impl::ResultSet& set : impl.sets) set = Impl::ResultSet();
    impl.colStart = nil;
    impl.colWidth = 0;
    impl.running = false;
}

bool MetalTap::RenderFrame(const GpuTapArgs& p_Args)
{
    Impl& impl = *m_Impl;
    if (!impl.running) return false;

    const auto tEnter = std::chrono::steady_clock::now();
    const int width = p_Args.width, height = p_Args.height;
    if (width <= 0 || height <= 0 || !p_Args.srcDevice || !p_Args.dstDevice || !p_Args.stream)
        return impl.Fail("args", "missing buffer or queue");

    int rowStep = p_Args.params.rowStep;
    if (rowStep < 1) rowStep = 1;

    @autoreleasepool
    {
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)p_Args.stream;
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)p_Args.srcDevice;
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)p_Args.dstDevice;
        if (queue.device != impl.dev)
            return impl.Fail("device", "host queue is on a different Metal device");

        // The scatter kernel indexes source rows in whole float4 pixels where
        // the CPU path (and CUDA) index in floats, so a row pitch that is not a
        // multiple of 16 bytes would have it read the wrong pixels - silently,
        // since trace, probe and preview index in floats and would still agree
        // with the CPU. Resolve pads nothing today; this is the guard for the
        // day it does, and the caller falls back to the CPU for the frame. The
        // buffers' lengths are checked for the same reason a bank checks a
        // cheque: a kernel reading past its buffer faults the whole command
        // buffer, and the mandatory passthrough is in that command buffer.
        const size_t pixelBytes = 4 * sizeof(float);
        const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
        if (p_Args.srcRowBytes < rowBytes || p_Args.dstRowBytes < rowBytes ||
            (p_Args.srcRowBytes % pixelBytes) != 0)
            return impl.Fail("stride", "row pitch is not a whole number of RGBA float pixels");
        if (src.length < p_Args.srcRowBytes * static_cast<size_t>(height - 1) + rowBytes ||
            dst.length < p_Args.dstRowBytes * static_cast<size_t>(height - 1) + rowBytes)
            return impl.Fail("bounds", "frame extends past its Metal buffer");

        // --- reserve a ring entry and its ticket, together ----------------------
        PublishHub& hub = Hub();
        uint64_t ticket = 0;
        const int slot = hub.Reserve(ticket);

        struct SlotGuard
        {
            PublishHub* hub;
            int slot;
            bool armed;
            ~SlotGuard() { if (armed && slot >= 0) hub->Release(slot); }
        } slotGuard{ &hub, slot, true };

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!cb) return impl.Fail("commandBuffer", "unavailable");

        // --- the mandatory passthrough, on the host's queue ---------------------
        {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            EncodeCopyRows(blit, dst, p_Args.dstRowBytes, src, p_Args.srcRowBytes, width, height);
            [blit endEncoding];
        }

        if (slot < 0)
        {
            [cb commit];
            impl.msEnqueueLast = MillisSince(tEnter);
            return true;
        }

        Impl::ResultSet& set = impl.sets[slot];

        // --- column table, rebuilt only when the frame size changes --------------
        //
        // Written into a shared buffer the GPU reads later in this very command
        // buffer; a frame of another size still in flight is not possible, the
        // hub ring being what it is, but to be safe the table is rebuilt into a
        // fresh buffer rather than over the old one.
        if (impl.colWidth != width)
        {
            std::vector<int> colStart;
            BuildColumnTable(width, colStart);
            id<MTLBuffer> table = [impl.dev newBufferWithBytes:colStart.data()
                                                         length:colStart.size() * sizeof(int)
                                                        options:MTLResourceStorageModeShared];
            if (!table) return impl.Fail("colStart", "allocation failed");
            impl.colStart = table;
            impl.colWidth = width;
        }

        // --- preview sizing, the CPU path's own rule -----------------------------
        uint32_t previewW = 0, previewH = 0;
        size_t previewBytes = 0;
        if (p_Args.publishPreview && p_Args.previewScale > 0.0f)
        {
            PreviewSizeFor(width, height, p_Args.previewScale, previewW, previewH);
            previewBytes = static_cast<size_t>(previewW) * previewH * 3;
            if (previewBytes > set.previewCapacity)
            {
                set.preview = [impl.dev newBufferWithLength:previewBytes options:MTLResourceStorageModeShared];
                if (!set.preview) return impl.Fail("preview", "allocation failed");
                set.previewCapacity = previewBytes;
            }
        }

        // --- the kernels ----------------------------------------------------------
        FrameInputs in;
        in.pixels   = src;
        in.colStart = impl.colStart;
        in.results  = set.results;
        in.preview  = previewBytes > 0 ? set.preview : nil;
        FillParams(in.params, width, height, rowStep, p_Args.srcRowBytes / sizeof(float),
                   p_Args.params.luma, previewW, previewH);
        EncodeFrame(cb, *impl.pipes, in);

        // --- open the slot, hand the frame to the worker ---------------------------
        char* slotBase = static_cast<char*>(hub.publisher.SlotAddress(ticket));
        if (!slotBase) return impl.Fail("slot", "address unavailable");

        hub.publisher.OpenSlot(ticket, p_Args.timelineTime,
                               static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                               p_Args.instanceId,
                               static_cast<uint64_t>(in.params.sampledRows) * static_cast<uint64_t>(width),
                               p_Args.params.colorSpace, p_Args.params.luma,
                               previewW, previewH, 0.0);

        {
            std::lock_guard<std::mutex> lock(hub.mutex);
            PublishHub::Pending& p = hub.pending[slot];
            p.cb           = cb;
            p.results      = set.results;
            p.preview      = in.preview;
            p.previewBytes = previewBytes;
        }

        [cb commit];

        // Queued, not committed: the worker closes the seqlock once the command
        // buffer completes and the memcpy has landed.
        slotGuard.armed = false;
        hub.Enqueue(slot);
    }

    impl.msEnqueueLast = MillisSince(tEnter);
    return true;
}

// =============================================================================
// MetalFallbackTap - what "GPU Acceleration off" runs on a Metal host.
// =============================================================================

struct MetalFallbackTap::Impl
{
    bool running = false;
    char error[256] = {};

    id<MTLDevice> dev;
    id<MTLBuffer> staging;
    size_t        stagingBytes = 0;
    id<MTLCommandBuffer> copy;    // the blit in flight, waited on by the worker

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
    bool                    busy = false;
    bool                    pending = false;

    double   msEnqueueLast = 0.0;
    double   msCopyLast = 0.0;
    double   msBinLast = 0.0;
    uint64_t published = 0;
    uint64_t skipped = 0;

    void Run()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || pending; });
                if (!pending) return;
                pending = false;
            }

            double msCopy = 0.0;
            bool ok = false;
            @autoreleasepool
            {
                // Outside the lock: the wait the render thread refuses to do.
                [copy waitUntilCompleted];
                ok = (copy.status == MTLCommandBufferStatusCompleted);
                msCopy = ok ? GpuMillis(copy) : 0.0;
                copy = nil;
            }

            if (ok)
            {
                FrameView view;
                view.pixels   = static_cast<const float*>(staging.contents);
                view.width    = width;
                view.height   = height;
                view.rowBytes = static_cast<int>(rowBytes);

                engine.Analyse(view, params, result);
                if (publishPreview) engine.BuildPreview(view, previewScale, result.preview);
                else { result.preview.width = 0; result.preview.height = 0; }

                Hub().publisher.Publish(result, timelineTime,
                                        static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height), instanceId);
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                msCopyLast = msCopy;
                msBinLast  = result.millis;
                if (ok) ++published;
                busy = false;
            }
        }
    }
};

MetalFallbackTap::MetalFallbackTap() : m_Impl(new Impl) {}

MetalFallbackTap::~MetalFallbackTap()
{
    Stop();
    delete m_Impl;
}

bool MetalFallbackTap::IsRunning() const { return m_Impl && m_Impl->running; }
const char* MetalFallbackTap::Error() const { return m_Impl ? m_Impl->error : "no context"; }

void MetalFallbackTap::Stats(FallbackStats& p_Out) const
{
    if (!m_Impl) return;
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    p_Out.msEnqueue = m_Impl->msEnqueueLast;
    p_Out.msCopy    = m_Impl->msCopyLast;
    p_Out.msBin     = m_Impl->msBinLast;
    p_Out.published = m_Impl->published;
    p_Out.skipped   = m_Impl->skipped;
    p_Out.pinned    = true;
    p_Out.busy      = m_Impl->busy;
}

bool MetalFallbackTap::Start()
{
    Impl& impl = *m_Impl;
    if (impl.running) return true;

    impl.dev = MTLCreateSystemDefaultDevice();
    if (!impl.dev)
    {
        std::snprintf(impl.error, sizeof(impl.error), "no Metal device");
        return false;
    }

    char note[160] = {};
    if (!HubAcquire(impl.error, sizeof(impl.error), note, sizeof(note))) return false;

    impl.stopping = false;
    impl.worker = std::thread([&impl] { impl.Run(); });
    impl.running = true;
    return true;
}

void MetalFallbackTap::Stop()
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
    impl.staging = nil;
    impl.stagingBytes = 0;
    impl.copy = nil;
    impl.running = false;
}

bool MetalFallbackTap::Submit(const SubmitArgs& p_Args)
{
    Impl& impl = *m_Impl;
    if (!impl.running) return false;
    if (p_Args.width <= 0 || p_Args.height <= 0 || !p_Args.srcDevice || !p_Args.stream) return false;

    const auto tEnter = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.busy)
        {
            // The worker still owns the staging buffer; overwriting it now
            // would tear the frame it is in the middle of reducing.
            ++impl.skipped;
            impl.msEnqueueLast = MillisSince(tEnter);
            return false;
        }
        impl.busy = true;
    }

    const size_t rowBytes = static_cast<size_t>(p_Args.width) * 4 * sizeof(float);
    const size_t bytes = rowBytes * static_cast<size_t>(p_Args.height);

    @autoreleasepool
    {
        if (!impl.staging || impl.stagingBytes < bytes)
        {
            impl.staging = [impl.dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            impl.stagingBytes = impl.staging ? bytes : 0;
            if (!impl.staging)
            {
                std::lock_guard<std::mutex> lock(impl.mutex);
                impl.busy = false;
                std::snprintf(impl.error, sizeof(impl.error), "no staging buffer");
                return false;
            }
        }

        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)p_Args.stream;
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)p_Args.srcDevice;
        // The staging buffer is on the default device. A blit on another
        // device's queue into it is invalid Metal usage - a GPU fault inside
        // Resolve on a Mac with two GPUs or an eGPU - so decline, as MetalTap
        // does, rather than encode it.
        if (queue.device != impl.dev)
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.busy = false;
            std::snprintf(impl.error, sizeof(impl.error), "host queue is on a different Metal device");
            return false;
        }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!cb)
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.busy = false;
            std::snprintf(impl.error, sizeof(impl.error), "commandBuffer unavailable");
            return false;
        }
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        EncodeCopyRows(blit, impl.staging, rowBytes, src, p_Args.srcRowBytes, p_Args.width, p_Args.height);
        [blit endEncoding];
        [cb commit];

        impl.copy           = cb;
        impl.params         = p_Args.params;
        impl.timelineTime   = p_Args.timelineTime;
        impl.instanceId     = p_Args.instanceId;
        impl.width          = p_Args.width;
        impl.height         = p_Args.height;
        impl.rowBytes       = rowBytes;
        impl.publishPreview = p_Args.publishPreview;
        impl.previewScale   = p_Args.previewScale;
    }

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.pending = true;
    }
    impl.cv.notify_one();

    impl.msEnqueueLast = MillisSince(tEnter);
    return true;
}

} // namespace scopedeck
