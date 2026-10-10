"""The two Resolve workers' failure reasons are part of their protocol: the
Timecode panel and the Subtitles preference show a sentence for each one
(TimecodeBridge.cpp's ResolveScriptingMessage). This pins the words, against
mock Resolve objects - no Resolve needed.

    python tools/resolve_worker_errors_check.py

Each scenario loads a fresh copy of each worker and calls its _poll_once()
the way the bridges do, with scriptapp() returning None (the free edition,
Studio with External Scripting off, Resolve not running), then a session
with no current project, then a project with no current timeline.
Exit 0 = pass, 1 = a worker answered with a different reason.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKERS = ("timecode_poll_worker.py", "subtitle_poll_worker.py")


def load(name):
    path = os.path.join(HERE, "..", name)
    spec = importlib.util.spec_from_file_location(name[:-3], path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Session:
    """Stands in for DaVinciResolveScript: scriptapp("Resolve") -> resolve."""

    def __init__(self, resolve=None, project=None):
        self.resolve, self.project = resolve, project

    def scriptapp(self, _name):
        return self if self.resolve else None

    def GetProjectManager(self):
        return self

    def GetCurrentProject(self):
        return self.project


class Project:
    def GetCurrentTimeline(self):
        return None


SCENARIOS = (
    ("scriptapp() returns None", Session(), "no_resolve"),
    ("no current project", Session(resolve=True), "no_project"),
    ("no current timeline", Session(resolve=True, project=Project()), "no_timeline"),
)

failures = 0
for name in WORKERS:
    for label, session, want in SCENARIOS:
        got = load(name)._poll_once(session).get("error")
        ok = got == want
        failures += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}: {label} -> {got!r}" + ("" if ok else f" (want {want!r})"))

print("PASS" if failures == 0 else f"FAIL ({failures})")
sys.exit(1 if failures else 0)
