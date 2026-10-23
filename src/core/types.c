#include "core/types.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const type_names[AMMIO_TYPE_COUNT] = {
    [AMMIO_TYPE_BOOL]    = "bool",
    [AMMIO_TYPE_UINT8]   = "uint8",
    [AMMIO_TYPE_INT8]    = "int8",
    [AMMIO_TYPE_UINT16]  = "uint16",
    [AMMIO_TYPE_INT16]   = "int16",
    [AMMIO_TYPE_UINT32]  = "uint32",
    [AMMIO_TYPE_INT32]   = "int32",
    [AMMIO_TYPE_UINT64]  = "uint64",
    [AMMIO_TYPE_INT64]   = "int64",
    [AMMIO_TYPE_FLOAT32] = "float32",
    [AMMIO_TYPE_FLOAT64] = "float64",
};

bool ammio_type_from_string(const char *str, ammio_type_t *out)
{
    if (!str)
        return false;
    for (int i = 0; i < AMMIO_TYPE_COUNT; i++)
    {
        if (strcmp(str, type_names[i]) == 0)
        {
            *out = (ammio_type_t)i;
            return true;
        }
    }
    return false;
}

const char *ammio_type_name(ammio_type_t type)
{
    return (type >= 0 && type < AMMIO_TYPE_COUNT) ? type_names[type] : "unknown";
}

const char *ammio_type_list(void)
{
    return "bool, uint8, int8, uint16, int16, uint32, int32, uint64, int64, float32, float64";
}

size_t ammio_type_size_bytes(ammio_type_t type)
{
    switch (type)
    {
        case AMMIO_TYPE_BOOL:
        case AMMIO_TYPE_UINT8:
        case AMMIO_TYPE_INT8:
            return 1;
        case AMMIO_TYPE_UINT16:
        case AMMIO_TYPE_INT16:
            return 2;
        case AMMIO_TYPE_UINT32:
        case AMMIO_TYPE_INT32:
        case AMMIO_TYPE_FLOAT32:
            return 4;
        case AMMIO_TYPE_UINT64:
        case AMMIO_TYPE_INT64:
        case AMMIO_TYPE_FLOAT64:
            return 8;
        default:
            return 0;
    }
}

bool ammio_type_is_integer(ammio_type_t type)
{
    return type != AMMIO_TYPE_BOOL && type != AMMIO_TYPE_FLOAT32 && type != AMMIO_TYPE_FLOAT64;
}

