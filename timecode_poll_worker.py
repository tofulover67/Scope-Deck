#!/usr/bin/env python3
"""Standalone worker invoked as a long-lived subprocess by TimecodeBridge.cpp
to read DaVinci Resolve's current timeline playhead position as an SMPTE
timecode string.

Runs out-of-process for the same reason subtitle_poll_worker.py does -
connecting to Resolve's scripting library (fusionscript.dll) when Resolve
isn't reachable has been observed elsewhere in this same project family to
hard-crash the interpreter (a native segfault) rather than raise a
catchable Python exception, which would otherwise take Scope Deck's own GUI
process down with it. A crash here only ever kills this worker; the parent
treats a dead/unresponsive worker as "no reading" and relaunches a fresh one.

Persistent, not one-shot: the parent used to spawn (and re-import
DaVinciResolveScript, and reconnect to Resolve, and pay for the process
launch) fresh on every ~2s poll, which made each poll take long enough that
the timeline could visibly move between when the parent sampled its own
frame position and when this worker's answer came back - baking that drift
into the parent's frame-offset math. Doing the Resolve connection once and
then just answering repeated requests over stdin/stdout makes each poll
cheap enough that the parent can require its own before/after samples to
match before it trusts the answer, instead of ever trusting a stale one.

Protocol: parent writes one line (any content - a bare "poll\n" is all it
sends) to this process's stdin per request; this process writes exactly one
line of JSON to stdout in response, then flushes. Runs until stdin closes
(EOF), at which point it exits.

Success line: {"timecode": "01:23:45:12", "fps": 24, "drop_frame": false,
"frame": 2694}, timecode straight from Timeline.GetCurrentTimecode().
Failure line (Resolve not running, no project, no timeline, or an
unexpected exception - all treated identically by the caller):
{"error": "<reason>"}.
"""

import json
import os
import sys


def _bootstrap_resolve_env():
    if sys.platform == "darwin":
        # Unconfirmed - see MAC_PORTING.md.
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


def _timecode_to_frame(tc: str, fps: int, drop: bool) -> int:
    h, m, s, f = (int(part) for part in tc.replace(";", ":").split(":"))
    frame = (h * 3600 + m * 60 + s) * fps + f
    if drop and fps in (30, 60):
        drop_per_min = 2 if fps == 30 else 4
        tot_min = h * 60 + m
        frame -= drop_per_min * (tot_min - tot_min // 10)
    return frame


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def _poll_once(dvr_script) -> dict:
    resolve = dvr_script.scriptapp("Resolve")
    if resolve is None:
        return {"error": "no_resolve"}
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        return {"error": "no_project"}
    timeline = project.GetCurrentTimeline()
    if timeline is None:
        return {"error": "no_timeline"}

    timecode = timeline.GetCurrentTimecode()
    fps_raw = timeline.GetSetting("timelineFrameRate")
    fps_nominal = int(round(float(fps_raw))) if fps_raw else 24
    drop_frame = timeline.GetSetting("dropFrameTimecode") == "1"
    current_frame = _timecode_to_frame(timecode, fps_nominal, drop_frame) if timecode else 0

    return {
        "timecode": timecode or "",
        "fps": fps_nominal,
        "drop_frame": drop_frame,
        "frame": current_frame,
    }


def main() -> None:
    try:
        dvr_script = _bootstrap_resolve_env()
    except Exception:
        _emit({"error": "bootstrap"})
        sys.exit(1)

    # --once: answer a single poll and exit, matching the old fire-and-forget
    # subprocess-per-poll behavior - kept for the non-Windows caller in
    # TimecodeBridge.cpp, which has no persistent-worker plumbing yet (see
    # MAC_PORTING.md) and would otherwise leave this loop reading a stdin
    # that's never going to close.
    if "--once" in sys.argv:
        try:
            _emit(_poll_once(dvr_script))
        except Exception:
            _emit({"error": "exception"})
        return

    # Persistent mode (Windows): one request per stdin line, answered one at
    # a time - the parent always waits for a response before sending the
    # next, so there is no need to tag requests/responses.
    for line in sys.stdin:
        if not line:
            break
        try:
            _emit(_poll_once(dvr_script))
        except Exception:
            _emit({"error": "exception"})


if __name__ == "__main__":
    main()
