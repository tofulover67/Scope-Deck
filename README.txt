SCOPE DECK
==========

External video scopes for DaVinci Resolve (and Premiere Pro on Windows).

Scope Deck is a native app that shows waveform, histogram, vectorscope,
false colour, audio meters and more for the frame your editing software is
rendering, on a second monitor or anywhere you like. A tiny plugin called
"Scope Tap" sits in Resolve's node tree and hands each frame to the app
through shared memory, so there is no capture card and no screen grab.

Windows and macOS, built from one source tree. Free for everyone - see
LICENSE (MIT, with the one condition that Scope Deck itself may not be sold).

Contents of this file
---------------------
  1. Quick install with the installer
  2. Installing by hand on Windows
  3. Installing by hand on macOS
  4. First use in Resolve
  5. Timecode and subtitles (Resolve Studio only)
  6. One display on a Mac (App Nap)
  7. Uninstalling
  8. Building from source
  9. Where things are in this repository


1. QUICK INSTALL WITH THE INSTALLER
===================================

Go to the Releases page of this repository and download the current release.

  Windows:  ScopeDeck-Setup-<version>.exe
            Installs the app, the Scope Tap plugin for Resolve and, if
            Premiere Pro is installed, the Scope Deck plugin for Premiere.
            It includes its own Python for the timecode and subtitle panels.
            NVIDIA cards use the GPU (CUDA); other cards use the CPU.
            The installer is not code-signed, so Windows may show
            "Windows protected your PC": click "More info", then "Run anyway".

  macOS:    ScopeDeck-<version>.pkg
            Installs Scope Deck.app into Applications and the Scope Tap
            plugin into /Library/OFX/Plugins. Works on Apple silicon and
            Intel Macs; the plugin uses the GPU through Metal.
            The package is not signed, so macOS blocks it the first time:
            open System Settings > Privacy & Security, scroll down, and
            click "Open Anyway" next to the message about the package.

Quit DaVinci Resolve before installing. Then read section 4.


2. INSTALLING BY HAND ON WINDOWS
================================

Use this if you would rather not run the .exe installer. Every file it would
have put down is in a plain zip on the Releases page:

  ScopeDeck-<version>-windows.zip

Step by step:

  1. Download the zip and unzip it: right-click the file, choose
     "Extract All...", then "Extract". You get a folder named
     ScopeDeck-<version>-windows containing:

       Scope Deck\              the app folder (scopedeck.exe, a "python"
                                folder, and two small .py files - keep these
                                together, the app looks for them beside it)
       ScopeTap.ofx.bundle\     the plugin for DaVinci Resolve
       ScopeTransmit.prm        the plugin for Premiere Pro (optional)
       README.txt, LICENSE      this file and the licence

  2. Quit DaVinci Resolve (and Premiere Pro, if you use it).

  3. Put the app somewhere permanent. Move the whole "Scope Deck" folder to
     a place you will not delete later, for example:

       C:\Program Files\Scope Deck

     (Windows asks for permission when you copy into Program Files; click
     "Continue".) Your Documents folder is fine too. Then make a shortcut:
     right-click scopedeck.exe > "Show more options" > "Send to" >
     "Desktop (create shortcut)".

  4. Install the Resolve plugin. Open File Explorer and go to:

       C:\Program Files\Common Files\OFX\Plugins

     If the "OFX" folder or the "Plugins" folder inside it does not exist
     yet, create them (right-click > New > Folder; click "Continue" when
     Windows asks). Then copy the whole ScopeTap.ofx.bundle folder into
     "Plugins". When you are done, this file must exist:

       C:\Program Files\Common Files\OFX\Plugins\ScopeTap.ofx.bundle\Contents\Win64\ScopeTap.ofx

  5. (Optional) Install the Premiere Pro plugin. Copy ScopeTransmit.prm into
     Premiere's shared plugin folder, which is normally:

       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore

     Then, in Premiere, open Edit > Preferences > Playback and tick
     "Scope Deck" under Video Device. In Scope Deck, choose Premiere under
     the Input menu.

  6. First run. Double-click scopedeck.exe (or your shortcut). Because the
     file is not code-signed, Windows may show "Windows protected your PC":
     click "More info", then "Run anyway". This happens once.

  7. Continue with section 4.

