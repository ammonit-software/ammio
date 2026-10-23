#include "core/var_table.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "uthash.h"
#include "util/clock.h"

struct ammio_var
{
    char *id;
    char *connection;
    ammio_type_t type;
    ammio_dir_t dir;
    ammio_value_t value;
    bool has_value;
    ammio_quality_t quality;
    uint64_t timestamp_ns;
    uint32_t write_seq;
    UT_hash_handle hh;
};

static ammio_var_t *table = NULL;
static mtx_t table_mutex;
static bool table_initialized = false;

static var_change_hook_t change_hook = NULL;
static void *change_hook_ctx = NULL;

// ---------------------------------------------------------------------------------------------
// Change notification: snapshots of changed variables are collected while the lock is held and
// delivered to the hook after it is released.
// ---------------------------------------------------------------------------------------------

#define CHANGES_INLINE 32

typedef struct
{
    ammio_var_snapshot_t inline_buf[CHANGES_INLINE];
    ammio_var_snapshot_t *buf;
    size_t count;
    size_t capacity;
} changes_t;

static void changes_init(changes_t *c, size_t expected)
{
    c->count = 0;
    c->buf = c->inline_buf;
    c->capacity = CHANGES_INLINE;
    if (change_hook && expected > CHANGES_INLINE)
    {
        ammio_var_snapshot_t *heap = malloc(expected * sizeof(*heap));
        if (heap)
        {
            c->buf = heap;
            c->capacity = expected;
        }
    }
}

static void snapshot_of(const ammio_var_t *var, ammio_var_snapshot_t *out)
{
    out->var_id = var->id;
    out->connection = var->connection;
    out->type = var->type;
    out->dir = var->dir;
    out->value = var->value;
    out->has_value = var->has_value;
    out->quality = var->quality;
    out->timestamp_ns = var->timestamp_ns;
    out->write_seq = var->write_seq;
}

static void changes_add(changes_t *c, const ammio_var_t *var)
{
    if (change_hook && c->count < c->capacity)
        snapshot_of(var, &c->buf[c->count++]);
}

static void changes_flush(changes_t *c)
{
    var_change_hook_t hook = change_hook;
    if (hook)
    {
        for (size_t i = 0; i < c->count; i++)
            hook(&c->buf[i], change_hook_ctx);
    }
    if (c->buf != c->inline_buf)
        free(c->buf);
}

// ---------------------------------------------------------------------------------------------

int var_table_init(void)
{
    if (table_initialized)
        return 0;
    if (mtx_init(&table_mutex, mtx_plain) != thrd_success)
        return -1;
    table = NULL;
    table_initialized = true;
    return 0;
}

void var_table_free(void)
{
    if (!table_initialized)
        return;

    mtx_lock(&table_mutex);
    ammio_var_t *var, *tmp;
    HASH_ITER(hh, table, var, tmp)
    {
        HASH_DEL(table, var);
        free(var->id);
        free(var->connection);
        free(var);
    }
    table = NULL;
    change_hook = NULL;
    change_hook_ctx = NULL;
    mtx_unlock(&table_mutex);
    mtx_destroy(&table_mutex);
    table_initialized = false;
}

ammio_var_t *var_table_add(const char *id, ammio_type_t type, ammio_dir_t dir, const char *connection)
{
    ammio_var_t *var = calloc(1, sizeof(*var));
    if (!var)
        return NULL;
    var->id = ammio_strdup(id);
    var->connection = ammio_strdup(connection ? connection : "");
    if (!var->id || !var->connection)
    {
        free(var->id);
        free(var->connection);
        free(var);
        return NULL;
    }
    var->type = type;
    var->dir = dir;
    var->quality = AMMIO_QUALITY_NO_DATA;

    mtx_lock(&table_mutex);
    ammio_var_t *existing = NULL;
    HASH_FIND_STR(table, id, existing);
    if (existing)
    {
        mtx_unlock(&table_mutex);
        free(var->id);
        free(var->connection);
        free(var);
        return NULL;
    }
    HASH_ADD_KEYPTR(hh, table, var->id, strlen(var->id), var);
    mtx_unlock(&table_mutex);
    return var;
}

ammio_var_t *var_table_find(const char *id)
{
    if (!id)
        return NULL;
    ammio_var_t *var = NULL;
    mtx_lock(&table_mutex);
    HASH_FIND_STR(table, id, var);
    mtx_unlock(&table_mutex);
    return var;
}

size_t var_table_count(void)
{
    mtx_lock(&table_mutex);
    size_t n = HASH_COUNT(table);
    mtx_unlock(&table_mutex);
    return n;
}

// id, type and dir never change after registration, so these need no lock
const char *var_id(const ammio_var_t *var) { return var->id; }
ammio_type_t var_type(const ammio_var_t *var) { return var->type; }
ammio_dir_t var_dir(const ammio_var_t *var) { return var->dir; }

void var_table_read(const ammio_var_t *var, ammio_var_snapshot_t *out)
{
    mtx_lock(&table_mutex);
    snapshot_of(var, out);
    mtx_unlock(&table_mutex);
}

void var_table_read_many(ammio_var_t *const *vars, size_t count, ammio_var_snapshot_t *out)
{
    mtx_lock(&table_mutex);
    for (size_t i = 0; i < count; i++)
        snapshot_of(vars[i], &out[i]);
    mtx_unlock(&table_mutex);
}

