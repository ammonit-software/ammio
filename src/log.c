#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <threads.h>
#include <time.h>

static atomic_int current_level = LOG_INFO;
static mtx_t log_mutex;
static once_flag log_once = ONCE_FLAG_INIT;

static void log_init(void)
{
    mtx_init(&log_mutex, mtx_plain);
}

void log_set_level(log_level_t level)
{
    atomic_store(&current_level, (int)level);
}

log_level_t log_get_level(void)
{
    return (log_level_t)atomic_load(&current_level);
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if ((int)level > atomic_load(&current_level))
        return;

    static const char *const names[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm);
#endif

    char line[1024];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld %s ",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L, names[level]);

    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
    va_end(args);

    call_once(&log_once, log_init);
    mtx_lock(&log_mutex);
    fputs(line, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    mtx_unlock(&log_mutex);
}
