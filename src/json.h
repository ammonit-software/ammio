#ifndef AMMIO_JSON_H
#define AMMIO_JSON_H

// Strict config-reading helpers. Every helper logs a precise error
// ("<ctx>.<key>: <problem>") and returns false when the value is invalid.
// "get" helpers accept a missing key (leaving *out untouched),
// "need" helpers report it as an error.

#include "cJSON.h"
#include "vars.h"

#include <stdbool.h>
#include <stdint.h>

cJSON *json_load_file(const char *path);

// `allowed` is a NULL-terminated list. Reports every unknown key.
bool json_check_keys(const cJSON *obj, const char *ctx, const char *const *allowed);

bool json_get_string(const cJSON *obj, const char *key, const char *ctx, const char **out);
bool json_need_string(const cJSON *obj, const char *key, const char *ctx, const char **out);

bool json_get_uint(const cJSON *obj, const char *key, const char *ctx,
                   uint32_t min, uint32_t max, uint32_t *out);
bool json_need_uint(const cJSON *obj, const char *key, const char *ctx,
                    uint32_t min, uint32_t max, uint32_t *out);

// Optional array; *out is NULL when the key is missing.
bool json_get_array(const cJSON *obj, const char *key, const char *ctx, const cJSON **out);

// Required "type" key holding one of the variable types.
bool json_need_type(const cJSON *obj, const char *ctx, type_t *out);

#endif
