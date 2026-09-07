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

- [ ] add sound support. spu.c currently only stubs the register reads, so the
  BIOS thinks the SPU is present but no audio is generated.

## Storage

- [x] add cd-rom support. cdrom.c mounts a .bin/.cue image (raw 2352-byte
  sectors), answers GetID as a licensed disc, and serves sectors through Setloc,
  Setmode, SeekL, ReadN/ReadS, and Pause. Sector data reaches RAM through the
  data FIFO and DMA channel 3. Mount with --cd. Verified end to end: the retail
  scph5501 BIOS reads the license region, parses SYSTEM.CNF, loads the boot
  executable, and runs it (Final Fantasy Tactics reaches its own startup code).

Next up (in order):

- [ ] multi-track .cue images. cdrom_mount today reads only the first quoted
  FILE name and sets a single track_offset of 0, so audio tracks and any layout
  past track 1 are invisible. Parse every TRACK and its INDEX lines, building a
  track table: track number, type (MODE1/MODE2 data vs AUDIO), the INDEX 00
  pregap, and the INDEX 01 start. Support both a single .bin addressed by byte
  offset and one FILE per track. From the table:
  - map an LBA to its track and know whether that track is data or audio.
  - add GetTN (0x13): reply with first and last track numbers in BCD.
  - add GetTD (0x14): given a track number, reply with that track's start MSF
    in BCD (track 0 asks for the lead-out).
  This is the prerequisite for CD-DA, which needs to locate and classify audio
  tracks.

- [ ] audio-track (CD-DA) playback for games that stream music. Depends on the
  track table above and on an SPU audio output path (see Audio below); until the
  SPU can emit sound the position and protocol side can still be built and
  tested. Work:
  - Play (0x03): start playback at the track in the parameter, or at the last
    Setloc when no parameter is given. It is a stub now (status reply only).
  - stream each 2352-byte audio sector as 588 stereo 16-bit PCM frames into the
    SPU CD-audio input, scaled by the CD volume registers (0x1F801DB0), and gate
    it on Mute/Demute (0x0B/0x0C, currently stubs).
  - GetlocL (0x10): reply with the data-sector header (amm, ass, asect, mode,
    file, channel, submode, coding) of the current position.
  - GetlocP (0x11): reply with the play position, track, index, relative MSF
    within the track, and absolute MSF, all in BCD.
  - honor the Setmode report bit (0x04, deliver an INT1 with position per sector)
    and the auto-pause bit (0x02, stop at the end of the track).

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
