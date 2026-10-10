#!/usr/bin/env python3
"""Standalone worker invoked as a long-lived subprocess by SubtitleBridge.cpp
to read the current DaVinci Resolve timeline's subtitle track and hand back
its cues - each one a (start frame, end frame, text) range.

Deliberately reports *ranges*, not "the text at the playhead right now".
Polling for the current text could only ever update as often as the parent
polls (a few times a second at best, and each poll costs a scripting round
trip), which is far too coarse for text meant to change exactly on a cut.
Everything needed to pick the right cue for a given frame is static data -
the cue ranges and the timeline's start frame - so this worker supplies
that once, and the parent matches the frame it is actually displaying
against it locally, every rendered frame, for free.

The frame the parent matches with is ScopeFrame::timelineTime, the OFX
effect time carried by the frame it is showing - so the caption always
belongs to the picture on screen next to it, rather than to wherever
Resolve's playhead has since moved on to.

That also makes the polls nearly free: between refreshes (_CACHE_TTL_SECONDS)
this makes no scripting calls at all, and re-sends the cue list only when it
has actually changed (see the rev handshake below).

Runs out-of-process for the same reason timecode_poll_worker.py does -
connecting to Resolve's scripting library (fusionscript.dll) when Resolve
isn't reachable has been observed elsewhere in this same project family to
hard-crash the interpreter (a native segfault) rather than raise a
catchable Python exception, which would otherwise take Scope Deck's own GUI
process down with it. A crash here only ever kills this worker; the parent
treats a dead/unresponsive worker as "no reading" and relaunches a fresh one.

Protocol: parent writes one line (any content - a bare "poll\n" is all it
sends) to this process's stdin per request; this process writes exactly one
line of JSON to stdout in response, then flushes. Runs until stdin closes
(EOF), at which point it exits.

Success line, when the cue list has changed since this process last sent it:
{"rev": 3, "start": 86400, "cues": [{"s": 86400, "e": 86448, "t": "cap"}, ...]}.
When it has not: {"rev": 3, "start": 86400} alone, and the parent keeps the
cues it already has (a fresh worker always starts by sending the full list,
so a relaunched worker can never leave the parent holding cues it never
received). "start" is Timeline.GetStartFrame() - see _poll_once for why the
parent needs it and why it is read rather than inferred.
Failure line (Resolve not running, no project, no timeline, or an unexpected
exception): {"error": "<reason>"}. The reasons are the same words
timecode_poll_worker.py uses, and the caller shows them under the Show
Subtitles preference (SubtitleBridge.cpp), so they are part of the protocol.

ensure_ascii=False on the way out (see _emit): the default True would escape
every non-ASCII character to a \\uXXXX sequence, which the C++ side's own
small parser does not decode - it only unescapes \\\\, \\", \\n, \\r, \\t, so
any subtitle track using non-Latin text needs to arrive as raw UTF-8 bytes
instead, matching stdout's own reconfigured encoding below.

Measured against a real Resolve session (2026-09-15, Resolve Studio
21.1.0.17): Timeline.GetTrackCount("subtitle")/GetItemListInTrack("subtitle",
i) both work exactly like the video/audio track calls; TimelineItem.GetName()
on a subtitle clip returns its actual caption text (confirmed by importing a
throwaway .srt into a scratch timeline, reading it back, then deleting that
timeline - see the old app's ERRORS.md); GetStart()/GetEnd() are in the same
absolute-frame space as GetCurrentTimecode() and GetStartFrame(), already
including the timeline's start offset (GetStartFrame() read back as 86400 on
a 23.976fps non-drop timeline whose start timecode is 01:00:00:00 - i.e.
3600 * 24, the nominal rate, matching how non-drop timecode labels count).
"""

import json
import os
import sys
import time


def _bootstrap_resolve_env():
    if sys.platform == "darwin":
        # Resolve's own documented locations (Developer/Scripting/README.txt);
        # fusionscript.so lives inside the app bundle.
        if "RESOLVE_SCRIPT_API" not in os.environ:
            os.environ["RESOLVE_SCRIPT_API"] = (
                "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting"
            )
        if "RESOLVE_SCRIPT_LIB" not in os.environ:
            os.environ["RESOLVE_SCRIPT_LIB"] = (
                "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fusionscript.so"
            )
    else:
        if "RESOLVE_SCRIPT_API" not in os.environ:
            os.environ["RESOLVE_SCRIPT_API"] = os.path.expandvars(
                r"%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
            )
        if "RESOLVE_SCRIPT_LIB" not in os.environ:
            os.environ["RESOLVE_SCRIPT_LIB"] = (
                r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll"
            )

    modules_path = os.path.join(os.environ["RESOLVE_SCRIPT_API"], "Modules")
    if modules_path not in sys.path:
        sys.path.append(modules_path)

    import DaVinciResolveScript as dvr_script  # noqa: E402
    return dvr_script


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


