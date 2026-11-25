#include "json.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cJSON *json_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        log_error("%s: cannot open file", path);
        return NULL;
    }

    char *data = NULL;
    size_t len = 0;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        long size = ftell(f);
        if (size >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)size + 1)))
            len = fread(data, 1, (size_t)size, f);
    }
    fclose(f);

    if (!data)
    {
        log_error("%s: cannot read file", path);
        return NULL;
    }
    data[len] = '\0';

    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(data, len + 1, &end, true); // +1: the NUL counts
    if (!json)
    {
        int line = 1;
        for (const char *p = data; end && p < end; p++)
            line += *p == '\n';
        log_error("%s:%d: invalid JSON", path, line);
    }
    free(data);
    return json;
}

bool json_check_keys(const cJSON *obj, const char *ctx, const char *const *allowed)
{
    bool ok = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj)
    {
        bool known = false;
        for (const char *const *k = allowed; *k && !known; k++)
            known = strcmp(item->string, *k) == 0;
        if (known)
            continue;

        char list[512] = "";
        for (const char *const *k = allowed; *k; k++)
        {
            if (k != allowed)
                strncat(list, ", ", sizeof(list) - strlen(list) - 1);
            strncat(list, *k, sizeof(list) - strlen(list) - 1);
        }
        log_error("%s: unknown key '%s' (expected: %s)", ctx, item->string, list);
        ok = false;
    }
    return ok;
}

static bool get_string(const cJSON *obj, const char *key, const char *ctx, bool required,
                       const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item)
    {
        if (required)
            log_error("%s: missing '%s'", ctx, key);
        return !required;
    }
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0')
    {
        log_error("%s.%s: must be a non-empty string", ctx, key);
        return false;
    }
    *out = item->valuestring;
    return true;
}

bool json_get_string(const cJSON *obj, const char *key, const char *ctx, const char **out)
{
    return get_string(obj, key, ctx, false, out);
}

bool json_need_string(const cJSON *obj, const char *key, const char *ctx, const char **out)
{
    return get_string(obj, key, ctx, true, out);
}

static bool get_uint(const cJSON *obj, const char *key, const char *ctx, bool required,
                     uint32_t min, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item)
    {
        if (required)
            log_error("%s: missing '%s'", ctx, key);
        return !required;
    }
    double d = cJSON_IsNumber(item) ? item->valuedouble : -1;
    if (d < min || d > max || d != floor(d))
    {
        log_error("%s.%s: must be an integer in %u..%u", ctx, key, min, max);
        return false;
    }
    *out = (uint32_t)d;
    return true;
}

bool json_get_uint(const cJSON *obj, const char *key, const char *ctx,
                   uint32_t min, uint32_t max, uint32_t *out)
{
    return get_uint(obj, key, ctx, false, min, max, out);
}

bool json_need_uint(const cJSON *obj, const char *key, const char *ctx,
                    uint32_t min, uint32_t max, uint32_t *out)
{
    return get_uint(obj, key, ctx, true, min, max, out);
}

bool json_get_array(const cJSON *obj, const char *key, const char *ctx, const cJSON **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    *out = NULL;
    if (!item)
        return true;
    if (!cJSON_IsArray(item))
    {
        log_error("%s.%s: must be an array", ctx, key);
        return false;
    }
    *out = item;
    return true;
}

bool json_need_type(const cJSON *obj, const char *ctx, type_t *out)
{
    const char *str = NULL;
    if (!json_need_string(obj, "type", ctx, &str))
        return false;
    if (!type_parse(str, out))
    {
        log_error("%s.type: unknown type '%s' (expected: %s)", ctx, str, type_names());
        return false;
    }
    return true;
}
