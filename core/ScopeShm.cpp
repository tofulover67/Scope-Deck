#include "ScopeShm.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <Windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace scopedeck
{

////////////////////////////////////////////////////////////////////////////////
// SharedBlock

SharedBlock::~SharedBlock()
{
    Close();
}

#ifdef _WIN32

bool SharedBlock::Open(const char* p_Name, size_t p_Size)
{
    Close();

    // Local\ scopes the mapping to the logon session, which is what we want: the
    // tap and the app are the same user, and a Global\ name would need privilege.
    const std::string name = std::string("Local\\") + p_Name;

    const DWORD sizeHigh = static_cast<DWORD>((static_cast<uint64_t>(p_Size) >> 32) & 0xFFFFFFFFull);
    const DWORD sizeLow  = static_cast<DWORD>(static_cast<uint64_t>(p_Size) & 0xFFFFFFFFull);

    // Backed by the page file, not a real file. Returns the existing mapping if
    // one is already there, which is exactly the create-or-open we want.
    HANDLE mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                          PAGE_READWRITE, sizeHigh, sizeLow,
                                          name.c_str());
    if (!mapping) return false;

    void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, p_Size);
    if (!view)
    {
        ::CloseHandle(mapping);
        return false;
    }

    m_Mapping = mapping;
    m_Data = view;
    m_Size = p_Size;
    return true;
}

void SharedBlock::Close()
{
    if (m_Data) { ::UnmapViewOfFile(m_Data); m_Data = nullptr; }
    if (m_Mapping) { ::CloseHandle(static_cast<HANDLE>(m_Mapping)); m_Mapping = nullptr; }
    m_Size = 0;
}

#else

bool SharedBlock::Open(const char* p_Name, size_t p_Size)
{
    Close();

    const std::string name = std::string("/") + p_Name;

    int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;

    // ftruncate on an already-sized object is harmless; on a fresh one it sets
    // the size. Shrinking never happens because the size is a compile-time constant.
    struct stat st {};
    if ((::fstat(fd, &st) == 0) && (static_cast<size_t>(st.st_size) < p_Size))
    {
        if (::ftruncate(fd, static_cast<off_t>(p_Size)) != 0)
        {
            ::close(fd);
            return false;
        }
    }

    void* view = ::mmap(nullptr, p_Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED)
    {
        ::close(fd);
        return false;
    }

    m_Fd = fd;
    m_Data = view;
    m_Size = p_Size;
    return true;
}

void SharedBlock::Close()
{
    if (m_Data) { ::munmap(m_Data, m_Size); m_Data = nullptr; }
    if (m_Fd >= 0) { ::close(m_Fd); m_Fd = -1; }
    m_Size = 0;
}

#endif

////////////////////////////////////////////////////////////////////////////////
// ScopePublisher

