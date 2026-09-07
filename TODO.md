# TODO

## controller port

Current state: a digital pad in slot 1 works. pad.c drives the SIO0 transfer
(0x01 address, 0x42 read, 0x5A, two button bytes) and pulses the acknowledge
interrupt (IRQ_PAD) after each byte but the last. Buttons come from pad_buttons,
active low, driven by the host keyboard in the SDL window. Slot 2 and the memory
card address report no device.

- [~] support the 4 main controller port types:
  - [x] Standard Digital Pad ID: 0x41
  - [ ] Standard Analog Flightstick ID: 0x53
  - [ ] DualShock Analog Pad ID: 0x73 (analog mode). Reports 0x41 in digital
    mode; the ANALOG button toggles between the two, so handle both IDs.
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

- [ ] memory card support. The card is addressed by a leading 0x81 select byte
  (controllers use 0x01), then a command selects read (0x52), write (0x57), or
  get-id (0x53). The BIOS shell already has a MEMORY CARD menu, so this is needed
  for the shell to manage saves. Back it with a 128 KB card image file.

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
  sector plays. Not modeled yet: the SPU IRQ on a sound-RAM address, CD audio
  routed into reverb, sweeps on the CD and reverb output volumes, 8-bit XA (levels
  A, decoded as silence for now), and the coarser hardware XA interleave timing.
  Voice mixing uses linear interpolation rather than the hardware Gaussian.

## Storage

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
  to end (D-pad + Cross selects and boots the highlighted game). Run it with
  --boot --sideload _out/loader.exe --games <dir>.
  - [ ] auto-sideload the loader when --games is set (no explicit --sideload)
  - [ ] scroll when there are more entries than fit on screen
- [ ] CD image support: index .bin/.cue/.iso and boot them via the game-execution
  path (trap the stock CD-ROM registers 0x1F801800-3 and serve sectors).
  Depends on cd-rom support above.
- [ ] stream file bytes over the backdoor so the guest can read image data
  directly (needed once the menu wants thumbnails, metadata, or CD sectors)

## Debugging

- [ ] add gdbserver interface. mipsel-unknown-elf-gdb exists
- [ ] add a handler so that a hotkey (or signal) will output CPU regs and a short stack trace.
