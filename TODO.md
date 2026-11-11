# TODO

## MVP roadmap

The MVP goal: boot the retail BIOS and play one commercial game to interactive
gameplay, with correct-enough video, working audio, controller input, and
memory-card saves. Most subsystems that feed that goal are done and tested (CPU
core, GPU rasterizer, SPU, CD-ROM, MDEC, DMA, controllers, memory cards, the emu
thread, and a gdbserver). The remaining blockers, in priority order:

- [x] **P0: GTE (COP2), the geometry transformation engine.** Implemented in
  gte.c / gte.h with the full register file, the move/load/store ops in cpu.c, and
  the command ops (RTPS/RTPT, NCLIP, MVMVA, the lighting ops, AVSZ3/4, SQR, OP,
  GPF, GPL, and the rest). See the GTE section below.
- [x] **P0: a `gte` conformance pass.** `make gte-conformance` mirrors the CPU
  suite over REF/PSX/CPUTest/GTE. NCLIP and AVSZ both PASS. Only those two ROMs
  are present locally; fetching the rest of the amidog GTE suite into REF/ to
  exercise RTPS/MVMVA/the lighting ops through conformance stays open.
- [x] **P0: first-game bring-up.** Drive one concrete title from BIOS to
  interactive gameplay. FF Tactics boots the retail BIOS from its mounted disc
  and now reaches its first tactical battle from a cold boot under scripted
  input. Progression: title screen (logo, map art, NEW GAME / CONTINUE /
  TUTORIAL / SOUND menu) by ~600M steps; NEW GAME selected with Cross; opening
  cutscene skipped to the name-entry screen; the default name "Ramza" and the
  birthday accepted; the Orbonne Monastery dialogue skipped; the isometric
  tutorial battle map by ~3.7B steps, with the GTE transforming the 3D
  battlefield correctly. Screenshots: assets/fft_title.png,
  assets/fft_orbonne_intro.png, assets/fft_tutorial_battle.png. Reproduce with
  assets/fft_tutorial_battle.script. The old "stall at 0x8001DD34" was a pre-GTE
  artifact: that address is the game's own attract/frame-wait loop, which it
  passes through normally once GTE answers its geometry. Two input facts learned:
  the SIO0 ACK fix below lets the BIOS see the pad, and FFT (USA) confirms
  in-game dialogs with Circle and cancels with Cross (the title menu selects with
  Cross). Still open: chase the thin vertical RGB line artifact over gameplay
  maps, and repeat the bring-up with a second title.
- [x] **P0: SIO0 ACK timing (fixed).** FFT reached the title but never responded
  to input, because it never advanced past selecting the pad (0x01) to the poll
  command (0x42). Disassembling the BIOS controller probe in RAM showed it writes
  the address byte, spins a fixed delay, clears both the SIO0 interrupt and the
  I_STAT pad-interrupt bit, then polls I_STAT for the acknowledge to reappear, and
  treats the port as empty unless it does. The acknowledge delay was 100 cycles,
  which fired it during the pre-clear spin, so the clear wiped it and every probe
  read no controller. Raised SIO_ACK_DELAY_CYCLES to 500, past the clear (which
  lands a few hundred cycles after the byte) and well within the probe's ~1800
  cycle timeout, matching hardware where the acknowledge trails the byte by about
  the baud period. The loader menu polls the port directly rather than through the
  BIOS probe, so it was never affected. pad-test, mc-test, the self tests, and the
  amalgamation stay green.
- [ ] **P1: GPU residuals once GTE lands.** Dithering (absent) and the mask bit /
  GP0(0xE6) set-mask/check-mask (absent). Minor visually but affect correctness on
  real scenes. The rest of the GPU (textures, CLUT, clip, offset, all four
  semi-transparency blend modes, STP bit, 24bpp display) is already in.
- [x] **P1: display range (GP1(06)/(07)).** Both the vertical and horizontal
  display ranges are now honored, so a mode that draws a smaller picture than it
  allows no longer leaks stale VRAM around the edges. Chrono Trigger's 224-line
  screens showed a red/pink band along the bottom after an FMV (the decoded 24bpp
  video sat in the rows below the picture); that band is gone. psx_gpu_active_width
  and psx_gpu_active_height derive the visible size from the ranges, clamped to the
  mode size, and both present and the headless dump use them. Chrono Trigger sets
  the horizontal range to exactly the mode width, so the visible horizontal change
  was only in the headless dump (it had hardcoded 320 columns). The "thin vertical
  RGB line over FFT maps" turned out to be the same GP1(06) issue and is fixed too:
  FFT's title uses a full-width (320) range, but its gameplay maps and battles use
  a 256-wide range, so the old code presented 64 columns of stale texture VRAM down
  the right edge. Honoring the horizontal range blacks that border (verified on the
  Orbonne cutscene map and the tutorial battle).
- [ ] **P1: timer/IRQ accuracy** sufficient for the sync/target modes games gate
  on (root counters exist; confirm against the first-game stall).

P2 polish, explicitly not MVP: loader menu scrolling and backdoor byte-streaming,
the 0x96 keyboard and 0x53 flightstick controllers, a gdb target.xml with real
FPU/GTE registers and gdbserver in the windowed build, the debug hotkey
register/stack dump, and the SPU tail items (CD/reverb output sweeps, 8-bit XA,
Gaussian interpolation).

