#ifndef SCHED_H_
#define SCHED_H_
#include <stdint.h>

// Advance the hardware clock by the given number of CPU cycles, called once per
// CPU step. The devices are not ticked every step: the scheduler accumulates
// cycles and only runs the device fan-out when the nearest scheduled event is
// due (see psx_hardware_sync for the other trigger).
void psx_hardware_advance(uint32_t cycles);

// Bring the devices current to the accumulated clock right now, without
// touching the next deadline. The memory bus calls this before a timing-device
// access so the register sees the right time; it must call psx_hardware_resched
// afterward, once the access has settled, to fold in any event it changed.
void psx_hardware_sync(void);

// Recompute the next deadline from the devices' current state. Paired after a
// psx_hardware_sync so a register access that arms, cancels, or reschedules an
// event is reflected before the next CPU step, without forcing a second flush.
void psx_hardware_resched(void);

// Reset the scheduler and the devices it drives.
void psx_hardware_reset(void);

#endif
