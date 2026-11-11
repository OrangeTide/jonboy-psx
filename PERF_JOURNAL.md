# Performance journal

A running log of core performance measurements, so optimizations can be judged
against a recorded baseline instead of memory. Add a new dated entry whenever a
change is meant to affect performance. Keep older entries intact.

## How to measure

Use the built-in benchmark, which runs the deterministic single-threaded core
unpaced and reports the emulation thread's wall time per emulated frame. See the
Testing section of the project notes for the full description.

```
./_out/jonboy --bench --bios <retail bios> [--cd <game>] \
    [--bench-warmup N] [--bench-frames N]
```

The budget is 16.67 ms per frame (1/60 s). The core must do all of its work for
a frame, CPU plus GPU rasterization plus SPU plus DMA plus CD, inside that
window, or the windowed build has no time left to present and audio underruns.

Read the numbers this way:

- `mean` and `median` are the stable figures. The same instructions run every
  time, so these repeat run to run within a few hundredths of a millisecond.
  Compare these across a change.
- `p99` and `max` are the tail. They move with host scheduling, so treat a
  single run's tail as indicative, not exact.
- `over budget` is the share of frames above 16.67 ms. A high share with a mean
  still under budget means the SPU ring is draining on the slow frames and
  refilling on the fast ones. It survives, but there is no headroom.
- The audio and video lines are modeled from the per-frame series (an SPU ring
  drained at 44.1 kHz, and a 60 Hz presenter), not captured from a device.
- The per-frame budget includes the end-of-frame VRAM publish copy, which the
  windowed emulation thread pays. It does the same copy on the live VRAM through
  the shared helper, so the region size matches the real build.
- The present-time transpose is reported on its own line, not in the budget. It
  runs on the vsync-limited main thread in parallel with the next frame's core
  work, so it does not compete with audio. The SDL texture upload and present
  themselves need a window and are not measured here.

## Canonical workloads

Keep the warmup and frame counts fixed so entries compare directly.

- **W1 bios-logo**: `--bench --bios scph5501.bin --bench-warmup 300
  --bench-frames 600`. Catches the animated BIOS boot logo, which is GPU heavy.
- **W2 ct-intro**: `--bench --bios scph5501.bin --cd "<Chrono Trigger cue>"
  --bench-warmup 900 --bench-frames 600`.

A game's heavy scenes (a full battle, an FMV) live much later than 900 frames.
Add new named workloads with their own warmup counts as the targets of
optimization become clearer, and record which commit introduced each one.

## Entries

### 2026-11-11, commit 67e0aa3 (baseline)

Host: Intel Xeon E5-2650 v2 @ 2.60 GHz, gcc 13.3.0, `-O2`.
First measurement, before any of the planned performance work. FASTTICK off.

| workload   | mean | median | p99 | max | over budget | audio underruns | video dropped |
|------------|------|--------|-----|-----|-------------|-----------------|---------------|
| W1 bios-logo | 15.12 ms | 17.43 ms | 18.17 ms | 20.95 ms | 66.0% | 0 | 0 |
| W2 ct-intro  | 10.00 ms |  9.23 ms | 13.56 ms | 17.27 ms |  0.2% | 0 | 0 |

Notes:

- This host is a 2.6 GHz server part with modest single-thread throughput, so
  the margins are tight. On a faster desktop the same code has more headroom.
- W1 is the striking one: the median frame is already over the 16.67 ms budget
  during the boot logo, and two thirds of frames exceed it. The mean stays just
  under, so the modeled ring does not underrun, but there is no slack. This is
  the kind of case the optimization work needs to pull back under budget.
- W2 sits near 1.67x real-time with essentially no over-budget frames, but this
  window is still early (menu and intro), not a heavy battle.

### 2026-11-11, shrink the windowed publish copy and present transpose

Optimizations 1 and 2: publish and transpose only the display rectangle for
normal 15-bit gameplay, instead of the whole 1024x512 VRAM. The debug F1 view,
24-bit FMV, and blank frames keep the full-VRAM path, selected by a per-frame
flag so the consumer always reads the layout that was published.