## controller port

Current state: a DualShock in slot 1 works in both digital and analog mode. pad.c
drives the SIO0 transfer (0x01 address, then the command byte and data) and pulses
the acknowledge interrupt (IRQ_PAD) after each byte but the last. The acknowledge
fires a short delay after the byte (psx_pad_tick, clocked from psx_hardware_tick,
SIO_ACK_DELAY_CYCLES), not on the same cycle as the data write, the way the
hardware pulses /ACK. Raising it synchronously let a driver that clears the SIO0
interrupt right after each byte, then waits for the acknowledge (psyqo's memory
card driver, and the BIOS), swallow it before the wait and read the port as having
no device. Buttons come from
pad_buttons (active low) and the analog sticks from pad_rstick_*/pad_lstick_*. The
SDL window feeds both the keyboard and, when one is connected, a real SDL gamepad:
its sticks drive the analog axes, its buttons/dpad/triggers combine with the
keyboard (active-low AND), and hotplug is handled. Slot 2 reports no device; the
memory card (0x81) answers via mc.c when mounted.

A timed system-event script can also drive the machine (script.c, --script
<file>), clocked by the VBLANK frame counter in sched.c. Each line is
"<frame> <command> ...": `pad <tokens>` sets the buttons and stick axes held from
that frame on; `analog on|off` sets DualShock analog mode; `disc <path>` inserts
or changes the disc and `disc eject` removes it. Pad keyframes are held; the other
events fire once, and the script replays if the frame counter restarts. It runs in
every mode, so it also gives headless runs a way to work the controller and swap
discs (for demos, captures, and driving the menu). Not tied to pad input, so more
event types drop in easily. Unit tested (test_script.c, `make script-test`).
A mid-run eject or disc swap now raises the drive-cover (lid) interrupt:
cdrom_eject and a runtime cdrom_mount deliver an INT5 carrying the shell-open
status bit (0x10), which stays set until a Getstat reads it back with a disc
present. The event is suppressed until the game has enabled CD-ROM interrupts,
so a cold boot with a disc already inserted (whose mount runs before the machine
starts) is unaffected. Unit tested (test_cdda.c). Nuance: the lid interrupt is a
single INT5 for both open and close rather than modeling the motor spin-up and a
separate close event.

- [~] support the 4 main controller port types:
  - [x] Standard Digital Pad ID: 0x41
  - [ ] Standard Analog Flightstick ID: 0x53
  - [x] DualShock Analog Pad ID: 0x73. Powers up in digital mode (0x41) and enters
    analog mode (0x73) through the config command sequence (0x43 enter config, 0x44
    set analog, 0x43 exit) or the ANALOG button (host Tab key, pad_toggle_analog).
    Config mode reports ID 0xF3 and answers the status/capability queries (0x45,
    0x46, 0x47, 0x4C, 0x4D) with the DualShock constants from pcsx-redux. Analog
    poll appends the four stick axes. Input: keyboard (L2/R2/L3/R3 on E/R/C/V,
    ANALOG on Tab) plus a real SDL gamepad when connected (sticks to the axes,
    buttons/dpad/triggers to the pad, guide button toggles analog, hotplug
    handled). Unit tested (test_pad.c, `make pad-test`). Digital poll stays
    byte-identical, so FFT still boots unchanged.
  - [ ] PS1 Keyboard ID: 0x96. High nibble 9 marks the keyboard device class. Low nibble 6 follows the standard controller-ID convention: 6 half-words, so 12 data bytes, of payload. see [[REF/kbpoll.c]], [[REF/keyboard.h]] (modifier flags and scancode table), [[REF/kbpoll-6kro.txt]]
    - Payload size sets the key rollover. The baseline poll in REF/kbpoll.c returns only 3 data bytes (modifier + 2 scancodes), which is a 2-key-rollover simplification and under-delivers what the 0x96 ID advertises. A full 0x96 device returns the 12 data bytes the ID promises (modifier + up to ~8 scancode slots) for 6KRO to 8KRO. Decide which frame size to emulate.
    - SPI Byte-Level Frame Layout (baseline 6-byte frame: 3 header + 3 data)
      - To emulate this in C, you must hook into your emulator's controller port polling loop. When a game like RPG Maker polls a port configured with a keyboard, the transaction looks like this:
      | Byte Index | Command (From PS1) | Response (From Keyboard Adapter) | Description |
      | --- | --- | --- | --- |
      | Byte 1 | 0x01 | 0xFF | Console wakes up the port. |
      | Byte 2 | 0x42 | 0x96 | Main Read Command (0x42). Adapter returns the Keyboard Device ID. |
      | Byte 3 | 0x00 | 0x5A | Command Sync. Adapter returns 0x5A ("Ready to transmit data"). |
      | Byte 4 | 0x00 | DATA 1 | Flags byte (such as Shift, Alt, Control modifiers, and special state bits). |
      | Byte 5 | 0x00 | DATA 2 | Scan Code Byte 1 (The active key being pressed / held). |
      | Byte 6 | 0x00 | DATA 3 | Scan Code Byte 2 (If multiple keys are held simultaneously). |

