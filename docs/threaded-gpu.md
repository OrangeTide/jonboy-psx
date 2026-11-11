# Threaded GPU design

Status: proposed, not yet implemented. This is the plan for moving software
rasterization off the emulation thread onto a dedicated GPU worker, so heavy
scenes stop competing with the audio budget.

## Why

The software rasterizer (`draw_triangle` / `draw_polygon` in `gpu.c`) runs
synchronously inside GP0 command processing, which the emulation thread drives
through `psx_gpu_send_command_gp0`, either from a direct MMIO write or from a DMA
channel 2 command list. So every triangle a game draws is billed to the same
thread the audio ring depends on staying ahead of. A heavy scene can push that
thread past the 16.67 ms frame budget, and audio underruns follow. The benchmark
confirms the core (which includes raster) is the limiter; the transpose and
publish work has already been trimmed, and tick batching is in. Rasterization is
the largest remaining block of emulation-thread work that a different core could
do in parallel.

This is the model duckstation and pcsx-redux use. The emulation thread produces a
stream of GPU commands; a GPU thread consumes it and draws into VRAM. The two
only meet where the CPU reads VRAM back, which is rare.

## Step 0: measure the ceiling first (done)

Before building anything, measure how much per-frame time rasterization actually
is, so the payoff is known. `--bench` now times `execute_completed_gp0_command`
(gated by gpu_profile in gpu.c) and reports a per-frame raster line as a subset of
the budget. That number is the upper bound on what a GPU worker can remove from
the emulation thread. The result (recorded in `PERF_JOURNAL.md`):

| workload     | raster share of budget |
|--------------|------------------------|
| BIOS logo (polygon heavy)         | 31-40% |
| FFT tutorial battle (3D gameplay) | 20% (median 25%) |
| Chrono Trigger intro (24-bit FMV) | 1% |

The payoff is strongly workload dependent. On polygon-driven scenes it is 20-40%
of the budget, confirmed on real gameplay (the FFT Orbonne battle, a GTE-driven
isometric 3D map). The boot logo's polygon-heavy frames are also the ones that
currently overrun 16.67 ms, so threading the GPU targets the cases that need it.
On FMV it is negligible, since that path is MDEC and upload bound, not
rasterization. So the work is worth doing, with its value on 2D/3D gameplay, not
video. The figure excludes CPU-to-VRAM image upload, so it is a lower bound.

## Ownership model

The design works because each piece of state has one clear owner.

- **Main RAM** (`system_ram`): the emulation thread only. DMA command lists are
  walked by the emulation thread (`dma_gpu_linked_list` reads RAM and feeds words
  in), so the worker never touches RAM. This keeps the bus single-threaded.
- **VRAM** (`vram.canvas`): the GPU worker only, for both writes (draws, fills,
  copies, CPU-to-VRAM payload) and reads (texture and CLUT sampling). The
  emulation thread touches VRAM only at a fence (below).
- **GPU drawing state** (the `gpu` struct's FIFO, draw area, draw offset,
  texpage, texture window, `gpu_status`): the worker only. These are mutated by
  GP0 and by the drawing-related GP1 subcommands, which now run on the worker.
- **GPU display state** (`disp_x`, `disp_y`, `disp_w`, `disp_h`, `disp_blank`,
  `disp_24bpp`, the display ranges): the emulation thread writes it from the
  display GP1 subcommands, and the main thread reads it at present time. This is
  already a benign cross-thread read today. It stays on the emulation thread and
  does not go through the GPU queue, because it is independent of VRAM contents.

The `gpu` struct already separates these two groups of fields cleanly, so the
split is a discipline over who writes which fields, not a struct rewrite.

## Architecture

```
  emulation thread                 GPU worker thread
  ----------------                 -----------------
  MMIO GP0 write  --\
                     >-- command ring --> pop word --> existing GP0 state
  DMA ch2 list    --/   (GP0/GP1 words)                machine + rasterizer
      |                                                     |
      | display GP1 (disp_*)                                v
      v                                               writes VRAM
  present reads disp_*                                reads VRAM (textures)
```

The emulation thread's `psx_gpu_send_command_gp0` becomes a push into a ring of
32-bit words. The worker pops words and runs the *existing* `gpu.c` GP0 state
machine unchanged, so the parser, FIFO assembly, primitive decode, and rasterizer
move as a unit. The command stream is identical and processed in the same order,
so the VRAM result is bit-for-bit what the synchronous path produces. Only the
timing of when a draw lands relative to a CPU read differs, and the fences below
make that safe.

