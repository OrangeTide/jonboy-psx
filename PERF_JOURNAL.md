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
- The benchmark measures the core only. The windowed build's VRAM publish copy
  and present-time transpose are separate main-thread work and are not counted
  here. Measure those by their effect on the live build.

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
