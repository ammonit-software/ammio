#include "conn.h"
#include "json.h"
#include "log.h"
#include "protocol.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define RETRY_MS 1000
#define ERR_LEN  256

static const protocol_t *const protocols[] = {
    &modbus_protocol,
    &opcua_protocol,
    &trdp_protocol,
};
#define PROTOCOL_COUNT (sizeof(protocols) / sizeof(protocols[0]))

typedef struct
{
    const char *name; // owned by the config JSON, which outlives the connection
    const protocol_t *proto;
    void *self;
    uint32_t period_ms;

    thrd_t thread;
    bool started;
    atomic_bool stop;
    mtx_t mtx;
    cnd_t wake;
} conn_t;

static conn_t *conns = NULL;
static size_t conn_count = 0;

var_t *conn_add_var(const char *ctx, const char *id, type_t type, dir_t dir)
{
    if (vars_find(id))
    {
        log_error("%s: duplicate var_id '%s'", ctx, id);
        return NULL;
    }
    var_t *var = vars_add(id, type, dir);
    if (!var)
        log_error("%s: out of memory", ctx);
    return var;
}

void conn_report(const char *conn, const char *what, bool *state, const char *problem)
{
    if (problem && !*state)
        log_warn("%s: '%s': %s", conn, what, problem);
    else if (!problem && *state)
        log_info("%s: '%s': OK again", conn, what);
    *state = problem != NULL;
}

// --- config ------------------------------------------------------------------

static const protocol_t *find_protocol(const char *name)
{
    for (size_t i = 0; i < PROTOCOL_COUNT; i++)
        if (strcmp(protocols[i]->name, name) == 0)
            return protocols[i];
    return NULL;
}

static void destroy_all(void)
{
    for (size_t i = 0; i < conn_count; i++)
    {
        conns[i].proto->destroy(conns[i].self);
        mtx_destroy(&conns[i].mtx);
        cnd_destroy(&conns[i].wake);
    }
    free(conns);
    conns = NULL;
    conn_count = 0;
}

int conns_load(const cJSON *root)
{
    if (!cJSON_IsObject(root) || !root->child)
    {
        log_error("config: expected an object of connections, "
                  "e.g. {\"plc\": {\"protocol\": \"modbus\", ...}}");
        return -1;
    }

    size_t n = (size_t)cJSON_GetArraySize(root);
    conns = calloc(n, sizeof(*conns));
    if (!conns)
        return -1;

    bool ok = true;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, root)
    {
        const char *name = entry->string;
        const char *proto_name = NULL;

        bool duplicate = false;
        for (const cJSON *prev = root->child; prev != entry && !duplicate; prev = prev->next)
            duplicate = strcmp(prev->string, name) == 0;
        if (duplicate)
        {
            log_error("%s: duplicate connection name", name);
            ok = false;
            continue;
        }
        if (!cJSON_IsObject(entry))
        {
            log_error("%s: must be an object", name);
            ok = false;
            continue;
        }
        if (cJSON_GetObjectItemCaseSensitive(entry, "interface") &&
            cJSON_GetObjectItemCaseSensitive(entry, "specification"))
        {
            log_error("%s: this is the ammio v0.5 config format (\"interface\"/\"specification\"); "
                      "see \"Migrating from v0.5\" in the README", name);
            ok = false;
            continue;
        }
        if (!json_need_string(entry, "protocol", name, &proto_name))
        {
            ok = false;
            continue;
        }

        const protocol_t *proto = find_protocol(proto_name);
        if (!proto)
        {
            log_error("%s.protocol: unknown protocol '%s' (expected: modbus, opcua, trdp)",
                      name, proto_name);
            ok = false;
            continue;
        }

        size_t vars_before = vars_count();
        uint32_t period_ms = 0;
        void *self = proto->create(name, entry, &period_ms);
        if (!self)
        {
            ok = false;
            continue;
        }

        conn_t *c = &conns[conn_count++];
        c->name = name;
        c->proto = proto;
        c->self = self;
        c->period_ms = period_ms;
        atomic_init(&c->stop, false);
        mtx_init(&c->mtx, mtx_plain);
        cnd_init(&c->wake);
        size_t added = vars_count() - vars_before;
        log_info("%s: %s, %zu variable%s", name, proto->name, added, added == 1 ? "" : "s");
    }

    if (!ok)
    {
        destroy_all();
        vars_clear();
        return -1;
    }
    return 0;
}

// --- runner ------------------------------------------------------------------

static void add_ms(struct timespec *ts, uint32_t ms)
{
    ts->tv_sec += (time_t)(ms / 1000);
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

// Sleeps until `deadline`; returns true if the connection is being stopped.
static bool sleep_until(conn_t *c, const struct timespec *deadline)
{
    mtx_lock(&c->mtx);
    while (!atomic_load(&c->stop) && cnd_timedwait(&c->wake, &c->mtx, deadline) == thrd_success)
        ;
    mtx_unlock(&c->mtx);
    return atomic_load(&c->stop);
}

static bool sleep_ms(conn_t *c, uint32_t ms)
{
    struct timespec deadline;
    timespec_get(&deadline, TIME_UTC);
    add_ms(&deadline, ms);
    return sleep_until(c, &deadline);
}

static int run(void *arg)
{
    conn_t *c = arg;
    char err[ERR_LEN], last_err[ERR_LEN] = "";
    bool up = false;
    struct timespec next = {0};

    while (!atomic_load(&c->stop))
    {
        if (!up)
        {
            err[0] = '\0';
            if (c->proto->connect(c->self, err, sizeof(err)) != 0)
            {
                // Log the first failure and any change of reason, not every retry.
                if (strcmp(err, last_err) != 0)
                {
                    log_warn("%s: cannot connect: %s (retrying every %d s)", c->name, err, RETRY_MS / 1000);
                    strcpy(last_err, err);
                }
                if (sleep_ms(c, RETRY_MS))
                    break;
                continue;
            }
            up = true;
            last_err[0] = '\0';
            log_info("%s: connected", c->name);
            timespec_get(&next, TIME_UTC);
        }

        err[0] = '\0';
        if (c->proto->cycle(c->self, err, sizeof(err)) != 0)
        {
            log_warn("%s: connection lost: %s", c->name, err);
            c->proto->disconnect(c->self);
            up = false;
            if (sleep_ms(c, RETRY_MS))
                break;
            continue;
        }

        if (c->period_ms)
        {
            // Fixed-rate schedule; if a cycle overran, restart from now instead of bursting.
            struct timespec now;
            timespec_get(&now, TIME_UTC);
            add_ms(&next, c->period_ms);
            if (before(&next, &now))
                next = now;
            if (sleep_until(c, &next))
                break;
        }
    }

    if (up)
        c->proto->disconnect(c->self);
    return 0;
}

int conns_start(void)
{
    for (size_t i = 0; i < conn_count; i++)
    {
        if (thrd_create(&conns[i].thread, run, &conns[i]) != thrd_success)
        {
            log_error("%s: cannot start thread", conns[i].name);
            return -1;
        }
        conns[i].started = true;
    }
    return 0;
}

void conns_stop(void)
{
    for (size_t i = 0; i < conn_count; i++)
    {
        conn_t *c = &conns[i];
        mtx_lock(&c->mtx);
        atomic_store(&c->stop, true);
        cnd_broadcast(&c->wake);
        mtx_unlock(&c->mtx);
    }
    for (size_t i = 0; i < conn_count; i++)
        if (conns[i].started)
            thrd_join(conns[i].thread, NULL);
    destroy_all();
}
