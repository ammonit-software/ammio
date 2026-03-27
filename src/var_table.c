#include "var_table.h"
#include "uthash.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <threads.h>

typedef struct
{
    char var_id[256];
    var_t var;
    UT_hash_handle hh;
} var_entry_t;

static var_entry_t *var_table = NULL;
static mtx_t var_table_mutex;

static uint64_t get_timestamp_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

type_t var_table_type_from_string(const char *type_str)
{
    if (strcmp(type_str, "uint8") == 0) return TYPE_UINT8;
    if (strcmp(type_str, "int8") == 0) return TYPE_INT8;
    if (strcmp(type_str, "uint16") == 0) return TYPE_UINT16;
    if (strcmp(type_str, "int16") == 0) return TYPE_INT16;
    if (strcmp(type_str, "uint32") == 0) return TYPE_UINT32;
    if (strcmp(type_str, "int32") == 0) return TYPE_INT32;
    if (strcmp(type_str, "float32") == 0) return TYPE_FLOAT32;
    if (strcmp(type_str, "float64") == 0) return TYPE_FLOAT64;
    return TYPE_UINT8;
}

int var_table_add(const char *var_id, type_t type, dir_t dir)
{
    var_entry_t *entry = calloc(1, sizeof(var_entry_t));
    if (!entry)
        return -1;

    strncpy(entry->var_id, var_id, 255);
    strncpy(entry->var.var_id, var_id, 255);
    entry->var.type = type;
    entry->var.dir = dir;

    mtx_lock(&var_table_mutex);
    HASH_ADD_STR(var_table, var_id, entry);
    mtx_unlock(&var_table_mutex);
    return 0;
}

int var_table_init(void)
{
    mtx_init(&var_table_mutex, mtx_plain);
    var_table = NULL;
    return 0;
}

void var_table_cleanup(void)
{
    mtx_lock(&var_table_mutex);
    var_entry_t *entry, *tmp;
    HASH_ITER(hh, var_table, entry, tmp)
    {
        HASH_DEL(var_table, entry);
        free(entry);
    }
    var_table = NULL;
    mtx_unlock(&var_table_mutex);
    mtx_destroy(&var_table_mutex);
}

int var_table_get(const char *var_id, var_t *out)
{
    if (!var_id || !out)
        return -1;

    var_entry_t *entry = NULL;
    mtx_lock(&var_table_mutex);
    HASH_FIND_STR(var_table, var_id, entry);
    if (entry)
        *out = entry->var;
    mtx_unlock(&var_table_mutex);

    return entry ? 0 : -1;
}

int var_table_set(const char *var_id, const var_t *in)
{
    if (!var_id || !in)
        return -1;

    var_entry_t *entry = NULL;
    mtx_lock(&var_table_mutex);
    HASH_FIND_STR(var_table, var_id, entry);
    if (!entry)
    {
        mtx_unlock(&var_table_mutex);
        return -1;
    }
    entry->var.value = in->value;
    entry->var.timestamp = get_timestamp_ns();
    mtx_unlock(&var_table_mutex);
    return 0;
}

int var_table_get_all(var_t **out, size_t *count)
{
    if (!out || !count)
        return -1;

    mtx_lock(&var_table_mutex);
    size_t n = HASH_COUNT(var_table);
    if (n == 0)
    {
        mtx_unlock(&var_table_mutex);
        *out = NULL;
        *count = 0;
        return 0;
    }

    var_t *arr = calloc(n, sizeof(var_t));
    if (!arr)
    {
        mtx_unlock(&var_table_mutex);
        return -1;
    }

    size_t i = 0;
    var_entry_t *entry, *tmp;
    HASH_ITER(hh, var_table, entry, tmp)
    {
        arr[i++] = entry->var;
    }
    mtx_unlock(&var_table_mutex);

    *out = arr;
    *count = n;
    return 0;
}

const char *var_table_type_to_string(type_t type)
{
    switch (type)
    {
        case TYPE_UINT8:   return "uint8";
        case TYPE_INT8:    return "int8";
        case TYPE_UINT16:  return "uint16";
        case TYPE_INT16:   return "int16";
        case TYPE_UINT32:  return "uint32";
        case TYPE_INT32:   return "int32";
        case TYPE_FLOAT32: return "float32";
        case TYPE_FLOAT64: return "float64";
        default:           return "unknown";
    }
}

const char *var_table_dir_to_string(dir_t dir)
{
    switch (dir)
    {
        case DIR_INPUT:  return "input";
        case DIR_OUTPUT: return "output";
        default:         return "unknown";
    }
}
