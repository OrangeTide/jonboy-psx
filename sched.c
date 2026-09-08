#include "sched.h"
#include "cpu.h"
#include "gpu.h"
#include "spu.h"
#include "timer.h"
#include "cdrom.h"
#include "dma.h"
#include "pad.h"
#include "script.h"

// Approximate NTSC timing: the ~33.87 MHz CPU clock over 60 Hz is about
// 564480 cycles per frame. VBLANK is raised once per frame.
#define FRAME_CYCLES 564480

static uint32_t vblank_acc;
static uint32_t frame_count; // video frames since reset, the script's clock

void
psx_hardware_reset(void)
{
    vblank_acc = 0;
    frame_count = 0;
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
        // A new video frame: advance the script clock and run any due events.
        frame_count++;
        script_frame(frame_count);
    }

    psx_timers_tick(cycles);
    psx_cdrom_tick(cycles);
    psx_dma_tick(cycles);
    psx_spu_tick(cycles);
    psx_pad_tick(cycles);
}