# The cue list, re-read from Resolve only every _CACHE_TTL_SECONDS. Walking
# a track's items costs one scripting RPC round trip per item per call, which
# is far too expensive to repeat on every poll against a timeline carrying a
# few hundred subtitle clips - and the ranges only change when someone edits
# the timeline, so there is nothing to gain from re-reading them faster.
# `rev` only increments when a refresh actually produces different cues, so
# an unchanged timeline never re-sends the list (see _poll_once).
_cache = {
    "cues": None,
    "start": 0,
    "rev": 0,
    "cached_at": 0.0,
    "timeline_key": None,
    "track_index": None,
    "count": -1,
}

# Short, because the check this gates is now ~2 scripting calls rather than
# ~3,300 (see _poll_once) - so this is really just "how long can a track or
# timeline switch take to show up", and a few seconds of that is already at
# the edge of feeling broken when it is the user's own deliberate action.
_CACHE_TTL_SECONDS = 2.0
_last_emitted_rev = None

# Every track ever read this session, keyed by (project id, timeline id, track
# index) - see _identity - each with the item count it was read at. Reading one track costs ~3
# scripting calls per cue - about half a second on a 1,100-cue track while
# Resolve is idle, but measured at ~18 seconds during playback, when Resolve
# has the engine busy. Switching between a pair of language tracks would
# otherwise pay that every single time; this way only the first visit to a
# given track does, and switching back is free.
_track_cache = {}


def _identity(obj):
    """Which project or timeline this is, for the cue cache.

    GetUniqueId(), not the display name: Resolve calls every project's first
    timeline "Timeline 1", so a name-keyed cache took another project's
    same-named timeline - with the same cue count - for the one already read,
    and kept showing the previous project's captions. Measured on Resolve
    21.1.0.17: Project and Timeline GetUniqueId() both return a UUID, stable
    across calls and distinct for every timeline, "(Synced)" duplicates
    included. Falls back to the name on an API without it."""
    try:
        uid = obj.GetUniqueId()
        if uid:
            return uid
    except Exception:
        pass
    try:
        return obj.GetName()
    except Exception:
        return None


def _first_enabled_subtitle_track(timeline, track_count: int):
    """Resolve timelines routinely carry several subtitle tracks side by side
    (one per language, say), and Resolve's own viewer burns in only the ones
    individually enabled - so reading every track and concatenating them, as
    this once did, stacked every language's caption on screen at once."""
    for candidate in range(1, track_count + 1):
        try:
            if timeline.GetIsTrackEnabled("subtitle", candidate):
                return candidate
        except AttributeError:
            # This Resolve/API version has no GetIsTrackEnabled - fall back
            # to "just the first track" rather than showing nothing forever.
            return 1
        except Exception:
            continue   # one malformed track shouldn't block checking the rest
    return None


def _read_cues(items):
    """The expensive half of a refresh: GetStart()/GetEnd()/GetName() are a
    scripting round trip each, so this is ~3 RPCs per subtitle clip - over
    3,300 of them on a timeline carrying 1,100+ cues. Measured against a
    real session that is slow enough, while Resolve is busy playing back, to
    overrun the parent's read timeout outright; see _poll_once for how it is
    kept off the common path."""
    cues = []
    for item in items:
        try:
            text = item.GetName()
            if text:
                cues.append((item.GetStart(), item.GetEnd(), text))
        except Exception:
            continue   # one malformed item shouldn't blank the whole track
    return cues


def _fail(reason: str) -> dict:
    """Any failure also forgets what was last sent, so the next successful
    poll re-sends the whole cue list. The parent drops its cues the moment
    it sees an error (it has no way to know whether the timeline they came
    from still exists), so without this reset an unchanged rev would mean
    the resent list is skipped as redundant - leaving the overlay blank for
    good after a single transient hiccup, until the cues happened to change
    or the worker was relaunched."""
    global _last_emitted_rev
    _last_emitted_rev = None
    return {"error": reason}


