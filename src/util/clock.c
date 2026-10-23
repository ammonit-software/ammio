#include "util/clock.h"

#include <threads.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define NS_PER_SEC 1000000000ULL
#define MAX_SLEEP_SLICE_NS (50ULL * 1000000ULL)

uint64_t clock_realtime_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

uint64_t clock_monotonic_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart / freq.QuadPart) * NS_PER_SEC +
                      ((now.QuadPart % freq.QuadPart) * NS_PER_SEC) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
#endif
}

void clock_sleep_ns(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ns / NS_PER_SEC),
        .tv_nsec = (long)(ns % NS_PER_SEC)
    };
    thrd_sleep(&ts, NULL);
}

void cycle_init(cycle_timer_t *cycle, uint32_t period_ms)
{
    cycle->period_ns = (uint64_t)period_ms * 1000000ULL;
    cycle->next_ns = clock_monotonic_ns() + cycle->period_ns;
    cycle->overruns = 0;
}

uint64_t cycle_remaining_ns(const cycle_timer_t *cycle)
{
    uint64_t now = clock_monotonic_ns();
    return cycle->next_ns > now ? cycle->next_ns - now : 0;
}

void cycle_wait(cycle_timer_t *cycle, const atomic_bool *running)
{
    for (;;)
    {
        if (running && !atomic_load(running))
            return;

        uint64_t now = clock_monotonic_ns();
        if (now >= cycle->next_ns)
            break;

        uint64_t remaining = cycle->next_ns - now;
        clock_sleep_ns(remaining < MAX_SLEEP_SLICE_NS ? remaining : MAX_SLEEP_SLICE_NS);
    }

    cycle->next_ns += cycle->period_ns;

    uint64_t now = clock_monotonic_ns();
    if (cycle->next_ns <= now)
    {
        // We fell behind by more than a period: skip the missed slots instead of bursting
        cycle->overruns++;
        cycle->next_ns = now + cycle->period_ns;
    }
}
