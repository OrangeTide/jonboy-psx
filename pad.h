#ifndef PAD_H_
#define PAD_H_
#include <stdint.h>

// SIO0 register block (0x1F801040 - 0x1F80104F): the controller and
// memory-card serial port. This is a stub that reports no device on either
// slot, so the BIOS shell finishes probing the port and shows its menu.
#define PAD_BASE 0x1F801040
#define PAD_SIZE 0x10

uint32_t psx_pad_read(uint32_t phys_addr, int size);
void psx_pad_write(uint32_t phys_addr, uint32_t value, int size);
void psx_pad_reset(void);

#endif
