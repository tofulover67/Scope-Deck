# Scope Deck

External video scopes for DaVinci Resolve (and Premiere Pro on Windows): a
native C++ app that shows waveform, histogram, vectorscope, false colour,
audio meters and more for the frame the host is rendering, on a second
monitor or anywhere you like, fed by a tiny OpenFX "tap" plugin through
shared memory.

Windows and macOS, from one source tree.

## Install

Download the current release from this repository's Releases page.

- **Windows:** `ScopeDeck-Setup-<version>.exe`. Installs the app, the Scope
  Tap plugin for Resolve and, if Premiere Pro is present, the Premiere plugin.
  NVIDIA cards use the CUDA path; others use the CPU path.
- **macOS:** `ScopeDeck-<version>.pkg`. Installs Scope Deck.app and the Scope
  Tap plugin into `/Library/OFX/Plugins`. Universal (Apple silicon and
  Intel). The package is not signed; allow it once under System Settings >
  Privacy & Security.

Then, in Resolve, add **Scope Tap** (OpenFX, *Scope Deck* group) to a node on
the Color page and open Scope Deck.

**One display on a Mac?** Keep part of Resolve's viewer visible beside Scope
Deck. When Resolve's window is completely covered, macOS puts it to sleep
(App Nap) and its playback runs fast with no sound. Scope Deck opens at three
quarters of the screen for this reason and remembers where you put it. The
installer also turns App Nap off for Resolve (it takes effect when Resolve
next starts); if you installed by hand, this is the command:

```bash
defaults write com.blackmagic-design.DaVinciResolve NSAppSleepDisabled -bool YES
```

## Build from source

Both builds use the vendored dependencies in `vendor/` (Dear ImGui, GLFW,
stb_image, Resolve's OpenFX SDK) - no package manager, no download.

**Windows** (Visual Studio Build Tools with the C++ workload; CUDA 12.8/12.9
optional for the GPU tap):

```
powershell -NoProfile -File .\build.ps1
```

**macOS** (Command Line Tools, `brew install cmake ninja`):

```bash
./build_mac.sh
```

`--run` launches the app, `--deploy` copies the plugin into Resolve's plugin
folder. `release.ps1` / `release_mac.sh` are what the GitHub release runs.

## Layout

| Path | What |
|---|---|
| `app/` | The app: `main.cpp` (panels, menus, layout), the scope image builders, the audio / timecode / subtitle bridges, screen capture. `app/mac/` holds the macOS-only Objective-C++. |
| `core/` | The wire format (`ScopeTypes.h`), its reader, the reduction engine, the shared-memory publisher, the CUDA reduction. No UI. |
| `plugin/` | `ScopeTap` (OpenFX, Resolve) and `ScopeTransmit` (Premiere Pro, Windows). |
| `tools/` | Conformance and contention harnesses, the headless probe, the self-test. |
| `installer/` | Inno Setup script (Windows) and the pkg definition (macOS). |
| `*_poll_worker.py` | The Python helpers the timecode and subtitle panels run against Resolve's scripting API. |

`PORTING.md` tracks every feature's status and the reasoning behind each
design decision; `GPU_PORT_HANDOFF.md` covers the GPU work. `AGENTS.md` is
the short list of rules for anyone (or any agent) changing the tree.
