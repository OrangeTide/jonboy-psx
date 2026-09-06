# TODO

## controller port

- [ ] support the 4 main controller port types:
  - [ ] Standard Digital Pad ID: 0x41
  - [ ] Standard Analog Flightstick ID: 0x53
  - [ ] DualShock Analog Pad ID: 0x73
  - [ ] Official PS1 Keyboard ID: 0x96 (represented in hex as 0x96, or high-nibble 9 indicating a keyboard type, and low-nibble 6 indicating 6 half-words / 3 bytes of payload data). see [[REF/kbpoll.c]]
    - SPI Byte-Level Frame Layout
      - To emulate this in C, you must hook into your emulator's controller port polling loop. When a game like RPG Maker polls a port configured with a keyboard, the transaction looks like this:
      | Byte Index | Command (From PS1) | Response (From Keyboard Adapter) | Description |
      | --- | --- | --- | --- |
      | Byte 1 | 0x01 | 0xFF | Console wakes up the port. |
      | Byte 2 | 0x42 | 0x96 | Main Read Command (0x42). Adapter returns the Keyboard Device ID. |
      | Byte 3 | 0x00 | 0x5A | Command Sync. Adapter returns 0x5A ("Ready to transmit data"). |
      | Byte 4 | 0x00 | DATA 1 | Flags byte (such as Shift, Alt, Control modifiers, and special state bits). |
      | Byte 5 | 0x00 | DATA 2 | Scan Code Byte 1 (The active key being pressed / held). |
      | Byte 6 | 0x00 | DATA 3 | Scan Code Byte 2 (If multiple keys are held simultaneously). |

## Audio

- [ ] add sound support

## Storage

- [ ] add cd-rom support
- [ ] add PSIO support
  - [ ] read from FAT32 image
  - [ ] read from a hosted directory
