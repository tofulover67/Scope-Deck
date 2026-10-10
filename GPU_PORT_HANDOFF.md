# GPU port: handoff

Written 2026-09-21 at the end of the session that measured the GPU question and
answered it. Everything here was measured on hardware, not reasoned about - this
codebase has a long history of punishing the second approach, including twice in
the session that produced this document.

Read this before touching the GPU work. `PORTING.md` is the feature-parity
changelog and does not cover any of this.

> **Status, 2026-09-22: the CUDA port described in §5c as future work is
> written, bit-exact on conformance, deployed, and past the ship gate** - 907
> renders of graded UHD at 23.99 fps with zero dropped frames. The sections
> below were written before it existed and their future tense is stale; §3's
> projections in particular are now superseded by measurement, and §3's
> aggregation ranking is contradicted outright. **§5c carries the current
> state, the real device cost, and what actually remains.** Read that before
> planning anything, or you will plan around a port that is already there -
> which happened once already, on 2026-09-22, and cost a session's orientation.
>
> **Status, 2026-10-10: there is a Metal port too** - §5e. Same conformance
> gate, same hub design, bit-exact, shipping in the macOS bundle. AMD and
> Intel hosts on Windows still fall through to the CPU path.

---

## 1. What state the tree is in

**Shipping code, changed this session:**

- **Wire format v9 -> v10.** The app-side "scope source" GPU design was deleted
  (see §4). Shared memory block dropped from **390 MB to 91 MB**.
- **`ScopeReader`'s mapping is now read-only** (`FILE_MAP_READ`). It used to be
  read/write solely so the app could write `scopeSourceRequest`; with that gone,
  page protection now enforces what was previously only convention.
- **`Preview Scale` default 100% -> 50%** in ScopeTap. At UHD the preview+publish
  leg costs ~21.7 ms at 100%, ~7 ms at 50%, ~3.3 ms at 25%, against a whole-tap
  total of ~26 ms. The old default spent the larger part of the tap's entire
  budget on a viewfinder. Existing projects keep their stored value; this only
  affects new instances.
- **The tap's log now splits `fetch=` from `copy=`.** Before that split, `copy`
  silently included both `fetchImage` calls, which caused a wrong conclusion and
  a wasted change (see §4, "threading the copy").

**Not shipping, build-gated:**

- **`ScopeTapGpuSpike`** - a throwaway measurement plugin. Separate `.ofx`,
  separate bundle, its own log (`%TEMP%\scope_tap_gpu_spike.log`). It publishes
  nothing to the app: as of 2026-09-21 its publish leg writes into a scratch
  mapping of its own (`Local\ScopeDeck.GpuSpike.Slot`), never the live
  `ScopeDeck.v1` block, and no reader ever opens it. Builds *only* when a CUDA
  compiler is present; the normal build is
  unaffected and prints a skip message. Files: `plugin/ScopeTapGpuSpike.{cpp,h}`,
  `plugin/spike/ScopeGpuSpike.{h,cu}`.

**The CUDA port itself, added after this section was written** (2026-09-22):

- **`core/ScopeCuda.{cu,h}`** - the reduction and the shipping tap. Two entry
  points over one set of kernels: `CudaReducer`, which takes a host-memory frame
  and fills a `ScopeResult` (conformance only, pure overhead for the tap), and
  `GpuTap`, which takes Resolve's device pointers and never brings bins into
  host memory at all.
- **`plugin/ScopeTap.cpp`** - wired behind the `gpuAcceleration` parameter and
  `isEnabledCudaRender`, with `CudaPassthrough` for the on-card blit and
  `SCOPE_TAP_NO_CUDA` as the kill switch. The parameter §4 called decorative is
  now the real control.
- **`core/ScopeShm.{h,cpp}`** - reserve / fill / commit slot machinery, so a DMA
  backend can fill a slot while the publisher still owns the seqlock protocol.
  Nothing in `ScopeShm` knows what CUDA is.
- **`core/ScopeCuda.cu`'s `CpuFallbackTap`** - what "GPU Acceleration off"
  runs on a CUDA host: an asynchronous readback plus the ordinary CPU
  reduction, on a worker thread so Resolve never waits for it.
- **`tools/ScopeConformance.cpp`** and the `scope_conformance_cuda` target,
  which also carries `--bench` for adversarial-content timing.

The CPU path is untouched underneath and is still what runs when the host is not
using CUDA.

**Version control now exists** (2026-09-22). The tree is a git repository with a
GitHub remote, branch `main`. This
replaces the manual copying that used to move the tree between machines - which
is also what put a `build-cuda/` cache from another machine into this one (§2).
`build/` and `build-cuda/` are ignored and deliberately do not travel.
`.gitattributes` is `* -text`, because this tree's line endings are mixed
as-found and a repository that exists to protect files must not rewrite them.

---

## 2. New-PC setup (the part that is easy to get wrong)

### Toolchain

| need | version | why |
|---|---|---|
| Visual Studio Build Tools | any recent | builds the app and real tap |
| **MSVC v143 toolset** | 14.4x | **CUDA will not accept anything newer** |
| **CUDA Toolkit** | **12.8 or 12.9** | **not 13.x** |
| DaVinci Resolve | with its OpenFX SDK | `CMakeLists.txt` finds it automatically |

Three traps, each of which cost time to discover:

1. **CUDA 12.9 rejects modern MSVC.** It supports VS 2017-2022 only. A VS 18 /
   MSVC 14.51 toolchain fails with `host_config.h: unsupported Microsoft Visual
   Studio version`. Fix: add the individual component **"MSVC v143 - VS 2022 C++
   x64/x86 build tools"** to the existing Build Tools install, then select it
   with `vcvarsall.bat x64 -vcvars_ver=14.4`.
   **Confirmed working 2026-09-21** on Visual Studio Build Tools **2026**
   (VS 18, channel `VisualStudio.18.Release`): its own installer offers the old
   toolset as component id
   `Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64`, listed as "MSVC
   v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)". It installs
   alongside 14.51 without disturbing it, and nvcc then reports
   `NVIDIA 12.9.41 with host compiler MSVC 19.44.35228.0`. No separate VS 2022
   install is needed.
2. **`-allow-unsupported-compiler` does not work.** `cudafe++` dies with an
   ACCESS_VIOLATION on a two-line kernel. Do not spend time on it.
3. **CUDA 13.0 dropped offline compilation for sm_50-sm_70**, so it cannot build
   the Maxwell/Pascal floor in the architecture list. Use 12.8/12.9. (12.9
   already warns that pre-sm_75 support is going away.)

Also: when installing CUDA, choose **Custom** and **uncheck the display driver**
unless the bundled one is newer than what is installed - otherwise it silently
downgrades the driver and invalidates prior measurements.

### Building

Normal build (app + real tap), no CUDA needed:

```
powershell -NoProfile -File .\build.ps1
```

