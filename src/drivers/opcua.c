// OPC UA client driver.
//
// Outputs are delivered by a subscription (monitored items sampled every period_ms) or, with
// "read_mode": "poll", read in one batched Read request per period. Inputs are written in one
// batched Write request when a client wrote them (or every period with "write_mode": "cyclic").
// Values are converted from whatever numeric type the server uses, with range checking, and
// the OPC UA status code becomes the variable's quality.

#include "drivers/driver.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>

#include "util/clock.h"
#include "util/log.h"

typedef struct ua_conn ua_conn_t;

typedef struct
{
    ua_conn_t *ua;
    ammio_var_t *var;
    ammio_type_t type;
    UA_NodeId node;
    char *node_str;
    uint32_t sent_seq;   // inputs: last write_seq sent
    bool sent_valid;
    bool failing;        // last operation on this node failed (logged once)
} ua_point_t;

struct ua_conn
{
    ammio_conn_t *conn;
    const char *name;

    char *endpoint;
    char *username;
    char *password;
    uint32_t period_ms;
    uint32_t timeout_ms;
    bool use_subscription;
    bool cyclic_writes;

    ua_point_t *inputs;
    size_t n_inputs;
    ua_point_t *outputs;
    size_t n_outputs;

    ammio_var_t **in_vars;
    ammio_var_snapshot_t *in_snaps;
    size_t *dirty;               // indices of inputs to write this cycle

    UA_Client *client;
    bool connected;
    bool subscribed;
    bool connect_failure_logged;
    thrd_t thread;
    bool thread_started;
    atomic_bool running;
};

// ---------------------------------------------------------------------------------------------
// Value conversion
// ---------------------------------------------------------------------------------------------

static const UA_DataType *ua_type_for(ammio_type_t type)
{
    switch (type)
    {
        case AMMIO_TYPE_BOOL:    return &UA_TYPES[UA_TYPES_BOOLEAN];
        case AMMIO_TYPE_UINT8:   return &UA_TYPES[UA_TYPES_BYTE];
        case AMMIO_TYPE_INT8:    return &UA_TYPES[UA_TYPES_SBYTE];
        case AMMIO_TYPE_UINT16:  return &UA_TYPES[UA_TYPES_UINT16];
        case AMMIO_TYPE_INT16:   return &UA_TYPES[UA_TYPES_INT16];
        case AMMIO_TYPE_UINT32:  return &UA_TYPES[UA_TYPES_UINT32];
        case AMMIO_TYPE_INT32:   return &UA_TYPES[UA_TYPES_INT32];
        case AMMIO_TYPE_UINT64:  return &UA_TYPES[UA_TYPES_UINT64];
        case AMMIO_TYPE_INT64:   return &UA_TYPES[UA_TYPES_INT64];
        case AMMIO_TYPE_FLOAT32: return &UA_TYPES[UA_TYPES_FLOAT];
        default:                 return &UA_TYPES[UA_TYPES_DOUBLE];
    }
}

// Converts any numeric/boolean scalar into the variable's type
static ammio_value_status_t variant_to_value(const UA_Variant *v, ammio_type_t type, ammio_value_t *out)
{
    if (!v->type || !v->data || !UA_Variant_isScalar(v))
        return AMMIO_VALUE_WRONG_TYPE;

    const void *d = v->data;
    const UA_DataType *t = v->type;

    if (t == &UA_TYPES[UA_TYPES_BOOLEAN]) return ammio_value_from_int64(type, *(const UA_Boolean *)d ? 1 : 0, out);
    if (t == &UA_TYPES[UA_TYPES_SBYTE])   return ammio_value_from_int64(type, *(const UA_SByte *)d, out);
    if (t == &UA_TYPES[UA_TYPES_INT16])   return ammio_value_from_int64(type, *(const UA_Int16 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_INT32])   return ammio_value_from_int64(type, *(const UA_Int32 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_INT64])   return ammio_value_from_int64(type, *(const UA_Int64 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_BYTE])    return ammio_value_from_uint64(type, *(const UA_Byte *)d, out);
    if (t == &UA_TYPES[UA_TYPES_UINT16])  return ammio_value_from_uint64(type, *(const UA_UInt16 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_UINT32])  return ammio_value_from_uint64(type, *(const UA_UInt32 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_UINT64])  return ammio_value_from_uint64(type, *(const UA_UInt64 *)d, out);
    if (t == &UA_TYPES[UA_TYPES_FLOAT])   return ammio_value_from_double(type, *(const UA_Float *)d, out);
    if (t == &UA_TYPES[UA_TYPES_DOUBLE])  return ammio_value_from_double(type, *(const UA_Double *)d, out);
    return AMMIO_VALUE_WRONG_TYPE;
}