- [x] memory card support (slot 1). mc.c backs a raw 128 KB image (the common
  .mcr/.mcd format shared by emulators and hardware card readers): 1024 frames of
  128 bytes, frame 0 the directory header. pad.c routes the SIO0 transaction by
  its first byte, 0x01 to the digital pad and 0x81 to the card. The card answers
  read (0x52) and write (0x57): ID bytes 0x5A/0x5D, command acknowledge 0x5C/0x5D,
  the 128-byte frame, the checksum (XOR of the address and data bytes), and the
  end byte (0x47 good, 0x4E bad checksum). The FLAG byte reports directory-unread
  (0x08) until the first read or write. Mount with --memcard <file>; a missing
  file is created as a freshly formatted card, and writes persist per frame to it.
  Get ID (0x53) returns the ID, acknowledge, and geometry bytes (04 00 00 80).
  Unit tested (test_mc.c, `make mc-test`: read, write, persistence, bad-checksum
  rejection, get-id, and the FLAG transition). Slot 2 is supported: mc.c holds two
  cards and pad.c routes a transaction to slot 0 or slot 1 by the SIO port-select
  bit (JOY_CTRL bit 13), so a card in either port answers. Mount them with
  --memcard (slot 1) and --memcard2 (slot 2). The slot-2 routing is unit tested
  through the SIO layer (test_pad.c: a card in slot 2 answers only on port 2,
  slot 1 stays inert). Not done yet: the PocketStation extensions.

## Audio

- [~] add sound support. Output backend and voice synthesis are in. main.c opens
  an SDL3 44.1 kHz stereo stream and drains the SPU each frame; spu.c owns a 512
  KB sound RAM, an output ring, and a 44.1 kHz mixer (psx_spu_tick, one frame per
  768 CPU cycles). The 24 voices decode ADPCM blocks from sound RAM, resample by
  their pitch (linear interpolation), shape with an ADSR envelope (the PEOPS
  coarse/fine counter model, see REF/pcsx-redux/src/spu/adsr.cc), loop on the
  block flags, set ENDX, and mix with per-voice and main volume. Key on/off
  (0x1F801D88/8C) start and release voices. Sound RAM is filled by DMA channel 4
  (dma.c) and the PIO transfer FIFO (0x1F801DA6/DA8). CD-DA is mixed in through
  spu_cd_audio, scaled by the CD (0x1F801DB0/DB2) and main volume and gated by
  the SPUCNT CD-enable bit. Unit tested (test_spu.c, `make spu-test`: CD mix and
  an ADPCM voice); spu.c has no SDL dependency so headless and the tests build
  without it. So sequenced music and sound effects now play. Reverb is in too:
  the Neill Corlett/PEOPS model runs on a work area in sound RAM at 22.05 kHz,
  fed by the reverb-enabled voices (EON) and gated by the SPUCNT reverb-master
  bit, with its ~30 coefficient/address registers, mBASE, and vLOUT/vROUT. Noise
  voices are in: the Dr. Hell/Xebra LFSR noise generator is clocked per output
  sample by the SPUCNT noise-frequency field, and a voice with its NON bit set
  outputs noise (still shaped by its envelope) instead of ADPCM. Pitch modulation
  is in: a voice with its PMON bit set scales its pitch step by the previous
  voice's output sample each frame. Volume sweeps are in: the per-voice and main
  volume registers, when in sweep mode (bit15), ramp their level each sample with
  the ADSR rate tables (linear/exponential, increase/decrease, both phases).
  XA-ADPCM streaming audio is in (xa.c, `make xa-test`): when the Setmode XA bit
  (0x40) is set and a Mode2 subheader marks a sector as audio, the CD read pump
  decodes its 18 sound groups to 16-bit PCM, resamples from the stream rate (4-bit
  37800 or 18900 Hz, stereo or mono) to 44100 Hz, and feeds it to spu_cd_audio
  instead of delivering it to the CPU. The ADPCM predictor history and resampler
  phase persist across sectors, and XA reads run at the real disc rate (single or
  double speed) so the SPU CD FIFO does not overrun. Setfilter (0x0D) is honored:
  with the Setmode XA-filter bit (0x08) set, only audio sectors whose subheader
  file and channel match the selected stream play, and a non-matching audio sector
  is dropped rather than delivered to the CPU; with the filter off, every audio
  sector plays. CD audio can be routed through reverb: when the SPUCNT CD-reverb
  bit (0x04) is set, the CD-volume-scaled CD/XA audio feeds the reverb input
  alongside the reverb-enabled voices, so its wet output adds to the mix. The
  sound-RAM IRQ (interrupt 9) is in: when SPUCNT bit 6 enables it, a voice ADPCM
  block read or a transfer-port access that touches the IRQ-address register
  (0x1F801DA4) latches SPUSTAT bit 6 and pulls the CPU SPU interrupt line; writing
  SPUCNT with the enable bit clear acknowledges it. Not modeled yet: sweeps on the
  CD and reverb output volumes, 8-bit XA (levels A, decoded as silence for now),
  and the coarser hardware XA interleave timing. Voice mixing uses linear
  interpolation rather than the hardware Gaussian.

## Storage

