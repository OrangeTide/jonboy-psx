# JonBoy PlayStation 1 Emulator: Code Review

Original review date: 2026-09-05, against commit `c1065e3`.
Reconciled: 2026-09-05, after Milestones 0 through 2.
Scope: full source tree (cpu, mem, dma, gpu, vram, timer, pad, psio, rom, main).

Status legend used below: **Fixed** (resolved), **Partly done** (some of it
landed), **Open** (not started). The original diagnosis text is kept for
context; the status line under each item reflects the current tree.

## 1. Summary

This started as an early skeleton with the pieces not wired together. Since then
the CPU work is done: the core now runs real code. Every memory access goes
through address translation and the MMIO/region decoder, the BIOS is mapped, the
reset vector is set, the full common instruction set and the exception path are
implemented, and the branch and load delay slots are correct. The emulator
passes its two self-tests and all 29 Peter Lemon CPU conformance tests, and it
can boot a PSIO firmware image through an injected boot stub.

What remains is mostly the graphics and peripheral subsystems: GPU primitive
rasterization, VBLANK and real timers, the controller protocol, and the audio,
CD-ROM, and GTE units. Those are tracked in the milestone roadmap (section 10).

The notes below rank the original blocking mistakes first, then document each
gap by subsystem with its current status.

## 2. What is already correct

These parts are sound and worth keeping:

- Little-endian read/write macros in `mem.h` are correct.
- Instruction field extraction macros in `decode.h` are correct, including
  sign extension.
- The CP0 Status Register bit definitions in `cpu.h` are thorough and accurate.
- The branch-delay-slot concept is present and the general two-phase idea is
  right, even if the implementation has issues (see 4.3).
- The DMA ordering-table clear (OTC, channel 6) is a reasonable implementation.
- The GP0 packet-length table in `gpu.c` is a good start.

## 3. Blocking bugs (fix these first)

These stopped the emulator from running anything at all. **All four are now
Fixed** (Milestones 0 and 1).

### 3.1 The CPU bypasses address translation and MMIO entirely
**Fixed.** `step_cpu` fetches and accesses memory through `memory_read_*` /
`memory_write_*`, which translate and dispatch MMIO.

`step_cpu()` fetches and loads with the raw `MEM_READ_32(cpu.PC)` macro, which
indexes `system_ram[]` directly. It never calls `memory_read_word()` /
`memory_write_word()` from `mem.c`. Consequences:

- No virtual-to-physical translation (`TO_PHYSICAL`). The reset PC and all
  kernel-segment code live at `0x8xxxxxxx` / `0xBFC00000`. Indexing
  `system_ram[0xBFC00000]` is a wild out-of-bounds access.
- No MMIO dispatch on loads or stores. The entire `read_hardware_word` /
  `write_hardware_word` path, and therefore the GPU command ports, the DMA
  triggers, the interrupt controller registers, the pad and timer, are dead
  code from the CPU's point of view.

Fix: route every CPU memory access through `memory_read_word` /
`memory_write_word` (and byte/halfword variants once they exist).

### 3.2 The reset vector is never set
**Fixed.** `cpu_reset()` clears state and sets the entry PC; the windowed and
boot paths reset to `0xBFC00000`.

`cpu` is a zero-initialized global, so `cpu.PC == 0` at startup. A real
PlayStation resets to `0xBFC00000` (BIOS entry). There is no CPU init routine
called from `main`. Execution starts reading `system_ram[0]`.

Fix: add `cpu_reset()` that sets `cpu.PC = 0xBFC00000` and clears register
state, and call it before the main loop.

### 3.3 The BIOS is loaded but never mapped
**Fixed.** `mem_load_bios` fills a `bios_rom[]` array and the decoder maps the
BIOS region at `0x1FC00000`. (`rom.c`/`rom_data[]` is now vestigial.)

`rom_load()` reads the file into a private `rom_data[]` buffer (the code even
carries a `TODO` about this). Nothing copies it into the address space, and
`memory_read_word` has no branch for the BIOS region (physical `0x1FC00000`).
Even with a correct PC there is no code to fetch.

Fix: give the BIOS its own backing store and decode `0x1FC00000..0x1FC7FFFF`
in the memory dispatcher, or map it as a region.