### The command queue

- A single-producer, single-consumer ring of `uint32_t` words, the same shape as
  the SPU output ring in `spu.c` (C11 atomic head and tail with release/acquire
  ordering). One queue preserves issue order across both MMIO writes and DMA
  command lists, which is what correctness needs.
- Sized to hold at least a few frames of commands so normal bursts never block.
- Backpressure: if the ring fills, the producer waits for space. Under a
  sustained overload the emulation thread blocks on the worker, which is correct
  (the CPU cannot run unboundedly ahead of the draws it will later read back),
  and in that case the workload is GPU-bound and threading has already bought the
  one extra core it can.
- GP0 and GP1 both enter the queue as tagged words so their relative order is
  preserved. Display GP1 is the exception and is handled inline on the emulation
  thread (it does not touch VRAM).

### The worker loop

```
  while running:
      word = ring_pop()            # blocks (futex/condvar) when empty
      feed word to the GP0/GP1 state machine   # existing gpu.c code
```

The worker owns all VRAM and drawing-state mutation. It signals the producer when
it drains below the fence watermark and when a requested fence completes.

## Synchronization points

These are the whole problem. Every place the emulation thread (or main thread)
reads VRAM or worker-owned drawing state is a point where the worker must have
caught up.

### Audit result (step 2, complete)

The exhaustive sweep is done and confirms the list below is complete. Findings:

- VRAM (`vram.canvas`) is touched in exactly three files: `gpu.c` (texture reads
  and primitive writes, all worker-internal), `vram.c`
  (`psx_gpu_write_pixel_payload` for CPU-to-VRAM, worker-side, and
  `psx_gpu_read_pixel_payload` for the VRAM-to-CPU readback), and `main.c`
  (`publish_frame` and the headless `dump_vram_ppm`). No other subsystem touches
  VRAM directly; MDEC output reaches the GPU through GP0/DMA, so it enters via the
  queue like any other command.
- Outside `gpu.c`, the only `gpu` struct fields read anywhere are display state
  (`disp_x`, `disp_y`, `disp_24bpp`, `disp_blank`), all in `main.c` present and
  publish. No drawing-state field (the FIFO, draw area, draw offset, texpage,
  texture window, `gpu_status`) is read across the thread boundary. Drawing state
  reaches the CPU only through the GPUREAD and GPUSTAT register functions.

The complete set of cross-thread points:

| # | point | code sites | fence |
|---|-------|------------|-------|
| 1 | VRAM-to-CPU readback (GPUREAD, GP0 0xC0) | `mem.c:437`, `dma.c:210` into `vram.c psx_gpu_read_pixel_payload` | drain the queue, then read; the drain also processes the 0xC0 setup (`rd_*`, `is_read_active`) the worker owns |
| 2 | present snapshot | `main.c publish_frame` | worker publishes at frame end, or drain at VBLANK |
| 3 | GPUSTAT | `mem.c:440` | always-ready now; add a queue-depth busy bit only if a title needs it |
| 4 | GP1 reset (0x00) / FIFO clear (0x01) | `mem.c:531` into `psx_gpu_send_command_gp1` | route through the queue as control tokens so ordering holds |

Confirmed non-fences:

- Command writes in: GP0 (`mem.c:527`, `dma.c:186`, `dma.c:208`) and GP1 display
  subcommands (`mem.c:531`) are one-way into the queue.
- GP1 display subcommands (0x03/05/06/07/08) write display state on the emulation
  thread and present reads it, which is already a cross-thread read today and is
  independent of VRAM, so it is unchanged and needs no fence.
- The headless `dump_vram_ppm` runs only in the deterministic modes, where the GPU
  is synchronous and there is no worker, so it needs no fence.
- No GP1 0x10 (GPU info / register readback) is implemented, so GPUREAD never
  returns drawing-state registers today. If 0x10 is added later it becomes a fifth
  fence: drain, or publish the handful of small register values.

The rest of this section is the detailed discussion of each point:

