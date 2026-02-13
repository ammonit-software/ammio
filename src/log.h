#ifndef LOG_H
#define LOG_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO
} log_level_t;

void log_set_level(log_level_t level);
void log_info(const char *fmt, ...);
void log_debug(const char *fmt, ...);

#endif
