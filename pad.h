#ifndef PAD_H_
#define PAD_H_
#include <stdint.h>

// SIO0 register block (0x1F801040 - 0x1F80104F): the controller and
// memory-card serial port. This is a stub that reports no device on either
// slot, so the BIOS shell finishes probing the port and shows its menu.
#define PAD_BASE 0x1F801040
#define PAD_SIZE 0x10

// Pad button bits, active low: a 0 bit means the button is held. The idle value
// is 0xFFFF (nothing pressed). The bit order matches the two data bytes the pad
// reports (low byte then high byte). The host input layer writes this. L3/R3 are
// the analog stick clicks and L2/R2 the lower shoulder buttons, all present on a
// DualShock.
#define PAD_BTN_SELECT   (1u << 0)
#define PAD_BTN_L3       (1u << 1)
#define PAD_BTN_R3       (1u << 2)
#define PAD_BTN_START    (1u << 3)
#define PAD_BTN_UP       (1u << 4)
#define PAD_BTN_RIGHT    (1u << 5)
#define PAD_BTN_DOWN     (1u << 6)
#define PAD_BTN_LEFT     (1u << 7)
#define PAD_BTN_L2       (1u << 8)
#define PAD_BTN_R2       (1u << 9)
#define PAD_BTN_L1       (1u << 10)
#define PAD_BTN_R1       (1u << 11)
#define PAD_BTN_TRIANGLE (1u << 12)
#define PAD_BTN_CIRCLE   (1u << 13)
#define PAD_BTN_CROSS    (1u << 14)
#define PAD_BTN_SQUARE   (1u << 15)

// Current pad button state, active low (0 = pressed). Set by the host input
// layer; read by the SIO0 transfer emulation. Idle is 0xFFFF.
extern uint16_t pad_buttons;

// Analog stick positions, 0x00..0xFF with 0x80 centered. Only reported in analog
// mode. Right stick first, then left, matching the DualShock report order.
extern uint8_t pad_rstick_x, pad_rstick_y;
extern uint8_t pad_lstick_x, pad_lstick_y;

// DualShock analog mode. pad_toggle_analog flips between digital (ID 0x41) and
// analog (ID 0x73), as the physical ANALOG button does; pad_analog_mode reports
// the current state (for an on-screen indicator).
void pad_toggle_analog(void);
int  pad_analog_mode(void);

uint32_t psx_pad_read(uint32_t phys_addr, int size);
void psx_pad_write(uint32_t phys_addr, uint32_t value, int size);
void psx_pad_reset(void);

#endif
