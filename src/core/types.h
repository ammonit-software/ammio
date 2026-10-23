#ifndef AMMIO_TYPES_H
#define AMMIO_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef enum
{
    AMMIO_TYPE_BOOL,
    AMMIO_TYPE_UINT8,
    AMMIO_TYPE_INT8,
    AMMIO_TYPE_UINT16,
    AMMIO_TYPE_INT16,
    AMMIO_TYPE_UINT32,
    AMMIO_TYPE_INT32,
    AMMIO_TYPE_UINT64,
    AMMIO_TYPE_INT64,
    AMMIO_TYPE_FLOAT32,
    AMMIO_TYPE_FLOAT64,
    AMMIO_TYPE_COUNT
} ammio_type_t;

typedef union
{
    bool b;
    uint8_t u8;
    int8_t i8;
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    uint64_t u64;
    int64_t i64;
    float f32;
    double f64;
} ammio_value_t;

typedef enum
{
    AMMIO_VALUE_OK,
    AMMIO_VALUE_WRONG_TYPE,   // not a number/bool, or a fraction for an integer type
    AMMIO_VALUE_OUT_OF_RANGE  // does not fit in the variable's type
} ammio_value_status_t;

// Largest integer magnitude a JSON number (IEEE double) carries exactly: 2^53
#define AMMIO_JSON_MAX_EXACT_INT 9007199254740992.0

bool ammio_type_from_string(const char *str, ammio_type_t *out);
const char *ammio_type_name(ammio_type_t type);
const char *ammio_type_list(void);  // "bool, uint8, ..." for error messages
size_t ammio_type_size_bytes(ammio_type_t type);
bool ammio_type_is_integer(ammio_type_t type);

ammio_value_status_t ammio_value_from_double(ammio_type_t type, double d, ammio_value_t *out);

// Accepts JSON numbers and booleans (true/false map to 1/0)
ammio_value_status_t ammio_value_from_json(ammio_type_t type, const cJSON *json, ammio_value_t *out);

// Converts a 64-bit integer received from a device into the target type with range checking
ammio_value_status_t ammio_value_from_int64(ammio_type_t type, int64_t v, ammio_value_t *out);
ammio_value_status_t ammio_value_from_uint64(ammio_type_t type, uint64_t v, ammio_value_t *out);

double ammio_value_to_double(ammio_type_t type, ammio_value_t value);
cJSON *ammio_value_to_json(ammio_type_t type, ammio_value_t value);
bool ammio_value_equal(ammio_type_t type, ammio_value_t a, ammio_value_t b);
void ammio_value_format(ammio_type_t type, ammio_value_t value, char *buf, size_t size);

char *ammio_strdup(const char *s);

#endif
