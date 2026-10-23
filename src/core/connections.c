#include "core/connections.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/driver.h"
#include "util/log.h"

struct ammio_conn
{
    char *name;
    const ammio_driver_t *driver;
    void *ctx;
    bool started;
    atomic_int state;
    ammio_var_t *status_var;
};

static const ammio_driver_t *const drivers[] = {
    &modbus_driver,
    &opcua_driver,
    &trdp_driver,
};
#define DRIVER_COUNT (sizeof(drivers) / sizeof(drivers[0]))

static ammio_conn_t **conns = NULL;
static size_t conn_count = 0;

static const ammio_driver_t *find_driver(const char *protocol)
{
    for (size_t i = 0; i < DRIVER_COUNT; i++)
    {
        if (strcmp(drivers[i]->protocol, protocol) == 0)
            return drivers[i];
    }
    return NULL;
}

static void protocol_list(char *buf, size_t size)
{
    buf[0] = '\0';
    for (size_t i = 0; i < DRIVER_COUNT; i++)
    {
        size_t len = strlen(buf);
        snprintf(buf + len, size - len, "%s\"%s\"", i ? ", " : "", drivers[i]->protocol);
    }
}

static void conn_free(ammio_conn_t *conn)
{
    if (!conn)
        return;
    if (conn->ctx && conn->driver->destroy)
        conn->driver->destroy(conn->ctx);
    free(conn->name);
    free(conn);
}

// Reads one top-level entry; returns the connection or NULL (errors reported via cfg)
static ammio_conn_t *load_connection(const char *name, const cJSON *entry, cfg_t *cfg)
{
    static const char *const keys[] = {"protocol", "settings", "interface", "specification", NULL};

    if (!cfg_is_valid_id(name))
    {
        cfg_error(cfg, name, "invalid connection name (use 1-255 printable characters, no spaces or quotes)");
        return NULL;
    }
    if (!cfg_expect_object(cfg, name, entry))
        return NULL;
    cfg_check_keys(cfg, name, entry, keys);

    // "interface" / "specification" are the names used before 0.6; still accepted
    const char *protocol = NULL;
    bool has_protocol = cfg_get_string(cfg, name, entry, "protocol", false, &protocol);
    const char *legacy_protocol = NULL;
    if (cfg_get_string(cfg, name, entry, "interface", false, &legacy_protocol))
    {
        if (has_protocol)
        {
            cfg_error(cfg, name, "use either 'protocol' or the deprecated 'interface', not both");
            return NULL;
        }
        cfg_warn(cfg, name, "'interface' is deprecated, rename it to 'protocol'");
        protocol = legacy_protocol;
    }
    if (!protocol)
    {
        if (!cJSON_GetObjectItemCaseSensitive(entry, "protocol") &&
            !cJSON_GetObjectItemCaseSensitive(entry, "interface"))
            cfg_error(cfg, name, "missing required key 'protocol'");
        return NULL;
    }

    const cJSON *settings = NULL;
    bool has_settings = cfg_get_object(cfg, name, entry, "settings", false, &settings);
    const cJSON *legacy_settings = NULL;
    if (cfg_get_object(cfg, name, entry, "specification", false, &legacy_settings))
    {
        if (has_settings)
        {
            cfg_error(cfg, name, "use either 'settings' or the deprecated 'specification', not both");
            return NULL;
        }
        cfg_warn(cfg, name, "'specification' is deprecated, rename it to 'settings'");
        settings = legacy_settings;
    }
    if (!settings)
    {
        if (!cJSON_GetObjectItemCaseSensitive(entry, "settings") &&
            !cJSON_GetObjectItemCaseSensitive(entry, "specification"))
            cfg_error(cfg, name, "missing required key 'settings'");
        return NULL;
    }

    const ammio_driver_t *driver = find_driver(protocol);
    if (!driver)
    {
        char list[128];
        protocol_list(list, sizeof(list));
        cfg_error(cfg, name, "unknown protocol \"%s\" (supported: %s)", protocol, list);
        return NULL;
    }

    ammio_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn || !(conn->name = ammio_strdup(name)))
    {
        free(conn);
        cfg_error(cfg, name, "out of memory");
        return NULL;
    }
    conn->driver = driver;
    atomic_init(&conn->state, AMMIO_CONN_STOPPED);

    // Every connection exposes its link state as a read-only variable
    char status_id[300];
    snprintf(status_id, sizeof(status_id), "%s.connected", name);
    conn->status_var = conn_add_var(conn, cfg, name, status_id, AMMIO_TYPE_BOOL, AMMIO_DIR_OUTPUT);
    if (conn->status_var)
    {
        ammio_value_t v = {.b = false};
        var_table_update(conn->status_var, v, AMMIO_QUALITY_GOOD);
    }

    char settings_path[CFG_PATH_MAX];
    cfg_path(settings_path, name, "settings");
    int errors_before = cfg->errors;
    conn->ctx = driver->create(conn, settings, settings_path, cfg);
    if (!conn->ctx || cfg->errors != errors_before)
    {
        conn_free(conn);
        return NULL;
    }
    return conn;
}

