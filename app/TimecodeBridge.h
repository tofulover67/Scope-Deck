#pragma once
#include <string>

namespace scopedeck
{
    void TimecodeBridgeStart();
    void TimecodeBridgeStop();
    
    // Call every frame to tell the bridge if any Timecode panel is active
    void TimecodeBridgeSetActive(bool p_Active);

    // Call every frame with the current ScopeFrame::timelineTime - the poll
    // thread needs a recent sample to anchor Resolve's own frame numbering
    // against (see its own comment), and has no access to the frame the main
    // loop is holding otherwise.
    void TimecodeBridgeNotifyFrame(double p_TimelineTime);

    // Returns the calculated timecode string based on the current timelineTime
    std::string TimecodeBridgeGetText(double p_TimelineTime);

    // A frame count as HH:MM:SS:FF - ';' before the frames for drop-frame,
    // whose count skips the dropped numbers the way the host displays them.
    // The Resolve path above and the Premiere path (which gets its rate,
    // drop-frame and start timecode from ScopeTransmit) share it.
    std::string FormatTimecode(long long p_Frame, int p_Fps, bool p_DropFrame);
}