If a script refuses to run ("not digitally signed"), it carries a
`Zone.Identifier` stream and the machine policy is RemoteSigned:
`Unblock-File .\build.ps1` once. **This applies to `deploy.ps1` too** - hit
again on the second machine 2026-09-22, where both were blocked.

Also: `.ps1` files do not *run* from `cmd`, they open in whatever is associated
with the extension - usually Notepad, with no error. From `cmd` use
`powershell -NoProfile -ExecutionPolicy Bypass -File "<full path>"`, or use a
PowerShell window in the first place.

Anything CUDA - the spike, the real tap's CUDA backend, and
`scope_conformance_cuda` - needs its **own build directory** under the older
toolset, so a plain `build/` keeps using the modern one:

```
"<VS>\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.4
cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-cuda
```

`vcvarsall.bat` also puts VS's own `cmake` and `ninja` on PATH, so neither needs
installing separately. Confirmed 2026-09-22 on a second machine (VS Build Tools
2026, toolsets 14.44 and 14.51 side by side, CUDA 12.9.41): selecting 14.4 gives
`cl` 19.44.35228 and CMake reports `NVIDIA 12.9.41 with host compiler MSVC
19.44.35228.0`. Full tree, 63 targets, zero warnings.

**Then run the gate before trusting anything:**

```
build-cuda\scope_conformance_cuda.exe --hd
build-cuda\scope_publish_race_cuda.exe
```

The first proves the kernels bin correctly; it drives `CudaReducer`, which is
synchronous, so it cannot see frames overlapping. The second drives the real
`GpuTap::RenderFrame` and publish hub into a scratch block and checks every
slot carries its own frame - added 2026-10-08 after one set of device buffers
per instance was found to be overwritten by the next frame mid-copy (192-195
of 300 slot reads wrong per run, fixed by one buffer set per ring slot). Both must
pass.

If CMake reports `Looking for a CUDA compiler - NOTFOUND` after installing CUDA,
there are two separate causes needing different fixes:

- **A stale cache** - reconfigure with `-U CMAKE_CUDA_COMPILER`, or delete the
  build directory outright. That is also the right move for a `build-cuda/`
  copied from another machine: its cache holds that machine's absolute
  toolchain paths.
- **A stale environment.** The CUDA installer adds its `bin` to the *machine*
  PATH, but any shell, terminal or editor already running keeps the environment
  it started with and never sees it. Restart the shell, or prepend
  `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin` for the
  invocation. `nvcc` missing from `where nvcc` while present on the machine
  PATH is the tell.

### Deploying