def _poll_once(dvr_script) -> dict:
    global _last_emitted_rev

    if _cache["cues"] is None or (time.monotonic() - _cache["cached_at"]) >= _CACHE_TTL_SECONDS:
        resolve = dvr_script.scriptapp("Resolve")
        if resolve is None:
            return _fail("no_resolve")
        project = resolve.GetProjectManager().GetCurrentProject()
        if project is None:
            return _fail("no_project")
        timeline = project.GetCurrentTimeline()
        if timeline is None:
            return _fail("no_timeline")

        # Cheap change-detection first: which project and timeline this is
        # (_identity - not the name, which every project's first timeline
        # shares) and its subtitle item count are a handful of RPCs, against
        # the ~3,300 a full
        # _read_cues costs on a timeline this size. Re-walking every item on
        # a fixed interval is what actually broke playback - the walk is slow
        # enough while Resolve is rendering that the parent's read timeout
        # fired mid-refresh, took the worker down as unresponsive, and wiped
        # the cue list, leaving the overlay blank until a fresh worker had
        # finished paying the same cost again. Measured as a ~10s on/off
        # cycle against a 1,112-cue timeline.
        #
        # Which track is enabled is part of the comparison, not just how many
        # cues it holds: parallel translation tracks are the normal reason to
        # have several in the first place, and they routinely carry the exact
        # same number of cues (same timings, translated text). Comparing
        # counts alone, enabling the Japanese track instead of the English
        # one read as "nothing changed" unless the two happened to differ in
        # length - which is luck, not detection.
        #
        # The remaining tradeoff: an edit that changes a cue's text or timing
        # without changing how many cues there are is not noticed. Adding,
        # deleting, or switching tracks or timelines all are. That is the
        # right way round for a scope monitor - the cues are being read here,
        # not authored.
        timeline_key = (_identity(project), _identity(timeline))
        track_index = _first_enabled_subtitle_track(timeline, timeline.GetTrackCount("subtitle"))
        items = (timeline.GetItemListInTrack("subtitle", track_index) or []) if track_index else []

        unchanged = (
            _cache["cues"] is not None
            and timeline_key == _cache["timeline_key"]
            and track_index == _cache["track_index"]
            and len(items) == _cache["count"]
        )

        if not unchanged:
            # A track already read at this exact item count is taken from
            # _track_cache rather than walked again - that is what makes
            # flipping between two language tracks instant instead of an
            # ~18-second stall each way during playback.
            key = timeline_key + (track_index,)
            remembered = _track_cache.get(key)
            if remembered is not None and remembered[1] == len(items):
                cues = remembered[0]
            else:
                cues = _read_cues(items)
                _track_cache[key] = (cues, len(items))

            if cues != _cache["cues"]:
                _cache["cues"] = cues
                _cache["rev"] += 1

            _cache["timeline_key"] = timeline_key
            _cache["track_index"] = track_index
            _cache["count"] = len(items)

        # The timeline's own first frame, in the same absolute frame space
        # GetStart()/GetEnd() report cues in - and the one number needed to
        # line that space up with OFX's timelineTime, which counts from 0 at
        # exactly this frame. Read straight from the timeline rather than
        # inferred by sampling the playhead: an offset derived from "where is
        # the playhead now" against "which frame has the app received"
        # silently absorbs however far the OFX/shared-memory pipeline is
        # lagging Resolve mid-playback, which is a variable few frames, and
        # showed up as cues landing early and jittering on rapid dialogue.
        #
        # Read on every refresh, not only when the cues change: it is one RPC,
        # and Start Timecode is a timeline setting that can be edited with
        # every cue left where it was. It goes out with every response, so the
        # parent picks a new value up without the cue list being re-sent.
        _cache["start"] = timeline.GetStartFrame()

        _cache["cached_at"] = time.monotonic()

    rev = _cache["rev"]
    if _last_emitted_rev == rev:
        return {"rev": rev, "start": _cache["start"]}

    _last_emitted_rev = rev
    return {
        "rev": rev,
        "start": _cache["start"],
        "cues": [{"s": s, "e": e, "t": t} for s, e, t in _cache["cues"]],
    }


def main() -> None:
    try:
        # Default console encoding on Windows is the system codepage, not
        # UTF-8 - without this, a non-ASCII subtitle raises
        # UnicodeEncodeError inside _emit and the poll silently reads back
        # as a dead worker (see the parent's ReadLineWithTimeout, which
        # only knows "no line came back", not why).
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    try:
        dvr_script = _bootstrap_resolve_env()
    except Exception:
        _emit({"error": "bootstrap"})
        sys.exit(1)

    # --once: answer a single poll and exit. No bridge uses it any more (both
    # platforms keep a persistent worker, below); it stays for checking the
    # Resolve connection by hand from a terminal.
    if "--once" in sys.argv:
        try:
            _emit(_poll_once(dvr_script))
        except Exception:
            _emit(_fail("exception"))
        return

    # Persistent mode: one request per stdin line, answered one at
    # a time - the parent always waits for a response before sending the
    # next, so there is no need to tag requests/responses.
    for line in sys.stdin:
        if not line:
            break
        try:
            _emit(_poll_once(dvr_script))
        except Exception:
            _emit(_fail("exception"))


if __name__ == "__main__":
    main()
