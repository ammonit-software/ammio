// OPC UA client. Each period, all inputs are written in one Write request
// and all outputs are read in one Read request.

#include "../json.h"
#include "../log.h"
#include "../protocol.h"

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    var_t *var;
    UA_NodeId node;
    const UA_DataType *ua_type;
    bool failing;
} node_t;

typedef struct
{
    const char *conn;
    const char *endpoint, *username, *password; // owned by the config JSON
    UA_Client *client;

    node_t *nodes; // inputs first, then outputs
    size_t input_count, count;

    // Prebuilt requests (node ids are shallow copies of nodes[i].node)
    UA_WriteValue *writes;
    value_t *write_values;
    UA_ReadValueId *reads;
} opcua_conn_t;

static const UA_DataType *ua_type_of(type_t type)
{
    switch (type)
    {
        case TYPE_BOOL:    return &UA_TYPES[UA_TYPES_BOOLEAN];
        case TYPE_UINT8:   return &UA_TYPES[UA_TYPES_BYTE];
        case TYPE_INT8:    return &UA_TYPES[UA_TYPES_SBYTE];
        case TYPE_UINT16:  return &UA_TYPES[UA_TYPES_UINT16];
        case TYPE_INT16:   return &UA_TYPES[UA_TYPES_INT16];
        case TYPE_UINT32:  return &UA_TYPES[UA_TYPES_UINT32];
        case TYPE_INT32:   return &UA_TYPES[UA_TYPES_INT32];
        case TYPE_FLOAT32: return &UA_TYPES[UA_TYPES_FLOAT];
        case TYPE_FLOAT64: return &UA_TYPES[UA_TYPES_DOUBLE];
        default:           return NULL;
    }
}

// --- config ------------------------------------------------------------------

static bool parse_nodes(opcua_conn_t *o, const cJSON *arr, const char *key, dir_t dir)
{
    static const char *const keys[] = {"name", "node_id", "type", NULL};
    bool ok = true;
    int index = 0;
    const cJSON *item;

    cJSON_ArrayForEach(item, arr)
    {
        char ctx[256];
        snprintf(ctx, sizeof(ctx), "%s.%s[%d]", o->conn, key, index++);

        const char *name = NULL, *node_id = NULL;
        type_t type = TYPE_FLOAT64;
        if (!cJSON_IsObject(item))
        {
            log_error("%s: must be an object", ctx);
            ok = false;
            continue;
        }
        bool item_ok = json_check_keys(item, ctx, keys);
        item_ok &= json_need_string(item, "name", ctx, &name);
        item_ok &= json_need_string(item, "node_id", ctx, &node_id);
        item_ok &= json_need_type(item, ctx, &type);
        if (!item_ok)
        {
            ok = false;
            continue;
        }

        UA_NodeId node;
        if (UA_NodeId_parse(&node, UA_STRING((char *)node_id)) != UA_STATUSCODE_GOOD)
        {
            log_error("%s.node_id: invalid node id '%s' (e.g. \"ns=1;s=Motor.Speed\" or \"ns=1;i=1001\")",
                      ctx, node_id);
            ok = false;
            continue;
        }

        var_t *var = conn_add_var(ctx, name, type, dir);
        if (!var)
        {
            UA_NodeId_clear(&node);
            ok = false;
            continue;
        }
        o->nodes[o->count++] = (node_t){.var = var, .node = node, .ua_type = ua_type_of(type)};
    }
    return ok;
}

static void opcua_destroy(void *self)
{
    opcua_conn_t *o = self;
    if (o->client)
        UA_Client_delete(o->client);
    for (size_t i = 0; i < o->count; i++)
        UA_NodeId_clear(&o->nodes[i].node);
    free(o->nodes);
    free(o->writes);
    free(o->write_values);
    free(o->reads);
    free(o);
}

