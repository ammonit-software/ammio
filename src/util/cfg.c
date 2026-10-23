#include "util/cfg.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "compat/net.h"

static void cfg_report(bool is_error, const char *path, const char *fmt, va_list args)
{
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    if (is_error)
        log_error("config: %s: %s", path && path[0] ? path : "(root)", msg);
    else
        log_warn("config: %s: %s", path && path[0] ? path : "(root)", msg);
}

void cfg_error(cfg_t *cfg, const char *path, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cfg_report(true, path, fmt, args);
    va_end(args);
    cfg->errors++;
}

void cfg_warn(cfg_t *cfg, const char *path, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cfg_report(false, path, fmt, args);
    va_end(args);
    cfg->warnings++;
}

const char *cfg_path(char *buf, const char *parent, const char *child)
{
    if (parent && parent[0])
        snprintf(buf, CFG_PATH_MAX, "%s.%s", parent, child);
    else
        snprintf(buf, CFG_PATH_MAX, "%s", child);
    return buf;
}

const char *cfg_path_index(char *buf, const char *parent, int index)
{
    snprintf(buf, CFG_PATH_MAX, "%s[%d]", parent ? parent : "", index);
    return buf;
}

bool cfg_check_keys(cfg_t *cfg, const char *path, const cJSON *obj, const char *const *allowed)
{
    bool ok = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj)
    {
        const char *key = item->string;
        if (!key || key[0] == '_' || strcmp(key, "description") == 0)
            continue;

        bool found = false;
        for (const char *const *a = allowed; *a; a++)
        {
            if (strcmp(key, *a) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            cfg_error(cfg, path, "unknown key '%s'", key);
            ok = false;
        }
    }
    return ok;
}

bool cfg_expect_object(cfg_t *cfg, const char *path, const cJSON *item)
{
    if (cJSON_IsObject(item))
        return true;
    cfg_error(cfg, path, "must be an object");
    return false;
}

bool cfg_expect_array(cfg_t *cfg, const char *path, const cJSON *item)
{
    if (cJSON_IsArray(item))
        return true;
    cfg_error(cfg, path, "must be an array");
    return false;
}

// Looks up a key; reports a missing required key. Returns NULL when absent.
static const cJSON *lookup(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item && required)
        cfg_error(cfg, path, "missing required key '%s'", key);
    return item;
}

bool cfg_get_object(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    const cJSON **out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;
    if (!cJSON_IsObject(item))
    {
        cfg_error(cfg, path, "'%s' must be an object", key);
        return false;
    }
    *out = item;
    return true;
}

bool cfg_get_array(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                   const cJSON **out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;
    if (!cJSON_IsArray(item))
    {
        cfg_error(cfg, path, "'%s' must be an array", key);
        return false;
    }
    *out = item;
    return true;
}

bool cfg_get_string(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    const char **out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;
    if (!cJSON_IsString(item) || !item->valuestring)
    {
        cfg_error(cfg, path, "'%s' must be a string", key);
        return false;
    }
    *out = item->valuestring;
    return true;
}

bool cfg_get_bool(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  bool *out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;
    if (!cJSON_IsBool(item))
    {
        cfg_error(cfg, path, "'%s' must be true or false", key);
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

bool cfg_get_int(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                 int64_t min, int64_t max, int64_t *out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;
    if (!cJSON_IsNumber(item) || item->valuedouble != floor(item->valuedouble))
    {
        cfg_error(cfg, path, "'%s' must be an integer", key);
        return false;
    }
    double d = item->valuedouble;
    if (d < (double)min || d > (double)max)
    {
        cfg_error(cfg, path, "'%s' = %.0f is out of range [%lld, %lld]", key, d,
                  (long long)min, (long long)max);
        return false;
    }
    *out = (int64_t)d;
    return true;
}

bool cfg_get_uint32(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                    uint32_t min, uint32_t max, uint32_t *out)
{
    int64_t v;
    if (!cfg_get_int(cfg, path, obj, key, required, min, max, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool cfg_get_type(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  ammio_type_t *out)
{
    const char *str;
    if (!cfg_get_string(cfg, path, obj, key, required, &str))
        return false;
    if (!ammio_type_from_string(str, out))
    {
        cfg_error(cfg, path, "'%s' = \"%s\" is not a valid type (expected one of: %s)", key, str,
                  ammio_type_list());
        return false;
    }
    return true;
}

bool cfg_get_enum(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  const char *const *choices, int *out)
{
    const char *str;
    if (!cfg_get_string(cfg, path, obj, key, required, &str))
        return false;

    char expected[256] = "";
    for (int i = 0; choices[i]; i++)
    {
        if (strcmp(str, choices[i]) == 0)
        {
            *out = i;
            return true;
        }
        size_t len = strlen(expected);
        snprintf(expected + len, sizeof(expected) - len, "%s\"%s\"", i ? ", " : "", choices[i]);
    }
    cfg_error(cfg, path, "'%s' = \"%s\" is not valid (expected one of: %s)", key, str, expected);
    return false;
}

bool cfg_get_value(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                   ammio_type_t type, ammio_value_t *out)
{
    const cJSON *item = lookup(cfg, path, obj, key, required);
    if (!item)
        return false;

    switch (ammio_value_from_json(type, item, out))
    {
        case AMMIO_VALUE_OK:
            return true;
        case AMMIO_VALUE_OUT_OF_RANGE:
            cfg_error(cfg, path, "'%s' is out of range for type %s", key, ammio_type_name(type));
            return false;
        default:
            cfg_error(cfg, path, "'%s' is not a valid %s value", key, ammio_type_name(type));
            return false;
    }
}

bool cfg_get_ipv4(cfg_t *cfg, const char *path, const cJSON *obj, const char *key, bool required,
                  uint32_t *out)
{
    const char *str;
    if (!cfg_get_string(cfg, path, obj, key, required, &str))
        return false;

    struct in_addr addr;
    if (inet_pton(AF_INET, str, &addr) != 1)
    {
        cfg_error(cfg, path, "'%s' = \"%s\" is not a valid IPv4 address", key, str);
        return false;
    }
    *out = ntohl(addr.s_addr);
    return true;
}

bool cfg_is_valid_id(const char *id)
{
    size_t len = id ? strlen(id) : 0;
    if (len == 0 || len > 255)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)id[i];
        if (c <= ' ' || c >= 0x7F || c == '"' || c == '\\')
            return false;
    }
    return true;
}