static ammio_quality_t quality_for_status(UA_StatusCode status)
{
    if (UA_StatusCode_isGood(status))
        return AMMIO_QUALITY_GOOD;
    if (UA_StatusCode_isUncertain(status))
        return AMMIO_QUALITY_STALE;
    return AMMIO_QUALITY_BAD;
}

// Stores a DataValue received from the server for an output point
static void apply_data_value(ua_point_t *p, const UA_DataValue *dv)
{
    UA_StatusCode status = dv->hasStatus ? dv->status : UA_STATUSCODE_GOOD;

    if (!dv->hasValue || UA_StatusCode_isBad(status))
    {
        if (!p->failing)
            log_warn("%s: '%s' (%s): %s", p->ua->name, var_id(p->var), p->node_str, UA_StatusCode_name(status));
        p->failing = true;
        var_table_set_quality_many(&p->var, 1, AMMIO_QUALITY_BAD);
        return;
    }

    ammio_value_t value;
    ammio_value_status_t rc = variant_to_value(&dv->value, p->type, &value);
    if (rc != AMMIO_VALUE_OK)
    {
        if (!p->failing)
            log_warn("%s: '%s' (%s): server sent a %s that does not fit %s", p->ua->name, var_id(p->var),
                     p->node_str, dv->value.type ? dv->value.type->typeName : "empty value",
                     ammio_type_name(p->type));
        p->failing = true;
        var_table_set_quality_many(&p->var, 1, AMMIO_QUALITY_BAD);
        return;
    }

    if (p->failing)
        log_info("%s: '%s' recovered", p->ua->name, var_id(p->var));
    p->failing = false;
    var_table_update(p->var, value, quality_for_status(status));
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

static bool parse_point(ua_conn_t *ua, const cJSON *item, const char *path, ammio_dir_t dir,
                        ua_point_t *p, cfg_t *cfg)
{
    static const char *const input_keys[] = {"var_id", "node_id", "type", "initial", NULL};
    static const char *const output_keys[] = {"var_id", "node_id", "type", NULL};

    if (!cfg_expect_object(cfg, path, item))
        return false;
    cfg_check_keys(cfg, path, item, dir == AMMIO_DIR_INPUT ? input_keys : output_keys);

    const char *id = NULL, *node = NULL;
    bool ok = cfg_get_string(cfg, path, item, "var_id", true, &id);
    ok &= cfg_get_string(cfg, path, item, "node_id", true, &node);
    ok &= cfg_get_type(cfg, path, item, "type", true, &p->type);
    if (!ok)
        return false;

    // Standard OPC UA string encoding: "ns=1;s=Motor.Speed", "ns=2;i=1001", "i=85", "ns=1;g=...", "b=..."
    if (UA_NodeId_parse(&p->node, UA_STRING((char *)node)) != UA_STATUSCODE_GOOD)
    {
        cfg_error(cfg, path, "'node_id' = \"%s\" is not a valid NodeId (e.g. \"ns=1;s=Motor.Speed\" or \"ns=1;i=1001\")", node);
        return false;
    }
    p->node_str = ammio_strdup(node);
    p->ua = ua;

    p->var = conn_add_var(ua->conn, cfg, path, id, p->type, dir);
    if (!p->var)
        return false;

    if (dir == AMMIO_DIR_INPUT)
    {
        ammio_value_t initial;
        if (cfg_get_value(cfg, path, item, "initial", false, p->type, &initial))
            var_table_set_value_silent(p->var, initial);
    }
    return true;
}

static void parse_points(ua_conn_t *ua, const cJSON *arr, const char *path, ammio_dir_t dir,
                         ua_point_t **out, size_t *count, cfg_t *cfg)
{
    size_t n = (size_t)cJSON_GetArraySize(arr);
    *out = calloc(n ? n : 1, sizeof(ua_point_t));
    *count = 0;
    if (!*out)
        return;

    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        char p[CFG_PATH_MAX];
        cfg_path_index(p, path, index++);
        ua_point_t *pt = &(*out)[*count];
        if (parse_point(ua, item, p, dir, pt, cfg))
            (*count)++;
        else
        {
            UA_NodeId_clear(&pt->node);
            free(pt->node_str);
            memset(pt, 0, sizeof(*pt));
        }
    }
}

