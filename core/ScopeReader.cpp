#include "ScopeReader.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace scopedeck
{

uint64_t NextFrameIdentity()
{
    // Starts at 1 so a default-constructed ScopeFrame (0) never matches a cache
    // built from a real frame.
    static std::atomic<uint64_t> s_Next{ 1 };
    return s_Next.fetch_add(1, std::memory_order_relaxed);
}

// How long without a publish before the UI should stop claiming the scopes are
// live. Resolve's render call is silent while the playhead is parked - a measured
// 36-second idle logged zero calls - so this is not "something broke", it is
// "nothing has changed". Two seconds is long enough not to flicker during normal
// scrubbing and short enough to notice a tap that was removed from the node.
static constexpr double kStaleAfterSeconds = 2.0;

double NowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

ScopeReader::~ScopeReader()
{
    Close();
}

#ifdef _WIN32

bool ScopeReader::Open(const char* p_Name)
{
    if (IsOpen()) return true;

    // OpenFileMapping, not CreateFileMapping: the app is a reader and should never
    // bring the block into existence. Creating it would commit kTotalShmSize of
    // pagefile (~372 MB at v9) for a block no tap is writing, and would let a
    // running app mask the real "no publisher" state behind a block full of zeroes.
    const std::string name = std::string("Local\\") + (p_Name ? p_Name : kShmName);

    HANDLE mapping = ::OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (!mapping)
    {
        m_Status = ReaderStatus::Closed;
        return false;
    }

    // Size 0 maps the whole section, deliberately rather than asking for
    // kTotalShmSize.
    //
    // Measured, not theorised: with a v8 tap installed and a v9 app, the block is
    // ~91 MB and kTotalShmSize is ~389 MB, so a sized MapViewOfFile fails outright.
    // The app would then report "no tap running" for what is actually a stale
    // ScopeTap.ofx - hiding the one error the user can act on behind the one they
    // cannot. Mapping the whole section lets the version check run and say so.
    //
    // Read-only. The app used to write one field into this mapping - the
    // scope-source request, for a GPU design that was measured and abandoned -
    // and now writes nothing at all, so page protection enforces what used to be
    // convention: a reader cannot corrupt the block a tap is publishing into.
    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view)
    {
        ::CloseHandle(mapping);
        m_Status = ReaderStatus::Closed;
        return false;
    }

    // How much actually got mapped, so payload reads can be bounds-checked against
    // the real block rather than against what this build wishes were there.
    MEMORY_BASIC_INFORMATION info{};
    const size_t mapped = (::VirtualQuery(view, &info, sizeof(info)) != 0)
                          ? size_t(info.RegionSize) : size_t(0);

    m_Mapping = mapping;
    m_Data    = view;
    m_Size    = mapped;
    m_Status  = ReaderStatus::NoPublisher;
    return true;
}

void ScopeReader::Close()
{
    if (m_Data)    ::UnmapViewOfFile(m_Data);
    if (m_Mapping) ::CloseHandle(static_cast<HANDLE>(m_Mapping));
    m_Data    = nullptr;
    m_Mapping = nullptr;
    m_Size    = 0;
    m_Status  = ReaderStatus::Closed;

    // The next Open may be a different block (the app switching between
    // Resolve and Premiere), whose write counter has nothing to do with this
    // one's - keeping it would skip every frame until the new counter passed
    // the old one.
    m_LastCounter = 0;
}

#else

bool ScopeReader::Open(const char* p_Name)
{
    if (IsOpen()) return true;

    const std::string name = std::string("/") + (p_Name ? p_Name : kShmName);

    // No O_CREAT, for the same reason Windows uses OpenFileMapping above.
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0)
    {
        m_Status = ReaderStatus::Closed;
        return false;
    }

    // The block's real size, not kTotalShmSize - see the Windows branch above for
    // why asking for this build's expected size hides a version mismatch behind a
    // mapping failure.
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        ::close(fd);
        m_Status = ReaderStatus::Closed;
        return false;
    }
    const size_t mapped = size_t(st.st_size);

    void* view = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED)
    {
        ::close(fd);
        m_Status = ReaderStatus::Closed;
        return false;
    }

    m_Fd     = fd;
    m_Data   = view;
    m_Size   = mapped;
    m_Status = ReaderStatus::NoPublisher;
    return true;
}