int connections_load(const cJSON *root, cfg_t *cfg)
{
    if (!cfg_expect_object(cfg, "", root))
        return -1;

    size_t n = (size_t)cJSON_GetArraySize(root);
    if (n == 0)
    {
        cfg_error(cfg, "", "no connections configured");
        return -1;
    }

    conns = calloc(n, sizeof(*conns));
    if (!conns)
        return -1;
    conn_count = 0;

    const cJSON *entry;
    cJSON_ArrayForEach(entry, root)
    {
        ammio_conn_t *conn = load_connection(entry->string, entry, cfg);
        if (conn)
            conns[conn_count++] = conn;
    }

    if (cfg->errors > 0)
    {
        connections_destroy();
        return -1;
    }
    return 0;
}

int connections_start(void)
{
    for (size_t i = 0; i < conn_count; i++)
    {
        ammio_conn_t *conn = conns[i];
        log_info("%s: starting (%s)", conn->name, conn->driver->protocol);
        conn_set_state(conn, AMMIO_CONN_CONNECTING);
        if (conn->driver->start(conn->ctx) != 0)
        {
            log_error("%s: failed to start", conn->name);
            conn->driver->stop(conn->ctx);
            conn_set_state(conn, AMMIO_CONN_STOPPED);
            return -1;
        }
        conn->started = true;
    }
    return 0;
}

void connections_stop(void)
{
    // Stop in reverse start order
    for (size_t i = conn_count; i-- > 0;)
    {
        ammio_conn_t *conn = conns[i];
        if (!conn->started)
            continue;
        log_info("%s: stopping", conn->name);
        conn->driver->stop(conn->ctx);
        conn->started = false;
        conn_set_state(conn, AMMIO_CONN_STOPPED);
    }
}

void connections_destroy(void)
{
    for (size_t i = 0; i < conn_count; i++)
        conn_free(conns[i]);
    free(conns);
    conns = NULL;
    conn_count = 0;
}

size_t connections_count(void)
{
    return conn_count;
}

size_t connections_list(ammio_conn_info_t *out, size_t max)
{
    for (size_t i = 0; i < conn_count && i < max; i++)
    {
        out[i].name = conns[i]->name;
        out[i].protocol = conns[i]->driver->protocol;
        out[i].state = (ammio_conn_state_t)atomic_load(&conns[i]->state);
    }
    return conn_count;
}

const char *ammio_conn_state_name(ammio_conn_state_t state)
{
    switch (state)
    {
        case AMMIO_CONN_STOPPED:    return "stopped";
        case AMMIO_CONN_CONNECTING: return "connecting";
        case AMMIO_CONN_CONNECTED:  return "connected";
        default:                    return "unknown";
    }
}

const char *conn_name(const ammio_conn_t *conn)
{
    return conn->name;
}

ammio_var_t *conn_add_var(ammio_conn_t *conn, cfg_t *cfg, const char *path, const char *id,
                          ammio_type_t type, ammio_dir_t dir)
{
    if (!cfg_is_valid_id(id))
    {
        cfg_error(cfg, path, "invalid var_id \"%s\" (use 1-255 printable characters, no spaces or quotes)", id);
        return NULL;
    }

    ammio_var_t *var = var_table_add(id, type, dir, conn->name);
    if (!var)
    {
        ammio_var_t *existing = var_table_find(id);
        if (existing)
            cfg_error(cfg, path, "duplicate var_id \"%s\" (var_ids must be unique across all connections)", id);
        else
            cfg_error(cfg, path, "out of memory registering \"%s\"", id);
        return NULL;
    }
    return var;
}

void conn_set_state(ammio_conn_t *conn, ammio_conn_state_t state)
{
    ammio_conn_state_t old = (ammio_conn_state_t)atomic_exchange(&conn->state, (int)state);
    if (old == state)
        return;

    if (state == AMMIO_CONN_CONNECTED)
        log_info("%s: connected", conn->name);
    else if (old == AMMIO_CONN_CONNECTED && state != AMMIO_CONN_STOPPED)
        log_warn("%s: connection lost", conn->name);

    var_table_mark_connection(conn->name, state == AMMIO_CONN_CONNECTED);
    if (conn->status_var)
    {
        ammio_value_t v = {.b = state == AMMIO_CONN_CONNECTED};
        var_table_update(conn->status_var, v, AMMIO_QUALITY_GOOD);
    }
}