namespace
{

inline std::atomic<uint64_t>* AtomicAt(void* p_Address)
{
    // The seqlock counters are plain uint64 in the wire struct so the Python
    // reader can see them; treating them as atomics here keeps the C++ side
    // correct without changing the layout.
    return reinterpret_cast<std::atomic<uint64_t>*>(p_Address);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////
// PublishAuthority

// One per block name per process, shared by every ScopePublisher on that name.
// It holds the two things that used to live in each publisher and so raced
// between them: the next ticket, and which slots are being written. Never
// destroyed - publishers live in plugin instances and statics whose teardown
// order relative to this is not ours to choose.
struct PublishAuthority
{
    std::mutex mutex;
    uint64_t   nextTicket = 0;
    bool       busy[kSlotCount] = {};
};

namespace
{

PublishAuthority* AuthorityFor(const char* p_Name)
{
    static std::mutex* s_Lock = new std::mutex();
    static auto* s_ByName = new std::map<std::string, PublishAuthority*>();
    std::lock_guard<std::mutex> lock(*s_Lock);
    PublishAuthority*& entry = (*s_ByName)[p_Name];
    if (!entry) entry = new PublishAuthority();
    return entry;
}

} // namespace

bool ScopePublisher::Start(const char* p_Name)
{
    if (m_Block.IsValid()) return true;

    const char* name = p_Name ? p_Name : kShmName;
    if (!m_Block.Open(name, static_cast<size_t>(kTotalShmSize))) return false;

    // Initialising under the block's lock, so two instances starting at once
    // cannot both decide the block is fresh and zero it under each other.
    m_Authority = AuthorityFor(name);
    std::lock_guard<std::mutex> lock(m_Authority->mutex);

    ShmHeader* header = static_cast<ShmHeader*>(m_Block.Data());

    // A second tap instance may find the block already initialised. Rewriting the
    // constants is harmless - they are compile-time identical - but the write
    // counter must not be reset, or readers would jump backwards.
    const bool alreadyLive = (header->magic == kMagic) && (header->version == kVersion);

    header->magic           = kMagic;
    header->version         = kVersion;
    header->headerSize      = static_cast<uint32_t>(sizeof(ShmHeader));
    header->slotCount       = kSlotCount;
    header->slotSize        = kSlotSize;
    header->waveformColumns = kWaveformColumns;
    header->waveformLevels  = kWaveformLevels;
    header->histogramBins   = kHistogramBins;
    header->planeCount      = kPlaneCount;
    header->binRangeLow     = kBinRangeLow;
    header->binRangeHigh    = kBinRangeHigh;
    header->vectorscopeSize = kVectorscopeSize;
    header->chromaRangeLow  = kChromaRangeLow;
    header->chromaRangeHigh = kChromaRangeHigh;
    header->vectorscopeBands = kBandCount;
    header->lowRangeMax     = kVectorscopeLowMax;
    header->highRangeMin    = kVectorscopeHighMin;
    header->previewCanvasWidth  = kPreviewCanvasWidth;
    header->previewCanvasHeight = kPreviewCanvasHeight;
    header->twinPeaksSize          = kTwinPeaksSize;
    header->twinPeaksDiffRangeLow  = kTwinPeaksDiffRangeLow;
    header->twinPeaksDiffRangeHigh = kTwinPeaksDiffRangeHigh;
    header->waveformTraceRows      = kWaveformTraceRows;

    if (!alreadyLive)
    {
        header->writeCounter = 0;
        std::memset(static_cast<char*>(m_Block.Data()) + sizeof(ShmHeader), 0,
                    static_cast<size_t>(kSlotSize * kSlotCount));
    }

    return true;
}

void ScopePublisher::Stop()
{
    m_Block.Close();
}

bool ScopePublisher::ReserveTicket(uint64_t& p_Ticket)
{
    if (!m_Block.IsValid()) return false;
    ShmHeader* header = static_cast<ShmHeader*>(m_Block.Data());

    std::lock_guard<std::mutex> lock(m_Authority->mutex);

    // Never behind what is already published - a block left live by an
    // earlier process carries its counter forward, and a ticket below it
    // would commit without ever being advertised.
    const uint64_t published = AtomicAt(&header->writeCounter)->load(std::memory_order_acquire);
    uint64_t ticket = std::max(m_Authority->nextTicket, published);

    for (uint32_t tries = 0; tries < kSlotCount; ++tries, ++ticket)
    {
        bool& busy = m_Authority->busy[ticket % kSlotCount];
        if (busy) continue;
        busy = true;
        m_Authority->nextTicket = ticket + 1;
        p_Ticket = ticket;
        return true;
    }
    return false;
}

void ScopePublisher::AbandonSlot(uint64_t p_Ticket)
{
    if (!m_Block.IsValid()) return;
    std::lock_guard<std::mutex> lock(m_Authority->mutex);
    m_Authority->busy[p_Ticket % kSlotCount] = false;
}

size_t ScopePublisher::BlockSize() const
{
    return m_Block.IsValid() ? static_cast<size_t>(kTotalShmSize) : 0;
}

void* ScopePublisher::SlotAddress(uint64_t p_Ticket) const
{
    if (!m_Block.IsValid()) return nullptr;
    char* base = static_cast<char*>(m_Block.Data());
    const uint32_t slotIndex = static_cast<uint32_t>(p_Ticket % kSlotCount);
    return base + sizeof(ShmHeader) + static_cast<size_t>(slotIndex) * kSlotSize;
}

uint64_t ScopePublisher::MinMaxOffset()
{
    return offsetof(SlotHeader, minRGB);
}

uint64_t ScopePublisher::ProbeOffset()
{
    return offsetof(SlotHeader, probeRGB);
}

void ScopePublisher::OpenSlot(uint64_t p_Ticket,
                              double p_TimelineTime,
                              uint32_t p_Width, uint32_t p_Height,
                              uint32_t p_InstanceId, uint64_t p_PixelsSampled,
                              uint32_t p_ColorSpace, const LumaWeights& p_Luma,
                              uint32_t p_PreviewWidth, uint32_t p_PreviewHeight,
                              double p_BinMillis)
{
    if (!m_Block.IsValid()) return;

    SlotHeader* slotHeader = static_cast<SlotHeader*>(SlotAddress(p_Ticket));
    std::atomic<uint64_t>* sequence = AtomicAt(&slotHeader->sequence);

    // Seqlock write: odd while in progress, even when settled. The release
    // fence is what stops a reader seeing the new sequence before the new data.
    //
    // Odd whatever it was before: a slot abandoned mid-write is already odd,
    // and a plain +1 would make it even - "settled" - for the whole of this
    // write. +2 keeps it odd and still changes the value, so a reader that
    // sampled it earlier fails its before/after comparison too.
    const uint64_t seq = sequence->load(std::memory_order_relaxed);
    sequence->store((seq & 1u) ? seq + 2 : seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    slotHeader->timelineTime  = p_TimelineTime;
    slotHeader->frameIndex    = p_Ticket + 1;   // unique across every publisher on the block
    slotHeader->width         = p_Width;
    slotHeader->height        = p_Height;
    slotHeader->instanceId    = p_InstanceId;
    slotHeader->pixelsSampled = static_cast<uint32_t>(
        p_PixelsSampled > 0xFFFFFFFFull ? 0xFFFFFFFFull : p_PixelsSampled);
    slotHeader->binMillis     = p_BinMillis;
    slotHeader->colorSpace    = p_ColorSpace;
    slotHeader->lumaCoeff[0]  = p_Luma.r;
    slotHeader->lumaCoeff[1]  = p_Luma.g;
    slotHeader->lumaCoeff[2]  = p_Luma.b;
    slotHeader->previewWidth  = p_PreviewWidth;
    slotHeader->previewHeight = p_PreviewHeight;

    // minRGB/maxRGB/probeRGB are NOT written here - they land by DMA.
}

void ScopePublisher::CommitSlot(uint64_t p_Ticket)
{
    if (!m_Block.IsValid()) return;

    char* base = static_cast<char*>(m_Block.Data());
    ShmHeader* header = reinterpret_cast<ShmHeader*>(base);
    SlotHeader* slotHeader = static_cast<SlotHeader*>(SlotAddress(p_Ticket));

    std::atomic<uint64_t>* sequence = AtomicAt(&slotHeader->sequence);
    const uint64_t seq = sequence->load(std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_release);
    sequence->store((seq & 1u) ? seq + 1 : seq + 2, std::memory_order_relaxed);   // -> even

    // Advertised only now, and only forward. Commits no longer arrive in ticket
    // order - a CPU instance's synchronous publish can finish while an earlier
    // GPU ticket's copies are still landing - and a plain store let that late
    // commit move the counter backwards. A frame overtaken this way is simply
    // never shown; its slot is still settled and free for reuse.
    std::atomic<uint64_t>* writeCounter = AtomicAt(&header->writeCounter);
    uint64_t current = writeCounter->load(std::memory_order_relaxed);
    while (current < p_Ticket + 1 &&
           !writeCounter->compare_exchange_weak(current, p_Ticket + 1,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
    {
    }

    std::lock_guard<std::mutex> lock(m_Authority->mutex);
    m_Authority->busy[p_Ticket % kSlotCount] = false;
}

bool ScopePublisher::Publish(const ScopeResult& p_Result,
                             double p_TimelineTime,
                             uint32_t p_Width,
                             uint32_t p_Height,
                             uint32_t p_InstanceId)
{
    if (!m_Block.IsValid()) return false;
    if (p_Result.waveform.size() != static_cast<size_t>(kWaveformCells)) return false;
    if (p_Result.histogram.size() != static_cast<size_t>(kHistogramCells)) return false;
    if (p_Result.vectorscope.size() != static_cast<size_t>(kVectorscopeTotalCells)) return false;
    if (p_Result.twinPeaks.size() != static_cast<size_t>(kTwinPeaksTotalCells)) return false;
    if (p_Result.waveformTrace.size() != static_cast<size_t>(kWaveformTraceCells)) return false;

    // 0x0 (video publish off) is valid - the reader treats it as "no preview
    // this frame" rather than an error, so there is nothing to clamp/validate
    // beyond not overrunning the fixed canvas region.
    const uint64_t previewBytes = static_cast<uint64_t>(p_Result.preview.width) *
                                  p_Result.preview.height * 3;
    const bool havePreview = (previewBytes <= kPreviewCanvasBytes) &&
                             (p_Result.preview.rgb.size() == static_cast<size_t>(previewBytes));

    // The same reserve -> open -> fill -> commit the GPU path drives by hand,
    // so a synchronous publish and an asynchronous one can share a block.
    uint64_t ticket = 0;
    if (!ReserveTicket(ticket)) return false;

    OpenSlot(ticket, p_TimelineTime, p_Width, p_Height, p_InstanceId,
             p_Result.pixelsSampled, p_Result.colorSpace, p_Result.luma,
             havePreview ? p_Result.preview.width : 0u,
             havePreview ? p_Result.preview.height : 0u,
             p_Result.millis);

    char* slot = static_cast<char*>(SlotAddress(ticket));
    SlotHeader* slotHeader = reinterpret_cast<SlotHeader*>(slot);
    for (int c = 0; c < 3; ++c)
    {
        slotHeader->minRGB[c]   = p_Result.minRGB[c];
        slotHeader->maxRGB[c]   = p_Result.maxRGB[c];
        slotHeader->probeRGB[c] = p_Result.probeRGB[c];
    }

    std::memcpy(slot + kWaveformOffset, p_Result.waveform.data(),
                static_cast<size_t>(kWaveformCells) * sizeof(uint32_t));
    std::memcpy(slot + kHistogramOffset, p_Result.histogram.data(),
                static_cast<size_t>(kHistogramCells) * sizeof(uint32_t));
    std::memcpy(slot + kVectorscopeOffset, p_Result.vectorscope.data(),
                static_cast<size_t>(kVectorscopeTotalCells) * sizeof(uint32_t));
    std::memcpy(slot + kTwinPeaksOffset, p_Result.twinPeaks.data(),
                static_cast<size_t>(kTwinPeaksTotalCells) * sizeof(uint32_t));
    std::memcpy(slot + kWaveformTraceOffset, p_Result.waveformTrace.data(),
                static_cast<size_t>(kWaveformTraceCells) * sizeof(float));
    if (havePreview && previewBytes > 0)
    {
        std::memcpy(slot + kPreviewOffset, p_Result.preview.rgb.data(),
                    static_cast<size_t>(previewBytes));
    }

    CommitSlot(ticket);
    return true;
}

} // namespace scopedeck
