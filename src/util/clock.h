#ifndef AMMIO_CLOCK_H
#define AMMIO_CLOCK_H

#include <stdatomic.h>
#include <stdint.h>

// Wall-clock time in nanoseconds since the UNIX epoch (used for variable timestamps)
uint64_t clock_realtime_ns(void);

// Monotonic time in nanoseconds (used for scheduling, never jumps)
uint64_t clock_monotonic_ns(void);

// Sleeps for the given number of nanoseconds
void clock_sleep_ns(uint64_t ns);

// Fixed-rate cycle timer: deadlines are computed from the previous deadline, not from
// "now", so the processing time of each cycle does not add up into period drift.
typedef struct
{
    uint64_t period_ns;
    uint64_t next_ns;
    uint64_t overruns;
} cycle_timer_t;

void cycle_init(cycle_timer_t *cycle, uint32_t period_ms);

// Nanoseconds left until the next deadline (0 if already due)
uint64_t cycle_remaining_ns(const cycle_timer_t *cycle);

// Sleeps until the next deadline, waking at least every 50 ms to check *running.
// When a cycle overruns its deadline the schedule is re-anchored to "now".
void cycle_wait(cycle_timer_t *cycle, const atomic_bool *running);

#endif