OFX plugins are scanned **only at Resolve startup**, so Resolve must be closed
- and *fully quit*, not merely returned to the Project Manager. A Resolve
sitting on the project browser still holds the loaded `.ofx` open and the copy
will fail or half-succeed. Check for a live `Resolve` process; do not trust the
window being gone.
`build.ps1 -Deploy` handles the real tap (prompts for admin). For the spike,
copy `build-cuda/bundle/ScopeTapGpuSpike.ofx.bundle` into
`C:\Program Files\Common Files\OFX\Plugins\`.

**After a `kVersion` bump both halves must be redeployed together** or the app
shows a version-mismatch message (cleanly - it names both numbers and stays up).

One trap: the plugin's **Open Scope Deck** button launches
`%LOCALAPPDATA%\ScopeDeck\scopedeck.exe`, *not* the build output. Either copy the
build there or set `SCOPE_DECK_APP` to the build path - `ScopeTap.cpp` checks
that environment variable first, which is exactly what it is for.

---

## 3. The measurements everything rests on

All UHD (3840x2160), 23.976 fps timeline (41.7 ms budget), RTX 5070 Ti, one
plugin instance, Scope Deck app closed. The tap's own log produces the CPU
numbers; the spike's log produces the GPU ones.

### Per-frame cost

| stage | CPU tap | GPU spike | |
|---|---|---|---|
| `fetch` (fetchImage) | 0.04 ms | - | free; not a hidden readback |
| passthrough copy | **10.54 ms** | **0.36 ms** | 29x - D2D vs DRAM |
| binning / scatter | 12.60 ms | **0.37 ms** | with warp aggregation |
| preview | 3.34 ms | 1.06 ms | on-card downscale + 1.5 MB D2H |
| **total** | **26.48 ms** | **~1.9 ms** | |

### The copy is DRAM-bandwidth-bound, not core-bound

A standalone benchmark of the identical copy: **14.1 ms on 1 thread, 8.8 ms on 8,
8.0 ms on 12** - a 1.8x ceiling, plateauing around 31 GB/s of traffic. Threading
it *inside Resolve* was tried and measured over 1169 UHD frames: **10.4 -> 10.5 ms,
i.e. nothing**, because Resolve's own render threads already own the memory
controller. That experiment is recorded as a comment in `ScopeTap.cpp` so nobody
repeats it. **No CPU-side change can move this ~10 ms floor.** Only not having
the frame in system memory at all does - which is the core argument for the port.

### Warp aggregation is mandatory, not an optimisation

Scatter cost by content, plain global atomics vs `__match_any_sync` aggregation:

| content | plain | aggregated |
|---|---|---|
| real footage | 1.82 ms (max **16.76**) | **0.37 ms** (max **0.82**) |
| worst case (all one cell) | **14.68 ms** | **0.49 ms** |
| uniform (fully spread) | 0.95 ms | 0.95 ms |

At 14.68 ms the plain kernel is **slower than the CPU path it replaces**. This is
not theoretical: on real footage 1 frame in 178 hit 16.76 ms - a fade to black or
a title card is enough. Aggregation costs nothing where there is nothing to
aggregate, so **there is no case for a toggle; do it always.**

Note the inversion under aggregation: real footage (0.37) beats the theoretical
worst case (0.49) beats synthetic uniform (0.95). Real frames have local
coherence, so neighbouring pixels share a bin and aggregate well.

### Re-baselined on an RTX 5080, 2026-09-21

The numbers above are an RTX 5070 Ti. Re-measured on a 5080 (84 SMs, driver
610.62, CUDA 12.9.41, MSVC 14.44), UHD 23.976, Scope Deck app closed, one tap
instance, render cache purged, 40 s of uncached playback per run. Same two
clips through both plugins - which is the control §3 itself lacked, since it
isolates the tap as the cause rather than the grade.

| run | rendered / timeline frames | render rate | skipped | p50 | p99 |
|---|---|---|---|---|---|
| tap, heavy grade | 710 / 1002 (71%) | **16.9 fps** | **293** | 21.27 ms | 26.35 ms |
| tap, clean | 980 / 979 (100%) | 24.0 fps | 0 | 19.57 ms | 25.38 ms |
| spike, heavy grade | 976 / 975 (100%) | **24.1 fps** | **0** | **0.78 ms** | 0.81 ms |
| spike, clean | 988 / 987 (100%) | 24.0 fps | 0 | 0.80 ms | 0.82 ms |

| stage | CPU (5080) | CPU (5070 Ti) | GPU (5080) | GPU (5070 Ti) |
|---|---|---|---|---|
| copy | **5.88 ms** | 10.54 ms | 0.33 ms | 0.36 ms |
| bin / scatter | 11.84 ms | 12.60 ms | **0.25 ms** | 0.37 ms |
| preview | ~3.5 ms (at 50%) | 3.34 ms (at 25%) | **0.20 ms** | 1.06 ms |
| **total** | **21.27 ms** | 26.48 ms | **0.78 ms** | ~1.9 ms |

Three things this establishes:

1. **The gate reproduces on different hardware.** 71% of frames rendered at
   16.9 fps here against §3's 69% at 16.5 fps - close enough that the original
   measurement was clearly not an artefact of one machine.
2. **Faster DRAM does not save the CPU path.** This box's passthrough copy is
   nearly half the 5070 Ti's (10.54 -> 5.88 ms) and it *still* drops 29% of a
   graded UHD clip. That is the strongest evidence yet behind §3's "no CPU-side
   change can move this floor".
3. **The ratio widened, it did not narrow.** 14x to **27x** at the median: the
   GPU side gained more from newer hardware than the CPU side did. Preview in
   particular went 1.06 -> 0.20 ms.

Warp aggregation on real graded footage held p50 0.25 / max 0.54 ms, inside
§3's aggregated figures, so that decision holds on consumer Blackwell.

One tail to carry into the port: `host` enqueue (CPU-side, counts against the
frame) spiked to **3.87 ms** once in ~2,000 renders against a 0.08 ms median,
while the GPU work itself never exceeded 1.08 ms. Budget p99 against the host
leg too, not just the kernels.

### The publish leg, priced 2026-09-21 (RTX 5080)

The one leg neither the original spike nor the re-baseline covered: the bins
still have to cross into system memory. 11,729,920 bytes per frame at UHD with
a 50% preview - 5,509,120 of bins in five arrays plus 6,220,800 of preview -
copied device-to-host into a **real named file-mapping view**, because the
destination being shared memory rather than heap is the whole question.

Graded UHD clip, 40 s each, ~955 frames per run, app closed, one instance.

| | publish off | pageable | pinned |
|---|---|---|---|
| `publish` (device) | - | 0.679 ms | **0.217 ms** |
| `host` (CPU stall) | 0.080 ms | **10.200 ms** | **0.100 ms** |
| GPU total | 0.779 ms | 1.461 ms | **1.004 ms** |
| skipped frames | 0 | 0 | 0 |

**That payload is not fixed - it scales with Preview Scale.** The slot is
30 MB because it *reserves* a preview canvas sized for UHD-at-100%; at the 50%
default only 6.2 MB of that is live. Set Preview Scale to 100% and the publish
really does move ~30.4 MB, which at the measured 54 GB/s is **~0.56 ms** rather
than 0.217, taking the GPU total to ~1.35 ms. Still inside budget, but the
0.217 ms figure belongs to the 50% default and should always be quoted with it.

**`cudaHostRegister` accepts a mapped file view.** 974 frames, zero failures.
The publish leg can therefore be a direct async D2H into the shared-memory slot
- no staging buffer, no host memcpy, none of what the CPU publisher does today.
11.73 MB in 0.217 ms is 54 GB/s, i.e. PCIe 5.0 rates. Pinned publishing costs
**0.225 ms** over the no-publish baseline and nothing measurable on the host.

**Pageable blocks the render thread for 10.2 ms** while reading 0.679 ms
device-side. Reading only the device number under-reports it by 15x - the
`fetch`/`copy` conflation of §1 happening again somewhere new. **Any GPU
transfer measurement here needs both its device time and its host time.**

An early write-up of this attributed that 10.2 ms to staging bandwidth, "the
same bytes at an effective 1.15 GB/s". **That was wrong**, and the seqlock
measurement below is what disproved it: a *pinned* copy blocks for 9.57 ms too,
when the render thread waits for it. Both numbers are the same phenomenon -
a render thread waiting for the stream to drain - not a property of pageable
memory. The device-side difference between the two is the real mechanism gap:
0.679 ms staged against 0.217 ms pinned.

### The seqlock handshake, priced 2026-09-21

Can the render thread simply wait for the copy to land before advertising the
slot? Graded UHD, same conditions, `cudaEventSynchronize` on the publish event
(never `cudaDeviceSynchronize`, which ofxGPURender.h forbids for a plugin
handed a stream):

| | pinned (async) | pinned + sync |
|---|---|---|
| `wait` on the render thread | 0.000 ms | **9.574 ms** p50, 10.665 p99 |
| `host` total | 0.090 ms | **9.670 ms** |
| GPU total | 1.003 ms | 1.001 ms |
| skipped frames | 0 | 0 |

**No.** 9.57 ms of waiting for 1.0 ms of work. The GPU total is unchanged
between the runs - nothing got slower, the thread just sat there. The wait is
queue latency: synchronising waits from *now* until our event executes, so it
includes everything Resolve already queued ahead of us, roughly 8.6 ms of grade
rendering. Blocking here means blocking on someone else's work.

**Therefore the publish handshake belongs on its own thread.** The render
thread enqueues and returns; a per-instance thread waits on the event and flips
the seqlock. Zero render-thread stall and no staleness.

The alternative of advertising one frame late is **dead**, for a reason that
has nothing to do with cost: when the playhead is parked - which is how scopes
are actually used - there is no next frame to trigger the deferred publish. A
frame-late design would show the previous nudge's result while the colourist
makes the next one, permanently one edit behind. (Credit where due: that
argument came from an outside review, not from this session.)

Neither run dropped a frame, so 9.6 ms is survivable at 24 fps today. It is a
quarter of the budget spent on nothing, and it would be fatal at 60p UHD's
16.7 ms.

Budget check against the agreed 3-4 ms target: copy 0.33 + scatter 0.25 +
preview 0.21 + publish 0.22 = **1.00 ms measured**, for a kernel doing 5 atomics
per pixel. The real one needs 11, plus the trace and the min/max/probe
reductions - a rough projection lands near 1.5 ms, inside target with room for
the projection to be wrong.

Three things this did NOT measure, all of which belong to the port:

1. **The seqlock handshake.** A slot cannot be advertised until its D2H has
   landed. Either a stream sync - reintroducing exactly the host wait the
   pinned path just avoided - or advertise one frame late, the way the spike's
   own event rings already read one frame late. Unmeasured, and it is the last
   piece of this leg.
2. **Multiple instances.** Resolve makes several; the real block is 91 MB over
   three slots. Whether several instances registering overlapping views is fine
   is untested.
3. **Portability of the registration.** One driver, one Windows build, one
   card. A data point, not a guarantee - and the pageable fallback must stay
   for when it is refused.

**Update 2026-09-22, now that the port exists.** (1) is answered: the handshake
runs on a background thread as §3 concluded it must, and live renders show
`enqueue` p50 0.110 ms with the render thread never waiting. (2) is *observed*,
not settled - two instances each registered their own 86 MB view and both
published cleanly, but that is one run at two instances, not the singleton
design §5c specifies, and teardown ordering remains untested. (3) is now two
cards and two drivers rather than one, which is still a data point. The pageable
fallback stays.

### The dropped-frames gate (the ship gate)

Same clip, **heavy grade**, uncached:

| | per-frame | frames rendered | achieved fps |
|---|---|---|---|
| Scope Tap (CPU) | 21.87 ms | 563 of 811 (69%) | **16.5** |
| GPU spike | 0.81 ms | 929 of 958 (97%) | **23.2** |

On a *clean* clip the CPU tap is fine (26.18 ms, 2 drops in 466). **The port does
not fix bare playback - it fixes graded playback.** At 21.87 ms the tap leaves
under 20 ms of a 41.7 ms frame for everything Resolve does. At 0.81 ms it leaves
41 ms. Headroom becomes dropped frames the moment anyone actually grades the shot,
which is the only reason they are running scopes.

Useful technique: **dropped frames can be read out of the logs** by counting
timeline-frame (`t=`) deltas greater than 1. The log-derived fps matched observed
fps almost exactly in every run.

---

## 4. Decisions already made (do not relitigate)

**The OFX CUDA contract works in Resolve.** Confirmed on hardware: `cuda=1`,
`stream=yes`, and `cudaPointerGetAttributes` reports **device** memory for both
source and output `kOfxImagePropData`. Render window equals bounds. Host-side
enqueue 0.14 ms. VRAM never below 9.8 GB of 16.3.

**The CPU fallback works.** With Resolve set to OpenCL the plugin gets
`cuda=0`, `ptr src=unregistered`, and falls through to the host memcpy (8.37 ms)
with no errors. It keys off `isEnabledCudaRender` per render and never
vendor-sniffs. (Resolve's own OpenCL mode is far slower at everything on an
NVIDIA card and takes ~1 minute to build its kernel cache on first launch - that
is not a plugin problem.)

**Option B - app-side measuring from a published float "scope source" - is dead
and deleted.** It could not touch the 10.5 ms copy floor (best case ~29% off the
tap vs ~14x for the tap-side path), and its mechanism was moving 12-100 MB per
frame through shared memory - spending the exact resource already proven to be
the bottleneck. It would also have needed a GL function loader and a 4.3 context
in an app that requests 3.3 and calls only GL 1.1 entry points.

**Threading the passthrough copy: tried, measured, reverted.** See §3.

**`sm_120` is RTX 50-series consumer Blackwell (compute capability 12.0) and is
NOT covered by `sm_100`**, which is datacenter Blackwell. The architecture list
in `CMakeLists.txt` ships SASS for 52/61/75/86/89/120 plus PTX at **both**
`compute_52` and `compute_120`. The low PTX floor matters: **PTX only JITs
forward**, so without a low one, any architecture below 120 with no SASS match
would fail to load rather than JIT.

~~**The `GPU Acceleration` checkbox in ScopeTap does nothing.**~~ **Superseded
2026-09-22**: it is the real control. `render()` fetches it and keys the CUDA path
off it together with `isEnabledCudaRender`, so the host's own GPU setting still
wins. Unticking it moves the reduction to the CPU rather than turning the scopes
off - see §5c. `SCOPE_TAP_NO_CUDA=1` plus a Resolve restart remains the kill
switch that stops the plugin advertising CUDA support at all, and is also the
only way to measure the CPU path against its historical self.

---

## 5. What to do next, in order

### 5a. `git init` - DONE 2026-09-22

Repository created, baseline commit made, pushed to a private GitHub remote.
See §1. `build/`, `build-cuda/`, `__pycache__/`, `test_stb.obj` and `*.log` are
ignored.

### 5b. The conformance harness - BUILT 2026-09-21

`tools/ScopeConformance.cpp` and the `scope_conformance` target exist and pass.
960 cases (8 patterns x 3 sizes x 8 colour spaces x 5 Row Steps), 3,840 backend
comparisons for the CPU-only `scope_conformance` - 4,800 for
`scope_conformance_cuda`, which adds CUDA as a fifth candidate per case. It drives identical synthetic
frames through the CPU reduction and the CUDA path and diffs the
results. It is the acceptance gate for the CUDA port *and* for every backend
after it, so it pays off more than once.

Shape as built: a plain single-threaded reference reduction lives inside the
harness and is the oracle; the real `ScopeEngine` runs as a candidate at 1, 2, 3
and 8 threads and is diffed against it bit-exactly. The CUDA path joined as one
more candidate in the same loop, needing no change to the matrix or the
comparison. On top of the diff, structural invariants (sampling coverage, bin
totals, the histogram-is-the-waveform-summed-over-columns identity) run against
every backend independently, which catches a bug both paths share - something a
diff by construction cannot.

Verified to actually fail: removing the rowStep alignment from `Analyse`'s band
splitting was injected deliberately, and the harness caught it at 3 and 8
threads while 1 thread still passed, naming the exact expected row count. A gate
that has never been seen to fail is not a gate.

Known gap, deliberate: negative `rowBytes` (OFX permits it, see `FrameView`) is
not exercised. Every frame here is top-down and positively strided.

The design decisions below are the foundation it was built on, kept for the
CUDA side:

- **Exact, not tolerance, wherever exactness is guaranteed.** Integer bins
  (waveform, histogram, vectorscope, twin peaks) are order-independent: with
  identical sampling they must be **bit-exact**, and a mismatch is a real
  sampling-coverage bug. min/max/probe likewise. The waveform trace is raw
  samples with no accumulation, so also exact. Only order-dependent float
  reductions need tolerance - and right now there are none.
- **Compile with `-fmad=false` and check the CPU side does not contract either.**
  Bin assignment runs through `0.2126f*r + 0.7152f*g + 0.0722f*b` then a
  scale-and-floor. nvcc contracts to FMA by default; a luma near a bin boundary
  then lands in different bins on the two paths and counts shift between
  *adjacent* cells. That looks exactly like the coverage bug the exact test
  exists to catch, and the natural reaction - loosening to a tolerance - destroys
  the invariant. With no contraction and matching operation order, IEEE-754
  makes both paths bit-identical.
- **Frame set:** full black, full white, mid grey, 100% bars, gradient, single
  hot pixel, out-of-range values (the tap has measured 1.0332 in the wild), and
  **dimensions not divisible by `rowStep`** - remainder coverage is where
  CPU/GPU divergence hides.
- **Matrix:** 8 colour spaces x `rowStep` 1/2/3/4/8 x that frame set.
- Do not gold-plate it. Exact bins + tolerance floats + deterministic frames is
  done. No image-diff frameworks.

`tools/ScopeSelfTest.cpp` already generates deterministic synthetic frames and
has a `--small` mode intended for exact bin checks - start from it.

**Three things writing it turned up, each of which the spike gets wrong and the
real kernel must not.** The spike was measuring cost, not correctness, so none
of these were bugs in it - but all three would fail conformance on day one.

1. **The column mapping is not the obvious division.** The CPU path assigns
   pixel `x` to the largest column `c` with `floor(c*w/cols) <= x`, via the
   `columnStart` boundary table. The spike uses `floor(x*cols/w)`, which looks
   equivalent and is not: at UHD it puts **256 of 3,840 pixels in a different
   column** (first at x=7), at 1920 **384 of 1,920**, and at a width below 512
   it disagrees on every pixel. Build the boundary table on the device, or
   derive the same mapping - do not re-derive it "more simply".
2. **Cb/Cr scales must come from the luma weights, not from literals.** The CPU
   path precomputes `0.5f / (1.0f - Kb)` and `0.5f / (1.0f - Kr)` once and
   multiplies; the spike hardcodes `0.5389f` and `0.6350f`, which are the
   rounded decimals of the Rec.709 values and not bit-identical to them. Also
   note the form: `(b - luma) * (0.5f / (1 - Kb))` and
   `(b - luma) * 0.5f / (1 - Kb)` do not round the same way, and there are
   eight colour spaces, not one.
3. **The waveform trace does accumulate**, contrary to this document's earlier
   claim that it is "raw samples with no accumulation". Each cell is a box
   average over that column's pixel range - a float sum, so it is exact only if
   a backend sums the same values in the same order. The buckets are tiny (7-8
   pixels at UHD), so a sequential per-thread sum reproduces it bit-exactly
   while a parallel reduction inside the bucket would not. That is a kernel
   constraint, not a reason to add a tolerance.

### 5c. The CUDA port itself - SHIPPED AND GATED, NVIDIA only

**Status 2026-09-22.** The port exists (files listed in §1), builds clean, passes
conformance bit-exactly, and has been observed running in Resolve. What follows
is first what was verified, then what genuinely remains. The design points that
came after were the spec it was built against and are kept for the record.

#### Verified 2026-09-22, RTX 5070 Ti box, CUDA 12.9.41 / MSVC 14.44

- **`scope_conformance_cuda --hd`: 960 cases, 960 passed, 0 failed, 4,800 backend
  comparisons.** Bit-exact against the CPU reduction across 8 colour spaces, 5
  row steps, 8 patterns and 3 sizes. The three traps §5b warned about - the
  `columnStart` mapping, Cb/Cr scales derived from the luma weights, and the
  accumulating trace - are all handled. Without `--hd` it is 640 cases in ~7 s;
  with it, 960 in ~41 s. Run the `--hd` form before believing a result.
- Both the CUDA `ScopeTap` and the full tree build with **zero warnings**.
- **Live in Resolve**, UHD Rec.709, two instances, 218 steady-state renders, log
  marker ` GPU ` with `enqueue=/total=/published=/skipped=/pinned=`:

  | | p50 | p90 | p99 | max |
  |---|---|---|---|---|
  | `enqueue` | 0.110 | 0.177 | 0.275 | 0.687 ms |
  | `total` (render thread) | **0.160** | 0.220 | 0.320 | **0.720 ms** |

  `skipped=0` and `pinned=1` throughout; `published` climbs monotonically. The
  publish handshake of §3 works as designed: `cudaHostRegister` took the 86 MB
  mapped view and the render thread never waits.

- **The two multi-millisecond samples in that run are both the FIRST frame after
  an instance starts** (22.03 ms and 23.83 ms, `enqueue` 5.6 and 6.5 ms) - CUDA
  context creation, the 86 MB pin and first kernel load, once per instance.
  Check the frame number before treating a spike here as the tail §3 warns
  about. It is not one.

#### The ship gate: PASSED 2026-09-22 on the deployed build

Graded UHD, uncached, Timeline Proxy Resolution **Full** (confirmed from the
log's own `3840x2160`, not from the setting), Scope Deck closed, and the
deployed bundles hash-verified against the build first - `deploy.ps1
-VerifyOnly` printed MATCH for both. Worth doing that check every time: the
bundles sitting in the OFX folder beforehand were **stale**, so without it this
would have measured an unknown binary.

The playback burst, isolated from the parked renders around it:

| | |
|---|---|
| renders | 907 |
| timeline frames | 45936 -> 46838 = 902 |
| achieved | **23.99 fps** on a 23.976 timeline |
| **dropped frames** | **0** (no `t=` gap > 1) |
| `skipped` | 0 |

| per-frame, render thread | p50 | p90 | p99 | max |
|---|---|---|---|---|
| `enqueue` | 0.086 | 0.094 | 0.118 | 0.188 ms |
| `total` | **0.120** | 0.130 | 0.150 | **0.220 ms** |

Nothing in the burst exceeded 0.5 ms. Against the CPU tap on the same box
(21.87 ms, 563 of 811 frames, 16.5 fps): **~182x at the median, and the drops
go to zero.**

Across the whole session exactly one render passed 1 ms (4.03 ms) and it is
frame #56, four frames after an instance started. The two genuinely slow frames
(19.69 and 33.72 ms) are each the *first* after a `GPU path started` - context
creation plus the 86 MB pin, once per instance. **Check the frame number before
treating anything here as a content tail.**

**How to read this log at all.** The tap logs parked and scrubbing renders as
well as playback, so frames divided by total wall time gives a meaningless
~7 fps. Split the log into bursts on wall gaps > 1 s and measure the playback
burst alone. The burst boundaries are obvious once you look; the aggregate is
not.

**No same-clip CPU control was run**, deliberately. The user's judgement was
that the grade already sits near where Resolve itself begins dropping frames,
which is precisely where a 21.87 ms tap decides the outcome - §3's "headroom
becomes dropped frames" restated. The control that does exist is the 5080
re-baseline, which put both plugins over the same two clips and reproduced
69%/16.5 fps as 71%/16.9 fps. So the CPU figure is not clip-specific to this
run; treat "this grade would have hurt the CPU path" as well-founded inference
rather than as measured here.

#### What the port actually costs, measured 2026-09-22

The gate above says Resolve does not drop frames. It does **not** say what the
tap costs, and for a while this document implied it did. Device-side timing
(`dev=` in the log, read on the publisher thread once a frame has landed) over
1,649 frames of graded UHD:

| | p50 | p90 | p99 | max |
|---|---|---|---|---|
| **device total** | **1.966** | 2.840 | **3.407** | 5.152 ms |
| kernels | 1.441 | 1.708 | 2.110 | 3.940 ms |
| publish D2H | 0.499 | 1.268 | 1.561 | 2.657 ms |

**This is 2.5x what the spike measured and the projection expected.** The spike
ran 5 atomics per pixel and came in at 0.78 ms; §5c's own projection said "a
rough projection lands near 1.5 ms, inside target with room for the projection
to be wrong". The projection was wrong in the direction that costs room:
**p99 3.41 ms sits at the top of the agreed 3-4 ms budget, not comfortably
inside it.** Nothing drops today at 41.7 ms, but there is far less margin than
the render-thread figure suggests, and 60p UHD's 16.7 ms would be tighter.

Three numbers, three different questions - keep them apart:

- `enqueue=` / `total=` are **render-thread wall time**. They decide whether
  Resolve drops frames. That is the ship gate, and it is 0.12 ms.
- `dev=` is **what the GPU tap costs**. That is the budget question, and it is
  ~2 ms.
- Neither is the other. Quoting one for the other is the `fetch=`/`copy=`
  conflation of §1 in a new place, and it happened again here before the
  instrumentation existed.

#### Adversarial content, measured 2026-09-22 - and §3's ranking is wrong

`scope_conformance_cuda --bench` drives the shipping kernels over hostile
content, kernel time only, UHD, 200 iterations per pattern. §3's aggregation
table predicted real footage fastest and a uniform spread slowest. On the full
kernel that ordering does not hold:

| content | p50 | p99 |
|---|---|---|
| gradient | 1.628 | **2.139** |
| spread (hash of x,y - nothing to aggregate) | 1.505 | 1.946 |
| black (fade) | 0.427 | 0.764 |
| white (blown) | 0.433 | 0.658 |
| grey (all one cell) | 0.415 | 0.656 |
| bars100 | 0.382 | 0.631 |
| hotpixel | 0.426 | 0.686 |
| outofrange | 0.524 | 0.797 |

The synthetic gradient is the **slowest** case, ahead of a fully spread field,
while every flat pattern sits near 0.4 ms. Live graded footage measures 1.441 ms
of kernel time, i.e. near the gradient end, not near the flat end - so the bench
is realistic and §3's "real footage is the easiest case" does not survive the
move from 5 atomics to the full kernel.

**Mechanism not established.** The gradient's R plane is constant in y, which
would put every row on the same addresses, and warp aggregation is intra-warp
only - but that is a hypothesis and nobody has measured it. It is the obvious
first thing to check if the 3.41 ms p99 ever needs to come down.

#### The publish hub, and the ticket race it fixes

Each instance used to own a `ScopePublisher` and allocate tickets from
`m_NextTicket` seeded off the shared write counter - a check-then-act across
instances. `ScopeShm.h` called a collision benign, "a lost frame for one of them
rather than a torn read", and **that was true only while publishing was
synchronous**:

- `OpenSlot` and `CommitSlot` are plain `seq + 1` increments with no parity
  enforcement. Two interleaved opens on one ticket take the sequence
  even -> odd -> even, so the slot **advertises itself as readable while two
  DMAs are still landing in it** and a reader passes the seqlock check on a torn
  frame. That is not a lost frame.
- The port widened the window from a memcpy to an async D2H that outlives the
  render call.
- Per-instance rings could not see each other either, so N instances had
  N x `kSlotCount` frames in flight over a ring of `kSlotCount`.

The mapping, its page-lock, the ticket counter, the in-flight ring and the
publisher thread are now **one set per process**, refcounted, with the ticket
and the ring entry taken under one lock. Instances keep their own device buffers
and publish stream, because they do genuinely reduce different frames at once.
This also dissolves §5c's multi-instance pinning question instead of answering
it: one registration, so no overlapping views and no teardown hazard.

Note the race was never *observed* - in every run so far the instances did not
overlap in time. It was found by reading, and fixed on that basis.

#### "GPU Acceleration" off now measures on the CPU

It used to publish nothing at all: the image passed through and the app simply
stopped updating, with a log line suggesting an environment variable. Unticking
a box called "GPU Acceleration" should mean the reduction moves to the CPU, and
now it does.

Getting there took two wrong explanations, both worth recording because the
answer was already in this document:

1. Shipped synchronously. Measured 45.00 ms p50 / 62.58 ms p99 for the whole
   path - about twice what the CPU tap ever cost - and the user reported it
   visibly holding Resolve up, correctly, since the wait lands on Resolve's
   render thread.
2. Blamed pageable memory and page-locked the staging buffer. **No effect.**
3. Split the number with CUDA events, which settled it:

   | | p50 | p90 | max |
   |---|---|---|---|
   | copy (device transfer) | 5.51 | 6.21 | 8.04 ms |
   | **wait (queue latency)** | **22.51** | **31.47** | 50.97 ms |

   Four fifths of it was the render thread sitting behind work Resolve had
   already queued. §3's publish-leg section says exactly this - a *pinned* copy
   blocks for 9.57 ms too, and the wait "is not a property of pageable memory" -
   and both wrong guesses talked straight past that paragraph.

`CpuFallbackTap` therefore does what the publish leg does: the render thread
enqueues the D2H on the host stream and returns; a worker waits on the copy
event, reduces on the CPU and publishes through the hub. Measured after:

| | synchronous | asynchronous |
|---|---|---|
| render thread `total` | 45.00 ms p50 | **0.090 ms** p50 |
| copy / bin | on the render thread | 5.70 / 19.84 ms, on the worker |

One staging buffer, page-locked, and a frame arriving while the worker is busy
is **skipped** rather than queued - overwriting it would tear the frame being
reduced. During scrubbing that ran 157 queued to 88 skipped, which is the design
working: ~40 scope updates a second, and Resolve untouched.

`bin` is higher than the CPU tap's historical 12.6 ms because the worker
competes with Resolve for cores instead of owning the render thread. That is the
trade, and it is the right way round.

**This path can never match the CPU tap on a non-CUDA host**, which gets its
frame in system memory for free. To compare the CPU reduction against its
historical self, use `SCOPE_TAP_NO_CUDA=1` and restart Resolve - that declines
CUDA support outright and runs the original path end to end. Unticking the
checkbox is a different measurement.

#### What remains

1. **p99 3.41 ms against a 3-4 ms budget.** Inside it, but the margin is thin,
   and 60p UHD's 16.7 ms frame would make it thinner. The gradient result above
   is where to start looking if it needs to come down.
2. **NVIDIA only.** AMD and Intel hosts get `isEnabledCudaRender == false` and
   fall through to the CPU path - correct scopes at 21-26 ms, dropping frames
   under a grade, exactly as before this work. See §5d, whose ordering question
   is still open and still a business call rather than a technical one.
3. **A one-off stall when a GPU instance starts**, 257 ms in the worst
   observation, which is `cudaHostRegister` on 86 MB running on the render
   thread. The hub made it once per process rather than once per instance;
   moving it off the render thread entirely would remove it.
4. **`draft` and `interactive` remain unexercised**, as §6 has always said.

#### The design spec it was built against (kept for the record)

- **The spike is a subset.** It runs 5 atomics per pixel; the real tap needs 11
  (waveform 4 + histogram 4 + vectorscope 1 + twin peaks 2), plus the waveform
  trace (a strided gather, no atomics - the easiest and most obviously profitable
  kernel), min/max/probe reductions, all 8 colour spaces, `rowStep`, and the shm
  publish the spike skips entirely. **Design against ~3 ms/frame, not 0.81.**
- **Budget: 3-4 ms** for the complete GPU tap, agreed 2026-09-21. That is ~4x
  the measured spike-plus-publish total of 1.00 ms, and still leaves Resolve
  ~38 ms of a 41.7 ms frame.
- **Make the shared block a process-level singleton**: one mapping, one
  `cudaHostRegister`, one publisher thread, refcounted teardown - and instances
  borrow it. Registration is per-process anyway, so this *dissolves* the
  multi-instance question rather than answering it: no instance ever registers,
  so overlapping views at different addresses never arise, and the teardown
  hazard (one instance destroyed while others still publish) goes with it.
  Fold the publish thread into the same singleton so the handshake and the
  instance story are one piece of plumbing, not two. (From the same outside
  review that killed frame-late publishing.)
- **Spec the tail, not the mean.** The whole lesson of this session is that tails
  drop frames. Target p99 under adversarial content (fades, blown highlights,
  bars), not just p50 on nice footage.
- **Do not let 41.7 ms calcify.** 60p UHD is a 16.7 ms budget, and that is where
  `rowStep` earns its keep.
- Re-run the dropped-frames gate in the *final* configuration before shipping.
  The spike cleared it at 0.81 ms; that is not the number that ships.

### 5d. Then Metal, then OpenCL - but decide the order deliberately

**Decided and done, 2026-10-10: Metal second.** See §5e. OpenCL (Windows AMD and
Intel) remains the open item, and the ordering argument below is kept for
the record.

The original plan had Metal second. This tree had never been built on macOS when
this was written; **as of 2026-10-10 it has** (see PORTING.md, "macOS port"):
the app, the CPU ScopeTap and both gates build and pass on Apple silicon, so a
Metal port now starts from a working macOS tree rather than from nothing. Note
Metal shader compilation needs either Xcode (`xcrun metal`) or runtime
compilation from source; the Command Line Tools alone do not carry the Metal
compiler. Metal second still means a different audience than Windows AMD or
Intel users getting anything. That may still be right if the Mac colourist audience
is the bigger commercial prize - but it is a business call, not a technical one,
and it has never actually been made.

---

### 5e. The Metal port - SHIPPED, bit-exact, measured on an M1 (2026-10-10)

**Files.** `core/ScopeMetal.{h,mm}` - `MetalReducer` (conformance),
`MetalTap` (the shipping path), `MetalFallbackTap` ("GPU Acceleration" off),
`MetalPassthrough`; `core/ScopeGpuTypes.h` holds `GpuTapArgs`/`GpuTapStats`,
now shared with CUDA so `plugin/ScopeTap.cpp` has ONE render body for both
backends (`SCOPE_TAP_GPU`, with `GpuTapBackend` / `GpuFallbackBackend` /
`GpuPassthrough` aliased per backend). The kernels are MSL source compiled at
run time by the Metal framework - which is what lets the whole thing build
with the Command Line Tools alone; no Xcode, no `xcrun metal`. Gates:
`scope_conformance_metal` (same harness, `SCOPE_HAVE_METAL`; the harness body
is generalised over `GpuReducer`) and `scope_publish_race_metal` (the real
tap through the real hub, one and two instances). `release_mac.sh` runs both.

**Built on the CUDA design, with three differences that follow from unified
memory and Metal's API:**

1. **No page-lock, no D2H.** Result buffers are `MTLStorageModeShared`; the
   publish leg is the hub worker's memcpy into the slot after the command
   buffer completes (0.68 ms at HD, measured in the race test). The mapping
   itself can be wrapped as an `MTLBuffer` (`newBufferWithBytesNoCopy` on the
   `shm_open` mapping was tested and works, and a blit wrote into it) - the
   kernels could then write straight into the slot and the memcpy would go.
   Not done: the memcpy is on the worker, not the render thread, and 0.7 ms
   there costs Resolve nothing. The next step if the worker ever falls
   behind.
2. **One command buffer on the host's queue, committed and forgotten.** The
   render thread encodes passthrough blit, fill, scatter, trace, probe and
   preview and returns; the worker waits on the command buffer (the Metal
   spelling of `cudaEventSynchronize`) and closes the seqlock. Device time
   comes from `GPUStartTime`/`GPUEndTime`.
3. **SIMD-group aggregation instead of `__match_any_sync`.** There is no
   match instruction, so the kernel loops: ballot pending lanes, lowest is
   leader, broadcast its address, ballot matches, leader adds the count -
   one round per distinct address. **Bounded at two rounds**, then plain
   atomics: unbounded it costs 32 rounds on spread content, measured 45 ms
   for the frame against 8 with plain atomics.

**The three bit-exactness traps hold on Metal.** The column table and the
Cb/Cr reciprocals transfer unchanged. **Contraction is trap 3 again, and
`fastMathEnabled = NO` does NOT prevent it** - measured with the harness's own
detector values before any kernel was written: `a*a - c` came back 2^-24
instead of 0 with fast math off. `#pragma METAL fp contract(off)` in the
source is what stops it; with the pragma the result is 0 and matches the CPU.
The first conformance run also found the histogram's output bound at buffer
index 1, on top of the parameter block; the trace and probe then read their
luma weights out of histogram counts. Every kernel's parameters are re-bound
before every dispatch now.

