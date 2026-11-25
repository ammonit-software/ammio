#include "vars.h"

#include "uthash.h"

#ifdef _MSC_VER
#pragma warning(disable : 4127 4702) // raised by uthash's macros (do/while(0), dead branches)
#endif

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

// --- types -------------------------------------------------------------------

static const struct
{
    const char *name;
    size_t size;
    double min, max; // integer range (unused for floats)
} type_info[TYPE_COUNT] = {
    [TYPE_BOOL]    = {"bool",    1, 0, 1},
    [TYPE_UINT8]   = {"uint8",   1, 0, UINT8_MAX},
    [TYPE_INT8]    = {"int8",    1, INT8_MIN, INT8_MAX},
    [TYPE_UINT16]  = {"uint16",  2, 0, UINT16_MAX},
    [TYPE_INT16]   = {"int16",   2, INT16_MIN, INT16_MAX},
    [TYPE_UINT32]  = {"uint32",  4, 0, UINT32_MAX},
    [TYPE_INT32]   = {"int32",   4, INT32_MIN, INT32_MAX},
    [TYPE_FLOAT32] = {"float32", 4, 0, 0},
    [TYPE_FLOAT64] = {"float64", 8, 0, 0},
};

bool type_parse(const char *str, type_t *out)
{
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (strcmp(str, type_info[t].name) == 0)
        {
            *out = (type_t)t;
            return true;
        }
    }
    return false;
}

const char *type_name(type_t type)
{
    return type < TYPE_COUNT ? type_info[type].name : "unknown";
}

const char *type_names(void)
{
    return "uint8, int8, uint16, int16, uint32, int32, float32, float64, bool";
}

size_t type_size(type_t type)
{
    return type_info[type].size;
}

const char *dir_name(dir_t dir)
{
    return dir == DIR_INPUT ? "input" : "output";
}

// --- values ------------------------------------------------------------------

bool value_from_double(type_t type, double d, value_t *out)
{
    if (!isfinite(d))
        return false;

    value_t v = {0};
    switch (type)
    {
        case TYPE_FLOAT32:
            if (fabs(d) > FLT_MAX)
                return false;
            v.f32 = (float)d;
            break;
        case TYPE_FLOAT64:
            v.f64 = d;
            break;
        default:
            // Integers and bool: must be integral and in range. Checking first
            // also avoids the undefined behaviour of an out-of-range cast.
            if (d != floor(d) || d < type_info[type].min || d > type_info[type].max)
                return false;
            switch (type)
            {
                case TYPE_BOOL:   v.b = d != 0;          break;
                case TYPE_UINT8:  v.u8 = (uint8_t)d;     break;
                case TYPE_INT8:   v.i8 = (int8_t)d;      break;
                case TYPE_UINT16: v.u16 = (uint16_t)d;   break;
                case TYPE_INT16:  v.i16 = (int16_t)d;    break;
                case TYPE_UINT32: v.u32 = (uint32_t)d;   break;
                case TYPE_INT32:  v.i32 = (int32_t)d;    break;
                default:          return false;
            }
    }
    *out = v;
    return true;
}

double value_to_double(type_t type, value_t v)
{
    switch (type)
    {
        case TYPE_BOOL:    return v.b ? 1 : 0;
        case TYPE_UINT8:   return v.u8;
        case TYPE_INT8:    return v.i8;
        case TYPE_UINT16:  return v.u16;
        case TYPE_INT16:   return v.i16;
        case TYPE_UINT32:  return v.u32;
        case TYPE_INT32:   return v.i32;
        case TYPE_FLOAT32: return v.f32;
        case TYPE_FLOAT64: return v.f64;
        default:           return 0;
    }
}

static uint64_t value_to_bits(type_t type, value_t v)
{
    switch (type)
    {
        case TYPE_BOOL:   return v.b ? 1 : 0;
        case TYPE_UINT8:  return v.u8;
        case TYPE_INT8:   return (uint8_t)v.i8;
        case TYPE_UINT16: return v.u16;
        case TYPE_INT16:  return (uint16_t)v.i16;
        case TYPE_UINT32: return v.u32;
        case TYPE_INT32:  return (uint32_t)v.i32;
        case TYPE_FLOAT32:
        {
            uint32_t u;
            memcpy(&u, &v.f32, sizeof(u));
            return u;
        }
        case TYPE_FLOAT64:
        {
            uint64_t u;
            memcpy(&u, &v.f64, sizeof(u));
            return u;
        }
        default:
            return 0;
    }
}

