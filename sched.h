#ifndef SCHED_H_
#define SCHED_H_
#include <stdint.h>

// Advance the hardware clock by the given number of CPU cycles. Generates the
// VBLANK interrupt once per frame and ticks the root counters.
void psx_hardware_tick(uint32_t cycles);

// Reset the scheduler and the devices it drives.
void psx_hardware_reset(void);

#endif