1. **VRAM-to-CPU readback.** A game issues GP0 0xC0, then drains the result
   through GPUREAD (`psx_gpu_read_vram_or_gp0`), directly or via a DMA channel 2
   block read (`dma_gpu_block` with `from_ram == 0`, which writes
   `psx_gpu_read_vram_or_gp0()` into RAM). The read must reflect every draw issued
   before the 0xC0. Fence: when the emulation thread reaches a GPUREAD that needs
   VRAM, it drains the queue (waits until the worker has consumed every word up to
   that point), then reads. This is the one fence that can stall the emulation
   thread, so it matters how often games use it. Most do not use it per frame;
   the ones that do (some effects, readback-based tricks) pay a drain.

2. **GPUREAD register info (GP1 0x10).** GPUREAD can also return drawing-state
   registers (draw area, draw offset, texture window), which the worker owns.
   Same fence, or a published snapshot of those small values updated as the worker
   consumes them.

3. **Present snapshot.** `publish_frame` copies VRAM on the emulation thread once
   per frame. With a worker, VRAM is the worker's. Two options: (a) drain the
   queue at the VBLANK boundary before publishing, which couples the publish to
   the worker finishing the frame, or (b) have the worker itself publish the
   snapshot when it finishes the frame's commands, so the emulation thread never
   reads VRAM. Option (b) is cleaner and keeps the publish copy on the worker,
   not the emulation thread. The frame boundary is a VBLANK, which the scheduler
   already marks.

4. **GPUSTAT busy bits.** Today `psx_gpu_get_status` always reports ready
   (bits 26/27/28). With a queue this stays correct for "ready to receive," since
   the ring absorbs commands. The one subtlety is a game that polls GPUSTAT for
   "drawing finished" before reading VRAM or before a buffer swap. If such a game
   appears, reflect queue-non-empty as a busy bit. Start always-ready and add the
   busy bit only if a real title needs it, to avoid modeling cost no game exercises.

5. **GP1 reset (0x00) and FIFO clear (0x01).** These affect the drawing FIFO and
   state the worker now owns, so they route through the queue as control tokens
   rather than running inline, to keep ordering with surrounding GP0. A full GPU
   reset drains the queue, resets both the worker's drawing state and the display
   state, and resumes.

## Timing and interrupts

- The DMA channel 2 completion interrupt currently fires a few cycles after the
  transfer (`psx_dma_tick` defers it). With the worker, the DMA transfer only
  walks RAM and enqueues words, which is fast; the completion interrupt fires on
  the same schedule as before. The draw finishes later, asynchronously. This
  matches real hardware, where DMA-done and draw-done are separate events. A game
  that waits for the DMA interrupt and then reads VRAM is covered by the readback
  fence.
- The GPU interrupt request (GP0 0x1F, `IRQ_GPU`) is raised by the worker when it
  processes that command, and posted to `cpu.i_stat` through the same atomic path
  other devices use. It is rarely used.

## Determinism

The threaded path is windowed-only, exactly like the CD prefetch worker and the
emulation thread itself. The headless, `--boot`, `--exe`, and `--gdb` modes keep
the synchronous GP0 path (worker disabled, draws run inline as today), so
conformance and the boot regression stay bit-for-bit reproducible. A build-time
or run-time flag selects the path, defaulting to synchronous for every
deterministic mode and threaded for the windowed build.

Because the worker runs the same `gpu.c` code on the same command stream in the
same order, the VRAM it produces is identical to the synchronous path. The
threaded build is not a different renderer; it is the same renderer on another
thread, with fences where the CPU looks at the result.

## Testing

- **Differential test (built).** `make gpu-thread-test` replays a deterministic
  command stream through both the synchronous path and the ring plus a real worker
  thread and asserts byte-identical VRAM. It runs clean under ThreadSanitizer.
  This is the primary correctness gate for the threading machinery. It covers
  VRAM-write equivalence and the ring, worker, and drain under real concurrency;
  it does not yet emit a GP0 0xC0 readback, so the readback fence still wants a
  separate stress (a game trace, or a stream that reads VRAM back and compares).
- **Readback-heavy workload.** The 24-bit FMV path (Lunar) exercises CPU-to-VRAM
  and VRAM reads heavily and is the natural stress test for the fences. The MDEC
  tests feed the GPU through DMA and are a second check.
- **Boot regressions.** Chrono Trigger and Final Fantasy Tactics boot correctly
  in the windowed build with the worker on, visually and against the synchronous
  VRAM dump.

## Rollout

1. Measure the raster fraction (step 0) and record it. Done: 20-40% of the budget
   on polygon workloads, 1% on FMV (see PERF_JOURNAL.md).
