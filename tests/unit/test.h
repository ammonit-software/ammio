#ifndef AMMIO_TEST_H
#define AMMIO_TEST_H

// Minimal test harness: each test is a void function registered in test_main.c

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int test_failures;
extern const char *test_current;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "  FAIL %s (%s:%d): %s\n", test_current, __FILE__, __LINE__, #cond); \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                             \
    do {                                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                            \
        if (_a != _b) {                                                                \
            fprintf(stderr, "  FAIL %s (%s:%d): %s == %s (%lld != %lld)\n", test_current, \
                    __FILE__, __LINE__, #a, #b, _a, _b);                               \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                             \
    do {                                                                               \
        const char *_a = (a), *_b = (b);                                               \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                       \
            fprintf(stderr, "  FAIL %s (%s:%d): %s == %s (\"%s\" != \"%s\")\n", test_current, \
                    __FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

#define CHECK_BYTES(buf, ...)                                                          \
    do {                                                                               \
        const unsigned char _exp[] = {__VA_ARGS__};                                    \
        if (memcmp((buf), _exp, sizeof(_exp)) != 0) {                                  \
            fprintf(stderr, "  FAIL %s (%s:%d): bytes of %s differ\n", test_current,   \
                    __FILE__, __LINE__, #buf);                                         \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

#define TEST(name) void name(void)

#endif
