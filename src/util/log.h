#ifndef AMMIO_LOG_H
#define AMMIO_LOG_H

#include <stdbool.h>

typedef enum
{
    AMMIO_LOG_DEBUG,
    AMMIO_LOG_INFO,
    AMMIO_LOG_WARN,
    AMMIO_LOG_ERROR,
    AMMIO_LOG_OFF
} ammio_log_level_t;

#if defined(__GNUC__) || defined(__clang__)
#define AMMIO_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define AMMIO_PRINTF(fmt_idx, args_idx)
#endif

void log_set_level(ammio_log_level_t level);
ammio_log_level_t log_get_level(void);

// Parses "debug", "info", "warn", "error", "off" or the numeric value (0..4)
bool log_level_from_string(const char *str, ammio_log_level_t *out);

void log_debug(const char *fmt, ...) AMMIO_PRINTF(1, 2);
void log_info(const char *fmt, ...) AMMIO_PRINTF(1, 2);
void log_warn(const char *fmt, ...) AMMIO_PRINTF(1, 2);
void log_error(const char *fmt, ...) AMMIO_PRINTF(1, 2);

#endif