**What was measured, `scope_conformance_metal --bench`, Apple M1 (the 8-core
GPU in a MacBook Air, the slowest Apple silicon there is), UHD, 200
iterations per pattern, kernel time only:**

| content | p50 | p90 | p99 |
|---|---|---|---|
| gradient (footage-like) | 8.20 | 11.38 | 13.90 ms |
| spread | 7.78 | 10.68 | 13.78 |
| black / white / grey / bars / hotpixel / outofrange | 7.9-8.7 | 11.3-11.7 | 12.6-13.7 |

The flat patterns and the spread one cost the same, which says the
aggregation is doing its job on both ends. The p90/p99 tail over a sustained
200-iteration run is the Air's GPU clock, not content: short runs show p99
within 10% of p50.

**Where the time goes, found by switching pieces off (variants of the
kernel, UHD, gradient):**

| | ms |
|---|---|
| everything except the scatter (fill, trace, probe) | 0.75 |
| scatter with loads and min/max only, no bins | 6.0 at 2 bands |
| scatter, bins, no atomics at all | 5.3 at 2 bands -> **3.5 at 8 bands** |
| full kernel | **~8** |

So the load path is latency-bound, not bandwidth-bound (8 bands of rows per
column, i.e. 4096 threadgroups in flight instead of 1024, nearly halved it;
unrolling the row loop four deep on top bought nothing), and the atomics cost
~4 ms over that floor however they are aggregated: a 12-run matrix over
unroll {1,4} x aggregation rounds {1,2,4} x plain-vs-aggregated threadgroup
atomics all landed between 7.1 and 9.4 ms p50 on footage. The shipping
configuration is the simplest one in the band: one row per trip, two rounds,
plain threadgroup atomics, eight bands.

