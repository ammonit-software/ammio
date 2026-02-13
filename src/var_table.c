#include "var_table.h"
#include "uthash.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <threads.h>

typedef struct
{
    char name[256];
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

int var_table_add(const char *name, type_t type)
{
    var_entry_t *entry = calloc(1, sizeof(var_entry_t));
    if (!entry)
        return -1;

    strncpy(entry->name, name, 255);
    strncpy(entry->var.name, name, 255);
    entry->var.type = type;

    mtx_lock(&var_table_mutex);
    HASH_ADD_STR(var_table, name, entry);
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

int var_table_get(const char *name, var_t *out)
{
    if (!name || !out)
        return -1;

    var_entry_t *entry = NULL;
    mtx_lock(&var_table_mutex);
    HASH_FIND_STR(var_table, name, entry);
    if (entry)
        *out = entry->var;
    mtx_unlock(&var_table_mutex);

    return entry ? 0 : -1;
}

int var_table_set(const char *name, const var_t *in)
{
    if (!name || !in)
        return -1;

    var_entry_t *entry = NULL;
    mtx_lock(&var_table_mutex);
    HASH_FIND_STR(var_table, name, entry);
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
