# Working on Scope Deck

Read this before changing anything. It applies to people and to AI coding
agents alike, and it is short on purpose.

## This repository is public

- **No personal information goes into the repo** - not in code, comments,
  docs, commit messages, scripts, logs or test fixtures. That means no email
  addresses, no API keys or tokens, no passwords, no private repository
  links, no home-directory or machine-specific paths, and no references to
  any agent's private memory or notes. The author's name, Trevor Siebe, is
  the one exception and is fine to use.
- Before every commit, check the diff for exactly those things. A
  `grep -rn -E '@[a-z]+\.[a-z]+|/Users/|C:\\Users' --exclude-dir=vendor` of
  the tree should stay empty.
- Vendored third-party code lives only under `vendor/` with its licence and
  provenance recorded in `vendor/VENDOR.md`. Adobe's Premiere Pro SDK is never
  committed (see `.gitignore`).

## One tree, two platforms

Windows and macOS build from the same sources. Platform code sits behind
`#ifdef _WIN32` / `#elif defined(__APPLE__)` in the file that needs it, and the
macOS Cocoa pieces live in `app/mac/*.mm` behind plain C++ headers. When you
change something platform-specific, keep the other platform's branch
compiling - you usually cannot test it, so make the smallest change there.

- Windows: `build.ps1` / `deploy.ps1` / `release.ps1` (MSVC, CUDA for the GPU
  tap, Inno Setup for the installer).
- macOS: `build_mac.sh` / `deploy_mac.sh` / `release_mac.sh` (Apple clang via
  the Command Line Tools, CMake + Ninja from Homebrew, pkgbuild for the
  installer). The OFX tap has a Metal path on macOS (`core/ScopeMetal.mm`,
  kernels compiled at run time - no Xcode needed); `scope_conformance_metal`
  and `scope_publish_race_metal` are its gates.
- The Premiere Pro plugin (`plugin/ScopeTransmit.cpp`) is Windows-only; its
  compiled copy in `prebuilt/` is fingerprinted against
  `core/ScopeControl.*`, `core/ScopeCore.*`, `core/ScopeShm.*` and
  `core/ScopeTypes.h` - change any of those and the Windows release refuses to
  build until `release.ps1 -UpdatePrebuiltTransmit` is run on a machine with
  the SDK.

## Before you call something done

- Build on the platform you are on and run the gates the release runs:
  `scope_conformance` (every reduction backend bit-exact against the
  reference; `-ffp-contract=off` is what keeps clang honest here) and
  `scope_publish_contention`.
- Update `PORTING.md` when a feature's status changes, and
  `GPU_PORT_HANDOFF.md` only for GPU-path work (CUDA and Metal alike). Those
  two files are the project's memory; nothing else is.
- `.gitattributes` turns line-ending conversion off for the whole tree because
  the files' endings are mixed as found. Do not "normalise" them.

## Conventions

4-space indent, braces on their own lines, `p_` parameters, `m_` members,
`g_` globals, and comments that say *why* - the existing files are the style
guide. New panels follow "Conventions for adding a panel" at the end of
`PORTING.md`.

## Releasing

Bump `VERSION` on `main`. The Release workflow builds the Windows installer
and the macOS package on CI and publishes both under one GitHub release; a
version that already has a release is built but not published.