void ScopeReader::Close()
{
    if (m_Data)    ::munmap(m_Data, m_Size);
    if (m_Fd >= 0) ::close(m_Fd);
    m_Data   = nullptr;
    m_Fd     = -1;
    m_Size   = 0;
    m_Status = ReaderStatus::Closed;
    m_LastCounter = 0;   // see the Windows Close() above
}

#endif

double ScopeReader::SecondsSinceLastFrame() const
{
    if (m_LastFrameAt <= 0.0) return 1e9;
    return NowSeconds() - m_LastFrameAt;
}

bool ScopeReader::ReadLatest(ScopeFrame& p_Out, bool p_OnlyIfNew)
{
    if (!IsOpen()) return false;

    const ShmHeader* head = Header();

    if (head->magic != kMagic)
    {
        m_Status = ReaderStatus::NoPublisher;
        return false;
    }

    if (head->version != kVersion)
    {
        // Almost always a stale ScopeTap.ofx still sitting in Program Files, since
        // OFX plugins are scanned only at Resolve startup. Recorded rather than
        // thrown - the app stays up and names both numbers.
        m_ShmVersion = head->version;
        m_Status     = ReaderStatus::VersionMismatch;
        return false;
    }

    // The version matched, so the writer claims this build's layout - but the
    // mapping still has to be big enough to hold it before any offset is trusted.
    // A block that is the right version and the wrong size means something is very
    // wrong upstream, and reading on would be reading past the end of the section.
    if (m_Size != 0 && m_Size < sizeof(ShmHeader) + kSlotSize * kSlotCount)
    {
        m_ShmVersion = head->version;
        m_Status     = ReaderStatus::VersionMismatch;
        return false;
    }

    uint64_t counter = head->writeCounter;
    if (counter == 0)
    {
        m_Status = ReaderStatus::NoPublisher;
        return false;
    }

    if (m_ForceReread)
    {
        // A payload the caller did not have last time is now wanted, so the slot
        // must be re-read even though it is the same frame.
        p_OnlyIfNew   = false;
        m_ForceReread = false;
    }

    if (p_OnlyIfNew && counter == m_LastCounter)
    {
        m_Status = (SecondsSinceLastFrame() > kStaleAfterSeconds)
                   ? ReaderStatus::Stale : ReaderStatus::Live;
        return false;
    }

    // Seqlock. The slot's sequence is odd while the writer is inside it, and
    // changes if a writer passes all the way through while we copy. Either means
    // this slot is not trustworthy - retry against whatever is newest now. The
    // writer never waits for us, so a contended frame is simply skipped; at three
    // slots and UI-rate polling, losing one is invisible.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint64_t slot = (counter - 1) % kSlotCount;
        const uint64_t base = sizeof(ShmHeader) + slot * kSlotSize;

        const volatile uint64_t* seqPtr =
            reinterpret_cast<const volatile uint64_t*>(static_cast<const uint8_t*>(m_Data) + base);

        const uint64_t before = *seqPtr;
        if (before & 1u)
        {
            counter = head->writeCounter;
            continue;
        }

        // Acquire on both sides of the copy, so neither the compiler nor the CPU
        // hoists a payload read above the first sequence read or sinks one below
        // the second. Without these the check can pass while the copy read torn
        // bytes - the classic way a seqlock reader silently stops working.
        std::atomic_thread_fence(std::memory_order_acquire);

        const bool ok = ReadSlot(base, p_Out);

        std::atomic_thread_fence(std::memory_order_acquire);

        if (ok && *seqPtr == before)
        {
            p_Out.binRangeLow     = head->binRangeLow;
            p_Out.binRangeHigh    = head->binRangeHigh;
            p_Out.chromaRangeLow  = head->chromaRangeLow;
            p_Out.chromaRangeHigh = head->chromaRangeHigh;
            p_Out.frameIndex      = NextFrameIdentity();

            m_LastCounter = counter;
            m_LastFrameAt = NowSeconds();
            m_Status      = ReaderStatus::Live;
            return true;
        }

        counter = head->writeCounter;
    }

    // Eight straight losses means the writer is publishing faster than we can copy,
    // not that anything is broken. Keep the previous frame on screen.
    m_Status = ReaderStatus::Live;
    return false;
}