This affects the windowed build only, so `--bench` (headless, no present) does
not show it. Measured instead with an operation-level microbench of the two
copies, full VRAM versus a 320x240 display region, on the baseline host:

| operation          | full VRAM | display region | thread            | saved/frame |
|--------------------|-----------|----------------|-------------------|-------------|
| publish memcpy (1) | 0.084 ms  | 0.018 ms       | emu (audio path)  | ~0.066 ms   |
| transpose (2)      | 1.238 ms  | 0.152 ms       | main (vsync)      | ~1.09 ms    |

The emulation-thread saving (1) is small in absolute terms but comes off the
budget the audio ring depends on. The larger saving (2) is on the main thread,
which is vsync limited and had slack, so it buys present and input-latency
headroom rather than audio headroom. The two share one region restructure, which
is why they land together.

Correctness: the compact path is pixel-identical to the full path for the
display rectangle (both resolve to the same column-major source index). Verified
in the running window: the BIOS boot logo (15-bit, compact path) and the Chrono
Trigger intro (full path) both render correctly, no garbage, offset, or tint.

### 2026-11-11, --bench now measures the publish copy and transpose

`--bench` previously measured the core only and did not touch the windowed
publish or present path, so changes there (optimizations 1 and 2) were invisible
to it. It now does the same end-of-frame work the windowed build does, through
the shared helpers, so the region sizes match. The publish copy is folded into
the per-frame budget (the emu thread pays it); the transpose is reported on its
own line (main thread, parallel, not in the budget, so the audio model stays
honest).

The budget line now including the publish copy shifts the recorded core figures
by well under the run-to-run jitter (the copy is ~0.02 ms for a 15-bit region,
~0.08 ms for a full snapshot), so earlier entries remain comparable.

New readings on this host, showing the transpose split by display path:

| workload   | budget mean | over budget | transpose mean | path            |
|------------|-------------|-------------|----------------|-----------------|
| W1 bios-logo | 13.86 ms  | 40.0%       | 0.36 ms        | 15-bit region   |
| W2 ct-intro  | 10.34 ms  |  0.3%       | 1.24 ms        | 24-bit full     |

The W2 transpose of 1.24 ms matches the full-VRAM microbench exactly, confirming
the Chrono Trigger intro is a 24-bit FMV on the full path, while a 15-bit screen
transposes only its region. The transpose cost is real but lands on the main
thread, which has the whole 16.67 ms in parallel, so it is headroom there rather
than pressure on the audio budget.

### 2026-11-11, enable hardware-tick batching (FASTTICK=8)

Optimization 3: batch the per-step hardware clock advance and interrupt poll to
every 8th instruction instead of every one. The deadline scheduler already makes
the per-step tick cheap, so the saving is mostly the per-instruction COP0
interrupt check; it is larger on CPU-bound work and smaller when a frame is
dominated by GPU raster or FMV. The real-time windowed build and the benchmark
turn it on; the deterministic modes (headless, exe, boot, gdb) stay at 0, so
conformance (29/29) and the boot regression remain bit-exact. JONBOY_FASTTICK
overrides the choice.

Chosen N by a clean A/B on W2 (three trials each, budget mean in ms):

| N  | trial 1 | trial 2 | trial 3 | mean    |
|----|---------|---------|---------|---------|
| 0  | 10.12   | 10.10   | 10.19   | 10.14   |
| 8  |  9.53   |  9.64   |  9.61   |  9.59   |

That is a steady ~5% on this GPU-heavy window (N=8 beat N=0 in every trial);
N=8 coarsens interrupt latency by at most 7 instructions (~14 cycles), which is
within the variation of real hardware. Larger N returned no reliable further gain
here and only widens that latency, so 8 is the pick.

Correctness: windowed Chrono Trigger boots into the intro with N=8 and renders
correctly; the deterministic suites are untouched.

Post-change baseline (default build, N=8 now active in --bench):

| workload   | budget mean | over budget | transpose mean |
|------------|-------------|-------------|----------------|
| W2 ct-intro | ~9.6 ms     | low         | 1.24 ms (full) |

### 2026-11-11, measure the rasterization fraction (threaded-GPU step 0)

