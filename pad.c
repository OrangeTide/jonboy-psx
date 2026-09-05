#include <stdint.h>
#include "pad.h"

// Returns 0xFFFF (bits are active-low on PSX, meaning 1 = unpressed, 0 = pressed)
uint32_t
psx_pad_read_data(void)
{
    return 0x0000FFFF;
}

// Bit 0: TX Ready, Bit 2: RX FIFO Not Empty, Bit 7: Acknowledge
uint32_t
psx_pad_read_stat(void)
{
    return 0x00000005; // Force TX ready status flags
}