**Against the budget.** The 3-4 ms figure was set for a desktop NVIDIA card
and this is the weakest Apple GPU; it is 2x over at UHD and inside it at HD
(the race test's HD frames: 3.26 ms device, passthrough and preview
included). The ship gate is still render-thread time and dropped frames, and
the render thread here encodes and returns. The CPU tap on this same machine
measured 12 ms *on the render thread* at HD (`fetch=0.03 copy=1-2 bin=8`),
so even at UHD the Metal path moves the whole cost off the thread Resolve is
waiting on. M1 Pro/Max and later parts have 2-8x this GPU.

**The ship gate: PASSED 2026-10-10, M1 MacBook Air, Resolve 21.0.4.**
Deployed through `build_mac.sh --deploy`, a graded 1920x1080 Rec.709 timeline
played for ~36 s with two tap instances live. From the log, bursts split on
wall gaps > 1 s, the playback burst alone:

| | |
|---|---|
| renders | 860 (878 in the session, every one on the GPU path) |
| longest consecutive run | 655 frames in 27.20 s = **24.1 fps** on a 24p timeline |
| **dropped frames** | **0** (no `t=` gap > 1) |
| `skipped` | 0 |

| per frame | p50 | p90 | p99 | max |
|---|---|---|---|---|
| `enqueue` (render thread) | 0.135 | 0.211 | 0.663 | 1.313 ms |
| `total` (render thread) | **0.260** | 0.410 | 1.030 | 2.070 ms |
| `dev` (device, async) | 7.92 | 10.46 | 13.05 | 13.62 ms |
| of which kernels | 6.83 | 9.31 | 11.92 | 12.59 ms |
| of which publish memcpy | 1.13 | 1.49 | 2.27 | 3.80 ms |

