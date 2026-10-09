// Named shared memory and the publisher that writes scope results into it.
//
// The platform split is confined to ScopeShm.cpp: named file mapping on Windows,
// shm_open + mmap on POSIX. Everything above this line is portable.

#pragma once

#include "ScopeCore.h"
#include "ScopeTypes.h"

#include <cstddef>
#include <cstdint>

namespace scopedeck
{

class SharedBlock
{
public:
    SharedBlock() = default;
    ~SharedBlock();

    SharedBlock(const SharedBlock&) = delete;
    SharedBlock& operator=(const SharedBlock&) = delete;

    // Creates the block, or opens it if another process got there first.
    bool Open(const char* p_Name, size_t p_Size);
    void Close();

    void*  Data() const { return m_Data; }
    size_t Size() const { return m_Size; }
    bool   IsValid() const { return m_Data != nullptr; }

private:
    void*  m_Data = nullptr;
    size_t m_Size = 0;

#ifdef _WIN32
    void* m_Mapping = nullptr;   // HANDLE, kept opaque to avoid pulling in Windows.h
#else
    int   m_Fd = -1;
#endif
};

// Per block name, per process - see ScopeShm.cpp.
struct PublishAuthority;

// Writes scope results into the shared block using a seqlock per slot, so a reader
// never sees a half-written frame and neither side ever blocks the other.
//
// Any number of ScopePublisher objects may share one block within a process -
// Scope Tap makes one per instance, plus the GPU hub's - and they coordinate
// through a PublishAuthority shared by everything on that block name: a ticket
// is only handed out for a slot no one else in the process is writing, and the
// write counter only ever moves forward. Before that existed, each publisher
// took its ticket from the committed counter with nothing reserved, so two
// instances could write one slot at once and the reader accepted the mix
// (tools/ScopePublishContention.cpp). Publishers in different processes on the
// same block are still not coordinated; nothing does that today.
class ScopePublisher
{
public:
    // p_Name defaults to the block every real tap and app share. The self-test
    // overrides it so a synthetic publisher can be run against a scratch block
    // while a real tap and app are live on the usual one - without that, testing
    // a wire-format change means closing Resolve first, and a mismatched version
    // written into the shared block stops the running app's poll timer dead.
    bool Start(const char* p_Name = kShmName);
    void Stop();
    bool IsRunning() const { return m_Block.IsValid(); }

    // Publishes one reduced frame. instanceId distinguishes taps, since Resolve
    // creates several plugin instances for the same node graph. Returns false
    // when nothing was written: a malformed result, or every slot still being
    // written by someone else in this process - a dropped scope frame, never a
    // wait.
    bool Publish(const ScopeResult& p_Result,
                 double p_TimelineTime,
                 uint32_t p_Width,
                 uint32_t p_Height,
                 uint32_t p_InstanceId);

    // ---- slot machinery for backends that fill a slot themselves -------------
    //
    // The GPU path does not hand over a ScopeResult: its bins never exist in
    // host memory at all, they arrive in the slot by DMA straight off the card.
    // So it drives the same seqlock in three steps instead of one - reserve,
    // fill (by whatever means), commit - while this class keeps sole ownership
    // of the protocol. Nothing here knows what CUDA is.
    //
    // The contract: every successful ReserveTicket ends in exactly one
    // CommitSlot or AbandonSlot, or that slot stays reserved for the life of
    // the process. Commits may arrive in any order - the write counter only
    // moves forward, so a late commit is simply a frame nobody is shown.

    void*  BlockData() const { return m_Block.Data(); }
    size_t BlockSize() const;

    // Reserves a slot no other publisher in this process is writing and returns
    // its ticket; SlotAddress() gives the bytes. False when all kSlotCount
    // slots are in flight - skip the frame. Tickets whose slot is busy are
    // passed over rather than waited for, so tickets need not be consecutive.
    bool  ReserveTicket(uint64_t& p_Ticket);
    void* SlotAddress(uint64_t p_Ticket) const;

    // Gives a reserved slot back without advertising it, for a write that
    // failed part-way. If the slot was opened it stays odd - readers keep
    // rejecting it - until a later OpenSlot reuses it.
    void AbandonSlot(uint64_t p_Ticket);

    // Marks the slot as being written (sequence -> odd, whatever it was before)
    // and fills every header field the host already knows. The fields the GPU
    // produces - minRGB, maxRGB, probeRGB - are deliberately left alone: they
    // arrive by DMA into their own offsets while the slot is open.
    void OpenSlot(uint64_t p_Ticket,
                  double p_TimelineTime,
                  uint32_t p_Width, uint32_t p_Height,
                  uint32_t p_InstanceId, uint64_t p_PixelsSampled,
                  uint32_t p_ColorSpace, const LumaWeights& p_Luma,
                  uint32_t p_PreviewWidth, uint32_t p_PreviewHeight,
                  double p_BinMillis);

    // Closes the seqlock (sequence -> even), advertises the slot as newest
    // unless a later ticket already was, and frees the slot for reservation.
    void CommitSlot(uint64_t p_Ticket);

    // Byte offsets a DMA backend writes into, relative to a slot address.
    static uint64_t MinMaxOffset();     // 6 floats: minRGB[3] then maxRGB[3]
    static uint64_t ProbeOffset();      // 3 floats

private:
    SharedBlock       m_Block;
    PublishAuthority* m_Authority = nullptr;   // shared, never owned
};

} // namespace scopedeck