static void ua_destroy(void *ctx);

static void *ua_create(ammio_conn_t *conn, const cJSON *settings, const char *path, cfg_t *cfg)
{
    static const char *const keys[] = {
        "endpoint", "username", "password", "period_ms", "timeout_ms", "read_mode", "write_mode", "nodes", NULL
    };
    static const char *const read_modes[] = {"subscription", "poll", NULL};
    static const char *const write_modes[] = {"on_change", "cyclic", NULL};

    ua_conn_t *ua = calloc(1, sizeof(*ua));
    if (!ua)
        return NULL;
    ua->conn = conn;
    ua->name = conn_name(conn);
    atomic_init(&ua->running, false);

    cfg_check_keys(cfg, path, settings, keys);

    const char *str;
    if (cfg_get_string(cfg, path, settings, "endpoint", true, &str))
    {
        if (strncmp(str, "opc.tcp://", 10) != 0)
            cfg_error(cfg, path, "'endpoint' must start with opc.tcp://");
        ua->endpoint = ammio_strdup(str);
    }

    bool has_user = cfg_get_string(cfg, path, settings, "username", false, &str);
    if (has_user)
        ua->username = ammio_strdup(str);
    bool has_pass = cfg_get_string(cfg, path, settings, "password", false, &str);
    if (has_pass)
        ua->password = ammio_strdup(str);
    if (has_user != has_pass)
        cfg_error(cfg, path, "'username' and 'password' must be given together");

    ua->period_ms = 100;
    cfg_get_uint32(cfg, path, settings, "period_ms", false, 1, 3600000, &ua->period_ms);
    ua->timeout_ms = 5000;
    cfg_get_uint32(cfg, path, settings, "timeout_ms", false, 1, 600000, &ua->timeout_ms);

    int mode = 0;
    cfg_get_enum(cfg, path, settings, "read_mode", false, read_modes, &mode);
    ua->use_subscription = mode == 0;
    mode = 0;
    cfg_get_enum(cfg, path, settings, "write_mode", false, write_modes, &mode);
    ua->cyclic_writes = mode == 1;

    const cJSON *nodes = NULL;
    char npath[CFG_PATH_MAX];
    cfg_path(npath, path, "nodes");
    if (cfg_get_object(cfg, path, settings, "nodes", true, &nodes))
    {
        static const char *const node_keys[] = {"inputs", "outputs", NULL};
        cfg_check_keys(cfg, npath, nodes, node_keys);

        const cJSON *arr;
        char apath[CFG_PATH_MAX];
        if (cfg_get_array(cfg, npath, nodes, "inputs", false, &arr))
            parse_points(ua, arr, cfg_path(apath, npath, "inputs"), AMMIO_DIR_INPUT, &ua->inputs, &ua->n_inputs, cfg);
        if (cfg_get_array(cfg, npath, nodes, "outputs", false, &arr))
            parse_points(ua, arr, cfg_path(apath, npath, "outputs"), AMMIO_DIR_OUTPUT, &ua->outputs, &ua->n_outputs, cfg);
        if (ua->n_inputs + ua->n_outputs == 0 && cfg->errors == 0)
            cfg_error(cfg, npath, "no inputs or outputs defined");
    }

    ua->in_vars = calloc(ua->n_inputs ? ua->n_inputs : 1, sizeof(*ua->in_vars));
    ua->in_snaps = calloc(ua->n_inputs ? ua->n_inputs : 1, sizeof(*ua->in_snaps));
    ua->dirty = calloc(ua->n_inputs ? ua->n_inputs : 1, sizeof(*ua->dirty));
    if (!ua->in_vars || !ua->in_snaps || !ua->dirty)
    {
        ua_destroy(ua);
        return NULL;
    }
    for (size_t i = 0; i < ua->n_inputs; i++)
        ua->in_vars[i] = ua->inputs[i].var;
    return ua;
}