Notes:
  - Python: the "python" folder inside "Scope Deck" is a complete, private
    copy of Python used only by the timecode and subtitle panels. Nothing is
    installed system-wide and nothing is added to PATH.
  - If you later move the "Scope Deck" folder, move all of it, not just
    scopedeck.exe.


3. INSTALLING BY HAND ON MACOS
==============================

Use this if you would rather not run the .pkg installer. The two bundles it
would have installed are in a plain zip on the Releases page:

  ScopeDeck-<version>-macos.zip

Step by step:

  1. Download the zip and double-click it. You get a folder named
     ScopeDeck-<version>-macos containing:

       Scope Deck.app           the app
       ScopeTap.ofx.bundle      the plugin for DaVinci Resolve
       README.txt, LICENSE      this file and the licence

  2. Quit DaVinci Resolve.

  3. Install the app: drag "Scope Deck.app" into your Applications folder.

  4. Install the Resolve plugin. In the Finder, choose Go > Go to Folder...
     (or press Shift-Command-G), type

       /Library/OFX/Plugins

     and press Return. If the folder does not exist, go to /Library instead,
     create a folder named "OFX" and, inside it, one named "Plugins" (the
     Finder asks for your password; that is normal). Drag
     "ScopeTap.ofx.bundle" into "Plugins". The Finder asks for your
     password again. When you are done, this must exist:

       /Library/OFX/Plugins/ScopeTap.ofx.bundle

     If you prefer the Terminal (Applications > Utilities), these two
     commands do the same thing, assuming the folder is still in Downloads:

       sudo mkdir -p /Library/OFX/Plugins
       sudo cp -R ~/Downloads/ScopeDeck-*-macos/ScopeTap.ofx.bundle /Library/OFX/Plugins/

  5. Allow the unsigned files to run. The app and plugin are not signed by
     Apple, and anything downloaded in a browser is marked for checking, so:

     - The first time you open Scope Deck, macOS may say it "cannot be
       opened" or "cannot check it for malicious software". Open
       System Settings > Privacy & Security, scroll down, and click
       "Open Anyway". Or right-click the app in Applications and choose
       "Open" instead of double-clicking.

     - Resolve may silently skip the plugin for the same reason. If Scope
       Tap does not appear in Resolve's OpenFX list, remove the download
       mark with these two Terminal commands (one line each):

       sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/ScopeTap.ofx.bundle
       xattr -dr com.apple.quarantine "/Applications/Scope Deck.app"

  6. Turn App Nap off for Resolve (strongly recommended on a Mac with one
     display; see section 6). The installer does this for you; by hand it is
     one Terminal command:

       defaults write com.blackmagic-design.DaVinciResolve NSAppSleepDisabled -bool YES

  7. (Optional) Python, for the timecode and subtitle panels. They need
     Python 3.10 or newer. The installer downloads one from python.org when
     the Mac has none; by hand, install one from https://www.python.org
     (the standard macOS installer is fine). Nothing else is needed. Note
     these two panels also need Resolve Studio - see section 5.

  8. Continue with section 4.

Permissions: the first time you open the Audio Meter, macOS asks for
permission to capture system audio; Screen Capture asks for Screen
Recording. Both live under System Settings > Privacy & Security. Because the
app is not signed by a developer account, macOS may ask again after an
update; if a permission looks switched on but does not work, switch it off
and on again in that list.


4. FIRST USE IN RESOLVE
=======================

  1. Start DaVinci Resolve and open a project.
  2. On the Color page, open the OpenFX panel (top right), find the
     "Scope Deck" group and drag "Scope Tap" onto a node - the last node is
     the usual choice, so the scopes show the finished grade.
  3. Open Scope Deck from Applications or the Start menu, or press the
     "Open Scope Deck" button in the Scope Tap plugin's controls.
  4. Play. The scopes follow the frame Resolve renders. Right-click any
     panel for its settings; drag panels to rearrange them or pop them out
     onto another monitor.