- [x] non-blocking sector reads for slow disc images. In the windowed build a
  prefetch worker thread (cdrom.c, started by cdrom_io_start from jonboy_runapp)
  reads raw sectors ahead of the drive cursor into a ring, so a slow image (for
  example one on an NFS mount) never blocks the CPU/render/input thread on an
  fread. The read and play pumps consume from the ring; on a cache miss they
  defer the sector by CDROM_RETRY_DELAY and retry rather than blocking, so the
  emulated disc simply reads slower while the rest of the machine keeps running.
  The worker is parked (pf_suspend) around any disc change so files are never
  closed mid-read, and a generation counter voids an in-flight read after a seek
  or swap. Headless, --boot, and --exe leave the worker off and read
  synchronously, so those paths stay bit-for-bit deterministic (reference traces
  and conformance unaffected). Unit tested end to end (test_cdda.c, `make
  cdda-test`); checked race-clean under helgrind.

- [x] add cd-rom support. cdrom.c mounts a .bin/.cue image (raw 2352-byte
  sectors), answers GetID as a licensed disc, and serves sectors through Setloc,
  Setmode, SeekL, ReadN/ReadS, and Pause. Sector data reaches RAM through the
  data FIFO and DMA channel 3. Mount with --cd. Verified end to end: the retail
  scph5501 BIOS reads the license region, parses SYSTEM.CNF, loads the boot
  executable, and runs it (Final Fantasy Tactics reaches its own startup code).

Next up (in order):

- [x] multi-track .cue images. cdrom.c now parses every FILE/TRACK/INDEX into a
  track table (number, data-vs-audio type, absolute INDEX 01 start LBA, and the
  file plus its base LBA), kept in a disc struct separate from the controller so
  a reset does not eject it. Supports a single .bin holding every track (laid out
  by INDEX 01 times) and one .bin per track (files laid end to end). Reads route
  through track_at_lba, so the file and byte offset come from the containing
  track. Added GetTN (0x13, first/last track in BCD) and GetTD (0x14, a track's
  start MSF in BCD; track 0 gives the lead-out). Security: a FILE reference is
  reduced to its base name and resolved only within the cue's own directory, so a
  hostile cue cannot reach outside it with an absolute path or a ".." component.
  Verified: single-bin and multi-bin layouts parse with correct start/lead-out
  LBAs, a malicious absolute/traversal path is contained, and Final Fantasy
  Tactics and Chrono Trigger still boot and run through the new read path. Not
  done: INDEX 00 pregaps and PREGAP/POSTGAP are not tracked (INDEX 01 gives each
  track start), which is fine for data boot and enough for CD-DA positioning.

- [x] audio-track (CD-DA) playback for games that stream music. Controller,
  protocol, and audible output are done and unit tested (test_cdda.c, spu.c mixes
  it to the SDL device; `make cdda-test`):
  - Play (0x03) starts at the track in the parameter, or the last Setloc when no
    parameter is given; Pause/Stop/Init and a ReadN stop playback.
  - the play pump streams one 2352-byte audio sector per tick, decoded to 588
    stereo 16-bit PCM frames and pushed to the SPU CD-audio input (spu_cd_audio).
    Mute/Demute (0x0B/0x0C) gate it to silence.
  - GetlocL (0x10) returns the last data sector's header/subheader; GetlocP
    (0x11) returns the play/read position (track, index, relative and absolute
    MSF) in BCD. GetTN/GetTD were added with the track table.
  - the Setmode report bit (0x04) delivers an INT1 position report per sector;
    the auto-pause bit (0x02) and the lead-out stop the drive with an INT4.
  - the play pump runs at the real CD-DA rate (75 sectors/second, 451584 CPU
    cycles per sector) so the pitch is correct, and the decoded PCM now reaches
    the SDL audio output through spu_cd_audio (see Audio above), so CD music is
    audible in the windowed emulator.
  Remaining: the report-mode INT1 cadence is one per sector rather than the
  hardware's coarser subchannel timing. (XA-ADPCM streaming audio is now decoded;
  see the Audio section.)

## DMA and interrupts

- [x] handle DICR/DPCR writes (0x1F8010F0/0x1F8010F4). These were silently
  dropped, so DMA completion interrupts never fired and any game waiting on a
  DMA event hung. The channel-flag bits (24-30) acknowledge write-1-to-clear.
- [x] SPU DMA (channel 4): consume a sound upload or zero-fill a read back, then
  let the deferred completion interrupt fire. This unblocks libspu's SpuInit,
  which uploads a block and waits on the SPU DMA event.
- [x] deliver DMA completion interrupts a few cycles late instead of inside the
  triggering register write, so the game code that kicked the DMA returns first.
- [x] commit a pending load-delay latch before taking an interrupt. Dropping it
  left the target register stale and corrupted a copy loop interrupted right
  after a load.