### 3.4 `rom_load` return contract is inverted
**Fixed.** `rom_load` returns `0` on success and `main` checks it correctly;
the size comparisons use `size_t`.

`rom_load` returns `1` on success and `0` on failure. `main.c` does
`if (result != 0) { ...cleanup...; return 1; }`. So a successful BIOS load is
treated as fatal and the program exits, while a missing file (returns `0`)
lets `main` continue with blank RAM. The logic is backwards in both directions.

Fix: settle on one convention. Recommend `0 == success`, negative == error,
and update both sites. Also note `fread` returns `size_t`; comparing it to
`-1` never triggers.

## 4. CPU correctness gaps

**All items in this section are now Fixed** (Milestones 1 and 2).

### 4.1 Almost no instructions are implemented
**Fixed.** The full common instruction set is implemented (SPECIAL, REGIMM,
immediate ALU, `LUI`, `J`/`JAL`, and all loads/stores including the unaligned
`LWL`/`LWR`/`SWL`/`SWR`). Verified by `test1.asm` and the 29 conformance tests.

Handled: `BEQ`, `BNE`, `J`, `ADDIU`, `LW`, partial `COP0`. The `SPECIAL`
group (opcode `0x00`) is an empty `break`, so none of `ADD/ADDU/SUB/SUBU/
AND/OR/XOR/NOR/SLT/SLTU/SLL/SRL/SRA/SLLV/SRLV/SRAV/JR/JALR/MULT/MULTU/DIV/
DIVU/MFHI/MFLO/MTHI/MTLO/SYSCALL/BREAK` exist. Also missing at the primary
opcode level: `LUI`, `ORI`, `ANDI`, `XORI`, `ADDI`, `SLTI`, `SLTIU`, `JAL`,
the `REGIMM` group (opcode `0x01`: `BLTZ/BGEZ/BLTZAL/BGEZAL`), `BGTZ`,
`BLEZ`, and every store and sub-word load: `SB/SH/SW/LB/LBU/LH/LHU/LWL/LWR/
SWL/SWR`. The bundled `test1.asm` uses `ori`, `li` (which is `lui`+`ori`),
`sll`, `sra`, `or`, `and`, `not` (`nor`), `sw`, `add`, `break`. Most of these
are unimplemented, so the test cannot pass yet.

### 4.2 HI / LO registers are missing from the CPU state
**Fixed.** `hi`/`lo` are in the CPU state and `MULT`/`MULTU`/`DIV`/`DIVU` and
`MFHI`/`MTHI`/`MFLO`/`MTLO` are implemented, including the divide edge cases.

`R3051_CPU` has no `hi`/`lo` fields, so `MULT`/`DIV`/`MFHI`/`MFLO` cannot be
implemented. Add them.

### 4.3 Branch-delay handling is fragile and has dead code
**Fixed.** Replaced with a `PC`/`next_pc` model; `current_pc`/`in_delay_slot`
feed correct EPC/BD reporting. One known simplification remains: in a
load-delay slot that writes the loaded register, the load currently wins (not
exercised by any current test).

The `DELAY_STATE_JUMPING -> DELAY_STATE_DELAY_SLOT` transition is written
twice (once at the top, once again a few lines down). The second block can
never fire. The overall flow happens to work for the simple cases, but the
duplication signals the state machine was not fully reasoned through. It also
does not defend against a branch in a delay slot. Rewrite it as a single clear
transition, and consider tracking `next_pc` explicitly rather than a state
enum.

### 4.4 No store path from the CPU
**Fixed.** All stores route through `memory_write_*`, honoring the SR cache
isolation bit.

Even once `SW` is decoded, there is no CPU call into `memory_write_word`, so
the CPU currently cannot write RAM or any hardware register. This is the store
half of 3.1.

### 4.5 No byte or halfword memory access
**Fixed.** `memory_read/write_byte` and `_halfword` exist and the sub-word
load/store opcodes use them.

`mem.c` only exposes word functions. Many registers and most game data use
8- and 16-bit access. Add `memory_read_byte/halfword` and matching writes, and
wire the sub-word load/store opcodes to them.

### 4.6 Exception and interrupt handling is partial
**Fixed.** A shared `raise_exception` handles SYSCALL, BREAK, arithmetic
overflow, and address errors, with correct EPC/BD and the BEV vector choice.
Interrupts are recognized at instruction boundaries using the full CAUSE.IP vs
SR.IM comparison. Covered by `test2.asm`.

