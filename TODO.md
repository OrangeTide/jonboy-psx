# TODO

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
  rejection, get-id, and the FLAG transition). Not done yet: the PocketStation
  extensions and slot 2.

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

## Debugging

- [ ] add gdbserver interface. mipsel-unknown-elf-gdb exists
- [ ] add a handler so that a hotkey (or signal) will output CPU regs and a short stack trace.