- [x] Final Fantasy Tactics crashed ~260M steps into its own init. Root cause:
  the CD-ROM data request cursor was reset on every write of the request bit
  (BFRD, 0x1F801803 bit7). The game drains one 2340-byte (whole-sector) read in
  two transfers, a 12-byte header/subheader read followed by a 2048-byte user
  data read, writing the request bit before each. Resetting the cursor on the
  second request re-delivered the 12-byte header, so every user-data DMA landed
  shifted +12 bytes. That corrupted an in-RAM size field, the game's allocator
  saw a 3 GB request and returned null, and the null cascaded into the eventual
  jump to garbage. Fixed by positioning the cursor only on the first request per
  sector (matching pcsx-redux's m_read==0 guard). Verified against a pcsx-redux
  reference trace: at the checkpoint (PC 0x80014440) RAM at 0x80067000 and the
  allocator result registers now match byte for byte, and the game runs past
  600M steps in the same code loop pcsx reaches (~0x8001DD18).
- EXP1 boot hook (verified, reused by the loader below): psio.c presents the
  cartridge as a ROM in Expansion Region 1. The scph5501 BIOS checks the pre-boot
  signature ("Licensed by Sony Computer Entertainment Inc.") at 0x1F000084 and,
  on a match, calls the function pointer at 0x1F000080. Only the pre-boot hook
  fires on scph5501; the post-boot hook does not. Offsets are in psio.h.

## MDEC (Macroblock Decoder)

- [x] MDEC decoder (mdec.c): decodes run-length-coded, quantized DCT blocks into
  monochrome (4/8-bit) or colour (15/24-bit) pixels for FMV and streamed texture
  decompression. Registers at 0x1F801820 (command/data) and 0x1F801824
  (control/status) in the general MMIO block; bulk data moves through DMA channel
  0 (RAM into the decoder) and channel 1 (pixels back to RAM), wired in dma.c.
  Commands: 1 decode, 2 set quant tables, 3 set IDCT scale table. The status
  register reports the busy, FIFO, DMA-request, depth, and remaining-word fields.
- The decode pipeline (run-length expand and dequantize, two-pass separable IDCT,
  YCbCr to RGB) uses the mednafen/Beetle fixed-point conventions: dequant in a
  <<4 domain with a sign-magnitude rounding bias, an IDCT over a pre->>3 scale
  matrix with 9-bit masking before the +-127 clamp, and a YCbCr conversion with
  the hardware's reduced-precision green channel.
- One dequant path, matching Beetle's WriteImageData exactly: the DC term is
  quantized by quant[0] and each AC term by q_scale*quant[k], and every
  coefficient (DC included) scatters from its zig-zag scan position into natural
  block order. When the effective quantizer is zero (a zero table entry, or
  q_scale zero for an AC term) the coefficient is taken raw and doubled, still
  through the zig-zag scatter. An earlier version special-cased q_scale == 0 as a
  separate linear-order, no-zig-zag mode and also skipped quant[0] for the DC;
  that passed the amidog ROMs but transposed and misplaced real FMV
  coefficients. The zig-zag scatter is always applied now.
- Verified against the amidog MDEC DCTBlockDecode CLUT4BPP and CLUT8BPP test ROMs
  (REF/PSX/MDEC): every block (flat DC, horizontal cosines at rising frequency,
  2-D gradient, checkerboard, and the letter "A") reconstructs with the correct
  structure and orientation. The residual brightness difference versus the
  reference PNGs is a single global display gamma applied by the hardware capture
  (a lone gamma near 1.8 reconciles every block at once, and it lands hardest on
  the mid-grey blocks and lightest on the high-contrast letter), not a decode
  error. nocash and Beetle arithmetic both independently agree with the linear
  output. Host unit test: `make mdec-test`.
- The colour (15/24-bit) path is unit-tested in `test_mdec.c`: a solid-colour
  macroblock (six DC-only blocks) reconstructs uniformly across all 256 pixels,
  neutral chroma renders grey, and positive Cr/Cb shift the hue red/blue as
  expected. This covers the six-block assembly, chroma subsampling, and the
  YCbCr-to-RGB conversion. `run_boot` prints an MDEC decode-command and
  macroblock tally (`mdec_get_stats`) so an FMV run shows a rising colour count.
- `test_mdec.c` also carries a byte-exact colour pin: it feeds an aggressive
  six-block macroblock (many AC terms, saturating levels, high zig-zag indices)
  through the real decoder and diffs both the 24- and 15-bit output against a
  reference pipeline transcribed independently from the mednafen/Beetle MDEC
  (dequant, YCbCr, and the RGB555 uint8 truncation). This exercises the AC path
  the DC-only checks miss and locks the decoder to the hardware-faithful result.
  All six blocks use a nonzero q_scale; the raw (zero-quantizer) path stays
  covered by the amidog ROMs.
- A stronger byte-exact cross-check drives the real decoder and the real
  mednafen/Beetle mdec.c on identical input and diffs the output. Unlike the
  hand-transcribed pin, it catches bugs shared between the decoder and a
  transcribed reference. It found the IDCT transpose and the q_scale == 0
  divergence above. The Beetle source is not in the repo; fetch it under
  REF/beetle-mdec (git-ignored) and run `make mdec-oracle`, which skips cleanly
  when the source is absent. Covers mono q8, mono q0 (the amidog path), and 24-
  and 15-bit colour macroblocks; all byte-exact.
- Real game FMV through the whole pipeline (CD stream to MDEC to GPU) now works:
  Lunar Silver Star Story boots past its old timer stall (see the timer
  clock-source fix), renders the copyright screen, and streams and decodes its
  intro FMV. The drive delivers the STR video sectors as INT1 and routes the
  interleaved XA audio to the SPU (mode 0xC0, ReadS from LBA 36748); the game
  issues MDEC decode commands and the decoded frames render as a sharp coherent
  image. Two bugs had to be cleared first. The MDEC-out DMA was decimated because
  the MEM_WRITE_32 macro double-evaluated its side-effecting argument (now a
  static inline in mem.h). The decoded frames then rendered "quilted" (per-8x8
  transpose) from the MDEC IDCT transpose and q_scale == 0 divergence fixed
  above, compounded by a missing 24bpp display readout (GP1(08) bit 4), now added
  in main.c. FF Tactics reaches its title screen and intro once GTE is present
  (see the MVP roadmap); its old ~0x8001DD18 loop was the attract/frame-wait
  loop, not a stall. Chrono Trigger has not been rechecked since GTE landed.
- The status "current block" field (bits 16-18) is left at its idle value rather
  than advancing per block during a decode; games generally use DMA rather than
  polling it.

## GTE (Geometry Transformation Engine, COP2)

The PS1 geometry coprocessor: a fixed-point vector/matrix unit the CPU drives
over coprocessor-2 instructions. It is the top MVP blocker (see the roadmap).
Reference math: nocash psx-spx (GTE section), and the pcsx-redux / mednafen GTE
implementations. Validation: REF/PSX/CPUTest/GTE (amidog).

Register file (to add to R3051_CPU in cpu.h):
- 32 data registers (cop2d 0-31): the vectors V0/V1/V2 (VXY0..VZ2), RGBC, OTZ,
  the accumulators IR0-IR3, the screen-coordinate FIFO SXY0-2/SZ0-3, the colour
  FIFO RGB0-2, the MAC0-3 accumulators, the IRGB/ORGB colour-conversion regs, the
  leading-zero count in/out LZCS/LZCR, and RES1.
- 32 control registers (cop2c 0-31): the rotation matrix RT, translation vector
  TR, the light matrix LLM and light colour matrix LCM, the background/far colour
  vectors BK/FC, the screen offset OFX/OFY, projection plane distance H, depth-cue
  scale/offset DQA/DQB, the Z-average scale factors ZSF3/ZSF4, and the FLAG error
  register.

Plan:
- [x] register file + the flag register (gte.c / gte.h, GTE struct), zeroed on
  reset via gte_reset from cpu_reset. The data and control banks and their
  read/write accessors carry the hardware read-back quirks: sign-extension on the
  VZ/IR/RT33-class registers, the SXYP FIFO push, the IRGB/ORGB colour
  conversion, the LZCS/LZCR leading count, the H sign-extend quirk, and the FLAG
  bit-12..30 mask with the bit-31 error summary. Unit tested (test_gte.c, `make
  gte-test`).
- [x] move and load/store ops in cpu.c: MFC2, CFC2, MTC2, CTC2 (primary opcode
  0x12 sub-ops), and LWC2 (0x32) / SWC2 (0x3A). MFC2/CFC2 write rt immediately,
  matching this core's MFC0 treatment (the hardware load delay on these is not
  modeled, same as MFC0); LWC2/SWC2 check word alignment and raise ADEL/ADES. A
  COP2 command instruction (bit 25 set) routes to gte_execute.
