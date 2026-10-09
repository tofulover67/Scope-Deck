// Reader half of the shared-memory wire format.
//
// The tap (ScopeTap.ofx, built from the old tree) is the writer; this is the only
// thing the app needs to see it. ScopeTypes.h is copied verbatim from the plugin
// side and must stay that way - the two halves agree by compiling the same struct
// definitions, not by two people keeping two files in step. kVersion is checked at
// runtime anyway, because a stale .ofx in Program Files is the failure that
// actually happens.
//
// Platform code is confined to the mapping calls in ScopeReader.cpp.

#pragma once

#include "ScopeTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scopedeck
{

// A fresh frame identity: unique for the life of this process, across every
// block, every input and every publisher. ScopeReader stamps each frame it
// accepts with one, and Screen Capture stamps its frames from the same counter.
uint64_t NextFrameIdentity();

// One published frame, copied out of the mapping so the caller can hold it across
// frames without racing the writer.
struct ScopeFrame
{
    double   timelineTime  = 0.0;

    // This app's identity for the frame (NextFrameIdentity), new on every
    // accepted read - the key every panel cache compares. Not the publisher's
    // number: each publisher used to count from zero on its own, so two Scope
    // Tap instances, a Resolve restart or a Resolve/Premiere switch could hand
    // two different pictures the same index and a cache kept the old result.
    uint64_t frameIndex    = 0;

    // The slot's own frameIndex as the publisher wrote it (ticket + 1, unique
    // per block). Diagnostics only - caches use frameIndex above.
    uint64_t publishIndex  = 0;

    uint32_t width         = 0;
    uint32_t height        = 0;
    uint32_t instanceId    = 0;
    uint32_t pixelsSampled = 0;

    // True extremes, unclamped by the bin range - the tap saw every pixel at full
    // resolution to get these, which is why they cannot be re-derived from bins.
    float    minRGB[3]     = { 0.0f, 0.0f, 0.0f };
    float    maxRGB[3]     = { 0.0f, 0.0f, 0.0f };

    double   binMillis     = 0.0;

    uint32_t colorSpace    = kSpaceRec709;
    float    lumaCoeff[3]  = { 0.0f, 0.0f, 0.0f };
    float    probeRGB[3]   = { 0.0f, 0.0f, 0.0f };

    uint32_t previewWidth  = 0;
    uint32_t previewHeight = 0;


    // Echoed from ShmHeader so a panel reads its axis ranges off the frame it is
    // drawing rather than off whatever the header says at paint time.
    float    binRangeLow   = kBinRangeLow;
    float    binRangeHigh  = kBinRangeHigh;
    float    chromaRangeLow  = kChromaRangeLow;
    float    chromaRangeHigh = kChromaRangeHigh;

    std::vector<uint32_t> waveform;      // kWaveformCells,        WaveformIndex()
    std::vector<uint32_t> histogram;     // kHistogramCells,       HistogramIndex()
    std::vector<uint32_t> vectorscope;   // kVectorscopeTotalCells, VectorscopeIndex()

    // White Balance ("Twin Peaks"): two diamonds, row = level ((G+B)/2 or
    // (G+R)/2), column = diff ((G-B)/2 or (G-R)/2) - kTwinPeaksTotalCells,
    // TwinPeaksIndex(). Always copied (512 KB/frame, the same order as
    // vectorscope), unlike the waveform trace's opt-in.
    std::vector<uint32_t> twinPeaks;

    // Raw per-scanline samples for the line-strip trace ("Enhanced Render"), not
    // counts - kWaveformTraceCells floats, WaveformTraceIndex(). Only filled when
    // the reader has been asked for it (SetWantWaveformTrace), because it is 2 MB
    // a frame that nothing reads while Enhanced Render is off.
    std::vector<float>    waveformTrace;

    // RGB8, row-major, previewWidth * previewHeight * 3 bytes. Empty when the tap
    // had "Publish Video" off this frame; the caller keeps showing its last good
    // one rather than flashing to blank.
    std::vector<uint8_t>  preview;

    bool HasPreview() const { return previewWidth > 0 && previewHeight > 0 && !preview.empty(); }

    // Value <-> waveform level, using this frame's own bin range.
    float ValueAtLevel(float p_Level) const
    {
        return binRangeLow + (p_Level / float(kWaveformLevels - 1)) * (binRangeHigh - binRangeLow);
    }
    float LevelOfValue(float p_Value) const
    {
        return (p_Value - binRangeLow) / (binRangeHigh - binRangeLow) * float(kWaveformLevels - 1);
    }

    const char* SpaceName() const { return ColorSpaceName(colorSpace); }
};

// Why the reader has nothing to show, so the UI can say something specific
// instead of an empty panel. Every one of these is a state a user hits.
enum class ReaderStatus
{
    Closed,           // never opened, or Open() failed
    NoPublisher,      // mapping exists but writeCounter is still 0
    VersionMismatch,  // the .ofx in Program Files is older or newer than this app
    Stale,            // last publish was a while ago - Resolve parked, or tap removed
    Live,
};

class ScopeReader
{
public:
    ScopeReader() = default;
    ~ScopeReader();

    ScopeReader(const ScopeReader&) = delete;
    ScopeReader& operator=(const ScopeReader&) = delete;

    // Opens the existing block. Returns false when no tap has ever created it -
    // that is the normal "Resolve isn't running" case, not an error worth a dialog.
    // Safe to call repeatedly; the UI retries on a timer.
    bool Open(const char* p_Name = kShmName);
    void Close();
    bool IsOpen() const { return m_Data != nullptr; }

    // Copies the newest slot if it is newer than the last one returned. Returns
    // false when there is nothing new, which is the common case at UI frame rate:
    // the tap publishes at timeline rate and the UI runs faster.
    //
    // p_OnlyIfNew == false forces a re-read of the current slot, for a panel that
    // just needs pixels again after a resize.
    bool ReadLatest(ScopeFrame& p_Out, bool p_OnlyIfNew = true);

    ReaderStatus Status() const { return m_Status; }

    // Whether to copy the waveform trace rows into each frame. Off by default: it
    // is 2 MB per frame and only Enhanced Render reads it, so a panel turns it on
    // for as long as it needs it rather than everyone paying for it always.
    //
    // Turning it ON forces the next ReadLatest to re-read the current slot even if
    // no new frame has been published. Without that, switching Enhanced Render on
    // while the playhead is parked leaves the trace empty forever - render() is
    // silent while nothing changes, so "wait for the next frame" can mean never,
    // and the panel silently falls back to the density trace.
    void SetWantWaveformTrace(bool p_Want)
    {
        if (p_Want && !m_WantWaveformTrace) m_ForceReread = true;
        m_WantWaveformTrace = p_Want;
    }
    bool WantWaveformTrace() const { return m_WantWaveformTrace; }

    // Populated when Status() == VersionMismatch, so the message can name both
    // numbers rather than saying "rebuild both sides" and leaving the user to guess.
    uint32_t ShmVersion() const { return m_ShmVersion; }
    static constexpr uint32_t ExpectedVersion() { return kVersion; }

    // Seconds since the last publish this reader observed. Drives Stale.
    double SecondsSinceLastFrame() const;

private:
    const ShmHeader* Header() const { return static_cast<const ShmHeader*>(m_Data); }
    ShmHeader*       MutableHeader() { return static_cast<ShmHeader*>(m_Data); }

    bool ReadSlot(uint64_t p_Base, ScopeFrame& p_Out) const;

    void*    m_Data = nullptr;
    size_t   m_Size = 0;

#ifdef _WIN32
    void*    m_Mapping = nullptr;   // HANDLE, kept opaque so Windows.h stays in the .cpp
#else
    int      m_Fd = -1;
#endif

    bool         m_WantWaveformTrace = false;
    bool         m_ForceReread       = false;
    uint64_t     m_LastCounter = 0;
    ReaderStatus m_Status      = ReaderStatus::Closed;
    uint32_t     m_ShmVersion  = 0;
    double       m_LastFrameAt = 0.0;
};

// Monotonic seconds. Exposed because the reader and the UI must agree on one clock
// when deciding what counts as stale.
double NowSeconds();

} // namespace scopedeck
