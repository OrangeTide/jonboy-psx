#ifndef SCHED_H_
#define SCHED_H_
#include <stdint.h>

// Advance the hardware clock by the given number of CPU cycles, called once per
// CPU step. The devices are not ticked every step: the scheduler accumulates
// cycles and only runs the device fan-out when the nearest scheduled event is
// due (see psx_hardware_sync for the other trigger).
void psx_hardware_advance(uint32_t cycles);

// Bring the devices current to the accumulated clock right now, and let the
// scheduler recompute its next deadline. The memory bus calls this before every
// access to a timing device, so a register read sees the right time and a
// register write that arms a new event is picked up immediately.
void psx_hardware_sync(void);

// Reset the scheduler and the devices it drives.
void psx_hardware_reset(void);

#endif
