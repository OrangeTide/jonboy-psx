#include "sched.h"
#include "cpu.h"
#include "gpu.h"
#include "spu.h"
#include "timer.h"
#include "cdrom.h"
#include "dma.h"
#include "mdec.h"
#include "pad.h"
#include "script.h"

#include <stdint.h>

// Approximate NTSC timing: the ~33.87 MHz CPU clock over 60 Hz is about
// 564480 cycles per frame. VBLANK is raised once per frame.
#define FRAME_CYCLES 564480

static uint32_t vblank_acc;
static uint32_t frame_count; // video frames since reset, the script's clock

// Cycle scheduler. Ticking every device on every CPU step is pure overhead when
// nothing is due, so instead the clock free-runs and the device fan-out only
// happens at the next scheduled event or when a device register is touched.
//
//   hw_cycle    the running CPU clock (advanced every step)
//   hw_synced   how far the devices have actually been ticked
//   hw_deadline the cycle at which the next event must be applied
//
// Between hw_synced and hw_cycle no device can raise an interrupt or change an
// observable value, because hw_deadline is the nearest such moment across every
// subsystem and the bus syncs before touching any device. So the flush applies
// exactly the elapsed cycles in one batched tick, reproducing the same
// interrupts at the same cycles as ticking two at a time would.
static uint64_t hw_cycle;
static uint64_t hw_synced;
static uint64_t hw_deadline;

// The old per-step tick body: advance the frame clock and every device by a
// batch of cycles. The batch is bounded to the nearest deadline, so each device
// crosses at most one of its own boundaries per call.
static void
hw_apply(uint32_t cycles)
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

// Cycles from now until the nearest event across all subsystems. Always finite
// and at least one: VBLANK and the timers are always counting, so this bounds
// every batch to a single crossing.
static uint32_t
hw_next_event(void)
{
    uint32_t d = FRAME_CYCLES - vblank_acc; // cycles to the next VBLANK
    uint32_t e;

    e = timer_next_event();

    if (e < d) {
        d = e;
    }

    e = cdrom_next_event();

    if (e < d) {
        d = e;
    }

    e = dma_next_event();

    if (e < d) {
        d = e;
    }

    e = pad_next_event();

    if (e < d) {
        d = e;
    }

    e = spu_next_event();

    if (e < d) {
        d = e;
    }

    return d ? d : 1;
}

// Apply the cycles that have elapsed since the last sync as one batched tick,
// then set the next deadline from the devices' current state.
static void
hw_flush(void)
{
    uint32_t delta = (uint32_t)(hw_cycle - hw_synced);

    if (delta) {
        hw_apply(delta);
        hw_synced = hw_cycle;
    }

    hw_deadline = hw_cycle + hw_next_event();
}

void
psx_hardware_advance(uint32_t cycles)
{
    hw_cycle += cycles;

    if (hw_cycle >= hw_deadline) {
        hw_flush();
    }
}

void
psx_hardware_sync(void)
{
    // Apply the cycles elapsed so far so the device about to be accessed sees
    // the right time. The deadline is left alone here: the access may change it,
    // so psx_hardware_resched recomputes it once afterward. No advance runs
    // between the two, so the stale deadline is never consulted in between.
    uint32_t delta = (uint32_t)(hw_cycle - hw_synced);

    if (delta) {
        hw_apply(delta);
        hw_synced = hw_cycle;
    }
}

void
psx_hardware_resched(void)
{
    hw_deadline = hw_cycle + hw_next_event();
}

void
psx_hardware_reset(void)
{
    vblank_acc = 0;
    frame_count = 0;
    hw_cycle = 0;
    hw_synced = 0;
    hw_deadline = 0; // first advance flushes and computes the real deadline
    psx_timers_reset();
    psx_spu_reset();
    psx_gpu_reset();
    psx_cdrom_reset();
    psx_pad_reset();
    mdec_reset();
}