Scope Tap passes the image through unchanged. It reduces each frame into
scope data on the GPU (CUDA on NVIDIA under Windows, Metal on macOS) and
otherwise on the CPU; either way Resolve's output is untouched.


5. TIMECODE AND SUBTITLES (RESOLVE STUDIO ONLY)
===============================================

The Timecode panel and the "Show Subtitles" overlay read Resolve's timeline
through Resolve's scripting interface. That interface exists only in
DaVinci Resolve Studio, and even there it is off until you switch it on:

  DaVinci Resolve > Preferences > System > General >
  "External scripting using": set to "Local".

The free edition of Resolve has no scripting, so on it the Timecode panel
reads "Resolve scripting unavailable (needs Resolve Studio with External
Scripting on)" and subtitles stay off. Everything else in Scope Deck works
the same on the free edition.

These two panels also need Python 3.10 or newer on the Mac (section 3,
step 7); on Windows the app carries its own.


6. ONE DISPLAY ON A MAC (APP NAP)
=================================

When DaVinci Resolve's window is completely covered - easy on a laptop with
Scope Deck in front - macOS puts Resolve to sleep ("App Nap") and its
playback runs fast with no sound until part of its window shows again.

Scope Deck opens at three quarters of the screen for this reason and
remembers where you put it. The .pkg installer also switches App Nap off for
Resolve; if you installed by hand, run this once in the Terminal (it takes
effect the next time Resolve starts):

  defaults write com.blackmagic-design.DaVinciResolve NSAppSleepDisabled -bool YES


7. UNINSTALLING
===============

Windows: if you used the installer, use "Add or remove programs". By hand:
delete the "Scope Deck" folder, delete
C:\Program Files\Common Files\OFX\Plugins\ScopeTap.ofx.bundle, and delete
ScopeTransmit.prm from Premiere's MediaCore folder if you installed it. Your
layouts live in %LOCALAPPDATA%\ScopeDeck - delete that folder too if you
want a clean slate.

macOS: drag Scope Deck.app from Applications to the Bin, and delete
/Library/OFX/Plugins/ScopeTap.ofx.bundle (the Finder asks for your
password). Your layouts live in
~/Library/Application Support/Scope Deck. To undo the App Nap setting:

  defaults delete com.blackmagic-design.DaVinciResolve NSAppSleepDisabled


8. BUILDING FROM SOURCE
=======================

Both builds use the vendored dependencies in vendor/ (Dear ImGui, GLFW,
stb_image, the OpenFX SDK) - no package manager, no download.

Windows (Visual Studio Build Tools with the C++ workload; CUDA 12.8 or 12.9
optional, for the GPU plugin):

  powershell -NoProfile -File .\build.ps1

macOS (Command Line Tools, plus "brew install cmake ninja"):

  ./build_mac.sh

"--run" launches the app and "--deploy" copies the plugin into Resolve's
plugin folder. release.ps1 and release_mac.sh are what the GitHub release
runs; they also produce the zips described in sections 2 and 3.


9. WHERE THINGS ARE IN THIS REPOSITORY
======================================

  app/             The app: main.cpp (panels, menus, layout), the scope
                   image builders, the audio / timecode / subtitle bridges,
                   screen capture. app/mac/ holds the macOS-only
                   Objective-C++.
  core/            The wire format (ScopeTypes.h), its reader, the
                   reduction engine, the shared-memory publisher, and the
                   CUDA (Windows) and Metal (macOS) reductions. No UI.
  plugin/          ScopeTap (OpenFX, for Resolve) and ScopeTransmit
                   (Premiere Pro, Windows only).
  tools/           Conformance and contention harnesses, the headless probe,
                   the self-test, the worker checks.
  installer/       The Inno Setup script (Windows) and the pkg definition
                   (macOS).
  *_poll_worker.py The Python helpers the timecode and subtitle panels run
                   against Resolve's scripting interface.

PORTING.md tracks every feature's status and the reasoning behind each
design decision; GPU_PORT_HANDOFF.md covers the GPU work. AGENTS.md is the
short list of rules for anyone (or any agent) changing the tree. LICENSE is
the licence.
