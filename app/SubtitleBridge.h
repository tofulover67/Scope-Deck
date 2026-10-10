#pragma once
#include <string>

namespace scopedeck
{
    void SubtitleBridgeStart();
    void SubtitleBridgeStop();

    // Call every frame to tell the bridge whether it should be polling at
    // all - on only when "Show Subtitles" is enabled AND a Source panel is
    // actually open to show the overlay on, so an idle app with the
    // preference set doesn't keep shelling out to Resolve for nothing.
    void SubtitleBridgeSetActive(bool p_Active);

    // Returns the subtitle text active at p_TimelineTime (\n-joined if more
    // than one cue overlaps it), or an empty string if there is none,
    // polling is inactive, Resolve isn't reachable, no Python interpreter
    // was found, or the OFX-time-to-Resolve-frame anchor isn't established
    // yet (see TimecodeBridgeGetResolveFrame).
    //
    // p_TimelineTime is the OFX effect time from the frame currently being
    // displayed - not anything the Python worker reports. Resolve's
    // scripting API cannot report a live playhead during playback, so the
    // worker only supplies the cue ranges and the per-frame matching
    // happens here, against the frame stream that does advance live.
    std::string SubtitleBridgeGetText(double p_TimelineTime);

    // Why the overlay is empty when the bridge knows: no Python, or the
    // worker's last reason (the free edition has no scripting, Studio ships
    // with it off, no project, no timeline). "" when nothing is wrong, or
    // when nothing has been asked yet. Shown under the Show Subtitles box.
    std::string SubtitleBridgeGetStatus();
}
