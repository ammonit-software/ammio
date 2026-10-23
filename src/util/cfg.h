#ifndef AMMIO_CFG_H
#define AMMIO_CFG_H

// Strict configuration readers. Every problem is reported with the JSON path where it was
// found and counted in cfg_t; callers check cfg->errors once the whole file has been read,
// so a single run reports every mistake instead of stopping at the first one.
//
// Getter convention:
//   - key missing and required  -> error, returns false
//   - key missing and optional  -> returns false, *out untouched (preset the default)
//   - key present but invalid   -> error, returns false
//   - key present and valid     -> returns true

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "core/types.h"
#include "util/log.h"

#define CFG_PATH_MAX 512

typedef struct
{
    int errors;
    int warnings;
} cfg_t;

void cfg_error(cfg_t *cfg, const char *path, const char *fmt, ...) AMMIO_PRINTF(3, 4);
void cfg_warn(cfg_t *cfg, const char *path, const char *fmt, ...) AMMIO_PRINTF(3, 4);

// Builds a child path: cfg_path(buf, "plc.settings", "registers") -> "plc.settings.registers"
const char *cfg_path(char *buf, const char *parent, const char *child);
const char *cfg_path_index(char *buf, const char *parent, int index);

// Rejects keys that are not in the NULL-terminated allowed list (catches typos such as "adress").
// "description" and keys starting with "_" are always allowed, so files can carry comments.
bool cfg_check_keys(cfg_t *cfg, const char *path, const cJSON *obj, const char *const *allowed);

bool cfg_expect_object(cfg_t *cfg, const char *path, const cJSON *item);
bool cfg_expect_array(cfg_t *cfg, const char *path, const cJSON *item);

bool cfg_get_object(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    const cJSON **out);
bool cfg_get_array(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                   const cJSON **out);
bool cfg_get_string(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    const char **out);
bool cfg_get_bool(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  bool *out);
bool cfg_get_int(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                 int64_t min, int64_t max, int64_t *out);
bool cfg_get_uint32(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    uint32_t min, uint32_t max, uint32_t *out);
bool cfg_get_type(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  ammio_type_t *out);

// Reads one of a fixed set of strings and returns its index in *out
bool cfg_get_enum(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  const char *const *choices, int *out);

// Reads a value for a variable of the given type (e.g. "initial"), range-checked
bool cfg_get_value(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                   ammio_type_t type, ammio_value_t *out);

// Reads a dotted IPv4 address; returns it in host byte order
bool cfg_get_ipv4(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  uint32_t *out);

// Variable and connection identifiers: printable ASCII, no whitespace, no quotes, 1..255 chars
bool cfg_is_valid_id(const char *id);

#endif
