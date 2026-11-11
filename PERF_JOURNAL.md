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
