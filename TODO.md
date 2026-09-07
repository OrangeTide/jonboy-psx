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

- [ ] add cd-rom support. cdrom.c implements the register interface and command
  handshake but always reports "no disc". Remaining: load a disc image (cue/bin
  or iso), map LBA to sectors, and deliver sector data.
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
