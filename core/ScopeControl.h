// A small side channel between the app and a plugin, beside the scope block.
//
// App -> plugin: settings. Scope Tap takes Preview Scale and Row Step as node
// parameters inside Resolve. A Premiere Transmit plugin has no parameters a
// user would ever find, so ScopeTransmit reads them from here instead and the
// app offers them in its Input menu.
//
// Plugin -> app: the sequence's timecode format. The frame's position
// already travels in the scope block (ScopeFrame::timelineTime, frames from
// the sequence's start); what the app cannot know is the rate, drop-frame
// and start timecode that position is shown against in Premiere.
//
// Each side writes only its own half.
//
// Every field is one aligned 32-bit atomic, so a reader never sees a torn
// value and neither side waits on the other. The two settings are independent,
// so there is no need for a seqlock across them.
//
// Deliberately separate from ScopeShm/ScopeReader: the app links the reader,
// the plugin links the writer, and both link this.

#pragma once

#include <atomic>
#include <cstdint>

namespace scopedeck
{

// Undecorated, like kShmName: Local\ is added on Windows.
inline constexpr const char* kControlNamePremiere = "ScopeDeck.Premiere.Control.v1";

constexpr uint32_t kControlMagic   = 0x42434453;   // 'SDCB'
constexpr uint32_t kControlVersion = 2;   // 2: added the plugin-written timecode half

// 0 = automatic: full size up to 1920 wide, scaled down to fit beyond it -
// what ScopeTransmit did before this block existed.
constexpr uint32_t kPreviewScaleAuto = 0;

struct ControlBlock
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;

    // Written by the app.
    std::atomic<uint32_t> previewScalePct;   // kPreviewScaleAuto, or 25..100
    std::atomic<uint32_t> rowStep;           // 1..16, as Scope Tap's Row Step

    // Written by the plugin, for the frames it is publishing. tcFps is the
    // nominal timecode rate (24 for 23.976); 0 until the plugin has seen a
    // sequence, which the app reads as "no timecode yet".
    std::atomic<uint32_t> tcFps;
    std::atomic<uint32_t> tcDropFrame;       // 1 = drop-frame
    std::atomic<int32_t>  tcStartFrame;      // the sequence's start timecode, in frames
    std::atomic<uint32_t> reserved;
};

static_assert(sizeof(ControlBlock) == 32, "ControlBlock is shared across processes - keep it flat");

// Owns one mapping of the block. Create() makes it if it does not exist yet
// (the app); Open() only attaches to an existing one (the plugin), and fails
// harmlessly while the app has never run. Both map it read-write, since each
// writes its own half.
class ControlChannel
{
public:
    ControlChannel() = default;
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    bool Create(const char* p_Name);
    bool Open(const char* p_Name);
    void Close();

    // Null until Create/Open succeeds. For Open, also null if the block is not
    // one this build understands (magic/version), so a reader falls back to
    // its defaults rather than acting on someone else's layout.
    ControlBlock* Block() const { return m_Block; }

private:
    bool Map(const char* p_Name, bool p_Create);

    ControlBlock* m_Block   = nullptr;
    void*         m_Mapping = nullptr;   // HANDLE, kept opaque
};

} // namespace scopedeck
