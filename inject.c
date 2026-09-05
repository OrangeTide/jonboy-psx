#include "inject.h"
#include <stddef.h>
#include <stdint.h>

// MIPS-I instruction words for the boot stub. These mirror bootpatch.s and are
// written to memory little-endian below, matching the PSX byte order.
const uint32_t psio_boot_stub[] = {
    0x40806000,  // mtc0    $0, $12   (Status = 0: disable interrupts)
    0x00000000,  // nop
    0x3c1d8020,  // lui     $29, 0x8020
    0x27bdff00,  // addiu   $29, $29, -256   ($sp = 0x801FFF00)
    0x24020000,  // addiu   $2, $0, 0
    0x24030000,  // addiu   $3, $0, 0
    0x3c049f00,  // lui     $4, 0x9f00       ($a0 = 0x9F000000)
    0x00800008,  // jr      $4               (jump to PSIO firmware)
    0x00000000   // nop (delay slot)
};

void
inject_boot_stub(uint8_t *rom_buffer)
{
    // The BIOS reset vector (0xBFC00000 -> physical 0x1FC00000) is the first
    // thing the CPU fetches. Overwriting it makes the CPU run the stub, which
    // jumps to the PSIO firmware. Store each word little-endian by hand so the
    // result is correct regardless of host byte order.
    size_t stub_words = sizeof(psio_boot_stub) / sizeof(psio_boot_stub[0]);

    for (size_t i = 0; i < stub_words; i++) {
        uint32_t w = psio_boot_stub[i];
        rom_buffer[i * 4 + 0] = (uint8_t)(w & 0xFF);
        rom_buffer[i * 4 + 1] = (uint8_t)((w >> 8) & 0xFF);
        rom_buffer[i * 4 + 2] = (uint8_t)((w >> 16) & 0xFF);
        rom_buffer[i * 4 + 3] = (uint8_t)((w >> 24) & 0xFF);
    }
}