Against the CPU tap on the same machine and the same timeline earlier that
day - `fetch=0.03 copy=1-2 bin=8 total=12 ms` on the render thread - that is
**~45x at the median on the thread Resolve waits on**, and the drops it was
not quite producing at HD stay at zero. Device time is higher than the bench
predicted for HD (7.9 ms against ~3.3 in the race test) because the GPU is now
shared with Resolve's own rendering; it runs off the render thread, so it
costs Resolve nothing until the GPU itself saturates, which an M1 at UHD with
a heavy grade may well reach. Playback *looked* slow to the user; the log says
Resolve held 24 fps, so that is the viewer, not the tap.

**What remains.**

1. **A UHD ship gate** on an Apple GPU that can play UHD at all; this Air
   cannot, with or without the tap.
2. **Zero-copy publish** (point 1 above) if the worker ever becomes the
   bottleneck.
3. **The load floor.** 3.5 ms to read 127 MB is well short of the M1's
   bandwidth. The threadgroup-per-column mapping reads each row in 512
   separate 128-byte pieces; a row-major tile mapping with the column table
   indexed per pixel (as CUDA does) would read 256-byte runs and is the next
   thing to try if UHD on small Apple GPUs matters.
4. **`SCOPE_TAP_NO_METAL`** is the kill switch, as `SCOPE_TAP_NO_CUDA` is.