Before committing to the threaded-GPU work (docs/threaded-gpu.md), measure how
much of the per-frame budget is GP0 primitive dispatch, the triangle/rect/fill/
copy rasterization a GPU worker would run instead. `--bench` now times that work
(gpu_profile in gpu.c, off except during the benchmark) and reports it as a
subset of the budget. It is the ceiling of what threading the GPU can remove from
the emulation thread.

| workload     | budget mean | raster mean | raster share | over budget |
|--------------|-------------|-------------|--------------|-------------|
| W1 bios-logo | 13.8-15.0 ms | 4.3-6.1 ms | 31-40%       | 41-66%      |
| W2 ct-intro  | ~10.0 ms     | 0.10 ms    | 1%           | ~0%         |
| W3 fft-battle | 13.4 ms     | 2.70 ms    | 20% (med 25%) | 1.2%       |

W3 is real gameplay: the Final Fantasy Tactics Orbonne tutorial battle, the
isometric 3D map that the GTE transforms and the rasterizer draws as textured and
gouraud polygons. Reached by warming up 16200 frames under
assets/fft_tutorial_battle.script (the last scripted input is at frame ~16037),
measured 600 frames in the battle. Run at JONBOY_FASTTICK=0 so the script, which
is tuned to the exact-timing core, lands in the battle; at the shipped N=8 the
raster work is identical and the budget a touch lower, so the share would be
marginally higher. That the raster time is a real 2.7 ms (not near-zero like the
FMV) confirms the battle was reached and is rendering polygons.

The payoff is strongly workload dependent, which is exactly what step 0 is for:

- The animated BIOS logo is polygon heavy (gouraud and textured primitives), and
  rasterization is 31-40% of its budget. This is also the window whose median
  frame is already over the 16.67 ms budget, so moving 4-6 ms of raster onto a
  worker would pull those frames back under budget. Threaded GPU directly targets
  the frames that currently overrun.
- The Chrono Trigger intro is a 24-bit FMV: MDEC decode and image upload, almost
  no polygon dispatch, so raster is 1%. Threading the GPU does nothing for it;
  its budget is bound by the CPU, MDEC, and SPU.

Conclusion: threaded GPU is worth building, but its value lands on polygon-driven
2D/3D scenes, not FMV. Confirmed on real gameplay (W3): the FFT tutorial battle
spends 20% of its budget (median 25%) on rasterization, between the boot logo and
the FMV, so moving raster to a worker frees a fifth to two fifths of the
emulation-thread budget on polygon workloads. The number excludes CPU-to-VRAM
image upload (payload writes), so it is a lower bound on what moves to the worker.

Caveat: the profiling adds two clock reads per completed GP0 command, which
slightly inflates the budget during a profiled run. The raster *share* (raster /
budget, both measured in the same run) is robust to this; the absolute budget in
these rows is marginally higher than an unprofiled run.

### 2026-11-11, threaded GPU windowed perf measurement

With the GPU worker built (JONBOY_GPU_THREAD, docs/threaded-gpu.md), measure what
it buys on the live windowed build, using the JONBOY_PERF HUD on the BIOS boot
logo (the heaviest raster case, ~40% of budget). The HUD reports the emulation
thread's share of the real-time budget and audio underruns per second. Comparing
the animated-logo phase (the heavy polygon phase of each run):

| build        | emu-thread budget | audio underruns/s |
|--------------|-------------------|-------------------|
| synchronous  | 95-98%            | 1-8               |
| threaded     | 56-57%            | 0                 |

Moving rasterization to the worker dropped the emulation thread's budget share by
about 40 points during the heavy scene, matching the 40% raster share from step 0,
and eliminated the audio underruns the synchronous build hit when it saturated.
The light early-boot phase is similar in both, since there is little raster to
offload. The spin-based gpu_sync fence cost no measurable time here, as expected:
the worker's ~40% of the frame finishes before the emulation thread's ~60% of
non-raster work, so the present fence rarely waits.

This confirms the threaded GPU delivers the predicted payoff on polygon workloads.
It stays off by default pending the remaining polish (gpu_status safety, a proper
worker wakeup instead of the spins, and a readback-fence stress), tracked in
docs/threaded-gpu.md.
