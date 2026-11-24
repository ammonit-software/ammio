#ifndef AMMIO_LOG_H
#define AMMIO_LOG_H

typedef enum
{
    LOG_ERROR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG
} log_level_t;

#if defined(__GNUC__) || defined(__clang__)
#define AMMIO_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define AMMIO_PRINTF(fmt, args)
#endif

void log_set_level(log_level_t level);
log_level_t log_get_level(void);

// Thread-safe: each call writes one complete line to stderr.
void log_msg(log_level_t level, const char *fmt, ...) AMMIO_PRINTF(2, 3);

#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

#endif
