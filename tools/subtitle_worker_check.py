"""Regression check for subtitle_poll_worker.py's cue cache, against mock
Resolve objects - no Resolve needed, and nothing to switch in a real one.

The bug (2026-10-08): the cache identified a timeline by its display name,
track index and cue count. Resolve names every project's first timeline
"Timeline 1", so switching to another project whose timeline had the same
name and the same number of cues read as "nothing changed" and kept showing
the previous project's captions; the start frame was only re-read on a change,
so an edited Start Timecode was missed too. The worker now keys on
Project/Timeline.GetUniqueId(), measured on Resolve 21.1.0.17 as UUIDs that are
stable across calls and distinct for every timeline, "(Synced)" copies
included.

    python tools/subtitle_worker_check.py [path/to/subtitle_poll_worker.py]

Drives _poll_once() the way SubtitleBridge does and tracks what the parent
would be showing: it keeps its cues until a response carries new ones.
Exit 0 = pass, 1 = a scenario showed the wrong caption or start.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "subtitle_poll_worker.py")


class Item:
    def __init__(self, text, start, end):
        self._t, self._s, self._e = text, start, end
    def GetName(self): return self._t
    def GetStart(self): return self._s
    def GetEnd(self): return self._e


class Timeline:
    def __init__(self, name, uid, start, cues, has_uid=True):
        self.name, self.uid, self.start, self.cues = name, uid, start, cues
        if has_uid:
            self.GetUniqueId = lambda: self.uid
    def GetName(self): return self.name
    def GetStartFrame(self): return self.start
    def GetTrackCount(self, kind): return 1 if kind == "subtitle" else 0
    def GetIsTrackEnabled(self, kind, index): return True
    def GetItemListInTrack(self, kind, index):
        return [Item(t, s, e) for (s, e, t) in self.cues]


class Project:
    def __init__(self, name, uid, timeline, has_uid=True):
        self.name, self.uid, self.timeline = name, uid, timeline
        if has_uid:
            self.GetUniqueId = lambda: self.uid
    def GetName(self): return self.name
    def GetCurrentTimeline(self): return self.timeline


class Session:
    """Stands in for DaVinciResolveScript: scriptapp("Resolve") -> resolve."""
    def __init__(self, project):
        self.project = project
    def scriptapp(self, _name): return self
    def GetProjectManager(self): return self
    def GetCurrentProject(self): return self.project


def fresh_worker():
    spec = importlib.util.spec_from_file_location("subtitle_poll_worker_under_test", WORKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Parent:
    """What SubtitleBridge holds: cues replaced only when a response has them."""
    def __init__(self, worker, session):
        self.worker, self.session = worker, session
        self.cues, self.start = None, None
    def poll(self):
        # Expire the worker's TTL, as two seconds of real time would.
        self.worker._cache["cached_at"] = -1e9
        response = self.worker._poll_once(self.session)
        if "cues" in response:
            self.cues = [c["t"] for c in response["cues"]]
        self.start = response.get("start")
        return response


failures = 0


def expect(label, got, want):
    global failures
    ok = got == want
    print(f"  {label:<52} {'ok' if ok else 'WRONG'}  (got {got!r}, want {want!r})")
    if not ok:
        failures += 1


def scenario(title, has_uid):
    print(title)
    worker = fresh_worker()
    a = Project("Project A", "proj-a", Timeline("Timeline 1", "tl-a", 86400, [(86400, 86448, "caption from A")], has_uid), has_uid)
    b = Project("Project B", "proj-b", Timeline("Timeline 1", "tl-b", 86400, [(86400, 86448, "caption from B")], has_uid), has_uid)
    session = Session(a)
    parent = Parent(worker, session)

    parent.poll()
    expect("project A, first poll", parent.cues, ["caption from A"])

    session.project = b
    parent.poll()
    expect("switched to project B (same timeline name, same count)", parent.cues, ["caption from B"])

    b.timeline.start = 90000
    parent.poll()
    expect("project B's Start Timecode changed", parent.start, 90000)

    session.project = a
    parent.poll()
    expect("back to project A (from the worker's track cache)", parent.cues, ["caption from A"])


print(f"subtitle worker cache check: {os.path.normpath(WORKER)}\n")
scenario("1. Resolve with GetUniqueId (21.x)", has_uid=True)
print()
scenario("2. Resolve without GetUniqueId (falls back to names)", has_uid=False)
print("\nnote: scenario 2 can only tell projects apart by name - here they are named differently.")
print(f"\n{'FAIL' if failures else 'PASS'} ({failures} wrong)")
sys.exit(1 if failures else 0)
