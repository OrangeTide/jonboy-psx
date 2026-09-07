#ifndef PAD_H_
#define PAD_H_
#include <stdint.h>

// SIO0 register block (0x1F801040 - 0x1F80104F): the controller and
// memory-card serial port. This is a stub that reports no device on either
// slot, so the BIOS shell finishes probing the port and shows its menu.
#define PAD_BASE 0x1F801040
#define PAD_SIZE 0x10

// Digital pad button bits, active low: a 0 bit means the button is held. The
// idle value is 0xFFFF (nothing pressed). The host input layer writes this.
#define PAD_BTN_SELECT   (1u << 0)
#define PAD_BTN_START    (1u << 3)
#define PAD_BTN_UP       (1u << 4)
#define PAD_BTN_RIGHT    (1u << 5)
#define PAD_BTN_DOWN     (1u << 6)
#define PAD_BTN_LEFT     (1u << 7)
#define PAD_BTN_L1       (1u << 10)
#define PAD_BTN_R1       (1u << 11)
#define PAD_BTN_TRIANGLE (1u << 12)
#define PAD_BTN_CIRCLE   (1u << 13)
#define PAD_BTN_CROSS    (1u << 14)
#define PAD_BTN_SQUARE   (1u << 15)

// Current digital pad state, active low (0 = pressed). Set by the host input
// layer; read by the SIO0 transfer emulation. Idle is 0xFFFF.
extern uint16_t pad_buttons;

uint32_t psx_pad_read(uint32_t phys_addr, int size);
void psx_pad_write(uint32_t phys_addr, uint32_t value, int size);
void psx_pad_reset(void);

#endif