- [x] the command ops (COP2 with bit 25 set, selected by the low 6 function
  bits), with the shared MAC/IR pipeline in gte.c: a 44-bit accumulator with
  sticky overflow tracking, the per-op shift (sf), the lm saturation floor, and
  the IR/colour/screen clamps. Implemented: RTPS/RTPT (perspective transform, the
  depth-cue and the SZ/SXY FIFO pushes), NCLIP (normal clipping), AVSZ3/AVSZ4 (Z
  averaging), the lighting ops NCDS/NCDT/NCCS/NCCT/NCS/NCT/CC/CDP/DPCS/DPCT/DCPL/
  INTPL, MVMVA (the general matrix-vector multiply with its mx/v/cv selector and
  the FC-bug far-colour path), SQR, OP, GPF, GPL. The math follows nocash psx-spx
  and the pcsx-redux reference.
- [x] FLAG register: each op clears FLAG, accumulates the MAC/IR/colour/SZ/SXY/
  divide overflow and saturation bits as it runs, and recomputes the master error
  bit (bit 31) from the sticky error mask at the end.
- [x] the RTPS/RTPT unsigned Newton-Raphson division (H / SZ3) with its overflow
  clamp to 0x1FFFF, built from the hardware UNR reciprocal table, not a plain
  integer divide.
- [x] wired the `gte-conformance` make target (same verdict scheme as the CPU
  suite, over REF/PSX/CPUTest/GTE). NCLIP and AVSZ both PASS. Only those two ROMs
  are present locally; RTPS/MVMVA/the lighting ops are validated against the
  pcsx-redux reference math, and fetching the rest of the amidog GTE suite into
  REF/ to exercise them through conformance stays on the list.
- Not modeled: the GTE command cycle cost (5-44 cycles per op). The interpreter
  charges the uniform per-instruction cost, so GTE-heavy code completes its
  geometry a little early relative to the CPU clock. This matches how most
  interpreters run the GTE and is not expected to affect correctness.

## Loader (in-emulation game menu)

Our own parallel-I/O backdoor loader (the real Cybdyn PSIO protocol is set aside;
we invent what we need). A guest loader ROM boots via the EXP1 hook, browses host
image files over the backdoor, and boots a selection. Backdoor registers are in
Expansion Region 2 (0x1F802000), defined in psio.h (LDR_REG_* / LDR_CMD_*).