## 6. Gotchas

- **Line endings are mixed across the tree** - `ScopeTypes.h` is LF,
  `ScopeReader.cpp` is CRLF, some files are mixed internally. Scripted edits that
  assume one or the other will silently fail to match.
- **`scope_format.py` does not exist in this tree.** It lives only in
  `Scope Deck (old)`, which is reference material and not a build dependency. The
  old "mirror changes into it" instruction has been removed from `ScopeTypes.h`.
- **`draft` and `interactive` are never set** by Resolve in this configuration -
  7,985 spike renders, all `draft=0`, including during hard scrubbing. Those code
  paths remain unexercised.
- **Resolve instantiates several plugin instances** for one node graph. Any
  per-instance allocation multiplies, and any measurement that does not control
  for instance count is not comparable. An early three-way comparison in this
  session was invalidated by exactly this.
- **The tap sees the playback proxy resolution, not the timeline resolution.**
  With Timeline Proxy Resolution on Half, a UHD timeline hands the tap
  1920x1080 frames and every measurement quietly becomes an HD one - at HD
  the tap is ~9 ms of a 41.7 ms budget instead of ~26 ms, so nothing drops
  and the run looks like a pass. Set it to Full before measuring, and
  confirm from the log's own `WxH` field rather than from the setting.
  The app already shows the same thing in its status line ("Live | 3840x2160
  Rec.709 | frame N"), straight from `SlotHeader::width/height` - so the tell
  is on screen the whole time. What neither the app nor the tap can know is
  the *timeline* resolution, so neither can say "this is proxy", only "this is
  what I measured".

- **The app's view runs ahead of Resolve's viewer during playback** - a
  consequence of the port, accepted rather than fixed. The tap publishes at
  render time, Resolve renders ahead of display, and the reader always shows the
  newest slot, so the app shows a frame Resolve has not put on screen yet.
  Parked, the two agree exactly, which is the case that matters for reading a
  scope. The old CPU tap hid this by being slow: at 21 ms and dropping 29% of
  frames its publishes lagged far enough to roughly coincide with the viewer.
  Three fixes were considered and all rejected: a fixed `playbackFrameDelay`
  would put the scope *behind* the viewer when parked, which is the same
  "one edit behind" failure that killed frame-late publishing; matching on
  `timelineTime` against the Timecode bridge cannot work because that bridge
  requires a before/after sample match and so by design refuses to sample a
  moving timeline; and a deeper slot ring plus playback detection is a wire
  format change for a cosmetic artifact. Note the app is not showing anything
  *wrong* - it is showing what Resolve most recently computed.

- **`scopedeck.exe` cannot be linked while the app is running** (`LNK1104`).
  Close it before building.

- **A `.ps1` does not run from `cmd`** - Windows opens it with whatever the
  extension is associated with, usually Notepad, and reports no error at all.
  From `cmd`: `powershell -NoProfile -ExecutionPolicy Bypass -File "<full
  path>"`. Both `build.ps1` and `deploy.ps1` also arrive mark-of-the-web
  blocked on a fresh machine and need `Unblock-File` once.

- **Run `deploy.ps1 -VerifyOnly` before believing any measurement.** The
  bundles sitting in the OFX folder on 2026-09-22 were stale, and a run
  against them would have measured an unknown binary. It hashes built
  against deployed and needs no admin.

- **The tap logs parked and scrubbing renders, not just playback.** Frames
  divided by total wall time therefore gives a meaningless number - ~7 fps
  on a run that actually sustained 23.99. Split the log into bursts on wall
  gaps over 1 s and measure the playback burst alone.

- **The first frame after a GPU instance starts is slow** - 19-257 ms across
  observations, from CUDA context creation and page-locking 86 MB. Check the
  frame number before treating a spike as a content tail; steady state has
  never exceeded 0.72 ms on the render thread.

- **Any wait on a CUDA stream includes everything Resolve queued ahead of
  you.** Measured twice now: 9.57 ms for the publish leg, 22.51 ms for the
  fallback readback. It is not transfer time and no amount of pinning,
  buffer tuning or bandwidth reasoning touches it - the only fix is not
  waiting on the render thread. Bracket transfers with CUDA events and
  report copy and wait separately, or the number will be misread again.