`check_hardware_interrupts` only inspects `SR_IM2` and only vectors a single
combined line. It hardcodes `0x80000080` and ignores the `BEV` bit (which is
set at boot, so the real boot vector is `0xBFC00180`). It does not populate the
`CAUSE` IP bits from `i_stat & i_mask` per line. `SYSCALL`/`BREAK` exceptions
are not raised at all. This is acceptable as a simplification for now but must
be revisited before the BIOS boots.

## 5. Memory map gaps

### 5.1 RAM size and mirroring
**Fixed.** `mem_read`/`mem_write` decode by region: 2 MB RAM mirrored across
`0x00000000..0x007FFFFF`, scratchpad, BIOS, the PSIO windows, and MMIO, with
bounds guards.

`system_ram` is 4 MB (`4096*1024`); PS1 main RAM is 2 MB, mirrored four times
across `0x00000000..0x007FFFFF`. There is no region decoding: `TO_PHYSICAL`
masks to `0x1FFFFFFF` (512 MB) and then indexes `system_ram[p_addr]`. Any
physical address above 4 MB (BIOS, scratchpad, most of the map) is an
out-of-bounds index. Introduce a proper region decoder (RAM, scratchpad, MMIO,
BIOS, expansion) instead of one flat array.

### 5.2 Scratchpad collides with the MMIO base
**Fixed.** The decoder checks the scratchpad region (`0x1F800000..0x1F8003FF`)
before the I/O block, so the two no longer overlap in practice.

`MMIO_BASE` is set to `0x1F800000`, but that address is the scratchpad
(D-cache used as fast RAM). Hardware registers start at `0x1F801000`. The
current code treats scratchpad accesses as MMIO and its offset math
(`offset >= 0x1000`) skips exactly the scratchpad region. Split scratchpad
(`0x1F800000..0x1F8003FF`) from the I/O block (`0x1F801000..`).

### 5.3 The DMA write dispatch has contradictory branches
**Open.** The hardcoded `0x10A8`/`0x10AC`/`0x10B0` cases in `write_hardware_word`
still coexist with the generic DMA decoder and should be removed.

In `write_hardware_word`, the generic DMA decoder (`0x1080..0x10E8`) already
handles per-channel `madr`/`bcr`/`chcr` and triggers transfers. The following
`switch` then also has hardcoded cases `0x10A8`/`0x10AC`/`0x10B0` that call
`psx_dma_set_channel_base(0, ...)` and friends. Those offsets belong to
channel 2, not channel 0, and the generic block does not `return`, so both
run. Remove the hardcoded cases and let the generic decoder own all channel
registers.

## 6. DMA gaps

**Partly done.** `run_dma_transfer` now dispatches per channel through a shared
`dma_finish` completion path (OTC on channel 6, PSIO SD reads on channel 3),
and `dma_finish` uses the correct DICR enable/flag fields.

Still open:
- Linked-list mode (SyncMode 2), which the GPU uses to stream command lists, is
  missing. This is the single most important DMA path for graphics.
- Block mode (SyncMode 1) is missing for the GPU/SPU channels.
- Unimplemented channels still run `dma_finish` (deliberately, so the guest is
  not left waiting on the start bit), but move no data.
- DMA does not feed the GPU yet.

## 7. GPU gaps

### 7.1 Copy-paste error breaks CPU-to-VRAM transfer
**Fixed.** The `0xA0` CPU-to-VRAM transfer sets up the streaming state
correctly and `0x02` fill-rect is implemented; the framebuffer renders (proven
by the conformance tests, which draw their PASS/FAIL glyphs this way).

In `execute_completed_gp0_command`, the `0xA0` case only adjusts packet length,
while the code that actually sets `vram.transfer_*` and `is_transfer_active`
sits under `case 0x40:` (labeled, incorrectly, as the `0xA0` handler). `0x40`
is a line-draw opcode. As a result the image-upload path never arms, and the
one working pixel route in the whole GPU is unreachable.

### 7.2 No rasterization
**Open.** This is the main remaining GPU work (Milestone 4).

Triangle, quad, rectangle, and shaded-primitive rasterization are all stubbed
(`psx_rasterize_triangle` is commented out). Only raw pixel upload would draw
anything, and per 7.1 that path is currently broken. Nothing renders.