- [x] backdoor device: scan the games directory, enumerate names, and boot a
  selected PS-EXE (deferred until the kernel is up). Wired through mem.c; the
  directory comes from --games. Verified end to end: a stub loader ROM boots
  Cube.exe from the menu index.
- [x] on-screen menu: loader/main.c (C via mipsel-gcc, make builds _out/loader.exe)
  brings up the GPU, enumerates images over the backdoor, draws the list with an
  embedded 8x8 font, moves a cursor with the D-pad, and boots on X. Verified end
  to end (D-pad + Cross selects and boots the highlighted game). Run the windowed
  build with --games <dir> (the menu auto-loads and the keyboard drives the pad),
  or headless with --boot --sideload _out/loader.exe --games <dir>.
  - [x] auto-sideload the loader when --games is set (no explicit --sideload).
    main.c defaults --sideload to _out/loader.exe, so the windowed build boots
    straight into the menu.
  - [ ] scroll the list when there are more entries than fit on screen. The menu
    in loader/main.c draws every entry from the top, so a directory with more
    games than screen rows runs the list off the bottom and the cursor can move
    onto entries that are not visible. Track a scroll offset (top visible index)
    that follows the cursor, clamp it so the cursor stays on screen, draw only the
    visible window of entries, and show an indicator when more entries exist above
    or below. The backdoor already reports the full entry count, so this is a
    guest-side (loader/main.c) change.
- [~] CD image support: loader_scan now indexes .cue/.bin/.iso alongside PS-EXE
  files and records each entry's type (LDR_TYPE_EXE / LDR_TYPE_CD). When the menu
  boots a CD entry, run_boot mounts the image with cdrom_mount, clears RAM, resets
  the hardware, and reboots the machine so the retail BIOS runs its normal disc
  boot sequence (the stock CD-ROM registers already serve sectors from cdrom.c).
  Unit tested (test_loader.c, `make loader-test`: indexing, EXE-vs-CD
  classification, path/name, and the boot strobe). The loader boot flow is shared
  between the headless run_boot loop and the windowed jonboy_runapp loop through
  service_loader_boot, so the menu (which the keyboard drives in the windowed
  build) can now select and boot a CD image interactively.
- [ ] stream file bytes over the backdoor so the guest can read image data
  directly (needed once the menu wants thumbnails, metadata, or CD sectors)

## Presentation and threading

- [x] run the emulation core on its own thread (windowed build only). The
  deterministic core (CPU, GPU, SPU, DMA, CD) runs on a dedicated emu thread
  (main.c, emu_thread, started by SDL_CreateThread in jonboy_runapp); the SDL
  main thread only pumps events, samples input, and presents. So a slow present
  or a blocked event pump (a window drag) no longer stalls emulation or audio,
  and a hitch in emulation no longer starves input. The core stays
  single-threaded internally, so it remains deterministic, and the headless,
  --boot, and --exe modes keep their own single-threaded loops (unchanged;
  reference traces and conformance unaffected). Seams: the emu thread publishes a
  raw column-major VRAM snapshot at end of frame (one flat memcpy into a triple
  buffer); the main thread transposes it to row-major and presents, paced by
  vsync. The transpose was moved off the emu thread deliberately (see
  Performance) so it does not spend the real-time budget the audio ring needs. Input is the pad globals the main thread writes
  and the emu thread reads (single words, a torn read is harmless). Audio is
  pushed from the emu thread straight to the SDL stream, whose own callback thread
  drains it, so it keeps playing through a main-thread stall. Shutdown sets an
  atomic stop flag, joins the emu thread, then stops the CD worker.
  - [x] triple-buffer the frame snapshot (main.c) so neither thread waits on the
    other. Three VRAM snapshot buffers with a lock-free single-producer/
    single-consumer handoff: frame_slot (an SDL_AtomicInt) holds the last
    published buffer index plus a FRAME_FRESH bit, and each side atomically
    exchanges its private index with that slot (SDL_SetAtomicInt), so the two
    threads never touch the same buffer and neither copies under a lock. The
    display geometry travels with each buffer in g_frame_meta.
  - [x] let the SDL audio callback pull the SPU ring directly (main.c
    audio_callback, registered with SDL_OpenAudioDeviceStream) instead of the emu
    thread pushing. The device now drains the SPU on demand from SDL's own audio
    thread, so audio is decoupled from the main thread entirely and a slow present
    or blocked event pump cannot starve it. The SPU output ring (spu.c) became a
    cross-thread single-producer/single-consumer queue: its two index counters are
    C11 atomics with release/acquire ordering (no SDL dependency added), so the
    reader sees a frame's samples before the advanced write index. On an underrun
    the callback feeds less and SDL pads with silence.

## Performance

The desktop core runs above real time on average but the windowed build can dip
below it in heavy scenes, which starves the SPU output ring and produces audible
static and a slight pitch drop. Target: real-time on a Pi 5, near real-time on Pi
3/4; desktop is the development platform.

- [x] real-time metric, two halves (main.c). Batch: `report_realtime()` prints
  `Perf: <console>s in <wall>s = <N>x real-time` at the end of every
  headless/exe/boot run. This is the trustworthy A/B number (unpaced, controlled).
  Live: `JONBOY_PERF=1` logs once a second from the emu thread `[perf] <fps>
  emulated, <P>% of real-time budget, audio underruns/s: <U>`, where P% is
  emu-thread busy time (step loop + publish) over wall time and U counts audio
  callbacks the SPU ring could not fill. Caveat: the live P% has ~5-10% run-to-run
  variance from background CPU load, so use the headless ratio for A/B and run any
  windowed A/B back to back with nothing else running.