// ---------------------------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------------------------

static void on_data_change(UA_Client *client, UA_UInt32 sub_id, void *sub_ctx, UA_UInt32 mon_id,
                           void *mon_ctx, UA_DataValue *value)
{
    (void)client; (void)sub_id; (void)sub_ctx; (void)mon_id;
    apply_data_value((ua_point_t *)mon_ctx, value);
}

static bool session_active(ua_conn_t *ua)
{
    UA_SessionState session;
    UA_Client_getState(ua->client, NULL, &session, NULL);
    return session == UA_SESSIONSTATE_ACTIVATED;
}

static void ua_disconnect(ua_conn_t *ua)
{
    UA_Client_disconnect(ua->client);
    ua->subscribed = false;
    if (ua->connected)
    {
        ua->connected = false;
        conn_set_state(ua->conn, AMMIO_CONN_CONNECTING);
    }
}

static bool create_subscription(ua_conn_t *ua)
{
    UA_CreateSubscriptionRequest req = UA_CreateSubscriptionRequest_default();
    req.requestedPublishingInterval = ua->period_ms;
    UA_CreateSubscriptionResponse resp = UA_Client_Subscriptions_create(ua->client, req, ua, NULL, NULL);
    UA_StatusCode sc = resp.responseHeader.serviceResult;
    UA_UInt32 sub_id = resp.subscriptionId;
    UA_CreateSubscriptionResponse_clear(&resp);
    if (sc != UA_STATUSCODE_GOOD)
    {
        log_warn("%s: cannot create subscription (%s), falling back to polling", ua->name, UA_StatusCode_name(sc));
        return false;
    }

    size_t n = ua->n_outputs;
    UA_MonitoredItemCreateRequest *items = calloc(n, sizeof(*items));
    void **contexts = calloc(n, sizeof(*contexts));
    UA_Client_DataChangeNotificationCallback *callbacks = calloc(n, sizeof(*callbacks));
    if (!items || !contexts || !callbacks)
    {
        free(items);
        free(contexts);
        free(callbacks);
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        items[i] = UA_MonitoredItemCreateRequest_default(ua->outputs[i].node);  // shallow copy
        items[i].requestedParameters.samplingInterval = ua->period_ms;
        items[i].requestedParameters.queueSize = 1;
        contexts[i] = &ua->outputs[i];
        callbacks[i] = on_data_change;
    }

    UA_CreateMonitoredItemsRequest mreq;
    UA_CreateMonitoredItemsRequest_init(&mreq);
    mreq.subscriptionId = sub_id;
    mreq.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
    mreq.itemsToCreate = items;
    mreq.itemsToCreateSize = n;

    UA_CreateMonitoredItemsResponse mresp =
        UA_Client_MonitoredItems_createDataChanges(ua->client, mreq, contexts, callbacks, NULL);

    bool ok = mresp.responseHeader.serviceResult == UA_STATUSCODE_GOOD && mresp.resultsSize == n;
    if (!ok)
    {
        log_warn("%s: cannot create monitored items (%s), falling back to polling", ua->name,
                 UA_StatusCode_name(mresp.responseHeader.serviceResult));
        UA_Client_Subscriptions_deleteSingle(ua->client, sub_id);
    }
    else
    {
        for (size_t i = 0; i < n; i++)
        {
            if (mresp.results[i].statusCode != UA_STATUSCODE_GOOD)
            {
                log_warn("%s: cannot monitor '%s' (%s): %s", ua->name, var_id(ua->outputs[i].var),
                         ua->outputs[i].node_str, UA_StatusCode_name(mresp.results[i].statusCode));
                ua->outputs[i].failing = true;
                var_table_set_quality_many(&ua->outputs[i].var, 1, AMMIO_QUALITY_BAD);
            }
        }
    }

    UA_CreateMonitoredItemsResponse_clear(&mresp);
    free(items);  // node ids are owned by the points, not cleared here
    free(contexts);
    free(callbacks);
    return ok;
}

