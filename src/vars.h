#ifndef AMMIO_VARS_H
#define AMMIO_VARS_H

// Variable store shared by the client API and the protocol adapters.
//
// Variables are registered while the config is loaded (single-threaded) and
// never removed while ammio runs, so a var_t pointer is a stable handle.
// Reading and writing values is thread-safe.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    TYPE_BOOL,
    TYPE_UINT8,
    TYPE_INT8,
    TYPE_UINT16,
    TYPE_INT16,
    TYPE_UINT32,
    TYPE_INT32,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_COUNT
} type_t;

typedef enum
{
    DIR_INPUT,  // client -> ammio -> SUT (client may write)
    DIR_OUTPUT  // SUT -> ammio -> client (read-only for clients)
} dir_t;

typedef union
{
    bool b;
    uint8_t u8;
    int8_t i8;
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    float f32;
    double f64;
} value_t;

// --- types -------------------------------------------------------------------

bool type_parse(const char *str, type_t *out);
const char *type_name(type_t type);
const char *type_names(void); // "bool, uint8, ..." for error messages
size_t type_size(type_t type); // size in bytes (bool = 1)

// --- values ------------------------------------------------------------------

// Exact conversion: fails if `d` is not representable in `type`
// (out of range, or not an integer for integer types, or not 0/1 for bool).
bool value_from_double(type_t type, double d, value_t *out);
double value_to_double(type_t type, value_t value);

// Big-endian (network order) encoding, type_size(type) bytes.
void value_to_be(type_t type, value_t value, uint8_t *dst);
value_t value_from_be(type_t type, const uint8_t *src);

// --- variable table ----------------------------------------------------------

typedef struct var var_t;

// Returns NULL if a variable with this name already exists.
var_t *vars_add(const char *name, type_t type, dir_t dir);
var_t *vars_find(const char *name);
size_t vars_count(void);
var_t *vars_at(size_t index); // in registration order
void vars_clear(void);

const char *var_name(const var_t *var);
type_t var_type(const var_t *var);
dir_t var_dir(const var_t *var);

// timestamp_ns (optional): wall-clock time of the last update, 0 if never set.
value_t var_get(const var_t *var, uint64_t *timestamp_ns);
void var_set(var_t *var, value_t value);

const char *dir_name(dir_t dir);

#endif
