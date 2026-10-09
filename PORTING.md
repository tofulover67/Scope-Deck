# Porting status: old PySide6 app -> this build

Tracks feature parity against `Scope Deck (old)/app/scope_deck.py`. Update this
when a feature moves between states - it is the only place that list lives,
there being no other planning doc for this build (see the old app's own
README.md and NEXT_STEPS_ENHANCED_RENDER.md for behavior/rationale references
when porting something new).

Status legend: Ported (full parity, or a deliberate redesign noted inline) /
Partial (missing some settings or a sub-feature) / Missing (not started) /
Skipped (deliberately not being ported).

## Fixed: Colorize made Enhanced Render ~4x more expensive, and it was none of
## the things it looked like

Reported as a large frame-rate drop on White Balance, then confirmed on
Vectorscope too: with Enhanced Render on, turning Colorize on took the app from
32 fps to 24. Measured per frame on a UHD White Balance panel, the Enhanced pass
cost 7.2 ms with Colorize against 1.7 ms without.

The fix was one function. Getting to it took two wrong answers, both arrived at
by reasoning about the code rather than measuring it:

1. **"It is the per-segment draw calls."** Colorize drew one `AddLine` per
   segment - 262,144 calls a frame, against 512 `AddPolyline` calls for the
   plain path. Batching them into one vertex reservation per run changed
   nothing measurable.
2. **"It is the vertex count."** Flat per-segment colour cannot share vertices
   between neighbours, so Colorize emitted 4 vertices per segment against 2 per
   point - 1,044,904 vertices against 524,244. Emitting shared vertices with
   per-*point* colour (the call sites already recorded one colour per point)
   halved the geometry exactly as intended, and the time did not move.

Splitting the timer into walk and emit settled it in one reading:

| | walk | emit | total |
|---|---|---|---|
| Colorize on | **5.7 ms** | 1.4 ms | 7.2 ms |
| Colorize off | **0.7 ms** | 1.0 ms | 1.7 ms |

The geometry was never the problem. The cost was the per-point colour maths in
the trace walk - `std::max({ r, g, b, 1e-4f })` plus three divides by the peak,
about 19 ns a point over 262,144 points. The `initializer_list` overload of
`std::max` does not reliably reduce to three comparisons, and the three divides
are serially dependent. `BeamColour` in `main.cpp` now does explicit
comparisons and a single reciprocal: walk **5.7 -> 1.2 ms**, whole pass
**7.2 -> 2.6 ms**.

The same expensive pattern existed in **two** places, which is why fixing the
three obvious call sites still left Waveform slow: `DrawEnhancedTrace` carries
its own private copy of the normalise-and-clamp, because its colorize applies
only to the luma plane (on a parade cell the plane's own tint *is* the
information). It now calls `BeamColour` too. Chromaticity was never affected -
it already had precomputed bytes.

Both earlier changes were kept - neither is a regression, the batching removes
262,144 redundant reservations a frame and the shared-vertex emitter halves the
geometry - but it is worth recording that neither was the bottleneck, and that
two rounds of plausible reasoning lost to one measurement.

`r * (1/peak)` is not bit-identical to `r / peak`. The result is quantised to
8 bits for display immediately afterwards, where a 1-ULP difference cannot
survive.

Left in place: a live readout in the status bar
(`enhanced 2.6 ms (walk 1.2 / emit 1.4) 261632 seg 524288 vtx x1`), accumulated
per frame across every Enhanced pass and reported one frame late because the
status bar draws before the panels do. It also prints `FALLBACK` if the
shared-vertex path could not be taken, since a silently-not-taken fast path
looks exactly like a fast path that does not help.

## Ported: Color Cube (3D RGB/XYZ/Lab point cloud)

The last entry on the "Cross-cutting systems" table flagged as the largest
single UI lift on the whole list - a genuine 3D point cloud with real camera
orbit, not a fixed-angle projection, per the old app's `ColorCubePanel`
(`scope_deck.py`, ~322 lines) and its colour-space math in `scope_format.py`
(`srgb_to_xyz`/`xyz_to_lab`, ~60 lines) - neither of which had any equivalent
anywhere in this tree before now (confirmed by grep; `PORTING.md` already
noted the still-unported Chromaticity panel needs the same XYZ step, which
this unblocks too).

New `PanelKind::ColorCube`, backed by two new pieces in `ScopeImages.h/.cpp`:
- `SrgbToXyz`/`XyzToLab` - the standard sRGB -> CIE XYZ (D65) -> CIE L*a*b*
  formulas, a straight port of the old app's own constants/matrix.
