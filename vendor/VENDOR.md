# Vendored dependencies

Source is committed in full — no submodules, no package manager. Pinned commits
recorded here so an update is a deliberate act with a diff, not a silent drift.

| Library | Version | Commit | Why |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | `docking` branch | `545d1a884ab7be89bbab4600a649a206c8667773` | UI. The docking branch (not master) because the panel layout depends on DockBuilder, and "Open in New Window" depends on multi-viewport. |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | `7b6aead9fb88b3623e3b3725ebb42670cbe4c579` | Window + GL 3.3 core context + input, on Windows and macOS from one code path. |
| [stb_image](https://github.com/nothings/stb) | v2.30 | `013ac3beddff3dbffafd5177e7972067cd2b5083` | Decodes a user-uploaded still (Compare panel) from PNG/JPG/etc into RGB8 - public domain/MIT dual-licensed, single header, decode-only (its write half is unused here; GLFW's own vendored copy of `stb_image_write.h` is a separate, encode-only header and not reused for this). |

| OpenFX SDK (Resolve's copy) | "Updated as of 12 May 2026" | n/a - copied from `C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX` | The ScopeTap plugin. `OpenFX-1.4/` (API headers) and `Support/` (C++ wrapper) only, both BSD-3-Clause; Blackmagic's sample plugins are not included. Resolve's copy rather than upstream OpenFX because its Support library carries the CUDA fields (`isEnabledCudaRender`, `pCudaStream`) the tap uses. Vendored so a machine with no Resolve install - the GitHub release build - can build the plugin. |

Trimmed after cloning: `.git`, `examples/`, and GLFW's `docs/` and `tests/`.

## Updating

Re-clone at the new tag into a scratch dir, diff against the vendored copy, then
replace and update the commit above. Do not edit vendored sources in place — if a
patch is ever unavoidable, add it here with the reason.

The OpenFX SDK has no upstream commit to pin: it comes from a Resolve install.
After a Resolve update, `diff -r` that folder's `OpenFX-1.4` and `Support`
against `vendor/openfx` and copy across whatever changed.

## GLFW and CMake 4

GLFW 3.4 declares `cmake_minimum_required(VERSION 3.4...3.28)`. CMake 4.0 dropped
compatibility with minimums below 3.5, so configuring sets
`CMAKE_POLICY_VERSION_MINIMUM=3.5`. That is GLFW's declaration being stale, not
anything this project does; it goes away when GLFW is next updated.