### 7.3 VRAM layout and the render loop
**Open.** Still column-major; not a correctness bug, worth flipping later.

`vram.canvas` is declared `[VRAM_WIDTH][VRAM_HEIGHT]` and indexed `[x][y]`,
which is column-major and strided for a row-major blit. The
"optimized for cache line streaming" comment in `main.c` describes the
opposite of what the access pattern does. VRAM is conventionally addressed
`[y][x]`. This is not a correctness bug for display, but the layout should be
flipped for both clarity and speed.

### 7.4 GP1 and status are minimal
**Open.** Unchanged since the original review.

Display-mode, display-area, and horizontal/vertical range GP1 commands are not
handled. `psx_gpu_get_status` fakes the ready bits, which is fine for now, but
it never reflects the real drawing or DMA state.

## 8. Timing, interrupts, and peripherals

**Mostly open.** The interrupt-recognition path itself is now correct (see 4.6),
but everything that would drive it, and the peripherals, are still stubs.

- The frame loop is placeholder timing. `CYCLES_PER_FRAME` is 1000 (debug),
  `TARGET_FPS` is 15, yet the comment claims "564,480 instructions to match
  60Hz". None of the three agree. There is no per-instruction cycle counting.
- No VBLANK interrupt is ever raised (`i_stat` bit 0). Any game that waits on
  VBLANK will hang forever. The renderer should drive the interrupt controller
  once per frame.
- `timer.c` is a fake counter that increments by 4 on every read, unrelated to
  real time. Root-counter modes, targets, and their interrupts are absent.
- `pad.c` always reports "no buttons pressed" and a fixed status. There is no
  real controller protocol (the SIO/JOY_DATA transfer handshake), so input is
  not yet possible.
- The CD-ROM, SPU (sound), MDEC, and GTE (Coprocessor 2, geometry) are entirely
  absent. GTE in particular is required by nearly every 3D game.

## 9. Build and documentation issues

**Fixed.** The README now documents the SDL3 build, the testing process, and the
`mipsel-unknown-elf` toolchain prerequisite. The test carts are built with
`objcopy -j .text -j .data` and are small (the 4 MB padding is gone).

- `README.md` is inconsistent with the code. It lists `libglfw3-dev` and a
  WASM/WebGL2 path, but the code and Makefile use SDL3, and there is no
  emscripten target. `emmake` alone is also not a command (it wraps make, as
  `emmake make`). Update the README to match SDL3.
- `_out/test1.cart` is 4 MB, which suggests `objcopy -O binary` is padding a
  gap from the link layout. Worth checking the linker script and load address
  once the CPU can actually execute it.
- The `mipsel-unknown-elf` toolchain is required to build the test cart but is
  not mentioned as a prerequisite.

## 10. Milestone roadmap

Each milestone should end in something observable. Build tests before or
alongside the CPU work so you always have a pass/fail signal.

### Milestone 0: Test harness and headless mode (DONE)
Status: done. Headless `--headless` mode dumps registers and memory, `BREAK`/
`SYSCALL` halt the CPU for the harness, and the `rom_load` contract is fixed.
- Add a headless run mode (no SDL window) that loads a flat binary at a known
  address, runs N instructions, then dumps registers and a memory range.
- Add a `BREAK`/`SYSCALL` handler that halts and prints `$s0` so `test1.asm`
  can report which sub-test failed.
- Fix `rom_load`'s return convention and the `main.c` check (bug 3.4).

### Milestone 1: A CPU that passes the bundled self-test (DONE)
Status: done. `test1.asm` reports `$s0 == 0xDEADBEEF`.
- Route all fetches and loads/stores through `memory_read_*` /
  `memory_write_*` (bug 3.1). Add byte and halfword accessors.
- Set the reset vector and add `cpu_reset()` (bug 3.2).
- Add a region decoder for RAM (with mirroring), scratchpad, and BIOS
  (bugs 3.3, 5.1, 5.2).
- Implement the full common instruction set: the `SPECIAL` group, `REGIMM`,
  the immediate ALU ops, `LUI`, `JAL`, and all loads/stores. Add `HI`/`LO`
  (gaps 4.1, 4.2, 4.4, 4.5).
- Rewrite the delay-slot state machine cleanly (gap 4.3).
- Goal: `test1.asm` reports `$s0 == 0xDEADBEEF`.