- `BuildColorCubePoints` - samples up to `kColorCubeMaxPoints` (6000, matching
  the old app's own cap) points from the frame's live preview and positions
  each in the selected space, normalised so RGB/XYZ/Lab all share one camera
  scale (same axis ranges the old app's `COLOR_CUBE_AXIS_RANGES` used).

`DrawColorCubePanel`/`DrawColorCubeSettings` (`main.cpp`) carry the rendering
and controls: middle-drag to orbit, Shift+middle-drag to pan, wheel to zoom,
`Z` to reset - the same middle-button convention common 3D tools use, so
panning doesn't fight orbiting - plus a Color Space dropdown (RGB/XYZ/Lab),
Spread (1-10, stretches the cloud off the neutral axis for legibility) and
Density (1-6, point size), all persisted (matching the old app's own
`PERSISTED_SETTINGS`); the camera itself is deliberately not, same reasoning
as every other panel's zoom/pan.

**Two deliberate redesigns**, not compromises:
- Points are drawn as ordinary `ImDrawList` primitives (`AddCircleFilled`)
  directly, not rasterised into a CPU pixel buffer and blitted as a texture
  the way the old app's own docstring describes needing to. That trick
  existed because `QPainter` has no cheap way to stamp a few thousand
  individual points; `ImDrawList` is GPU-batched already and does not need
  it - almost certainly faster, not just simpler.
- The 6000-point subsample is evenly strided (`BuildColorCubePoints`'s own
  comment) rather than the old app's true random `choice(replace=False)` -
  O(sampleCount) regardless of preview resolution instead of O(totalPixels)
  to draw from. Still deterministic per frame (seeded from `frameIndex`,
  same reasoning the old app's own frame-seeded RNG existed for - repainting
  the same video frame for any reason must not visibly re-jitter the cloud).

One correctness detail worth calling out since it would have been an easy
place to get it subtly wrong: Lab's neutral (black-white) axis is **not**
the cube's own diagonal corner-to-corner the way RGB's and XYZ's both
happen to be (each of those two spaces normalises every axis independently
against its own white point, so black lands at exactly (-1,-1,-1) and white
at (1,1,1) either way) - Lab's neutral axis is L alone at a*=b*=0, i.e.
(-1,0,0) to (1,0,0), nowhere near the cube's diagonal. `ColorCubeNeutralAxis`
special-cases this rather than assuming one geometry fits all three spaces;
verified visually (screenshot) that Lab's dashed Black-White line actually
runs along a different path than RGB/XYZ's.

Verified with a screenshot against a synthetic feed, one panel per colour
space (RGB/XYZ/Lab side by side) - correct wireframe, axes, labels, and a
visibly different (correctly curved, non-linear) cloud shape for Lab versus
the two linear spaces. Not yet verified interactively (orbit/pan/zoom/reset,
Spread/Density) against real footage - this environment cannot drive mouse
drags on the running app itself.

## Added: meter ballistics for the Audio Meter's bar (fixes the flicker, smooths vertical movement)

Both reported together, and it's one mechanism: the bar was reading
`AudioMeterLevels::peakDb` directly every video frame with no smoothing at
all - snapping straight to whatever the WASAPI capture thread's latest
~10-20ms packet happened to read. Real audio varies far more buffer-to-buffer
than a meter should visually read as "the level," so at 60fps this looked
like flicker/jumpy vertical movement rather than a smooth rise and fall.

Added `AudioMeterState::displayDb` - a per-channel smoothed value, separate
from the existing peak-hold marker, updated with simple meter ballistics:
fast attack (200dB/sec - rises to a louder reading fast enough to feel
instant) and a deliberately slower release (20dB/sec - falls back rather than
snapping down). The bar's fill and zone colouring now read this smoothed
value; peak-hold still tracks the *raw*, unsmoothed reading directly (renamed
to `rawDb` for clarity), so a brief transient still registers on the hold
marker even though the bar itself no longer chases every individual reading.

## Fixed (the actual root cause): Low-Pass Kernel turned a masked trace's gaps into a near-total blank

Reported after the Smooth Trace fix above didn't fully resolve it: masking
still blanked the Waveform "with or without Smooth Trace." Root-caused with a
safe, offline repro - a temporary env-var hook forcing a mask on at startup
against a synthetic feed, no mouse/keyboard input - once it became clear the
underlying masked trace data itself was correct (confirmed by direct
diagnostic: ~28% finite / 72% NaN, exactly matching the mask's geometric
coverage) while the *rendered* result was still blank in one specific
settings combination but not another. The one difference: Low-Pass Kernel
was non-zero.

Root cause in `BoxBlurColumns` (`ScopeImages.cpp`), used for both Signal
Prefilter and Low-Pass Kernel: it keeps a single running sum via a sliding
window (`sum += entering - leaving`) rather than recomputing each window from
scratch, for speed. That trick assumes every value in the window is real.
The instant a NaN (a masked-out gap) slides into the window, the running
`sum` itself becomes NaN - and unlike the leaving/entering values, which are
only ever added or subtracted once each, a NaN already folded into a running
total never "expires" back out through ordinary arithmetic. From that column
onward, *every remaining column in that row* comes out NaN, not just the
ones actually inside the gap - given a mask leaves most columns NaN to begin
with, the first one hit early in a row poisons nearly the whole rest of it,
and across every row and plane this reduced to the near-total blank in the
report. This is not a masking-specific bug in spirit - any Enhanced Render
consumer running a Low-Pass Kernel over data with real gaps in it would hit
this - masking is just the first thing in this build that produces gaps at
all.

Fixed by tracking a running *valid-sample count* alongside the running sum,
skipping NaN entries out of both rather than assuming a fixed window width -
a masked-out column blurs using only its real neighbours, and a gap stays a
gap rather than spreading. Verified against the exact failing configuration
(Smooth Trace on, Low-Pass Kernel 18, a mask active) via the same safe
offline repro: was a total blank, now shows the correct trace confined to
the mask.

## Fixed: Smooth Trace could permanently blank a Waveform's Enhanced Render trace

Reported after the NaN-gap fix below: with "Smooth Trace" on, the Waveform's
Enhanced Render trace went completely blank, and stayed blank - reproduced
on a genuinely fresh app relaunch, no mask ever drawn. Confirmed by having
the user uncheck Smooth Trace live: the trace came back immediately.

Root cause, and it predates today's session: `PrepareEnhancedTrace`
(`ScopeImages.cpp`) blends each frame's trace 50/50 with the previous
frame's (`p_Out[i] = 0.5*p_Out[i] + 0.5*prevTrace[i]`), across every future
frame while Smooth Trace stays on. If *any* column ever goes NaN for even a
single frame - the mask case fixed below is one way, but the same failure
mode exists for any other latent NaN source, e.g. some real-footage edge
case in the reader/tap's own trace math, nothing to do with masking - that
NaN lands in `prevTrace`. Every following frame then blends against it, and
`0.5 * x + 0.5 * NaN` is NaN regardless of `x`: one bad frame permanently
poisons that column for as long as Smooth Trace stays on, no matter how
good every later frame's own sample is. Given enough frames, enough columns
eventually take their turn going NaN and never recover, until most or all
of the trace is gone. This bug already existed before today - the old code
just fed NaN vertices into `AddPolyline` unquestioned, which happened to
still render *something* (if a corrupted something), so a permanently
blanked column read as a rendering quirk rather than the connected-through
regression the NaN-gap fix below made visible for the first time.

Fixed in the blend itself: a NaN current sample stays a gap rather than
being blended with a stale previous value (drawing a "ghost" of data that
no longer exists is exactly what the NaN-gap fix below exists to avoid);
a NaN *previous* sample is treated as nothing to blend against yet and the
current value passes through unsmoothed, the same as that column's own
first frame - so one bad frame decays back to normal on the next good one
instead of latching forever.

## Fixed: masked Enhanced Render plunged to 0 outside the mask instead of leaving a gap

Reported with a screen recording: with a mask active, the Waveform's
Enhanced Render trace dropped to the very bottom outside the mask's
horizontal extent, reading as if that part of the frame were pure black -
it wasn't; there was just no mask there to measure.

Root cause in `BuildMaskedWaveformTrace` (`ScopeImages.cpp`): a waveform
column with zero masked pixels in it (nothing in the mask falls in that
column at all) wrote `r=g=b=luma=0` - a real, displayable value indistinguishable
from a genuinely black sample - rather than "nothing measured here." Every
reader of this trace (`DrawEnhancedTrace` for the Waveform,
`DrawVectorscopeEnhanced`, and White Balance's Twin Peaks trace) then drew a
straight line connecting that fake zero to its real neighbours, which is
exactly the plunge in the recording.

Fixed at the source: those columns now get `NaN` instead of `0`. All three
readers already had (or, for the Waveform, now have) a "break the line
here" mechanism for exactly this kind of gap - the Vectorscope's and White
Balance's own out-of-range checks, and a new equivalent added to the
Waveform's `DrawEnhancedTrace`, which previously just clamped every sample
into range unconditionally. `std::isnan` is checked explicitly before each
one's existing range/distance test, rather than relied on to fail it
naturally - IEEE comparisons against NaN are always false, so an
unguarded `< / >` check would have silently let a NaN point through
instead of treating it as an out-of-range break.

Not an accuracy bug in the *bins* (Histogram/Vectorscope/Waveform density,
which already just count fewer samples when a mask excludes them - correctly
representing "less data," never a fabricated value) - this was specific to
the trace's per-column average, the one place a "no data" case needed its
own value instead of falling out naturally from a count that's still
accurate at zero.

## Fixed: two spatial-mask CPU paths were single-threaded, one of them very expensive at UHD

Follow-up to a measurement pass (see the GPU-acceleration checkbox discussion
above): a standalone microbenchmark against `ScopeImages.cpp`'s functions
directly (bypassing the app and `scope_selftest`, since the latter never
populates a preview buffer) found `BuildDimmedPreviewImage` (the Source
panel's masked/dimmed tint) costing **~49ms at UHD**, and
`BuildMaskedWaveformTrace` (part of `BuildMaskedScopeBins`, feeding masked
Waveform/Vectorscope Enhanced Render) a further chunk on top - both plain,
single-threaded per-pixel loops, unlike `BuildScopeBinsFromRGB` right next to
the second one, which already split work across threads with a per-thread-
private-buffer merge (the same pattern `ScopeCore.cpp`'s own `Reduce()` uses).

Threaded both the same way: split by row range (image rows for the preview
tint, the 256 fixed trace rows for the waveform trace) across
`std::thread::hardware_concurrency()` workers via `std::async`. Neither
needed a merge step afterwards - each thread writes only its own disjoint
output rows, unlike the bins case, which has to merge per-thread histograms
into one.

Measured after, same UHD synthetic frame: `BuildDimmedPreviewImage` dropped
**49ms -> 5.4ms** (~9x). `BuildMaskedWaveformTrace` dropped to under 1ms on
its own - but the masked-bins path *as a whole* only improved modestly
(25.9ms -> 20.3ms), because breaking `BuildMaskedScopeBins` apart afterwards
showed `BuildScopeBinsFromRGB` - already-threaded, not touched here - is now
the one carrying essentially all of that 20ms itself. Its threading was
already there before this change; what it actually needs is a different fix
(likely the per-call allocate-and-zero of full-size private bin buffers for
every thread, plus the serial merge afterwards, rather than the core loop
itself) - flagged here as a distinct, separate follow-up rather than folded
into this one, since "add multithreading" and "reduce an already-threaded
function's per-call overhead" are different problems.

## Added: "GPU Acceleration" checkbox in ScopeTap - placeholder, not wired up

Requested after a discussion of what would actually be needed to move the
scope-measuring work (currently CPU-only, see the "GPU Acceleration toggle"
row in the table below) to the GPU: a real control to design around, added
now, deliberately not implemented yet.

`plugin/ScopeTap.cpp`'s `describeInContext` gets a new `BooleanParamDescriptor`
(`kParamGpuAcceleration`), placed right after Row Step - the two are the same
family (both about the cost of the scope-bin measuring work, not the preview
image Publish Video/Preview Scale control), defaulted off. Its hint says
outright that it currently does nothing. Not fetched into the plugin
instance and not read anywhere in `render()` - genuinely inert, on purpose,
so it shows up in Resolve's Inspector for the GPU measuring path to be
designed against without half-wiring anything ahead of that design.

## Fixed: the spatial mask fought itself with two or more Source panels open

Reported with a screen recording: dragging a mask on one Source panel showed
a completely different-looking shape on a second, simultaneously-open Source
panel - not a rendering glitch, the mask geometry itself was actually wrong.

Root cause in `UpdateSpatialMaskInput` (`main.cpp`): `App::spatialMask` and
`App::maskInput` are deliberately global/shared - the mask means the same
region regardless of which panel is showing it - but this function is called
once per Source panel *instance* every frame, each with that panel's own fit
rect. The initial press correctly checked `hovered` (this panel's own
window-hovered state) before starting a drag, but the *continuation* blocks -
"editing an existing mask" and "drawing a brand new mask" - only checked
`in.editing`/`in.drawingNew`, both plain global flags with no idea which
panel's mouse-drag they were meant to belong to. The instant a drag started
on one Source panel, every *other* open Source panel's own call that same
frame also saw those flags set, and reinterpreted the very same global mouse
position against its own (differently positioned) fit rect - overwriting
whatever the panel actually being dragged in had just written. Whichever
panel happened to run last in the per-frame loop "won" for that frame, which
is exactly the flicker-between-two-wrong-shapes behaviour in the recording.

Fixed by adding `MaskInputState::owningPanelId`, set to the acting panel's id
when a drag actually starts (grabbing a handle, the body, or Ctrl/Shift-
drawing a new one), and checked by both continuation blocks before they're
allowed to touch the shared mask - every other panel's call now sees "not
mine" and does nothing, so only the panel the drag actually started on can
move it. Cleared back to -1 on release. Single-Source-panel setups never hit
this at all, which is why it went unnoticed until two were open at once.

## Added: Audio Meter panel (system-audio loopback, not an old-app feature)

The old Python app never had audio metering at all - this is new, not a port,
prompted by the user asking whether Resolve exposes any audio access.

Checked both routes into Resolve before building anything: its OFX SDK
(`...\Support\Developer\OpenFX\OpenFX-1.4\include`) has no audio suite of any
kind - `ScopeTap` cannot be extended to carry audio the way it carries video,
full stop - and its scripting API (`...\Support\Developer\Scripting\README.txt`)
has plenty of audio-*editing* calls (tracks, clips, sync) but nothing that
returns live sample data or a meter/loudness value. Same wall `ERRORS.md`
already hit trying to drive Resolve's transport: read-only structural
queries, no live signal access. Confirmed this is normal by checking how Nobe
OmniScope - a real, shipping competitor - handles the identical gap: it
supports proper capture hardware (DeckLink/UltraStudio SDI) *and* advertises
"capture your computer's audio output directly - no cables required", i.e.
the same OS-level loopback this app has no hardware to avoid needing.

New panel kind `PanelKind::AudioMeter` (`main.cpp`), fed by a new
`AudioMeterBridge.h/.cpp` - WASAPI loopback capture of the current default
render device, following the same background-thread-plus-mutex shape
`TimecodeBridge` already established (`AudioMeterBridgeStart/Stop`,
`AudioMeterBridgeSetActive` gating the capture thread so it only opens the
stream while an Audio Meter panel is actually open, `AudioMeterBridgeGetLevels`
read once per draw). Per-channel peak/RMS in dBFS, up to 8 channels;
`DrawAudioMeterPanel` renders one bar per channel (green/yellow/red by
headroom) with a peak-hold indicator, `-60dB` floor for the bar's own display
range chosen for readability, not because the data is any less precise below
that.

Windows-only (`#ifdef _WIN32`), same as `FocusResolveWindow` - WASAPI has no
cross-platform equivalent, so this needs a real Core Audio/AVAudioEngine tap
when this project ever gets a macOS build, not a recompile.

Superseded by the fix below the same day: this originally captured the whole
system's default output device (any app's audio, not just Resolve's) -
changed to target Resolve's process specifically once asked for that.

**Not yet verified interactively**: clean build passes and the app launches
without crashing, but the actual capture path (does it show a real level
while Resolve is actually playing something, does peak-hold decay look
right, does it correctly show nothing once Resolve exits) needs a hands-on
check - this environment cannot drive the running GUI or generate test audio
itself.

## Fixed: Preferences had the same native-OS-window issue every Settings window used to

Reported with a screenshot: the Preferences window still had a native Windows
titlebar/minimize/maximize/close, the exact "Settings windows could sprout
their own native OS window" bug fixed for every per-panel Settings window
earlier - `DrawPreferences` just never got the same treatment, since it
isn't one of the `DrawXSettings` functions that fix touched.

Applied the same two fixes: `ImGui::SetNextWindowViewport(GetMainViewport()->ID)`
before `Begin` (pinning it to the main app window instead of leaving ImGui's
multi-viewport logic free to decide otherwise), and the same guarded
`BringWindowToDisplayFront` every per-panel Settings window already has, so
it also stays on top and its own Combo (Scope Source, currently disabled)
doesn't hit the dropdown-breaking bug fixed above.

## Fixed: the mask's dimmed-region tint lagged one frame behind its own outline

Reported with a screen recording: rotating/moving the mask fast, the yellow
outline and handles tracked the mouse immediately, but the actual light/dark
tint on the image visibly trailed behind by a beat, catching up once the drag
slowed or stopped.

Root cause in `DrawSourcePanel` (`main.cpp`): the dimmed preview
(`BuildDimmedPreviewImage`) was built and uploaded to the texture *before*
`UpdateSpatialMaskInput` ran for that same frame - so every frame, the tint
was drawn from whatever the mask was *before* that frame's own mouse-drag
delta got applied, while the overlay outline (`DrawSpatialMaskOverlay`,
called after `UpdateSpatialMaskInput`) always drew the just-updated, current
mask. One frame of lag, every single frame, invisible once the mask stops
changing (stale value == current value) but compounding visibly during a
fast continuous drag - exactly the symptom in the recording.

Fixed by reordering: mask input (and the zoom/fit rect it needs) now runs
*before* the dimmed preview is rebuilt, sized from the incoming frame's own
dimensions rather than the texture's so it no longer needs to wait for that
frame's upload to happen first. The tint and the outline now both read the
same, already-current-for-this-frame mask.

## Fixed: the "always on top" fix broke every Combo dropdown in every Settings window

Reported: Mask Shape's dropdown couldn't be clicked at all - the list opened
but nothing in it responded.

Root cause is the very next fix below, the day it landed:
`ImGui::BringWindowToDisplayFront` on a Settings window, called unconditionally
every frame, is a no-op once that window is already the front-most one - but
opening a Combo's popup makes the *popup* the new front-most window the
instant it appears (popups are just another window in the same display-order
list). On the very next frame, the parent Settings window's own unconditional
re-raise is no longer a no-op - it shoves the popup it just opened back
behind itself immediately, so the dropdown list is still visible but nothing
in it can be hit-tested or clicked. Every Combo in every Settings window
had this same failure waiting - Mask Shape was just the one actually
exercised.

Fixed by skipping the re-raise while this window owns any open popup
(`ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)`) -
applied to all 15 `DrawXSettings` functions that had the original fix.

## Fixed: Settings windows could sprout their own native OS window, and could lose the "always on top" fight

Two related complaints about per-panel Settings windows: they were following
ImGui's own multi-viewport logic and occasionally becoming their own natively
decorated OS window (the same "Open in New Window" decoration this build
turned on for panels), which reads as clunky for a small utility popup that
isn't meant to be a first-class window in its own right; and they didn't
reliably stay drawn on top of the main UI - the code already draws every
panel's Settings window after all panels for exactly that reason, but that
only wins the first frame after opening: clicking back on a panel (adjusting
one of its own controls while its Settings window is still up) raises that
panel's own display order the same way any click does, and from then on it
can render over the Settings window on every later frame regardless of
submission order.

Fixed both in `PanelCommon`/`main.cpp`:
- Added `PanelCommon::panelViewportId`, refreshed every frame right after the
  panel's own `ImGui::Begin` (`main.cpp`'s panel-window loop). `PositionSettingsWindow`
  (`PanelCommon.cpp`) now calls `ImGui::SetNextWindowViewport(panelViewportId)`
  unconditionally, every frame it runs, not just while placing the window -
  pinning the Settings window to whichever OS window its panel already lives
  in (the main app window normally, or a popped-out panel's own window) is
  what stops ImGui's own viewport logic from ever deciding to give it a
  native window of its own.
- Every `DrawXSettings` function now calls `ImGui::BringWindowToDisplayFront()`
  on itself right after `Begin` - display order only, not `FocusWindow`, so
  this never steals keyboard/nav focus from whatever the user is actually
  typing into, but does mean the Settings window wins the "on top" fight
  every single frame instead of only the first one.

## Changed: Audio Meter now captures Resolve specifically, not the whole system's output

Reported after the first cut (whole-default-device WASAPI loopback, see
below): the user doesn't want other apps' audio measured at all, only
Resolve's.

Windows has a real, separate mechanism for exactly this - process-specific
loopback activation (`ActivateAudioInterfaceAsync` with
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, added Windows 10 2004+), the
same one behind Windows 11's own per-app volume mixer and OBS's "audio output
capture" source - distinct from and additional to the older whole-device
`AUDCLNT_STREAMFLAGS_LOOPBACK` flag the first cut used, not a repurposing of
it. Confirmed present in this machine's installed SDK (10.0.26100.0) before
touching any code.

`AudioMeterBridge.cpp`'s `LoopbackStream::Open` now: finds Resolve's process
id (same window-enumeration-plus-`QueryFullProcessImageNameA` technique
`main.cpp`'s own `FocusResolveWindow` already uses, matching by exe filename
rather than title), builds an `AUDIOCLIENT_ACTIVATION_PARAMS` targeting that
PID with `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` (Resolve's own
child/helper processes count as Resolve, nothing else does), and calls
`ActivateAudioInterfaceAsync` - asynchronous by design (a real round trip to
the audio engine), so the capture thread blocks on an event in a small
`IActivateAudioInterfaceCompletionHandler` implementation rather than
polling. Re-resolves Resolve's PID every time it opens rather than trusting a
cached one - Resolve not running yet, or having been restarted, is the
ordinary case here, not an edge case.

One consequence: a process-loopback activation has no real device behind it
to query a mix format from, so the format is no longer detected (the
whole-device version's float/PCM16/PCM32 branching is gone) - it is simply
fixed at 2-channel 48kHz float32, the same canonical choice Microsoft's own
`ApplicationLoopback` sample uses; the audio engine handles resampling
Resolve's actual internal mix into that format.

Needs `mmdevapi.lib` linked explicitly now (`CMakeLists.txt`) -
`ActivateAudioInterfaceAsync` is a plain DLL export called by name, unlike
the `CoCreateInstance`-based calls elsewhere in this file, which resolve
their DLL through COM and need no import lib of their own.

## Adjusted: colour thresholds and a numbered dB scale, matching Resolve's own meter

Confirmed working (screenshot showed real, moving levels from Resolve) - two
follow-up refinements requested against a reference photo of Resolve's own
audio meter. Moved the yellow/red thresholds in `DrawAudioMeterPanel` from
-6/-3dB to -10/-5dB, matching what Resolve's own meter uses (not a broadcast
standard either way, just matching the reference now instead of an arbitrary
first guess). Added a shared numbered scale down the left edge - 0, -5, -10,
-15, -20, -30, -40, -50, -60 - with a faint horizontal line at each value
across the bars, the same way Resolve's own meter reads. The labels reserve
their own column (measured from the widest label's text width) before the
bars are laid out, rather than overlapping them.

Not a faithful reproduction of Resolve's actual scale spacing - Resolve's
own meter compresses the lower range (the gaps between -20/-30/-40/-50/-60
in the reference photo are visibly smaller than the gaps between 0/-5/-10/
-15/-20), which reads as a non-linear/log-like scale below -20dB. This
version keeps the same linear dB-to-pixel mapping the bars themselves already
used and just places the same label values on it, so the numbers are right
but their spacing doesn't compress the same way below -20dB.

## Fixed (likely root cause): the completion handler had no COM marshaling support at all

Found by comparing `ActivationCompletionHandler` against Microsoft's own
`ApplicationLoopback` sample line-by-line, once the split below narrowed the
`E_POINTER` down to `ActivateAudioInterfaceAsync`'s own synchronous return
specifically (not the async result, and not activation itself finding
Resolve). The one real difference: the sample's equivalent callback object
derives from WRL's `RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase,
IActivateAudioInterfaceCompletionHandler>` - `FtmBase` gives it a
free-threaded marshaler for free. `ActivateAudioInterfaceAsync` needs to hand
this completion callback across an apartment/thread boundary to actually
deliver its result, and a hand-rolled COM object with no marshaling support
at all - which is what `ActivationCompletionHandler` was - cannot be passed
across that boundary; there's no proxy/stub registered for a private
interface like this one, so standard marshaling has nothing to fall back on
either.

Fixed by aggregating the standard free-threaded marshaler
(`CoCreateFreeThreadedMarshaler`, called once in the constructor) and
delegating any `QueryInterface` this object doesn't handle itself to it -
the plain C++ equivalent of what `FtmBase` gives WRL types automatically.
Flagged as "likely" rather than confirmed outright: this is a real,
concrete difference from working reference code and a well-known
requirement for exactly this pattern, not a guess, but it hasn't been
confirmed against Resolve's actual audio yet.

## Refined: split "activation failed" into three distinct stages

Reported: the message read "Couldn't tap Resolve's audio (0x8000000E)" - a
plain COM `E_POINTER`. `ActivateAudioInterfaceAsync` activation genuinely has
two independent places it can fail through - its own synchronous return, and
the *asynchronous* result `ActivateCompleted`/`GetActivateResult` deliver
later, on a different thread - and the one status/`ActivationFailed` bucket
above collapsed both into the same message, so which of the two actually
produced that `E_POINTER` was still unknown. Split into
`ActivationCallFailed` (the synchronous return itself), `ActivationTimedOut`
(the wait on `ActivateCompleted` never woke up), and `ActivationResultFailed`
(`ActivateCompleted` fired, but `GetActivateResult` reported failure) - each
with its own message in `DrawAudioMeterPanel`, so the next report pins down
which stage this is actually stuck at instead of re-guessing across all of
`LoopbackStream::Open` again. Checked the construction itself line-by-line
against Microsoft's own `ApplicationLoopback` sample
(`Windows-classic-samples`) first - the `AUDIOCLIENT_ACTIVATION_PARAMS`/
`PROPVARIANT`/`ActivateAudioInterfaceAsync` call shape matches it, so this
isn't yet a confirmed root cause, just a narrower question.

## Fixed: Audio Meter gave no way to tell why it read silence

Reported after the Resolve-specific rework above: the meter showed no levels
at all, with nothing to say why - "Resolve isn't running", "found Resolve but
the tap itself failed", and "tapped it fine, nothing is just playing right
now" all looked identical (the same generic "no audio captured" text), which
made the actual failure impossible to diagnose remotely.

Added `AudioMeterStatus` (`AudioMeterBridge.h`) - `ResolveNotFound` /
`ActivationFailed` / `StreamInitFailed` / `Capturing` - plus the failing
`HRESULT` where one exists, threaded through every return path of
`LoopbackStream::Open` and published alongside the levels every time.
`DrawAudioMeterPanel` now shows a distinct message per stage, including the
raw hex code for the two failure states, instead of one flat "no audio
captured" line. Also caught and fixed a real bug while wiring this up: two of
the three "nothing to show" paths in `AudioMeterBridge.cpp`'s capture loop
published a default-constructed `AudioMeterLevels`, whose status defaults to
`ResolveNotFound` - meaning an *already-open, healthy* stream that just had
no packets for a beat, or had just delivered a real (possibly silent)
packet, would have both reported "Resolve isn't running" the instant this
status field started being read, even while actively capturing.

**A likely real cause worth ruling out first, before reading anything into
whatever the new message says**: process-loopback capture taps Resolve's
audio at the Windows audio engine/session level. If Resolve's Fairlight audio
output is configured for an ASIO device (common with a dedicated audio
interface on a colorist workstation), ASIO talks to that hardware directly
and bypasses the Windows audio engine entirely - there would be nothing for
WASAPI-based capture, process-specific or otherwise, to see, and no
diagnostic message can distinguish that from Resolve legitimately being
silent. Switching Resolve's audio output to a standard WASAPI/DirectSound
device would confirm or rule this out.

## Redesigned: Audio Meter's colour zones are now always visible, not just once already too loud

Reported after the first cut of the Audio Meter panel (see below): the
green/yellow/red distinction only showed up once the signal actually crossed
into a zone - a quiet signal just read as a plain green bar, with no visual
cue for where the danger zones even were until the level got there. Redesigned
`DrawAudioMeterPanel` to paint dim green/yellow/red bands across the *whole*
scale, always, the same way a hardware meter's face is marked - -60 to -6dB
dim green, -6 to -3 dim yellow, -3 to 0 dim red - and the actual level lights
up those same zones at full brightness as it climbs through them (green from
the floor, then yellow past -6, then red past -3), rather than the whole
filled bar switching to one flat colour picked by wherever the instant level
happens to sit.

**What the meter is based on**: WASAPI loopback capture of the system's
current default *output* device - see "Audio Meter panel" below for the full
research trail (Resolve has no OFX audio suite and no scripting call for live
levels; this is the same "capture your computer's audio output directly - no
cables required" approach Nobe OmniScope itself offers). It reads whatever the
OS is actually sending to speakers/headphones right now, not a tap on
Resolve's own internal audio bus specifically - accurate to what you'd hear
through that device, but it'll also show any other app's sound if something
else on the machine is making noise, and won't reflect Resolve's routing at
all if its output isn't going through the system's current default device.

## Added: launch maximized

The window opened at a fixed 1600x900 every launch, regardless of what
"Remember Layout and Settings on Close" restored for the panels inside it -
window size/position live in GLFW, not ImGui's .ini, so nothing was pulling
that from anywhere. Fixed with `glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE)`
before `glfwCreateWindow` - still a real bordered OS window, just starting at
the size someone would otherwise reach by clicking maximize once, so the
taskbar, Alt-Tab, and "Open in New Window" popouts all keep behaving exactly
as they already do.

## Ported: Layout Presets (General menu)

The old app's "General > Layout Presets" menu (`_serialize_layout`/`_apply_layout`/
`_save_preset`/`_load_preset`/`_delete_preset` in `scope_deck.py`) saved a named
snapshot of "which content sits where, at what splitter sizes, with which
per-panel settings" as its own hand-rolled dict in `settings.json`, restored
by tearing down and rebuilding every row/panel widget from scratch.

This build already has a much more capable version of that same
serialization sitting right there for free: the settings handler above
(`CollectSettingFields`/`ScopeDeckSettings_WriteAll`/`ReadLine`) writes the
entire panel roster and every setting into the same .ini ImGui itself uses
for dock structure and window positions - "Remember Layout and Settings on
Close" is exactly this, autosaved under one fixed name. A preset is the same
data, saved under a name the user picks instead: `SaveLayoutPreset` calls
`ImGui::SaveIniSettingsToDisk` with a per-preset filename under
`%LOCALAPPDATA%\ScopeDeck\presets\`, and `ApplyPendingLoadPreset` calls
`ImGui::LoadIniSettingsFromDisk` - the exact function ImGui itself calls to
read layout.ini at startup - which rebuilds the dock tree, every window's
position (including a popped-out one's own OS window), and this app's own
`[ScopeDeck][State]` section (panel roster + settings) all in one call,
instead of a second serialization format to keep in sync with the first.

Reusing that machinery meant the fields were free, but the load path still
needed to be deferred to the same point in the frame as `ApplyPendingAdd`/
`ApplyPendingClose` (`DockBuilderFinish`/dock-tree rebuild is not safe from
inside the menu item that requested it, mid-submission) - so `General >
Layout Presets > <name>` just queues `App::pendingLoadPreset`, and
`ApplyPendingLoadPreset` runs it after every panel for the frame has already
been submitted. Save and Delete touch no ImGui state at all (a straight
file write/remove), so those run directly from their own popups
(`DrawLayoutPresetDialogs`) instead of being deferred.

## Fixed: two more bugs in a popped-out window - Add Section jumped away, and the OS "X" left panels behind

Reported after trying the previous fix: "Add Section" on a popped-out window
now grew the right window, but the whole thing jumped to a different screen
location when it happened; separately, the native Windows "X" on a popped-out
window did not close the entire thing.

**Add Section jumping**: the fix below created the standalone window's new
floating dock node with `DockBuilderSetNodeSize` but never
`DockBuilderSetNodePos` - the node defaulted to position (0, 0) instead of
where the window actually was, so docking the existing window into it moved
it there. Fixed by also calling `DockBuilderSetNodePos(targetNode,
fromWindow->Pos)` before docking, so the new node starts exactly where the
window already was and nothing visibly moves.

**OS "X" leaving panels behind**: `PlatformRequestClose` (imgui.cpp) applies
"to all windows in this viewport" in a single pass - every panel sharing that
platform window gets `*p_open = false` set in the same frame. But
`App::closePanelId` was a single `int`, so when a popped-out window held more
than one panel (which Add Section now makes possible), each panel's close
request overwrote the last, leaving only one of them actually queued for
removal - the rest stayed open, so the window itself never fully closed even
though its native titlebar had already vanished. Fixed by making it
`std::vector<int> closePanelIds` and having `ApplyPendingClose` drain and
process every id queued that frame instead of just the last one written.

## Fixed: Add Section on a popped-out window dropped it behind the main window

Reported after the position-jump/OS-"X" fix above: Add Section on a popped-out
window now grew and stayed put, but the whole window would drop behind the
main one the instant the section was added - not closed, just buried.

Root cause is the node structure change itself: before the split, the
standalone window owns its platform viewport directly (one window, one node,
no separate container needed). Splitting that node into two children means
the pair now needs a shared hidden "host" window to carry the viewport
instead of either child owning it outright - the same kind of host window the
main dockspace has always used - and that host window gets created as part of
the docking churn `DockBuilderFinish` and the next frame's docking requests
queue up. It doesn't reliably inherit being front-most the way the window it
replaced already was.

Fixed with an explicit `ImGui::FocusWindow(fromWindow)` right after
`DockBuilderFinish`, whenever the split happened on a standalone window's
freshly built node (`finishRoot != p_DockspaceId`). ImGui syncs the OS
window's front-most state (`Platform_SetWindowFocus`) whenever the focused
window's viewport changes mid-frame, so re-asserting focus on the window the
user was actively adding a section to is what pulls the whole thing back in
front of the main window again.

## Fixed: "Add Section" on a popped-out window added it to the main window instead

Reported next: once a panel was genuinely popped into its own OS window (see
the auto-merge fix below), "Add Section" on it did not grow that window into
its own multi-panel layout - it silently added the new section back in the
*main* window instead. Root cause in `ApplyPendingAdd` (`main.cpp`): it splits
`fromWindow->DockId`, the dock node the source panel already belongs to -
which is exactly right for a panel still part of the main dockspace, but a
genuinely standalone window has no dock node of its own at all (`DockId == 0`),
and the code's fallback for that case was the main dockspace's own ID.

Fixed by giving a standalone window a dock node of its own the moment it
needs one: `DockBuilderAddNode(0, ImGuiDockNodeFlags_None)` (a *floating*
node - "carries its own window" rather than being attached to one, per
`DockBuilderDockWindow`'s own comment in `imgui_internal.h`) then
`DockBuilderDockWindow` the existing window into it - the same bootstrap
sequence ImGui's own docking demo uses to start a fresh dockspace, and it
stays in the same OS window/viewport throughout since docking a window into
a node never moves it to a different viewport on its own. That node is then
split exactly like the main-dockspace case already was. `DockBuilderFinish`
now targets whichever root actually needs walking - the standalone window's
freshly-built node, not the unrelated main dockspace - since the two are
now genuinely separate hierarchies.

Once a popped-out window has been grown this way, it behaves like a second,
independent dockspace: further Add Section calls on any panel inside it walk
its own `DockId` normally (the `== 0` fallback only ever fires once, for the
first split), and panels can be freely rearranged within it exactly like the
main window - which is what "build two complete UI setups across monitors"
actually requires.

## Fixed: "Open in New Window" never actually left the main OS window

The previous "borderless window" fix (below) was itself only a partial
diagnosis. Confirmed with a screen recording: clicking "Open in New Window"
did not create a second OS window at all - the panel undocked, then ImGui's
viewport **auto-merge** immediately reabsorbed it back into rendering as
part of the main window, because a just-undocked window starts out exactly
where it was docked, which always overlaps the main viewport. The result
looked like a broken, headerless floating pane drifting over the other
panels, still inside the one "Scope Deck" OS window the whole time (one
titlebar, one taskbar entry) - never a second window a user could drag to
another monitor.

Fixed with `io.ConfigViewportsNoAutoMerge = true` (`main.cpp`, next to the
decoration flag below) - ImGui's own default re-merges any floating window
overlapping the main viewport; this makes every undocked window unconditionally
its own genuine OS window regardless of where it happens to sit on screen,
which is what "build two complete UI setups across monitors" actually needs -
drag it wherever after that, like any other window.

Also fixed in the same pass: the native window's own close button (the OS
"X", or Alt-F4) did nothing, reported separately by the user. Root cause:
`ImGui::Begin(name, nullptr, flags)` passed `nullptr` for `p_open`, and ImGui
only acts on a platform window's close request (`PlatformRequestClose`) when
`p_open` is non-null - passing `nullptr`, as this did, is a documented way to
tell ImGui "there is no close button, don't touch this window's lifetime,"
which was never the intent here. Now passes a real bool and routes a false
result into the same `closePanelId` the in-canvas close button already
uses, so both close a panel the same way (and both respect the same "can't
close the last panel" guard in `ApplyPendingClose`). One side effect worth
knowing: since `p_open` is now non-null, ImGui also draws its own small close
"x" on a *docked* panel's tab (previously absent) - redundant with the
in-canvas close button, cosmetic only, not fixed here since it wasn't part
of what broke.

## Fixed: "Open in New Window" opened a borderless, chrome-less window

Undocking did create a genuine second OS window (`ImGuiConfigFlags_ViewportsEnable`
was already on) - it just did not *look* like one, because ImGui's own
default (`io.ConfigViewportsNoDecoration = true`) leaves secondary viewports
borderless with nothing but ImGui's own small in-canvas titlebar for chrome:
no native titlebar, no minimise/maximise, nothing that reads as a real OS
window at a glance. Reported with a screenshot of what a normal window's
titlebar looks like, for comparison.

Fixed with two changes in `main.cpp`: `io.ConfigViewportsNoDecoration = false`
(so a secondary viewport gets real native decoration), plus
`ImGuiWindowFlags_NoTitleBar` on every panel's own `Begin()` call (so ImGui
does not *also* draw its own redundant titlebar underneath the native one -
irrelevant while docked, where the dock tab bar is shown either way
regardless of this flag). Both are plain ImGui/GLFW config, not Windows-
specific code, so this carries over to a macOS build with no changes needed
there - native titlebars are exactly as much "just turn decoration back on"
on Cocoa as on Win32, through the same vendored GLFW backend.

## Implemented: "Open in New Window"

Was a permanently-disabled stub (`ImGui::MenuItem("Open in New Window", nullptr, false, false)`)
since Phase 1 - multi-viewport was already on unconditionally (`ConfigFlags_ViewportsEnable`
set in `main()`), so dragging a tab out already worked; the menu item just
never called anything. Wired up via `ImGui::DockContextQueueUndockWindow` -
the queued request the docking branch's own drag-to-undock gesture posts
internally, not the immediate `DockContextProcessUndockWindow`, since a dock
structural change isn't safe from inside the window it changes mid-submission
(the same reason Add Section/Close Section go through `ApplyPendingAdd`/
`ApplyPendingClose` instead of acting immediately). Disabled when the panel
is already its own floating window, so it can't be clicked as a no-op.

## Added: "Space Brings Resolve to Focus" (Preferences) - not real transport control

Investigated whether this app could play/pause Resolve's own timeline
directly, prompted by the user recalling the old Python version tried and
failed at this. Confirmed via `Scope Deck (old)/ERRORS.md` ("Spacebar from
the app cannot drive Resolve's transport (two ways tried)"): Resolve's
scripting API has no play/pause/transport method at all, in any language -
this is a Resolve-API-level wall, not a Python-vs-C++ one. The two OS-level
workarounds tried before both failed for reasons that apply regardless of
language: `PostMessage`-ing a key to Resolve's window was silently dropped
(Qt only routes key events to the focused widget of the truly *active*
window), and `SendInput` after forcing it to the foreground returned 0
(likely blocked by UIPI, since Resolve typically runs at a different
integrity level than this app).

What shipped instead, once the user rescoped the ask: `spaceFocusesResolve`
(off by default) switches focus to Resolve's own window on Space, so the
user's *next* real Space press lands there and actually toggles play/pause -
one extra keystroke instead of alt-tabbing by hand. `FocusResolveWindow` in
`main.cpp` finds Resolve by its owning process name (`Resolve.exe`), not
window title - matching by title false-positived on an unrelated Explorer
window the one time this was tried before, per the same ERRORS.md writeup.
No key is injected into Resolve at all; this only changes which window is in
front, which is the one part of the old attempts that was never the problem.

## The OFX plugin now builds from this tree, not "Scope Deck (old)"

Migrated in, not just referenced: `plugin/ScopeTap.{h,cpp}` and the write
side of the wire format, `core/ScopeCore.{h,cpp}` (the reduction engine) and
`core/ScopeShm.{h,cpp}` (the shared-memory publisher), plus
`tools/ScopeSelfTest.cpp` (drives the reduction engine with synthetic frames
with no Resolve running). `core/ScopeTypes.h` was already a byte-identical
copy of the old tree's own (confirmed with `diff` before deleting nothing) -
one file now, not two that only a comment promised would stay in sync.

`CMakeLists.txt` gained `scopecore_writer` (a second, separate library from
the existing `scopecore` - the app never needs OFX or the reduction engine,
and the plugin never needs `ScopeReader`, so there is no one library that
serves both without exposing the wrong half to the wrong consumer), a
`scope_selftest` target, and `ScopeTap` itself (an OFX `MODULE` target
against Resolve's OpenFX SDK, packaged into `build/bundle/ScopeTap.ofx.bundle`
exactly as the old tree's own CMakeLists.txt did it - that packaging step
was copied verbatim, not redesigned). `build.ps1` gained a `-Deploy` flag
doing the same elevated copy into `C:\Program Files\Common Files\OFX\Plugins`
the old tree's own script did.

Not deleted from "Scope Deck (old)": its own `core/`, `plugin/`, and
`tools/ScopeSelfTest.cpp` are now redundant duplicates, kept there
deliberately until the old app itself is fully ported and that whole tree
is ready to remove in one pass, rather than dismantling it piecemeal.
`tools/*.py` in the old tree (GPU parity/mask-interaction/waveform-trace
checks) were **not** migrated - those test the old Python app's own Qt/GPU
internals specifically, not the wire format, and have no equivalent to
migrate to.

## OFX plugin's "Open Scope Deck" button now launches this app

The plugin (`Scope Deck (old)/plugin/ScopeTap.cpp`, `changedParam`'s
`kParamOpenApp` handler) used to shell out to `python scope_deck.py`,
resolved from a `SCOPE_DECK_APP` env var or a `%APPDATA%\Scope Deck\app\`
default. It now launches `scopedeck.exe` directly (no interpreter needed),
defaulting to `%LOCALAPPDATA%\ScopeDeck\scopedeck.exe` - the same folder
this app's own settings file already lives in - with `SCOPE_DECK_APP` still
available as an override (handy at dev time to point straight at
`Scope Deck C++/build/scopedeck.exe`). The launched process's working
directory is set to the exe's own folder rather than left as Resolve's,
so `timecode_poll_worker.py` (which must sit alongside `scopedeck.exe` -
see the CMake post-build copy step) resolves correctly regardless of what
launched it. The macOS half of that change is unverified - mirrored from
the Windows logic, but there is no macOS build of this app to check it
against yet.

Deploying the rebuilt plugin (`build.ps1 -Deploy` in `Scope Deck (old)`)
into `C:\Program Files\Common Files\OFX\Plugins` still has to be done by
hand, with Resolve closed - not something to do without the user present.

## Enhanced Render: every scope defaults on, plus a global kill switch

Waveform already defaulted to Enhanced Render on; Vectorscope and White
Balance now do too, per user request - there is no longer a scope that opens
in the binned/density trace by default. `Preferences::disableEnhancedRender`
(off by default) is a single checkbox ("Disable Enhanced Render", under
Rendering) that overrides all three panels' own Enhanced Render checkboxes
at once, for a low-power machine, without hunting down every panel
individually - it does not change what each panel's own setting says, only
whether that setting is allowed to draw anything for the session. The
`pauseEnhancedDuringPlayback` preference removed earlier was a different,
narrower idea (drop to the binned trace only while the timeline is moving);
this is a plain, unconditional off switch.

## Fixed: zoom-to-cursor was wrong on every image-fit panel

`ZoomPan::HandleInput`'s cursor-anchor math and `ZoomPan::Apply` have to be
called against the *same* rectangle, or the anchor is solved for a rect Apply
never draws into. Source, White Balance, False Color, Skin Tone, Noise, Focus
Peaking, Banding Analyzer and A/B Difference were all calling `HandleInput`
against the raw panel rect (`avail`) but `Apply` against `FitAspect(avail, ...)`
- a different, letterboxed/pillarboxed rect whenever the content's aspect ratio
doesn't match the panel's own. White Balance's 1:2 combined image made this the
most visible (reported by the user), but the same bug was latent in every panel
listed above. Fixed by computing the unzoomed `FitAspect` rect once per panel
and feeding that same `fitBase` to both calls. Waveform, Histogram, Vectorscope
and Grain Analyzer already did this correctly (they zoom the panel/plot rect
directly, no separate image-fit step) and needed no change.

## Fixed: Timecode panel had no close button or right-click menu

`DrawTimecodePanel` never set `PanelCommon::panelRect`, which every other
panel sets every frame - `BeginPanelContextMenu` tests the mouse against
whatever `panelRect` currently holds, so a panel that never sets it tests
against the struct's zero-initialised default, a rect no click can ever land
inside. Same root cause took out the close button, which just needed the
usual `DrawPanelCloseButton` call this panel never made. Fixed by giving it
the same `PlotRect()`/`DrawPanelBackground`/`panelRect`/`DrawPanelCloseButton`
boilerplate every other panel already has.

## Fixed (partially): Timecode not connecting on a clean machine

`TimecodeBridge.cpp` shelled out to a bare `python` command to run
`timecode_poll_worker.py` against Resolve's scripting API. On a machine with
no Python installed, `python` on Windows commonly resolves to the Microsoft
Store's app-execution-alias stub, which - run head-less the way this bridge
does - just exits with no output, indistinguishable from "Resolve isn't open
yet". Fixed by probing a short list of common launchers once at startup
(`py -3`, `py`, `python3`, `python`, in that order - `py` is what the
official python.org installer registers, and is a more reliable signal than
`python` that a real interpreter exists) and caching whichever one actually
works. When none do, the panel now reads "No Python found" instead of the
same blank dashes "Resolve not running" shows, so the two failure modes read
as different problems.

This does not remove the Python dependency itself - Resolve's scripting API
has no supported way to reach from plain C++ without either a Python (or
Lua) interpreter or reverse-engineering `fusionscript`'s native ABI, neither
of which this pass attempted. A user without Python installed still won't
get a live timecode; they'll now at least get a message that says why.

## Fixed: Timecode read from 00:00:00:00 instead of Resolve's own start timecode

`ScopeFrame::timelineTime` is the OFX effect time, which counts frames from
0 at the timeline's start - not what Resolve displays, which starts from the
project's own Start Timecode (01:00:00:00 by default, but a per-project
setting, not a universal constant). The panel was converting `timelineTime`
straight to h:m:s:f with no offset, so it read 00:00:00:00-based instead of
matching Resolve's own readout - reported after the fact by comparing
against a real Resolve session.

Fixed by having the poll thread derive the real offset from Resolve itself
rather than assuming +1 hour: `timecode_poll_worker.py` already returns
`"frame"` (Resolve's own frame count, from its `GetCurrentTimecode()` via
the same `_timecode_to_frame` conversion `SubtitleBridge` uses); the C++ side
now snapshots `ScopeFrame::timelineTime` right before each poll
(`TimecodeBridgeNotifyFrame`, called every render frame from `main.cpp`) and
stores `frameOffset = workerFrame - timelineTimeAtPoll`. Every
`TimecodeBridgeGetText` call (once per render frame, for a smooth per-frame
readout between the ~2-second polls) adds that offset before doing the
h:m:s:f math, so the displayed timecode matches Resolve's own regardless of
what that project's actual start timecode is set to.

## Audited: no user-specific paths or PII in the shipped app

Checked the C++ source tree (`app/`, `core/`, `tools/`, `CMakeLists.txt`,
`build.ps1`) and the compiled `scopedeck.exe` for hardcoded absolute paths,
usernames, or machine-specific strings. Found none - the only hits were in
`build/CMakeCache.txt` and other CMake-generated build metadata (not part of
the shipped executable, regenerated fresh by `cmake -S -B` on any machine).
Settings persistence already resolves a per-user path at runtime
(`%LOCALAPPDATA%\ScopeDeck\layout.ini`, see `main.cpp`), not something baked
in. The Resolve scripting environment variables `timecode_poll_worker.py`
sets (`RESOLVE_SCRIPT_API`/`RESOLVE_SCRIPT_LIB`) are Blackmagic's own
documented install locations, the same on every machine - not user-specific.

## Added: Compare panel, and A/B Difference gained still upload too

Not a port of anything in the old app - a new feature, designed from a
thought-dump rather than a spec, so the shape below is a proposal that was
confirmed with the user before building, not an assumption.

**Stills are now a shared concept.** `StillImage.h/.cpp` factors "a picture,
either captured from Source or loaded from disk" out of A/B Difference (which
used to only support capturing) into its own module: `CaptureStillFromFrame`,
`LoadStillFromFile` (decodes via the newly-vendored `stb_image.h` - PNG/JPEG/
BMP/GIF/etc., forced to RGB8), and `OpenStillFileDialog` (a native Win32
`GetOpenFileNameW`). A/B Difference's own `DifferenceState` now holds a
`StillImage` instead of its own `hasRef`/`refPixels`/`refWidth`/`refHeight`,
and its Settings gained "Load Still..." alongside "Capture Reference Still"
(both now behind the shared `DrawStillCaptureRow`, so the two panels' buttons
cannot drift apart).

**Compare is a new panel kind**, not a mode of Difference - a still viewer
with an optional scope overlay (Waveform/Vectorscope/Histogram), rather than
overloading Difference's pixel-diff-math model with a second purpose. The
overlay is computed from the *still's own pixels*, not the live signal -
`BuildScopeBinsFromRGB` (extracted from `BuildMaskedScopeBins`'s per-pixel
reduction, now shared by both the masked-live-scope path and this) runs once
per still (cached via `StillImage::generation`, bumped on every capture/
load - a still never changes on its own, so re-deriving its bins every frame
the way a live signal has to would be pure waste) and the result is drawn as
a translucent HUD over the still, each overlay kept at its own natural
aspect (`FitAspect`) rather than stretched to the still's.

Deliberately does **not** try to show the live signal and the still's scope
in one panel - to compare against the current source, place an ordinary live
Waveform/Vectorscope/Histogram panel next to Compare and compare by eye. That
was one of two shapes considered (the other being a dual-signal split/blend
view) and confirmed with the user as the simpler, more consistent-with-the-
rest-of-the-app one before implementation started.

## Added: app icon (Windows)

Source art is `app/icon/scopedeck_icon.png` (1080x1080, palette PNG with a
1-bit transparent background). `tools/make_icon.py` - dev-time only, needs
Pillow - downsamples it to a committed `app/icon/scopedeck.ico` at 16, 20, 24,
32, 40, 48, 64 and 256px, so building never runs Python.

`app/icon/scopedeck.rc` names the resource **`GLFW_ICON`**, which is what
GLFW's Win32 backend looks up when it registers its window class
(`vendor/glfw/src/win32_window.c`). That one resource covers the exe in
Explorer, the taskbar, Alt-Tab, and every ImGui viewport opened as its own
window - no `glfwSetWindowIcon` call, no runtime code. Verified by loading
`GLFW_ICON` from the built exe at 16/24/32/48/256px, the same `LoadImageW`
call GLFW makes.

**macOS still needs its own icon.** `glfwSetWindowIcon` is a no-op on Cocoa and
the `.rc` is Windows-only; a Mac build needs an `.icns` (built from the same PNG
with `iconutil`) set as the app bundle's `CFBundleIconFile` /
`MACOSX_BUNDLE_ICON_FILE`.

## Panels

| Feature | Status | Notes |
|---|---|---|
| Source | Ported | Spatial mask editing included, and now actually restricts Waveform/Histogram/Vectorscope/White Balance, not just the preview-tier panels' dimming - see "Spatial mask now affects the scopes" below. No qualifier tie-in (see Histogram Qualifier below). |
| Waveform | Ported | Luma/Parade/YRGB, Range, Scale, Enhanced Render, Colorize - full parity. |
| Histogram | Partial | Channel modes ported. Missing: drag-to-qualify value-range selection per lane. |
| Vectorscope | Ported | Low/Mid/High/All/3-Stack, zoom, colorize, targets, Enhanced Render. |
| White Balance | Ported | "Twin Peaks", incl. Enhanced Render - the beam trace maps to (diff, level) per diamond instead of (Cb, Cr), reusing the Vectorscope's own treatment (`DrawWhiteBalanceEnhanced` in `main.cpp`, shares `TwinPeaksToPlot` with the density graticule). Masked too, via the shared `MaskedScopeBins::waveformTrace`. |
| False Color | Partial | Exposure-zone mapping ported (5 modes). Verify against old README for any algorithm-choice options beyond mode selection. |
| Skin Tone | Partial | Model/Color Mode/Tolerance/Limits/Grey-non-skin all present; unverified against old app's exact behavior in edge cases. |
| Noise | Ported | High-pass / Single channel / Chroma B-G. |
| Timecode | Partial | Stub vs. old app's full TimecodeBridge/SubtitleBridge integration. |
| Focus Peaking | Ported | Threshold, Grayscale BG, Peaking Color (Red/Cyan/Yellow/Green). |
| Banding Analyzer | Ported | Gain only, matches old app. |
| A/B Difference | Ported, extended | Gain + Capture/Load/Clear reference still (loading from disk is new, not in the old app - see "Compare panel" above). The still is live-captured/loaded pixel data, deliberately not persisted (matches old app's own reasoning for its capture-only version). |
| Compare | New (not in old app) | Still viewer (capture or load) with an optional Waveform/Vectorscope/Histogram overlay computed from the still's own pixels - see "Compare panel" above for the full design rationale. |
| Audio Meter | New (not in old app) | Per-channel peak/RMS dBFS bars with peak-hold and always-visible colour zones, fed by Windows' per-process loopback capture targeting Resolve's own process specifically (not the whole system's output) - see "Audio Meter panel" and "Audio Meter now captures Resolve specifically" above. Windows-only. Not yet verified against real playback. |
| Chromaticity (CIE x,y) | Ported | Spectral locus, blackbody curve, 3 gamut triangles + white point, density scatter (Gain/Colorize), per-gamut "% outside" readout, cursor readout, plus **Enhanced Render** - an exact per-pixel beam trace (`BuildChromaticityTrace`/`DrawChromaticityEnhanced`) that reads preview pixels directly rather than the shared `waveform_trace` mixin, since CIE xy is a genuine division and averaging RGB first (the way Vectorscope/White Balance's Enhanced Render can, being linear) would reintroduce real error - same reasoning Sat-vs-Luma's beam trace uses. Includes the same chroma-jump run-break Vectorscope/White Balance needed for the identical isolated-highlight sunburst risk, though the break-distance threshold is a reasoned default (25% of the plot's diagonal span) rather than measured fresh against real footage the way theirs was. The density path is not spatial-mask/qualifier-aware (matching the old app's own `_ensure_analysis` exactly); Enhanced Render *is* mask/qualifier-aware (matching the old app's own `_samples`). |
| Luma vs Sat (renamed from Sat vs Luma) | Ported (redesigned twice) | First pass was a 2D density plot of real saturation vs. luma; Trevor didn't like it (nor the matching Sat vs Hue pass) and asked for DaVinci Resolve's actual "Curves" look instead. Now a black->white luma-gradient background with a plain luma population histogram drawn over it - reusing the app's own existing full-precision Y-plane histogram (`ScopeFrame::histogram`/`App::maskedScope.histogram`, same data the Histogram panel reads) rather than a fresh per-pixel pass, since that data already exists every frame. Two-tone (dark+light) stroke for the outline/labels, needed here specifically because this background reaches genuine white at its own right edge (unlike Sat vs Hue's dimmed wheel). No longer plots saturation at all - see Sat vs Hue's entry below for why. |
| Hue vs Sat (renamed from Sat vs Hue) | Ported (redesigned again) | Trevor didn't like the initial density-plot redesign and asked for the actual DaVinci Resolve "Curves" look instead: a dimmed hue-wheel background (`BuildHueGradientImage`) with a plain 1D hue population histogram drawn over it (`BuildHueHistogramAnalysis`), using the exact same line+trapezoid-fill technique `DrawHistogramLane` already uses for the Histogram panel - not the 2D density plot Sat vs Luma still uses. `RgbToSatLumaHue` (HSV-style max-min chroma - a third distinct hue/sat formula in this app alongside `HslHueSatLight` and Skin Tone's own Cb/Cr-angle model) still supplies the hue value; saturation is only a >2% gate now (a near-neutral pixel has no meaningful hue), not a plotted quantity. Old app's Polar/Wheel style variant not ported - Rectangular only. |
| Timetrace | Skipped | Deliberately not being ported. |
| Grain Analyzer | Ported (redesigned) | Deliberately *not* a port of the old app's 2D FFT (`np.fft.fft2` + radial average / spectrum image) - that was a second-hand read even in the old app, and porting an FFT for one panel was a lot of machinery for what it bought. Reimplemented as a "grain size" curve from a box-blur pyramid instead (`BuildGrainAnalyzerProfile` in ScopeImages.h/.cpp): each band is the RMS difference between the image blurred at one radius and the next. Answers "what size is the grain" directly instead of by way of spatial frequency, and reuses no FFT dependency. The old app's "2D FFT Spectrum" image style was dropped entirely - Noise already shows *where* grain/noise is visually; this shows *what size* it is. Revised after user feedback that clips hardly differed: now 9 bands on a gentler radius progression (was 6, doubling each step) for a less blocky curve, and the profile is raw RMS energy rather than self-normalised to its own peak band - the old normalisation forced every clip's tallest band to read 1.0 regardless of how much grain was actually there, which is exactly why two very-differently-grainy clips used to plot as near-identical shapes. Gain (now defaulting to 25x, up from 1x, range widened to 1-100) is what actually brings that raw energy into a visible range, and is a first calibration, not a measured constant. |
| Video panel (dedicated preview w/ Composition Grid, Overlay Scope, Overlay Opacity) | Missing | Distinct from the basic Source preview panel that exists today. ~750 lines old code. |
| Color Cube (3D RGB/XYZ/Lab plot) | Ported (redesigned) | Real camera orbit (middle-drag/Shift+middle-drag/wheel/Z), Spread and Density controls, all three colour spaces sharing one camera scale - see "Ported: Color Cube" above for the two deliberate rendering redesigns (ImDrawList points instead of a CPU-rasterised texture blit; an evenly-strided subsample instead of a true random one) and the Lab-neutral-axis correctness note. New `SrgbToXyz`/`XyzToLab` in ScopeImages.h/.cpp also unblocks Chromaticity below, which needs the same XYZ step. Verified visually against a synthetic feed; not yet interactively (this environment cannot drive mouse orbit/pan/zoom itself). **2026-09-18 fix:** orbit camera was two independently-accumulated yaw/pitch Euler totals reapplied in a fixed order every frame, which only matches "horizontal drag = yaw about the screen's vertical axis" intuition when pitch is exactly 0 - never true at the default view or after "Z" resets (pitch -20). Trevor reported this as "have to rotate it fully for my inputs to make sense." Replaced with a real incremental trackball: `ColorCubeState::rotation` is now a full 3x3 matrix, and each drag step composes a small rotation about the camera's fixed screen axes onto it (`Mat3RotX/Y/Mul/Orthonormalize` in `main.cpp`), matching intuitive screen-relative dragging at every orientation, not just near pitch=0. |
| Stills panel | Missing | ~141 lines old code. |
| Grid View | Missing | Explicitly flagged as not built in the Preferences UI. ~370 lines old code (GridViewWidget + RefSlotWidget + ScalableImageWidget). |
| Premiere Pro input | New (not in old app) | `plugin/ScopeTransmit.cpp`, a Premiere Transmit plugin (Preferences > Playback > Video Device) running the same `ScopeEngine` and publishing to its own block (`kShmNamePremiere`), so the app offers Resolve and Premiere as separate inputs. Preview Scale / Row Step live in the app's Input menu and reach the plugin through `core/ScopeControl.h`, which also carries the sequence's timecode rate, drop-frame and start back to the app - so Premiere timecode needs no Python bridge. The Audio Meter taps Premiere's process when it is the input. Measured 2026-09-24 at 1080p23.976: every frame delivered, ~0.6 ms of Premiere's time per frame, ~10 ms of scopes on the plugin's own thread. Built only where Adobe's SDK is present (never committed); releases package `prebuilt/ScopeTransmit.prm`, checked against a fingerprint of its sources - see release.ps1. |
| Screen Capture input | Ported, extended | `app/ScreenCapture.h/.cpp`. Input -> Screen Capture opens a snipping-tool style picker (dimmed screenshot of every monitor, drag a rectangle, Esc/right-click cancels) and captures that region on a background thread, targeting 60 Hz. Unlike the old app's Python port of the reduction, the pixels go through the tap's own `ScopeEngine` (compiled into the app) and come out as an ordinary `ScopeFrame`, so every panel reads identical data whichever input is active. Also unlike the old app: no pixel budget/subsampling, and a region may span monitors. Timecode and subtitles are off while capturing (they read Resolve's timeline). The region is remembered for the session only. **Measured 2026-09-24** (GDI `BitBlt`): the preview is byte-identical to a direct grab of the same region; 1920x1080 runs at ~34 fps with the grab 24 ms and the scopes 9 ms, 640x360 at the full 60. The grab (a GPU readback) is the limit - DXGI Desktop Duplication is the known way past it. Picker not yet exercised interactively. |

## Spatial mask now affects the scopes

Previously the spatial mask (drawn on the Source panel) only dimmed the
preview-tier panels (Source, False Color, Skin Tone) - it had zero effect on
Waveform/Histogram/Vectorscope/White Balance, because those come from the
tap's own pre-binned wire-format arrays, computed over the full-resolution
frame before the app ever sees it. There is no way to restrict bins the tap
already summed without changing the OFX plugin's wire format.

Fixed the same way the old Python app did it (its own qualifier system
independently re-derived masked bins app-side rather than asking the plugin
to re-bin - see `analyse_rgb_frame` in `Scope Deck (old)/app/scope_format.py`):
`BuildMaskedScopeBins` (`app/ScopeImages.h/.cpp`) re-bins Waveform/Histogram/
Vectorscope/White Balance from scratch, over the downscaled preview pixels the
app already has, counting only the pixels the mask keeps (weight >= 0.5,
matching the old app's own qualifier threshold). Cached once per frame in
`App::maskedScope` (`EnsureMaskedScopeBins` in `main.cpp`), recomputed when the
frame or the mask geometry changes, and read by all four panels via optional
`p_CountsOverride`/`p_ReferenceOverride` parameters on
`BuildWaveformComposite`/`BuildVectorscopeImage`/`BuildWhiteBalanceImage` and
`DrawHistogramLane`.

Enhanced Render (Waveform and Vectorscope's line-trace mode) is masked too,
via `BuildMaskedWaveformTrace` (`ScopeImages.h/.cpp`): the same per-scanline
sampling `BuildWaveformTrace` (the tap's own, unmasked) uses, but read from
the preview and restricted to the mask the same way the binned path is. One
shared buffer (`MaskedScopeBins::waveformTrace`) covers both scopes, since
they read the same underlying trace array. `PrepareEnhancedTrace` takes an
optional trace-override parameter for this, same pattern as the binned
`p_CountsOverride` parameters.

Known limitations, both inherited from the old app's identical tradeoff:
- **Precision**: a masked readout is only as precise as the preview image
  (8-bit, downscaled, clipped to [0,1]) - it loses the tap's super-black/
  super-white headroom and full source resolution that the *unmasked* bins
  have. Real, but the same cost the old app accepted for the same reason.
- An empty-result mask (e.g. an inverted mask that swallows the whole frame)
  shows a small "Spatial mask matches no pixels" notice on the affected
  panel (`DrawMaskEmptyNotice` in `main.cpp`), driven by `MaskedScopeBins::empty`.

## Cross-cutting systems

| System | Status | Notes |
|---|---|---|
| Histogram Qualifier (drag-to-select value range per channel, masks Waveform/Histogram/Vectorscope/White Balance/Source/False Color/Skin Tone) | Ported | Click-drag on a Histogram lane (Luma/YRGB only, not Stacked); per-plane ranges combine across every open Histogram panel (`RebuildValueQualifier`, `ChannelQualifier`). |
| Qualifier panel (dedicated Hue/Saturation/Luminance range + Invert) | Ported | New `PanelKind::Qualifier` (`DrawQualifierPanel`); global state (`App::valueQualifier.hsl`), ANDed with the Histogram's per-channel ranges via `ValueQualifierPasses`/`CombinedMaskWeight` in `ScopeImages.h`. Neither qualifier persists across restarts (same as spatialMask - live grading state, not a standing preference). |
| GPU Acceleration toggle (preview panels: Video/FalseColor/SkinTone/Noise) | Missing | Preferences shows the control but disabled. Preview panels are CPU-only (ScopeImages.cpp). Old app's GPU surfaces crashed the NVIDIA driver (QOpenGLWidget) - see the old app's notes before reattempting. |
| Goniometer/Lissajous (stereo width, L-R vs L+R) | New (not in old app) | `PanelKind::Goniometer`, reads `AudioMeterBridge`'s new raw sample ring buffer (`AudioRingSnapshot`) directly - the level meter's own peak/RMS have already discarded the waveform. |
| Real-Time Spectrum Analyzer (20Hz-20kHz) | New (not in old app) | `PanelKind::SpectrumAnalyzer`; new `AudioAnalysis.h/.cpp` (radix-2 FFT + Hann-windowed magnitude spectrum) - no FFT existed anywhere in this codebase before (Grain Analyzer was deliberately built *without* one). 4096-sample window, log-scaled frequency axis, 50/60Hz hum landmarks called out explicitly. |
| True Peak (dBTP) detection | New (not in old app) | Added directly to the existing Audio Meter panel as a cyan tick above the sample-peak reading, not a separate panel. `AudioMeterBridge.cpp`'s `AccumulatePacket` computes a real 4x-oversampled inter-sample estimate (Catmull-Rom interpolation) - explicitly **not** claimed as certified ITU-R BS.1770-4 compliance (that standard specifies exact filter coefficients this doesn't reproduce), same disclosure precedent as the Signal Pre-filter's EBU R103 note. |
| Radial Spectrum (frequency wheel, glow-styled) | Removed 2026-09-19 | Built, then went through three outer-ring redesigns in one session (a drawn wave, drifting particles, transient-triggered ripple rings) - the last one Trevor "sort of liked" after tuning, but asked to shelve the whole panel rather than keep iterating. Full last-working code (and two untried ideas for next time - a retro LED-segment equalizer, a kaleidoscope/mandala mode) archived outside the repository, not left in the codebase disabled. |
| Multi-Channel/Surround Polar Plot (5.1/7.1.4/Atmos beds) | Deferred, not started | Capture is currently hard-coded to request 2 channels from Windows process-loopback activation; whether requesting more (6/12 channels) actually returns discrete channels for Resolve's process, rather than silently failing or still delivering a stereo downmix, is unverified. Even if it works, a typical stereo-monitoring workstation would never show real spatial data regardless, since Windows/Resolve would already have downmixed before the tap ever sees it. Scoped out of the initial audio feature batch on Trevor's own call, pending a separate multichannel-capture feasibility test. |
| Layout Persistence & Presets (named presets, not just remember-on-close) | Ported | "Layout Presets" in the General menu: Save Current Layout as Preset.../a preset list to load/Delete Preset, redesigned around ImGui's own .ini save/load instead of the old app's hand-rolled row/panel serialization - see the fix note below. |
| Preferences window | Partial | Several controls present but disabled pending their subsystem (GPU accel, Grid View persistence, Resolve transport/scripting bridges, playback frame delay). |
| Panel management (Add Section, Solo, Content-switch, docking) | Ported | Uses ImGui docking instead of Qt splitters - a framework difference, not a gap. |

## Conventions for adding a panel

See `app/main.cpp`'s own header comment and the existing panels (Waveform,
Noise are good templates) for the pattern:

1. Add to `enum class PanelKind` and `PanelKindName()`.
2. Add a `struct XState { ... }` and a member of the same name on `struct Panel`.
3. Write `DrawXPanel(App&, Panel&)` and `DrawXSettings(Panel&)` (or `(App&, Panel&)`
   if it needs app-level state), ending settings with `DrawSharedSettings(...)`.
4. Register both in the two dispatch `switch`es near the end of `DrawFrame`.
5. Add persisted fields to `CollectSettingFields` if the state should survive
   a restart.
6. CPU pixel-processing panels (preview-tier: False Color/Skin Tone/Noise/Focus
   Peaking/Banding Analyzer/A-B Difference) put their `BuildXImage()` function in
   `ScopeImages.h/.cpp`, threaded the same way `BuildNoiseImage` is.
