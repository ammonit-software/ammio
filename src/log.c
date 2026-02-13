#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static log_level_t current_level = LOG_INFO;

void log_set_level(log_level_t level)
{
    current_level = level;
}

static void log_print(const char *level_str, const char *fmt, va_list args)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("%04d-%02d-%02d %02d:%02d:%02d [%s] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec,
           level_str);
    vprintf(fmt, args);
    printf("\n");
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_print("INFO", fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...)
{
    if (current_level > LOG_DEBUG) return;
    
    va_list args;
    va_start(args, fmt);
    log_print("DEBUG", fmt, args);
    va_end(args);
}