### Milestone 2: Third-party CPU conformance (DONE)
Status: done. All 29 Peter Lemon per-opcode CPU tests pass via the `--exe`
runner (`make conformance`), and the exception path is implemented and covered
by `test2.asm`. Two follow-ups remain open: Amidog's `psxtest_cpu.exe` needs GPU
primitive rasterization and VBLANK timing before it renders (deferred to later
milestones), and a golden-log comparison against a reference emulator is not yet
set up.
- Run an external MIPS/CPU test suite (see section 11) and drive the pass rate
  up. Add a golden-log comparison against a reference emulator if possible.
- Implement `SYSCALL`/`BREAK`/overflow exceptions and a correct general
  exception path, including the `BEV` vector (gap 4.6).

### Milestone 3: BIOS boots to the shell
- Obtain a legal open BIOS (section 11) and map it correctly.
- Implement the interrupt controller fully, VBLANK generation once per frame,
  and real root counters (timers).
- Implement enough of DMA (linked-list, block) to let the BIOS talk to the GPU
  (section 6).
- Goal: the BIOS animated logo / shell renders.

### Milestone 4: 2D output
- Fix the CPU-to-VRAM transfer opcode bug (gap 7.1) and the VRAM layout
  (gap 7.3).
- Implement flat and Gouraud triangle and quad rasterization, textured
  primitives, and rectangles (gap 7.2).
- Implement GP1 display control so the framebuffer maps to the visible area.
- Goal: a homebrew 2D demo draws correctly.

### Milestone 5: Input and a playable homebrew
- Implement the controller SIO protocol for a digital pad (gap in section 8).
- Goal: a simple homebrew game is playable end to end.

### Milestone 6: 3D and audio
- Implement the GTE (Coprocessor 2).
- Implement the CD-ROM controller and enough of the SPU for sound.
- Goal: a simple commercial disc boots and runs.

## 11. External resources

