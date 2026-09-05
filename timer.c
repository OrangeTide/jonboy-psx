#include "timer.h"

static uint16_t fake_timer_value[3] = {0, 0, 0};
static uint32_t fake_timer_mode[3] = {0, 0, 0};

uint32_t
psx_timer_get_value(int timer_index)
{
    if (timer_index < 0 || timer_index >= 3) return 0;

    // Increment the value slightly every time the game checks it to simulate elapsed time
    fake_timer_value[timer_index] += 4;
    return (uint32_t)fake_timer_value[timer_index];
}

uint32_t
psx_timer_get_mode(int timer_index)
{
    if (timer_index < 0 || timer_index >= 3) return 0;

    return fake_timer_mode[timer_index];
}