static void *opcua_create(const char *conn, const cJSON *spec, uint32_t *period_ms)
{
    static const char *const keys[] = {
        "protocol", "endpoint", "username", "password", "period_ms", "inputs", "outputs", NULL};

    opcua_conn_t *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->conn = conn;

    const cJSON *inputs = NULL, *outputs = NULL;
    *period_ms = 100;

    bool ok = json_check_keys(spec, conn, keys);
    ok &= json_need_string(spec, "endpoint", conn, &o->endpoint);
    ok &= json_get_string(spec, "username", conn, &o->username);
    ok &= json_get_string(spec, "password", conn, &o->password);
    ok &= json_get_uint(spec, "period_ms", conn, 1, 3600000, period_ms);
    ok &= json_get_array(spec, "inputs", conn, &inputs);
    ok &= json_get_array(spec, "outputs", conn, &outputs);

    if (!o->username != !o->password)
    {
        log_error("%s: set both 'username' and 'password', or neither", conn);
        ok = false;
    }

    // Keep validating after an error so that one run reports every problem.
    size_t n_in = (size_t)cJSON_GetArraySize(inputs);
    size_t n_out = (size_t)cJSON_GetArraySize(outputs);
    if (n_in + n_out == 0)
    {
        log_error("%s: no inputs or outputs defined", conn);
        ok = false;
    }
    else
    {
        o->nodes = calloc(n_in + n_out, sizeof(*o->nodes));
        o->writes = calloc(n_in ? n_in : 1, sizeof(*o->writes));
        o->write_values = calloc(n_in ? n_in : 1, sizeof(*o->write_values));
        o->reads = calloc(n_out ? n_out : 1, sizeof(*o->reads));
        o->client = UA_Client_new();
        if (o->nodes && o->writes && o->write_values && o->reads && o->client)
        {
            ok &= parse_nodes(o, inputs, "inputs", DIR_INPUT);
            o->input_count = o->count;
            ok &= parse_nodes(o, outputs, "outputs", DIR_OUTPUT);
        }
        else
        {
            ok = false;
        }
    }

    if (!ok)
    {
        opcua_destroy(o);
        return NULL;
    }

    for (size_t i = 0; i < o->input_count; i++)
    {
        UA_WriteValue_init(&o->writes[i]);
        o->writes[i].nodeId = o->nodes[i].node;
        o->writes[i].attributeId = UA_ATTRIBUTEID_VALUE;
        o->writes[i].value.hasValue = true;
    }
    for (size_t i = o->input_count; i < o->count; i++)
    {
        UA_ReadValueId *r = &o->reads[i - o->input_count];
        UA_ReadValueId_init(r);
        r->nodeId = o->nodes[i].node;
        r->attributeId = UA_ATTRIBUTEID_VALUE;
    }
    return o;
}

// --- runtime -----------------------------------------------------------------

static int opcua_connect(void *self, char *err, size_t err_len)
{
    opcua_conn_t *o = self;
    UA_StatusCode sc = o->username
        ? UA_Client_connectUsername(o->client, o->endpoint, o->username, o->password)
        : UA_Client_connect(o->client, o->endpoint);
    if (sc != UA_STATUSCODE_GOOD)
    {
        snprintf(err, err_len, "%s: %s", o->endpoint, UA_StatusCode_name(sc));
        UA_Client_disconnect(o->client);
        return -1;
    }
    return 0;
}

static bool session_alive(opcua_conn_t *o)
{
    UA_SessionState state;
    UA_Client_getState(o->client, NULL, &state, NULL);
    return state == UA_SESSIONSTATE_ACTIVATED;
}

static void report_status(opcua_conn_t *o, node_t *n, const char *op, UA_StatusCode sc)
{
    char problem[160];
    snprintf(problem, sizeof(problem), "%s failed: %s", op, UA_StatusCode_name(sc));
    conn_report(o->conn, var_name(n->var), &n->failing, sc == UA_STATUSCODE_GOOD ? NULL : problem);
}

