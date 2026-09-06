# TODO

## controller port

Current state: pad.c answers every port access with 0xFF, so no device is
acknowledged yet. Implementing any type below means driving the SIO transfer
handshake (0x01 poll, 0x42 read, 0x5A sync, then the per-device payload).

- [ ] support the 4 main controller port types:
  - [ ] Standard Digital Pad ID: 0x41
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
- [ ] add PSIO support. psio.c mounts a raw image file (load_virtual_sd_card)
  and DMA channel 3 streams words from it, but the command register decode
  (mount, seek, read sector, report status) and the LBA-to-offset mapping are
  still TODO in psio.c.
  - [ ] read from FAT32 image (parse the filesystem, not just a raw image)
  - [ ] read from a hosted directory