static value_t value_from_bits(type_t type, uint64_t bits)
{
    value_t v = {0};
    switch (type)
    {
        case TYPE_BOOL:   v.b = bits != 0;              break;
        case TYPE_UINT8:  v.u8 = (uint8_t)bits;         break;
        case TYPE_INT8:   v.i8 = (int8_t)(uint8_t)bits; break;
        case TYPE_UINT16: v.u16 = (uint16_t)bits;       break;
        case TYPE_INT16:  v.i16 = (int16_t)(uint16_t)bits; break;
        case TYPE_UINT32: v.u32 = (uint32_t)bits;       break;
        case TYPE_INT32:  v.i32 = (int32_t)(uint32_t)bits; break;
        case TYPE_FLOAT32:
        {
            uint32_t u = (uint32_t)bits;
            memcpy(&v.f32, &u, sizeof(u));
            break;
        }
        case TYPE_FLOAT64:
            memcpy(&v.f64, &bits, sizeof(bits));
            break;
        default:
            break;
    }
    return v;
}

void value_to_be(type_t type, value_t value, uint8_t *dst)
{
    uint64_t bits = value_to_bits(type, value);
    size_t n = type_size(type);
    for (size_t i = 0; i < n; i++)
        dst[i] = (uint8_t)(bits >> (8 * (n - 1 - i)));
}

value_t value_from_be(type_t type, const uint8_t *src)
{
    uint64_t bits = 0;
    size_t n = type_size(type);
    for (size_t i = 0; i < n; i++)
        bits = (bits << 8) | src[i];
    return value_from_bits(type, bits);
}

// --- variable table ----------------------------------------------------------

struct var
{
    char *id;
    type_t type;
    dir_t dir;
    value_t value;          // guarded by values_mutex
    uint64_t timestamp_ns;  // guarded by values_mutex
    UT_hash_handle hh;
};

static var_t *by_id = NULL;     // uthash index
static var_t **ordered = NULL;  // registration order
static size_t count = 0, capacity = 0;

static mtx_t values_mutex;
static once_flag values_once = ONCE_FLAG_INIT;

static void values_init(void)
{
    mtx_init(&values_mutex, mtx_plain);
}

var_t *vars_add(const char *id, type_t type, dir_t dir)
{
    call_once(&values_once, values_init);

    if (vars_find(id))
        return NULL;

    if (count == capacity)
    {
        size_t new_cap = capacity ? capacity * 2 : 64;
        var_t **grown = realloc(ordered, new_cap * sizeof(*grown));
        if (!grown)
            return NULL;
        ordered = grown;
        capacity = new_cap;
    }

    var_t *var = calloc(1, sizeof(*var));
    char *id_copy = var ? malloc(strlen(id) + 1) : NULL;
    if (!id_copy)
    {
        free(var);
        return NULL;
    }
    strcpy(id_copy, id);
    var->id = id_copy;
    var->type = type;
    var->dir = dir;

    HASH_ADD_KEYPTR(hh, by_id, var->id, strlen(var->id), var);
    ordered[count++] = var;
    return var;
}

var_t *vars_find(const char *id)
{
    var_t *var = NULL;
    HASH_FIND_STR(by_id, id, var);
    return var;
}

size_t vars_count(void)
{
    return count;
}

var_t *vars_at(size_t index)
{
    return index < count ? ordered[index] : NULL;
}

void vars_clear(void)
{
    HASH_CLEAR(hh, by_id);
    for (size_t i = 0; i < count; i++)
    {
        free(ordered[i]->id);
        free(ordered[i]);
    }
    free(ordered);
    ordered = NULL;
    count = capacity = 0;
}

const char *var_id(const var_t *var) { return var->id; }
type_t var_type(const var_t *var) { return var->type; }
dir_t var_dir(const var_t *var) { return var->dir; }

value_t var_get(const var_t *var, uint64_t *timestamp_ns)
{
    mtx_lock(&values_mutex);
    value_t v = var->value;
    if (timestamp_ns)
        *timestamp_ns = var->timestamp_ns;
    mtx_unlock(&values_mutex);
    return v;
}

void var_set(var_t *var, value_t value)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    mtx_lock(&values_mutex);
    var->value = value;
    var->timestamp_ns = now;
    mtx_unlock(&values_mutex);
}