static bool ua_connect(ua_conn_t *ua)
{
    UA_StatusCode sc = ua->username
        ? UA_Client_connectUsername(ua->client, ua->endpoint, ua->username, ua->password)
        : UA_Client_connect(ua->client, ua->endpoint);

    if (sc != UA_STATUSCODE_GOOD)
    {
        if (!ua->connect_failure_logged)
        {
            log_warn("%s: cannot connect to %s: %s (retrying every %u ms)", ua->name, ua->endpoint,
                     UA_StatusCode_name(sc), ua->period_ms);
            ua->connect_failure_logged = true;
        }
        UA_Client_disconnect(ua->client);
        return false;
    }

    ua->connected = true;
    ua->connect_failure_logged = false;
    for (size_t i = 0; i < ua->n_inputs; i++)
        ua->inputs[i].sent_valid = false;
    conn_set_state(ua->conn, AMMIO_CONN_CONNECTED);

    ua->subscribed = ua->use_subscription && ua->n_outputs > 0 && create_subscription(ua);
    return true;
}

// Returns false if the request itself failed (connection problem)
static bool write_inputs(ua_conn_t *ua)
{
    if (ua->n_inputs == 0)
        return true;

    var_table_read_many(ua->in_vars, ua->n_inputs, ua->in_snaps);

    size_t n = 0;
    for (size_t i = 0; i < ua->n_inputs; i++)
    {
        const ua_point_t *p = &ua->inputs[i];
        const ammio_var_snapshot_t *s = &ua->in_snaps[i];
        if (s->has_value && (ua->cyclic_writes || !p->sent_valid || p->sent_seq != s->write_seq))
            ua->dirty[n++] = i;
    }
    if (n == 0)
        return true;

    UA_WriteValue *wv = calloc(n, sizeof(*wv));
    if (!wv)
        return true;
    for (size_t k = 0; k < n; k++)
    {
        size_t i = ua->dirty[k];
        UA_WriteValue_init(&wv[k]);
        wv[k].nodeId = ua->inputs[i].node;  // shallow
        wv[k].attributeId = UA_ATTRIBUTEID_VALUE;
        wv[k].value.hasValue = true;
        // All members of ammio_value_t start at offset 0 and match the UA scalar layouts
        UA_Variant_setScalar(&wv[k].value.value, &ua->in_snaps[i].value, ua_type_for(ua->inputs[i].type));
    }

    UA_WriteRequest req;
    UA_WriteRequest_init(&req);
    req.nodesToWrite = wv;
    req.nodesToWriteSize = n;
    UA_WriteResponse resp = UA_Client_Service_write(ua->client, req);
    free(wv);  // shallow: nothing inside is owned

    UA_StatusCode sc = resp.responseHeader.serviceResult;
    if (sc != UA_STATUSCODE_GOOD || resp.resultsSize != n)
    {
        log_warn("%s: write failed: %s", ua->name, UA_StatusCode_name(sc));
        UA_WriteResponse_clear(&resp);
        return false;
    }

    for (size_t k = 0; k < n; k++)
    {
        size_t i = ua->dirty[k];
        ua_point_t *p = &ua->inputs[i];
        UA_StatusCode r = resp.results[k];

        // A rejected write is not retried until the client writes again
        p->sent_seq = ua->in_snaps[i].write_seq;
        p->sent_valid = true;

        if (r != UA_STATUSCODE_GOOD)
        {
            if (!p->failing)
                log_warn("%s: server rejected write of '%s' (%s, %s): %s", ua->name, var_id(p->var),
                         p->node_str, ammio_type_name(p->type), UA_StatusCode_name(r));
            p->failing = true;
            var_table_set_quality_many(&p->var, 1, AMMIO_QUALITY_BAD);
        }
        else if (p->failing)
        {
            p->failing = false;
            var_table_set_quality_many(&p->var, 1, AMMIO_QUALITY_GOOD);
        }
    }
    UA_WriteResponse_clear(&resp);
    return true;
}

