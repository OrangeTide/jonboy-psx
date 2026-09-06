#include "sched.h"
#include "cpu.h"
#include "gpu.h"
#include "spu.h"
#include "timer.h"
#include "cdrom.h"
#include "pad.h"

// Approximate NTSC timing: the ~33.87 MHz CPU clock over 60 Hz is about
// 564480 cycles per frame. VBLANK is raised once per frame.
#define FRAME_CYCLES 564480

static uint32_t vblank_acc;

void
psx_hardware_reset(void)
{
    vblank_acc = 0;
    psx_timers_reset();
    psx_spu_reset();
    psx_gpu_reset();
    psx_cdrom_reset();
    psx_pad_reset();
}

void
psx_hardware_tick(uint32_t cycles)
{
    vblank_acc += cycles;

    if (vblank_acc >= FRAME_CYCLES) {
        vblank_acc -= FRAME_CYCLES;
        cpu.i_stat |= (1u << IRQ_VBLANK);
    }

    psx_timers_tick(cycles);
    psx_cdrom_tick(cycles);
}