static int write_inputs(opcua_conn_t *o, char *err, size_t err_len)
{
    for (size_t i = 0; i < o->input_count; i++)
    {
        o->write_values[i] = var_get(o->nodes[i].var, NULL);
        // value_t members share offset 0 and match the OPC UA scalar layouts
        UA_Variant_setScalar(&o->writes[i].value.value, &o->write_values[i], o->nodes[i].ua_type);
    }

    UA_WriteRequest req;
    UA_WriteRequest_init(&req);
    req.nodesToWrite = o->writes;
    req.nodesToWriteSize = o->input_count;
    UA_WriteResponse resp = UA_Client_Service_write(o->client, req);

    int rc = 0;
    UA_StatusCode sc = resp.responseHeader.serviceResult;
    if (sc != UA_STATUSCODE_GOOD || resp.resultsSize != o->input_count)
    {
        snprintf(err, err_len, "write: %s", UA_StatusCode_name(sc));
        rc = -1;
    }
    else
    {
        for (size_t i = 0; i < o->input_count; i++)
            report_status(o, &o->nodes[i], "write", resp.results[i]);
    }
    UA_WriteResponse_clear(&resp);
    return rc;
}

static int read_outputs(opcua_conn_t *o, char *err, size_t err_len)
{
    size_t n_out = o->count - o->input_count;

    UA_ReadRequest req;
    UA_ReadRequest_init(&req);
    req.nodesToRead = o->reads;
    req.nodesToReadSize = n_out;
    req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
    UA_ReadResponse resp = UA_Client_Service_read(o->client, req);

    int rc = 0;
    UA_StatusCode sc = resp.responseHeader.serviceResult;
    if (sc != UA_STATUSCODE_GOOD || resp.resultsSize != n_out)
    {
        snprintf(err, err_len, "read: %s", UA_StatusCode_name(sc));
        rc = -1;
    }
    else
    {
        for (size_t i = 0; i < n_out; i++)
        {
            node_t *n = &o->nodes[o->input_count + i];
            const UA_DataValue *dv = &resp.results[i];

            if (dv->hasStatus && dv->status != UA_STATUSCODE_GOOD)
            {
                report_status(o, n, "read", dv->status);
                continue;
            }
            if (!dv->hasValue || !UA_Variant_hasScalarType(&dv->value, n->ua_type))
            {
                // Never reinterpret a value of another type: report the mismatch instead.
                char problem[160];
                snprintf(problem, sizeof(problem), "server value is %s, configured type is %s",
                         dv->value.type ? dv->value.type->typeName : "empty",
                         type_name(var_type(n->var)));
                conn_report(o->conn, var_name(n->var), &n->failing, problem);
                continue;
            }

            value_t v = {0};
            memcpy(&v, dv->value.data, n->ua_type->memSize);
            var_set(n->var, v);
            conn_report(o->conn, var_name(n->var), &n->failing, NULL);
        }
    }
    UA_ReadResponse_clear(&resp);
    return rc;
}

static int opcua_cycle(void *self, char *err, size_t err_len)
{
    opcua_conn_t *o = self;

    if (o->input_count && write_inputs(o, err, err_len) != 0)
        return -1;
    if (o->count > o->input_count && read_outputs(o, err, err_len) != 0)
        return -1;

    UA_Client_run_iterate(o->client, 0); // keep-alive and channel renewal
    if (!session_alive(o))
    {
        snprintf(err, err_len, "session closed");
        return -1;
    }
    return 0;
}

static void opcua_disconnect(void *self)
{
    UA_Client_disconnect(((opcua_conn_t *)self)->client);
}

const protocol_t opcua_protocol = {
    .name = "opcua",
    .create = opcua_create,
    .connect = opcua_connect,
    .cycle = opcua_cycle,
    .disconnect = opcua_disconnect,
    .destroy = opcua_destroy,
};