static bool poll_outputs(ua_conn_t *ua)
{
    size_t n = ua->n_outputs;
    if (n == 0)
        return true;

    UA_ReadValueId *ids = calloc(n, sizeof(*ids));
    if (!ids)
        return true;
    for (size_t i = 0; i < n; i++)
    {
        UA_ReadValueId_init(&ids[i]);
        ids[i].nodeId = ua->outputs[i].node;  // shallow
        ids[i].attributeId = UA_ATTRIBUTEID_VALUE;
    }

    UA_ReadRequest req;
    UA_ReadRequest_init(&req);
    req.nodesToRead = ids;
    req.nodesToReadSize = n;
    req.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
    UA_ReadResponse resp = UA_Client_Service_read(ua->client, req);
    free(ids);

    UA_StatusCode sc = resp.responseHeader.serviceResult;
    if (sc != UA_STATUSCODE_GOOD || resp.resultsSize != n)
    {
        log_warn("%s: read failed: %s", ua->name, UA_StatusCode_name(sc));
        UA_ReadResponse_clear(&resp);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        apply_data_value(&ua->outputs[i], &resp.results[i]);
    UA_ReadResponse_clear(&resp);
    return true;
}

static int ua_thread(void *arg)
{
    ua_conn_t *ua = arg;
    cycle_timer_t cycle;
    cycle_init(&cycle, ua->period_ms);

    while (atomic_load(&ua->running))
    {
        if (!ua->connected && !ua_connect(ua))
        {
            cycle_wait(&cycle, &ua->running);
            continue;
        }

        bool ok = write_inputs(ua);
        if (ok && !ua->subscribed)
            ok = poll_outputs(ua);

        // Process subscription notifications and keep-alives until the next cycle is due
        while (ok && atomic_load(&ua->running))
        {
            uint64_t remaining_ms = cycle_remaining_ns(&cycle) / 1000000ULL;
            UA_StatusCode sc = UA_Client_run_iterate(ua->client, (UA_UInt32)(remaining_ms < 50 ? remaining_ms : 50));
            if (sc != UA_STATUSCODE_GOOD || !session_active(ua))
            {
                ok = false;
                break;
            }
            if (remaining_ms == 0)
                break;
        }

        if (!ok || !session_active(ua))
            ua_disconnect(ua);
        cycle_wait(&cycle, &ua->running);
    }
    return 0;
}

static int ua_start(void *ctx)
{
    ua_conn_t *ua = ctx;

    ua->client = UA_Client_new();
    if (!ua->client)
    {
        log_error("%s: cannot create OPC UA client", ua->name);
        return -1;
    }
    UA_ClientConfig *config = UA_Client_getConfig(ua->client);
    config->timeout = ua->timeout_ms;

    atomic_store(&ua->running, true);
    if (thrd_create(&ua->thread, ua_thread, ua) != thrd_success)
    {
        log_error("%s: cannot create thread", ua->name);
        atomic_store(&ua->running, false);
        return -1;
    }
    ua->thread_started = true;
    return 0;
}

static void ua_stop(void *ctx)
{
    ua_conn_t *ua = ctx;

    atomic_store(&ua->running, false);
    if (ua->thread_started)
    {
        thrd_join(ua->thread, NULL);
        ua->thread_started = false;
    }
    if (ua->client)
    {
        UA_Client_disconnect(ua->client);
        UA_Client_delete(ua->client);
        ua->client = NULL;
    }
    ua->connected = false;
    ua->subscribed = false;
}

static void free_points(ua_point_t *points, size_t n)
{
    if (!points)
        return;
    for (size_t i = 0; i < n; i++)
    {
        UA_NodeId_clear(&points[i].node);
        free(points[i].node_str);
    }
    free(points);
}

static void ua_destroy(void *ctx)
{
    ua_conn_t *ua = ctx;
    if (!ua)
        return;
    free(ua->endpoint);
    free(ua->username);
    free(ua->password);
    free_points(ua->inputs, ua->n_inputs);
    free_points(ua->outputs, ua->n_outputs);
    free(ua->in_vars);
    free(ua->in_snaps);
    free(ua->dirty);
    free(ua);
}

const ammio_driver_t opcua_driver = {
    .protocol = "opcua",
    .create   = ua_create,
    .start    = ua_start,
    .stop     = ua_stop,
    .destroy  = ua_destroy,
};
