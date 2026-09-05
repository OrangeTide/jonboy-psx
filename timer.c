#include "timer.h"
#include "cpu.h"

// Mode register bits we act on. The clock-source select bits (8-9) and the
// sync bits (0-2) are stored but not yet interpreted; every counter runs from
// the system clock for now.
#define TMODE_RESET_ON_TARGET (1u << 3)  // wrap at target instead of 0xFFFF
#define TMODE_IRQ_ON_TARGET   (1u << 4)  // raise IRQ when the target is hit
#define TMODE_IRQ_ON_WRAP     (1u << 5)  // raise IRQ when 0xFFFF is passed
#define TMODE_REACHED_TARGET  (1u << 11) // sticky, cleared on mode read
#define TMODE_REACHED_WRAP    (1u << 12) // sticky, cleared on mode read

typedef struct {
    uint32_t counter; // low 16 bits are the visible value
    uint32_t mode;
    uint32_t target;
} PsxTimer;

static PsxTimer timers[3];

void
psx_timers_reset(void)
{
    for (int i = 0; i < 3; i++) {
        timers[i].counter = 0;
        timers[i].mode = 0;
        timers[i].target = 0;
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
}

void
psx_timer_set_mode(int timer_index, uint32_t value)
{
    if (timer_index < 0 || timer_index >= 3) {
        return;
    }

    timers[timer_index].mode = value;
    timers[timer_index].counter = 0; // writing the mode resets the counter
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
        t->counter += cycles;

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
