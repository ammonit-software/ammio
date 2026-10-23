#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

static ammio_log_level_t current_level = AMMIO_LOG_INFO;
static once_flag log_once = ONCE_FLAG_INIT;
static mtx_t log_mutex;

static void log_init_mutex(void)
{
    mtx_init(&log_mutex, mtx_plain);
}

void log_set_level(ammio_log_level_t level)
{
    current_level = level;
}

ammio_log_level_t log_get_level(void)
{
    return current_level;
}

bool log_level_from_string(const char *str, ammio_log_level_t *out)
{
    static const char *const names[] = {"debug", "info", "warn", "error", "off"};

    for (int i = 0; i <= AMMIO_LOG_OFF; i++)
    {
        if (strcmp(str, names[i]) == 0)
        {
            *out = (ammio_log_level_t)i;
            return true;
        }
    }

    char *end = NULL;
    long n = strtol(str, &end, 10);
    if (end != str && *end == '\0' && n >= AMMIO_LOG_DEBUG && n <= AMMIO_LOG_OFF)
    {
        *out = (ammio_log_level_t)n;
        return true;
    }
    return false;
}

static void log_print(ammio_log_level_t level, const char *fmt, va_list args)
{
    static const char *const level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    if (level < current_level)
        return;

    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    time_t secs = ts.tv_sec;
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &secs);
#else
    localtime_r(&secs, &t);
#endif

    // Format the whole line first so lines from different threads never interleave
    char line[2048];
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000L,
                     level_str[level]);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(line))
        vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);

    call_once(&log_once, log_init_mutex);
    mtx_lock(&log_mutex);
    FILE *out = level >= AMMIO_LOG_WARN ? stderr : stdout;
    fputs(line, out);
    fputc('\n', out);
    fflush(out);
    mtx_unlock(&log_mutex);
}

#define DEFINE_LOG_FN(name, level)          \
    void name(const char *fmt, ...)         \
    {                                       \
        va_list args;                       \
        va_start(args, fmt);                \
        log_print(level, fmt, args);        \
        va_end(args);                       \
    }

DEFINE_LOG_FN(log_debug, AMMIO_LOG_DEBUG)
DEFINE_LOG_FN(log_info, AMMIO_LOG_INFO)
DEFINE_LOG_FN(log_warn, AMMIO_LOG_WARN)
DEFINE_LOG_FN(log_error, AMMIO_LOG_ERROR)