int var_table_list(ammio_var_snapshot_t **out, size_t *count)
{
    mtx_lock(&table_mutex);
    size_t n = HASH_COUNT(table);
    ammio_var_snapshot_t *arr = NULL;
    if (n > 0)
    {
        arr = calloc(n, sizeof(*arr));
        if (!arr)
        {
            mtx_unlock(&table_mutex);
            return -1;
        }
        size_t i = 0;
        ammio_var_t *var, *tmp;
        HASH_ITER(hh, table, var, tmp)
        {
            snapshot_of(var, &arr[i++]);
        }
    }
    mtx_unlock(&table_mutex);

    *out = arr;
    *count = n;
    return 0;
}

// Stores a value; returns true if anything observable changed. Caller holds the lock.
static bool store(ammio_var_t *var, ammio_value_t value, ammio_quality_t quality, uint64_t now)
{
    bool changed = !var->has_value || var->quality != quality ||
                   !ammio_value_equal(var->type, var->value, value);
    var->value = value;
    var->has_value = true;
    var->quality = quality;
    var->timestamp_ns = now;
    return changed;
}

void var_table_client_write_many(ammio_var_t *const *vars, const ammio_value_t *values, size_t count)
{
    changes_t changes;
    changes_init(&changes, count);
    uint64_t now = clock_realtime_ns();

    mtx_lock(&table_mutex);
    for (size_t i = 0; i < count; i++)
    {
        ammio_var_t *var = vars[i];
        // An input that is written while its connection is down keeps the DISCONNECTED flag
        ammio_quality_t q = var->quality == AMMIO_QUALITY_DISCONNECTED ? AMMIO_QUALITY_DISCONNECTED
                                                                         : AMMIO_QUALITY_GOOD;
        if (store(var, values[i], q, now))
            changes_add(&changes, var);
        var->write_seq++;
    }
    mtx_unlock(&table_mutex);

    changes_flush(&changes);
}

void var_table_update(ammio_var_t *var, ammio_value_t value, ammio_quality_t quality)
{
    var_table_update_many(&var, &value, 1, quality);
}

void var_table_update_many(ammio_var_t *const *vars, const ammio_value_t *values, size_t count,
                           ammio_quality_t quality)
{
    changes_t changes;
    changes_init(&changes, count);
    uint64_t now = clock_realtime_ns();

    mtx_lock(&table_mutex);
    for (size_t i = 0; i < count; i++)
    {
        if (store(vars[i], values[i], quality, now))
            changes_add(&changes, vars[i]);
    }
    mtx_unlock(&table_mutex);

    changes_flush(&changes);
}

void var_table_set_value_silent(ammio_var_t *var, ammio_value_t value)
{
    changes_t changes;
    changes_init(&changes, 1);

    mtx_lock(&table_mutex);
    bool changed = !var->has_value || !ammio_value_equal(var->type, var->value, value);
    var->value = value;
    var->has_value = true;
    if (var->quality == AMMIO_QUALITY_NO_DATA)
        var->quality = AMMIO_QUALITY_GOOD;
    if (changed)
    {
        var->timestamp_ns = clock_realtime_ns();
        changes_add(&changes, var);
    }
    mtx_unlock(&table_mutex);

    changes_flush(&changes);
}

void var_table_set_quality_many(ammio_var_t *const *vars, size_t count, ammio_quality_t quality)
{
    changes_t changes;
    changes_init(&changes, count);

    mtx_lock(&table_mutex);
    for (size_t i = 0; i < count; i++)
    {
        if (vars[i]->quality != quality)
        {
            vars[i]->quality = quality;
            changes_add(&changes, vars[i]);
        }
    }
    mtx_unlock(&table_mutex);

    changes_flush(&changes);
}

void var_table_mark_connection(const char *connection, bool connected)
{
    changes_t changes;
    mtx_lock(&table_mutex);
    changes_init(&changes, HASH_COUNT(table));

    ammio_var_t *var, *tmp;
    HASH_ITER(hh, table, var, tmp)
    {
        if (strcmp(var->connection, connection) != 0)
            continue;

        ammio_quality_t q = var->quality;
        if (!connected)
            q = AMMIO_QUALITY_DISCONNECTED;
        else if (!var->has_value)
            q = AMMIO_QUALITY_NO_DATA;
        else if (var->dir == AMMIO_DIR_INPUT)
            q = AMMIO_QUALITY_GOOD;
        // outputs holding an old value stay DISCONNECTED until fresh data arrives

        if (q != var->quality)
        {
            var->quality = q;
            changes_add(&changes, var);
        }
    }
    mtx_unlock(&table_mutex);

    changes_flush(&changes);
}

void var_table_set_change_hook(var_change_hook_t hook, void *ctx)
{
    mtx_lock(&table_mutex);
    change_hook = hook;
    change_hook_ctx = ctx;
    mtx_unlock(&table_mutex);
}

const char *ammio_quality_name(ammio_quality_t quality)
{
    switch (quality)
    {
        case AMMIO_QUALITY_NO_DATA:      return "no_data";
        case AMMIO_QUALITY_GOOD:         return "good";
        case AMMIO_QUALITY_STALE:        return "stale";
        case AMMIO_QUALITY_DISCONNECTED: return "disconnected";
        case AMMIO_QUALITY_BAD:          return "bad";
        default:                         return "unknown";
    }
}

const char *ammio_dir_name(ammio_dir_t dir)
{
    return dir == AMMIO_DIR_INPUT ? "input" : "output";
}
