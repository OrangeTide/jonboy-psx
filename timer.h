#ifndef TIMER_H_
#define TIMER_H_
#include <stdint.h>

// The three PSX root counters (0x1F801100 - 0x1F801128). Each has a current
// value, a mode register, and a target value.
uint32_t psx_timer_get_value(int timer_index);
uint32_t psx_timer_get_mode(int timer_index);
uint32_t psx_timer_get_target(int timer_index);
void psx_timer_set_value(int timer_index, uint32_t value);
void psx_timer_set_mode(int timer_index, uint32_t value);
void psx_timer_set_target(int timer_index, uint32_t value);

// Advance all counters by the given number of CPU cycles, raising timer
// interrupts as configured.
void psx_timers_tick(uint32_t cycles);
void psx_timers_reset(void);

#endif
