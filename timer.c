#include "timer.h"
#include "savestate.h"
#include "cpu.h"

// Mode register bits we act on. The sync bits (0-2) are stored but not yet
// interpreted; the clock-source bits (8-9) are honored below.
#define TMODE_RESET_ON_TARGET (1u << 3)  // wrap at target instead of 0xFFFF
#define TMODE_IRQ_ON_TARGET   (1u << 4)  // raise IRQ when the target is hit
#define TMODE_IRQ_ON_WRAP     (1u << 5)  // raise IRQ when 0xFFFF is passed
#define TMODE_REACHED_TARGET  (1u << 11) // sticky, cleared on mode read
#define TMODE_REACHED_WRAP    (1u << 12) // sticky, cleared on mode read

// System clocks per counter tick for the non-system-clock sources. Timer1's
// HBlank source ticks once per scanline: the ~564480-cycle NTSC frame spans 263
// lines, so about 2146 CPU cycles per HBlank. Timer0's dot clock depends on the
// horizontal resolution; 8 cycles is a reasonable stand-in for the common 320
// wide mode (the exact GPU mode is not plumbed here yet). Timer2's alternate
// source is simply the system clock divided by eight.
#define TIMER_HBLANK_CYCLES  2146
#define TIMER_DOTCLOCK_CYCLES 8
#define TIMER_DIV8_CYCLES     8

typedef struct {
    uint32_t counter; // low 16 bits are the visible value
    uint32_t mode;
    uint32_t target;
    uint32_t frac;    // system cycles accumulated toward the next counter tick
} PsxTimer;

static PsxTimer timers[3];

// System cycles per counter tick, from the mode's clock-source bits (8-9). The
// meaning of the source select differs per counter: timer0 can take the GPU dot
// clock, timer1 the HBlank, and timer2 the system clock divided by eight.
static uint32_t
timer_divisor(int index, uint32_t mode)
{
    uint32_t src = (mode >> 8) & 3u;

    switch (index) {
    case 0:
        return (src & 1u) ? TIMER_DOTCLOCK_CYCLES : 1u; // src 1/3 = dot clock

    case 1:
        return (src & 1u) ? TIMER_HBLANK_CYCLES : 1u;   // src 1/3 = HBlank

    case 2:
        return (src & 2u) ? TIMER_DIV8_CYCLES : 1u;     // src 2/3 = sysclock/8

    default:
        return 1u;
    }
}

void
psx_timers_reset(void)
{
    for (int i = 0; i < 3; i++) {
        timers[i].counter = 0;
        timers[i].mode = 0;
        timers[i].target = 0;
        timers[i].frac = 0;
    }
}

uint32_t
psx_timer_get_value(int timer_index)
{
    if (timer_index < 0 || timer_index >= 3) {
        return 0;
    }

    return timers[timer_index].counter & 0xFFFF;
}

uint32_t
psx_timer_get_mode(int timer_index)
{
    if (timer_index < 0 || timer_index >= 3) {
        return 0;
    }

    uint32_t mode = timers[timer_index].mode;
    // The reached-target and reached-wrap flags clear when the mode is read.
    timers[timer_index].mode &= ~(TMODE_REACHED_TARGET | TMODE_REACHED_WRAP);
    return mode;
}

uint32_t
psx_timer_get_target(int timer_index)
{
    if (timer_index < 0 || timer_index >= 3) {
        return 0;
    }

    return timers[timer_index].target & 0xFFFF;
}

void
psx_timer_set_value(int timer_index, uint32_t value)
{
    if (timer_index < 0 || timer_index >= 3) {
        return;
    }

    timers[timer_index].counter = value & 0xFFFF;
    timers[timer_index].frac = 0;
}

void
psx_timer_set_mode(int timer_index, uint32_t value)
{
    if (timer_index < 0 || timer_index >= 3) {
        return;
    }

    timers[timer_index].mode = value;
    timers[timer_index].counter = 0; // writing the mode resets the counter
    timers[timer_index].frac = 0;
}

void
psx_timer_set_target(int timer_index, uint32_t value)
{
    if (timer_index < 0 || timer_index >= 3) {
        return;
    }

    timers[timer_index].target = value & 0xFFFF;
}

void
psx_timers_tick(uint32_t cycles)
{
    for (int i = 0; i < 3; i++) {
        PsxTimer *t = &timers[i];
        uint32_t before = t->counter;

        // Convert the elapsed system cycles into counter ticks for this timer's
        // clock source, carrying the sub-tick remainder so the average rate is
        // exact. The scheduler bounds a batch to the nearest crossing, so the
        // counter advances past at most one target or wrap per call.
        uint32_t div = timer_divisor(i, t->mode);
        uint32_t total = t->frac + cycles;
        t->frac = total % div;
        t->counter += total / div;

        // Reached the target this tick?
        if (t->target != 0 && before < t->target && t->counter >= t->target) {
            t->mode |= TMODE_REACHED_TARGET;

            if (t->mode & TMODE_IRQ_ON_TARGET) {
                cpu.i_stat |= (1u << (IRQ_TIMER0 + i));
            }

            if (t->mode & TMODE_RESET_ON_TARGET) {
                t->counter -= t->target;
            }
        }

        // Passed 0xFFFF this tick?
        if (t->counter > 0xFFFF) {
            t->mode |= TMODE_REACHED_WRAP;

            if (t->mode & TMODE_IRQ_ON_WRAP) {
                cpu.i_stat |= (1u << (IRQ_TIMER0 + i));
            }

            t->counter &= 0xFFFF;
        }
    }
}

uint32_t
timer_next_event(void)
{
    // The nearest system cycle at which any counter reaches its target or wraps
    // past 0xFFFF. Each counter's distance is measured in its own ticks, then
    // scaled back to system cycles by its clock divisor (less the cycles already
    // banked toward the next tick). Bounding a batch to this keeps
    // psx_timers_tick's one-crossing logic and the observable counter exact.
    uint32_t best = 0xFFFFFFFFu;

    for (int i = 0; i < 3; i++) {
        PsxTimer *t = &timers[i];
        uint32_t div = timer_divisor(i, t->mode);
        uint32_t ticks = 0x10000 - t->counter; // ticks to wrap

        if (t->target != 0 && t->counter < t->target) {
            uint32_t dt = t->target - t->counter;

            if (dt < ticks) {
                ticks = dt;
            }
        }

        // System cycles until that crossing: whole ticks scaled by the divisor,
        // minus the fraction already accumulated toward the next tick.
        uint32_t d = ticks * div - t->frac;

        if (d < best) {
            best = d;
        }
    }

    return best ? best : 1;
}

int
timer_state_save(FILE *f)
{
    return SS_WR(f, timers, sizeof timers) ? 0 : -1;
}

int
timer_state_load(FILE *f)
{
    return SS_RD(f, timers, sizeof timers) ? 0 : -1;
}
