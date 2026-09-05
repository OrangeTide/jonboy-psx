# JonBoy

A PlayStation 1 (R3000A) emulator, under construction.

## Building

The windowed emulator uses SDL3. On Linux, install SDL3 and its
`pkg-config` file, then build:

```
make
```

`make` produces:

- `_out/jonboy`, the emulator.
- `_out/test1.cart` and `_out/test2.cart`, the CPU self-tests (see below).

### Test toolchain

The self-test carts are assembled from MIPS assembly, so building them needs a
`mipsel-unknown-elf` binutils toolchain (`as`, `ld`, `objcopy`). If that
toolchain is absent, `make _out/jonboy` still builds the emulator on its own.

## Running

```
./_out/jonboy
```

This opens the SDL window and starts the CPU at the BIOS reset vector
(`0xBFC00000`). Place a BIOS image named `bios.bin` in the working directory to
have it mapped and executed. Without one, the emulator still runs, against an
empty ROM. Press Escape or close the window to quit.

## Testing

There are two layers of tests: small self-tests that ship with the source, and
an external conformance suite run against third-party test ROMs.

### Headless modes

The emulator runs without a window for testing. Two modes are available:

- `--headless <file>` loads a flat binary into RAM and runs it. Options:
  `--load-addr <hex>` (physical load address, default 0), `--pc <hex>` (entry
  point, default the load address), and `--steps <n>` (instruction budget).
  When it stops it prints all registers, `$s0`, and a memory window.

- `--exe <file.exe>` loads and runs a PlayStation executable (PS-EXE). It reads
  the entry PC, GP, and SP from the header. Options: `--steps <n>` and
  `--vram-out <file>` to write the visible 320x240 framebuffer region as a PPM
  image.

### Self-tests

Two hand-written tests live in the source tree and assemble into cart images.
Both use the `BREAK` instruction as a monitor trap: under `--headless` it stops
the CPU and the harness reports `$s0`. Success is `$s0 == 0xDEADBEEF`.

- `test1.asm` covers register and immediate arithmetic, sign extension, logical
  and shift operations, and the branch and load delay slots. It is assembled
  with `.set noreorder` so the delay slots are exactly as written.

- `test2.asm` covers the exception path. It installs a handler at the general
  exception vector (`0x80000080`), triggers arithmetic overflow, a syscall, and
  an unaligned load, then checks that each vectored correctly, left its
  destination register untouched, and returned through `RFE`.

Run them:

```
./_out/jonboy --headless _out/test1.cart --steps 3000
./_out/jonboy --headless _out/test2.cart --steps 3000
```

Each should end with `RESULT $s0 = 0xDEADBEEF`.

### CPU conformance suite

The CPU instruction set is validated against Peter Lemon's per-opcode PSX CPU
tests. Each test is a bare-metal PS-EXE that computes results and draws a green
`PASS` or red `FAIL` glyph per subtest into VRAM. The `--exe` runner reads the
verdict back by sampling the color of the result column, so no display is
needed.

These reference ROMs are not part of this repository. Place them under `REF/`,
keeping their original layout, so the tests are found at
`REF/PSX/CPUTest/CPU/<OP>/*.exe`. The `REF/` directory is ignored by git.

Run one test and save its rendered framebuffer:

```
./_out/jonboy --exe REF/PSX/CPUTest/CPU/ADD/CPUADD.exe --vram-out add.ppm
```

Run the whole suite and get a pass/fail tally:

```
make conformance
```

All 29 opcode tests currently pass. This covers ADD, ADDI, ADDIU, ADDU, AND,
ANDI, DIV, DIVU, MULT, MULTU, NOR, OR, ORI, SUB, SUBU, XOR, XORI, the shifts
(SLL, SLLV, SRA, SRAV, SRL, SRLV), and the loads and stores (LB, LH, LW, SB,
SH, SW).

Amidog's `psxtest_cpu.exe` is not yet usable as an automated check. It renders
its output with GPU polygon primitives, which are not implemented yet, so it
produces a blank framebuffer. It is expected to work once primitive
rasterization and VBLANK timing are in place.