2. Audit every emulation-thread and main-thread read of VRAM and worker-owned
   drawing state, and enumerate the fences. Done: see "Audit result" above. Four
   cross-thread points, VRAM touched in three files only, no drawing-state read
   crosses the boundary outside the register functions.
3. Add the command ring and the worker, behind a flag, running the existing GP0
   handler. Done, and it folds in the drain fences rather than shipping a racy
   intermediate: the GP0 ring and the worker thread are in place (gpu.c
   gpu_submit_gp0 / gpu_worker_pop_and_run / gpu_sync, worker in main.c), GP0 from
   the bus and DMA is submitted to the worker, and the three drain fences from the
   audit are wired, the readback fence inside psx_gpu_read_vram_or_gp0 (points 1
   and the readback part of 2), the present fence in publish_frame (point 3a, the
   drain-at-VBLANK option), and the reset/FIFO-clear drains in GP1 0x00/0x01
   (point 5). GPUSTAT is left undrained (point 4), always-ready. Enabled by
   JONBOY_GPU_THREAD, default off; the deterministic modes never set it and stay
   bit-exact. Validated visually: the BIOS boot logo (polygon rasterization) and
   the Chrono Trigger intro (24-bit FMV, CPU-to-VRAM image upload) both render
   correctly with the worker on, no tearing, clean shutdown. Conformance stays
   29/29 with the flag off.
4. Remaining before default-on:
   - Differential test: done. `make gpu-thread-test` (test_gpu_thread.c) replays a
     deterministic ~31k-word stream (fills, polygons, gouraud, CPU-to-VRAM
     uploads, VRAM-to-VRAM copies) through both the synchronous path and the ring
     plus a real worker thread, and asserts the final VRAM is byte-identical. It
     passes repeatedly and is clean under ThreadSanitizer (`setarch -R` with
     `-fsanitize=thread`), which confirms the release/acquire ring ordering and
     the gpu_sync happens-before have no data race.
   - Readback-heavy validation (Lunar 24-bit FMV, MDEC tests) to exercise the
     readback fence hard. The differential test covers VRAM-write equivalence and
     the ring/worker/drain concurrency; it does not emit GP0 0xC0 readback, so the
     readback fence still wants a dedicated stress.
   - Make gpu_status access safe. GP0 0xE1 (texpage) on the worker updates
     gpu_status bits that GPUSTAT reads on the emulation thread, a benign race
     today (games poll the ready bits, which are always set) but to be made clean
     (publish or atomic) before the flag goes on by default.
   - Replace the spin waits (gpu_sync, ring-full backpressure) and the worker's
     SDL_Delay(0) idle with a proper wakeup (semaphore or condvar), so a drain
     does not burn a core.
   - Measure the emulation-thread budget drop with the windowed `JONBOY_PERF` HUD
     under a heavy scene, and record it against the step 0 raster baseline.
5. Turn it on by default for the windowed build once the above are done.

## Expected payoff and how it is measured

The win is the raster fraction measured in step 0, removed from the
emulation-thread budget and run in parallel on another core. On a workload where
rasterization is, say, a third of the per-frame core time, the emulation-thread
budget drops by about that much, which is headroom for the CPU interpreter and
audio. The ceiling is one extra core of parallelism: once the worker is itself
saturated on a heavy scene, backpressure couples the threads again and the
benefit is capped at the slower of the two stages.

`--bench` is single-threaded, so it does not run the worker. It measures the
payoff indirectly: the step 0 raster line is the work that moves off the budget,
so the post-change emulation-thread budget is approximately the pre-change budget
minus that line. The live `JONBOY_PERF` HUD on the windowed build, under a heavy
scene, is the direct confirmation that the emulation thread's share of real time
dropped.

## Open questions

- Whether any target game polls GPUSTAT for draw completion, which decides if the
  busy bit is needed (point 4).
- Whether to publish the frame snapshot from the worker (point 3b) or fence at
  VBLANK (point 3a). The worker-publish option keeps all VRAM access on one
  thread and is preferred unless it complicates the frame-handoff triple buffer.
- Queue sizing and whether words or pre-parsed primitives are enqueued. Words are
  the smaller change and reuse the parser; primitives would move the FIFO parse
  onto the emulation thread, which is the opposite of the goal, so words are
  preferred.