bool ScopeReader::ReadSlot(uint64_t p_Base, ScopeFrame& p_Out) const
{
    const uint8_t* slot = static_cast<const uint8_t*>(m_Data) + p_Base;

    SlotHeader sh{};
    std::memcpy(&sh, slot, sizeof(SlotHeader));

    p_Out.timelineTime  = sh.timelineTime;
    p_Out.publishIndex  = sh.frameIndex;   // frameIndex is stamped on acceptance
    p_Out.width         = sh.width;
    p_Out.height        = sh.height;
    p_Out.instanceId    = sh.instanceId;
    p_Out.pixelsSampled = sh.pixelsSampled;
    std::memcpy(p_Out.minRGB, sh.minRGB, sizeof(p_Out.minRGB));
    std::memcpy(p_Out.maxRGB, sh.maxRGB, sizeof(p_Out.maxRGB));
    p_Out.binMillis     = sh.binMillis;
    p_Out.colorSpace    = sh.colorSpace;
    std::memcpy(p_Out.lumaCoeff, sh.lumaCoeff, sizeof(p_Out.lumaCoeff));
    std::memcpy(p_Out.probeRGB,  sh.probeRGB,  sizeof(p_Out.probeRGB));
    p_Out.previewWidth      = sh.previewWidth;
    p_Out.previewHeight     = sh.previewHeight;

    p_Out.waveform.resize(kWaveformCells);
    std::memcpy(p_Out.waveform.data(), slot + kWaveformOffset,
                kWaveformCells * sizeof(uint32_t));

    p_Out.histogram.resize(kHistogramCells);
    std::memcpy(p_Out.histogram.data(), slot + kHistogramOffset,
                kHistogramCells * sizeof(uint32_t));

    p_Out.vectorscope.resize(kVectorscopeTotalCells);
    std::memcpy(p_Out.vectorscope.data(), slot + kVectorscopeOffset,
                kVectorscopeTotalCells * sizeof(uint32_t));

    p_Out.twinPeaks.resize(kTwinPeaksTotalCells);
    std::memcpy(p_Out.twinPeaks.data(), slot + kTwinPeaksOffset,
                kTwinPeaksTotalCells * sizeof(uint32_t));

    if (m_WantWaveformTrace)
    {
        p_Out.waveformTrace.resize(kWaveformTraceCells);
        std::memcpy(p_Out.waveformTrace.data(), slot + kWaveformTraceOffset,
                    kWaveformTraceCells * sizeof(float));
    }
    else if (!p_Out.waveformTrace.empty())
    {
        // Release it rather than leave a stale 2 MB buffer behind when Enhanced
        // Render is switched off - and so a panel that reads it without asking
        // gets an obviously empty array instead of last minute's frame.
        p_Out.waveformTrace.clear();
        p_Out.waveformTrace.shrink_to_fit();
    }

    // A preview larger than the canvas would mean the tap and this reader disagree
    // about kPreviewCanvasWidth/Height, which the version check should already have
    // caught. Refuse rather than read past the region.
    if (sh.previewWidth > kPreviewCanvasWidth || sh.previewHeight > kPreviewCanvasHeight)
        return false;

    if (sh.previewWidth > 0 && sh.previewHeight > 0)
    {
        const size_t bytes = size_t(sh.previewWidth) * sh.previewHeight * 3;
        p_Out.preview.resize(bytes);

        // TODO(perf): this is a full copy of the preview (up to ~25 MB at UHD/100%)
        // which is then uploaded to a texture - two passes over the same pixels.
        // The fix is to upload straight from the mapping and let the seqlock check
        // decide whether to keep the texture, which means the GL upload has to
        // happen inside this function. Left alone until a real UHD preview measures
        // slow; at the default preview scale it is a few MB.
        std::memcpy(p_Out.preview.data(), slot + kPreviewOffset, bytes);
    }
    else
    {
        // "Publish Video" was off this frame. The caller keeps whatever it already
        // had rather than clearing - a blank flash is worse than a held frame.
        p_Out.previewWidth  = 0;
        p_Out.previewHeight = 0;
    }

    return true;
}

} // namespace scopedeck