### Documentation
- nocash PSX specs ("psx-spx", the "Everything You Have Always Wanted to Know
  about the Playstation" document): the definitive hardware reference.
  https://psx-spx.consoledev.net/
- The mirrored/community-maintained version and its GitHub source are searchable
  and copyable, which helps when implementing opcodes and register maps.

### CPU / hardware test ROMs (free, most run without a BIOS)
- PeterLemon/PSX on GitHub: a large suite of small, self-contained test ROMs
  with source, covering CPU, GPU, GTE, DMA, timers, and more.
  https://github.com/PeterLemon/PSX
- Amidog's PSX tests (`psxtest_cpu`, `psxtest_cpu_gte`, etc.): widely used for
  CPU and GTE conformance. Note some require a real BIOS.
- The PCSX-Redux project ships CPU/hardware tests and a well-documented test
  methodology you can mirror.

### Legal BIOS replacement
- OpenBIOS from the PCSX-Redux project: an open-source, freely distributable
  BIOS replacement. Use this instead of a copyrighted Sony BIOS image while
  developing. https://github.com/grumpycoders/pcsx-redux

### Homebrew SDK and sample games (free, buildable from source)
- PSn00bSDK: an open-source PS1 SDK with example programs (2D, 3D, input,
  CD-ROM). The examples make excellent incremental targets and are legal to
  redistribute. https://github.com/Lameguy64/PSn00bSDK
- The PSXDEV community hosts additional free homebrew and tutorials.

### Reference emulators for cross-checking behavior
- PCSX-Redux and DuckStation are readable, actively maintained references. Use
  them to capture golden logs and to compare register or VRAM state when a test
  diverges.

### Development tip
- Keep a reference emulator's instruction trace next to yours and diff them.
  The fastest way to find a CPU bug is the first instruction where the two
  traces disagree.

## 12. Performance notes for low-power targets (Raspberry Pi 3)

Added 2026-09-07. These are performance and portability findings, not
correctness bugs, aimed at running the emulator on a Raspberry Pi 3
(quad-core Cortex-A53, ARMv8-A with NEON, VideoCore IV / GLES 2.0). They are
ranked by payoff per unit of effort. Measure on the actual hardware before
committing to any rewrite: interpreter emulators on the A53 are usually
CPU-interpreter-bound, not fill-bound, so confirm the bottleneck first.

### 12.1 Build at an optimizing level (biggest, cheapest win)
**Open.** The Makefile builds every binary at `-Og` (`CFLAGS=-Wall -W -g -Og`).
Switch to `-O2 -mcpu=cortex-a53 -fomit-frame-pointer` for Pi builds. This is
zero maintenance and zero accuracy risk, and it helps this codebase in specific
ways:
- `draw_triangle` calls `put_pixel`, `blend_555`, `sample_texel`, `unpack_555`,
  `rgb_to_555`, and `modulate` once per pixel. All are file-scope `static`; at
  `-Og` they stay as real calls, at `-O2` they inline into the pixel loop.
- The CPU core double-dispatches: `delay_slot_dest()` runs a full `switch` and
  the main execute path runs another `switch` per instruction. `-O2` turns both
  into jump tables and keeps `cpu` state in registers across a step.
- `-mcpu=cortex-a53` lets GCC schedule for the A53's in-order dual-issue
  pipeline, which is far more scheduling-sensitive than a desktop core.

Expect roughly 2x to 4x whole-program speedup before any other change.

### 12.2 Upload only the visible VRAM rectangle, not all of VRAM
**Open.** `refresh_screen` (`main.c`) transposes and copies the full 1024x512
`vram.canvas` into the streaming texture every frame, but only a
256/320/512/640 by 240/480 slice is ever displayed (`hres[]` in `gpu.c`). That
is roughly 5x to 10x more pixel traffic than needed, and the column-major
`canvas[x][y]` layout makes the transpose read strided and cache-hostile (a
512-short stride per step). Transpose and lock only the current display
rectangle. Small change, real saving on the Pi's memory bandwidth. Related to
gap 7.3 (VRAM layout is column-major).

### 12.3 Rasterizer: integer fixed-point before any SIMD
**Open.** `draw_triangle` steps its edge functions in integers but computes
barycentrics and colors/UVs in `float`, doing int-to-float-to-int conversions
per pixel. On the A53 those conversions are comparatively slow. Convert the
color and UV interpolation to incremental fixed-point stepping (the same way
the edge functions already step). This removes all per-pixel FP with no SIMD
and makes any later vectorization straightforward. Do this before evaluating
NEON.

### 12.4 On the rasterizer rewrite question (NEON vs asm vs GLES)
Guidance, not a defect. None of the three is low-effort, and the rasterizer is
likely not the first bottleneck.
- NEON intrinsics: moderate-to-high effort, moderate payoff. The half-space
  loop vectorizes across a scanline, but PSX primitives are frequently only a
  few pixels wide, so utilization is poor except on large flat/Gouraud fills
  (backgrounds). Worth it only for the large-fill path, and only after 12.3.
- Hand aarch64 assembly: not recommended. Little gain over NEON intrinsics with
  `-O2` A53 scheduling, and it costs portability and the amalgamation build.
- OpenGL ES 2.0 on VideoCore IV: highest ceiling, highest effort and accuracy
  risk. A GLES backend means reimplementing PSX-exact semantics in shaders
  (BGR555, the mask/STP bit, the four semi-transparency modes in `blend_555`,
  dithering, texture windows, CLUT, affine UV) and reconciling the shared
  1 MB VRAM-as-texture-memory model (0x80 VRAM-to-VRAM, 0xA0/0xC0 streaming).
  This is the standard "hardware renderer" project, measured in weeks, and the
  right move only if the goal is upscaling, not a quick perf patch.

### 12.5 Use the other three cores
**Open.** The emulator is single-threaded (`main.c` step loop), and the GPU
draws synchronously inside the DMA/register write path. The Pi 3 has four A53
cores; three sit idle. Decoupling rasterization onto a worker core (a command
queue drained per frame) has a higher ceiling than SIMD-ing the current serial
rasterizer, since it reclaims 3x of the machine. Medium-to-high structural
effort, but more headroom than 12.4.

### Suggested order
1. `-O2 -mcpu=cortex-a53` (12.1), then re-measure.
2. Upload only the visible VRAM rect (12.2).
3. Profile on the actual Pi to confirm CPU-bound vs fill-bound.
4. If fill-bound: fixed-point interpolation (12.3), then NEON the large-fill
   path only.
5. Shelve GLES and hand asm as perf patches; revisit GLES only for upscaling.