- [x] move the VRAM transpose off the emu thread. publish_frame now memcpys a raw
  column-major snapshot (~0.09 ms); present_frame does the ~1.29 ms/frame
  transpose on the main thread, which is vsync limited and has spare budget.
  Provably pixel-identical. This pulled heavy scenes back under the real-time line.
- [x] gate per-instruction bookkeeping calls. service_loader_boot and
  maybe_dump_frame were called on every instruction in the run loops; gating them
  behind a loop-invariant flag (inactive for a plain BIOS/disc boot) bought ~16%
  on the CPU-bound BIOS phase (300M-step boot 23.8M -> 27.6M steps/s).
- [ ] triangle rasterizer: investigated, NOT a fruitful target for FFT. draw_triangle
  is ~17% in profiles but already well-optimized. Measured coverage ratio 0.443
  (56% of inner-loop iterations fall outside the triangle), but those iterations
  are cheap next to covered textured pixels, so a scanline/span rewrite is
  estimated ~4% at most and cannot stay bit-exact for gouraud/textured (float
  accumulation rounds differently). Two bit-exact micro-opts (hoist the flat-color
  pack out of the pixel loop; skip the texture-window transform when unset) were
  proven pixel-identical but measured immaterial for FFT (0.13% headless), so they
  were not kept. Would only help 2D/sprite-heavy games.
- [x] cached-interpreter prototype: located the win and proved it. A literal
  decode/micro-op cache is not worth building here (decode.h is trivial
  field-extraction macros, so decode is nearly free, and threaded/computed-goto
  dispatch and per-instruction lookup tables both already measured slower). The
  only real lever in that family is batching the per-instruction hardware tick and
  interrupt check to block granularity. Prototyped as `JONBOY_FASTTICK=N` in
  step_cpu (cpu.c), gated and default-off: it advances N*cycles in bulk every N-th
  instruction so total timing stays exact and only interrupt latency coarsens by
  up to N-1. Unset/0 keeps exact per-instruction behavior, so determinism and
  conformance are unchanged by default. N=6 measured +12% on the CPU-bound BIOS
  phase (26.4M -> 29.5M steps/s) but only +2.7% on the full FFT boot-to-battle,
  since the battle is bound by GPU/GTE/MDEC and real instruction work, not
  per-instruction overhead. Validated: conformance passes, FFT boots to the
  battle, and the final battle frame is bit-identical to the default build.
  Now productized: cpu_set_fasttick() makes it a runtime setting, and the
  real-time windowed build and the --bench harness enable N=8 while the
  deterministic modes (headless, exe, boot, gdb) stay at 0. A clean A/B on the
  Chrono Trigger window measured ~5% (budget mean 10.14 -> 9.59 ms). The env
  override JONBOY_FASTTICK still wins for experiments.
- [ ] evaluate JONBOY_FASTTICK on the Pi 3/4/5. Per-instruction overhead and the
  scheduler flush's memory latency are a larger share of the budget on a slower
  in-order CPU, so tick-batching should help more there than on desktop. Sweep
  N=4/6/8 against the baseline `Perf:` ratio. If it pays off, productize it as
  ticking at true branch boundaries rather than modulo-N.
- [ ] dynarec (the ceiling, large effort). The remaining desktop in-game headroom
  needs native code generation, not interpreter micro-opts; keep it portable C
  (hand-rolled emitter, no C++ JIT library) so the amalgamation and determinism
  story survive. See the design notes discussed for the x86-64 / AArch64 targets.

## Debugging

- [x] gdbserver interface (gdb.c, `--gdb [port]`, default 1234). A GDB Remote
  Serial Protocol server on a loopback TCP socket drives the CPU under
  mipsel-unknown-elf-gdb: it boots the BIOS headless, starts halted at the reset
  vector, and answers register (g/G/p/P), memory (m/M), breakpoint (Z0/z0,
  tracked by address in the step loop), single step (s), continue (c), and
  Ctrl-C interrupt. Registers are the standard MIPS o32 set (32 GPR, status, lo,
  hi, badvaddr, cause, pc, and a zeroed FPU bank) declared to gdb through a
  target description over qXfer, so gdb parses the layout without guessing. The
  continue loop calls service_loader_boot, so sideload and the loader menu work
  under gdb too. Watchpoints (Z2 write, Z3 read, Z4 access) are supported: mem.c
  has an optional per-access hook (armed only while a watchpoint exists, and
  bypassed by the instruction fetch via mem_fetch_word), the step loop stops when
  a watched load or store fires, and the stop reply is a T packet naming the
  watchpoint so gdb reports it. Verified end to end with mipsel-unknown-elf-gdb:
  connect, register and memory read/write round-trips, a breakpoint at a BIOS
  address, continue, single step, and a write watchpoint that stops on the store
  with the correct old/new values. Not done: a target.xml with real FPU/GTE
  registers, and running the gdbserver alongside the windowed build.
- [ ] add a handler so that a hotkey (or signal) will output CPU regs and a short stack trace.