ammio_value_status_t ammio_value_from_int64(ammio_type_t type, int64_t v, ammio_value_t *out)
{
    ammio_value_t r;
    memset(&r, 0, sizeof(r));

    switch (type)
    {
        case AMMIO_TYPE_BOOL:
            if (v != 0 && v != 1) return AMMIO_VALUE_OUT_OF_RANGE;
            r.b = v != 0;
            break;
        case AMMIO_TYPE_UINT8:
            if (v < 0 || v > UINT8_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.u8 = (uint8_t)v;
            break;
        case AMMIO_TYPE_INT8:
            if (v < INT8_MIN || v > INT8_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.i8 = (int8_t)v;
            break;
        case AMMIO_TYPE_UINT16:
            if (v < 0 || v > UINT16_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.u16 = (uint16_t)v;
            break;
        case AMMIO_TYPE_INT16:
            if (v < INT16_MIN || v > INT16_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.i16 = (int16_t)v;
            break;
        case AMMIO_TYPE_UINT32:
            if (v < 0 || v > (int64_t)UINT32_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.u32 = (uint32_t)v;
            break;
        case AMMIO_TYPE_INT32:
            if (v < INT32_MIN || v > INT32_MAX) return AMMIO_VALUE_OUT_OF_RANGE;
            r.i32 = (int32_t)v;
            break;
        case AMMIO_TYPE_UINT64:
            if (v < 0) return AMMIO_VALUE_OUT_OF_RANGE;
            r.u64 = (uint64_t)v;
            break;
        case AMMIO_TYPE_INT64:
            r.i64 = v;
            break;
        case AMMIO_TYPE_FLOAT32:
            r.f32 = (float)v;
            break;
        case AMMIO_TYPE_FLOAT64:
            r.f64 = (double)v;
            break;
        default:
            return AMMIO_VALUE_WRONG_TYPE;
    }

    *out = r;
    return AMMIO_VALUE_OK;
}

ammio_value_status_t ammio_value_from_uint64(ammio_type_t type, uint64_t v, ammio_value_t *out)
{
    if (type == AMMIO_TYPE_UINT64)
    {
        memset(out, 0, sizeof(*out));
        out->u64 = v;
        return AMMIO_VALUE_OK;
    }
    if (v > (uint64_t)INT64_MAX)
        return (type == AMMIO_TYPE_FLOAT32 || type == AMMIO_TYPE_FLOAT64)
                   ? ammio_value_from_double(type, (double)v, out)
                   : AMMIO_VALUE_OUT_OF_RANGE;
    return ammio_value_from_int64(type, (int64_t)v, out);
}

ammio_value_status_t ammio_value_from_double(ammio_type_t type, double d, ammio_value_t *out)
{
    if (type == AMMIO_TYPE_FLOAT64)
    {
        memset(out, 0, sizeof(*out));
        out->f64 = d;
        return AMMIO_VALUE_OK;
    }
    if (type == AMMIO_TYPE_FLOAT32)
    {
        if (isfinite(d) && fabs(d) > FLT_MAX)
            return AMMIO_VALUE_OUT_OF_RANGE;
        memset(out, 0, sizeof(*out));
        out->f32 = (float)d;
        return AMMIO_VALUE_OK;
    }

    // Integer and bool types: reject fractions, NaN/inf and anything a double cannot hold exactly
    if (!isfinite(d) || d != floor(d))
        return AMMIO_VALUE_WRONG_TYPE;
    if (fabs(d) > AMMIO_JSON_MAX_EXACT_INT)
        return AMMIO_VALUE_OUT_OF_RANGE;

    return ammio_value_from_int64(type, (int64_t)d, out);
}

ammio_value_status_t ammio_value_from_json(ammio_type_t type, const cJSON *json, ammio_value_t *out)
{
    if (!json)
        return AMMIO_VALUE_WRONG_TYPE;
    if (cJSON_IsBool(json))
        return ammio_value_from_int64(type, cJSON_IsTrue(json) ? 1 : 0, out);
    if (cJSON_IsNumber(json))
        return ammio_value_from_double(type, json->valuedouble, out);
    return AMMIO_VALUE_WRONG_TYPE;
}

double ammio_value_to_double(ammio_type_t type, ammio_value_t value)
{
    switch (type)
    {
        case AMMIO_TYPE_BOOL:    return value.b ? 1.0 : 0.0;
        case AMMIO_TYPE_UINT8:   return value.u8;
        case AMMIO_TYPE_INT8:    return value.i8;
        case AMMIO_TYPE_UINT16:  return value.u16;
        case AMMIO_TYPE_INT16:   return value.i16;
        case AMMIO_TYPE_UINT32:  return value.u32;
        case AMMIO_TYPE_INT32:   return value.i32;
        case AMMIO_TYPE_UINT64:  return (double)value.u64;
        case AMMIO_TYPE_INT64:   return (double)value.i64;
        case AMMIO_TYPE_FLOAT32: return value.f32;
        case AMMIO_TYPE_FLOAT64: return value.f64;
        default:                 return 0.0;
    }
}

cJSON *ammio_value_to_json(ammio_type_t type, ammio_value_t value)
{
    char buf[32];

    switch (type)
    {
        case AMMIO_TYPE_BOOL:
            return cJSON_CreateBool(value.b);
        case AMMIO_TYPE_UINT64:
            // Emit 64-bit integers verbatim: going through a double would round them
            snprintf(buf, sizeof(buf), "%" PRIu64, value.u64);
            return cJSON_CreateRaw(buf);
        case AMMIO_TYPE_INT64:
            snprintf(buf, sizeof(buf), "%" PRId64, value.i64);
            return cJSON_CreateRaw(buf);
        case AMMIO_TYPE_FLOAT32:
        case AMMIO_TYPE_FLOAT64:
        {
            double d = ammio_value_to_double(type, value);
            // JSON has no NaN/Infinity
            return isfinite(d) ? cJSON_CreateNumber(d) : cJSON_CreateNull();
        }
        default:
            return cJSON_CreateNumber(ammio_value_to_double(type, value));
    }
}

bool ammio_value_equal(ammio_type_t type, ammio_value_t a, ammio_value_t b)
{
    switch (type)
    {
        case AMMIO_TYPE_BOOL:    return a.b == b.b;
        case AMMIO_TYPE_UINT8:   return a.u8 == b.u8;
        case AMMIO_TYPE_INT8:    return a.i8 == b.i8;
        case AMMIO_TYPE_UINT16:  return a.u16 == b.u16;
        case AMMIO_TYPE_INT16:   return a.i16 == b.i16;
        case AMMIO_TYPE_UINT32:  return a.u32 == b.u32;
        case AMMIO_TYPE_INT32:   return a.i32 == b.i32;
        case AMMIO_TYPE_UINT64:  return a.u64 == b.u64;
        case AMMIO_TYPE_INT64:   return a.i64 == b.i64;
        // Bitwise comparison so NaN -> NaN does not count as a change every cycle
        case AMMIO_TYPE_FLOAT32: return memcmp(&a.f32, &b.f32, sizeof(float)) == 0;
        case AMMIO_TYPE_FLOAT64: return memcmp(&a.f64, &b.f64, sizeof(double)) == 0;
        default:                 return false;
    }
}

void ammio_value_format(ammio_type_t type, ammio_value_t value, char *buf, size_t size)
{
    switch (type)
    {
        case AMMIO_TYPE_BOOL:    snprintf(buf, size, "%s", value.b ? "true" : "false"); break;
        case AMMIO_TYPE_UINT64:  snprintf(buf, size, "%" PRIu64, value.u64); break;
        case AMMIO_TYPE_INT64:   snprintf(buf, size, "%" PRId64, value.i64); break;
        case AMMIO_TYPE_FLOAT32:
        case AMMIO_TYPE_FLOAT64: snprintf(buf, size, "%.9g", ammio_value_to_double(type, value)); break;
        default:                 snprintf(buf, size, "%.0f", ammio_value_to_double(type, value)); break;
    }
}

char *ammio_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}
