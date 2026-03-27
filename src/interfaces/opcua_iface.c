#include "opcua_iface.h"
#include "interface.h"
#include "../var_table.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>

#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>

#define OPCUA_MAX_NODES     128
#define OPCUA_MAX_NAME_LEN  256
#define OPCUA_MAX_EP_LEN    512

typedef struct {
    char      var_id[OPCUA_MAX_NAME_LEN];
    type_t    type;
    UA_NodeId node_id;
} opcua_node_t;

// Module state
static cJSON        *opcua_config = NULL;
static UA_Client    *client       = NULL;
static char          endpoint[OPCUA_MAX_EP_LEN] = {0};

static opcua_node_t  input_nodes[OPCUA_MAX_NODES];
static int           input_count = 0;

static opcua_node_t  output_nodes[OPCUA_MAX_NODES];
static int           output_count = 0;

static thrd_t        thread_process;
static atomic_bool   running   = false;
static bool          connected = false;

// Parse "ns=X;s=name" or "ns=X;i=1234" into a UA_NodeId.
// String variants are allocated (caller must UA_NodeId_clear).
static UA_NodeId parse_node_id(const char *str)
{
    int ns = 0;
    sscanf(str, "ns=%d", &ns);

    const char *s = strstr(str, ";s=");
    const char *i = strstr(str, ";i=");

    if (s)
        return UA_NODEID_STRING_ALLOC(ns, s + 3);
    if (i)
        return UA_NODEID_NUMERIC(ns, (UA_UInt32)atoi(i + 3));

    log_debug("opcua: unrecognised node id format: %s", str);
    return UA_NODEID_NUMERIC(0, 0);
}

// Write a var_table input value to the OPC UA server
static void write_to_server(opcua_node_t *node)
{
    var_t var;
    if (var_table_get(node->var_id, &var) != 0)
        return;

    UA_Variant value;
    UA_Variant_init(&value);

    // Keep the scalar data alive until after the synchronous write call
    union {
        UA_Byte u8; UA_SByte i8;
        UA_UInt16 u16; UA_Int16 i16;
        UA_UInt32 u32; UA_Int32 i32;
        UA_Float f32; UA_Double f64;
    } v;

    switch (node->type)
    {
        case TYPE_UINT8:   v.u8  = var.value.u8;  UA_Variant_setScalar(&value, &v.u8,  &UA_TYPES[UA_TYPES_BYTE]);    break;
        case TYPE_INT8:    v.i8  = var.value.i8;  UA_Variant_setScalar(&value, &v.i8,  &UA_TYPES[UA_TYPES_SBYTE]);   break;
        case TYPE_UINT16:  v.u16 = var.value.u16; UA_Variant_setScalar(&value, &v.u16, &UA_TYPES[UA_TYPES_UINT16]);  break;
        case TYPE_INT16:   v.i16 = var.value.i16; UA_Variant_setScalar(&value, &v.i16, &UA_TYPES[UA_TYPES_INT16]);   break;
        case TYPE_UINT32:  v.u32 = var.value.u32; UA_Variant_setScalar(&value, &v.u32, &UA_TYPES[UA_TYPES_UINT32]);  break;
        case TYPE_INT32:   v.i32 = var.value.i32; UA_Variant_setScalar(&value, &v.i32, &UA_TYPES[UA_TYPES_INT32]);   break;
        case TYPE_FLOAT32: v.f32 = var.value.f32; UA_Variant_setScalar(&value, &v.f32, &UA_TYPES[UA_TYPES_FLOAT]);   break;
        case TYPE_FLOAT64: v.f64 = var.value.f64; UA_Variant_setScalar(&value, &v.f64, &UA_TYPES[UA_TYPES_DOUBLE]);  break;
        default: return;
    }

    UA_StatusCode sc = UA_Client_writeValueAttribute(client, node->node_id, &value);
    if (!UA_StatusCode_isGood(sc))
        log_debug("opcua: write failed for %s: %s", node->var_id, UA_StatusCode_name(sc));
}

// Read a value from the OPC UA server and push it into var_table
static void read_from_server(opcua_node_t *node)
{
    UA_Variant value;
    UA_Variant_init(&value);

    UA_StatusCode sc = UA_Client_readValueAttribute(client, node->node_id, &value);
    if (!UA_StatusCode_isGood(sc))
    {
        log_debug("opcua: read failed for %s: %s", node->var_id, UA_StatusCode_name(sc));
        UA_Variant_clear(&value);
        return;
    }

    if (!UA_Variant_isScalar(&value))
    {
        UA_Variant_clear(&value);
        return;
    }

    var_t var = {0};
    var.type = node->type;

    switch (node->type)
    {
        case TYPE_UINT8:   var.value.u8  = *(UA_Byte   *)value.data; break;
        case TYPE_INT8:    var.value.i8  = *(UA_SByte  *)value.data; break;
        case TYPE_UINT16:  var.value.u16 = *(UA_UInt16 *)value.data; break;
        case TYPE_INT16:   var.value.i16 = *(UA_Int16  *)value.data; break;
        case TYPE_UINT32:  var.value.u32 = *(UA_UInt32 *)value.data; break;
        case TYPE_INT32:   var.value.i32 = *(UA_Int32  *)value.data; break;
        case TYPE_FLOAT32: var.value.f32 = *(UA_Float  *)value.data; break;
        case TYPE_FLOAT64: var.value.f64 = *(UA_Double *)value.data; break;
        default:
            UA_Variant_clear(&value);
            return;
    }

    var_table_set(node->var_id, &var);
    UA_Variant_clear(&value);
}

static int thread_process_func(void *arg)
{
    (void)arg;

    uint32_t period_ms = 100;
    cJSON *p = cJSON_GetObjectItem(opcua_config, "period_ms");
    if (p && cJSON_IsNumber(p))
        period_ms = (uint32_t)p->valuedouble;

    struct timespec ts = {
        .tv_sec  = (time_t)(period_ms / 1000),
        .tv_nsec = (long)((period_ms % 1000) * 1000000L)
    };

    while (running)
    {
        if (!connected)
        {
            UA_StatusCode sc;
            cJSON *user = cJSON_GetObjectItem(opcua_config, "username");
            cJSON *pass = cJSON_GetObjectItem(opcua_config, "password");

            if (user && cJSON_IsString(user) && pass && cJSON_IsString(pass))
                sc = UA_Client_connectUsername(client, endpoint, user->valuestring, pass->valuestring);
            else
                sc = UA_Client_connect(client, endpoint);

            if (!UA_StatusCode_isGood(sc))
            {
                log_debug("opcua: connect retry failed: %s", UA_StatusCode_name(sc));
                thrd_sleep(&ts, NULL);
                continue;
            }
            connected = true;
            log_info("opcua: connected");
        }

        for (int i = 0; i < input_count; i++)
            write_to_server(&input_nodes[i]);

        for (int i = 0; i < output_count; i++)
            read_from_server(&output_nodes[i]);

        // Housekeeping: keepalive, async callbacks
        UA_Client_run_iterate(client, 0);

        // Detect dropped connection
        UA_SecureChannelState ch_state;
        UA_SessionState       sess_state;
        UA_StatusCode         conn_status;
        UA_Client_getState(client, &ch_state, &sess_state, &conn_status);
        if (sess_state != UA_SESSIONSTATE_ACTIVATED)
        {
            log_info("opcua: session lost, will reconnect");
            connected = false;
        }

        thrd_sleep(&ts, NULL);
    }

    return 0;
}

static int opcua_iface_init(cJSON *config)
{
    opcua_config = config;

    cJSON *nodes = cJSON_GetObjectItem(config, "nodes");
    if (!nodes)
    {
        log_debug("opcua: no nodes defined");
        return 0;
    }

    cJSON *inputs = cJSON_GetObjectItem(nodes, "inputs");
    if (inputs && cJSON_IsArray(inputs))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, inputs)
        {
            if (input_count >= OPCUA_MAX_NODES)
                break;

            cJSON *var_id  = cJSON_GetObjectItem(item, "var_id");
            cJSON *node_id = cJSON_GetObjectItem(item, "node_id");
            cJSON *type    = cJSON_GetObjectItem(item, "type");

            if (!var_id || !cJSON_IsString(var_id) || !node_id || !cJSON_IsString(node_id))
                continue;

            opcua_node_t *node = &input_nodes[input_count];
            strncpy(node->var_id, var_id->valuestring, OPCUA_MAX_NAME_LEN - 1);
            node->type    = type && cJSON_IsString(type) ?
                            var_table_type_from_string(type->valuestring) : TYPE_UINT16;
            node->node_id = parse_node_id(node_id->valuestring);

            var_table_add(node->var_id, node->type, DIR_INPUT);
            log_debug("opcua: input  %s -> %s", node->var_id, node_id->valuestring);
            input_count++;
        }
    }

    cJSON *outputs = cJSON_GetObjectItem(nodes, "outputs");
    if (outputs && cJSON_IsArray(outputs))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, outputs)
        {
            if (output_count >= OPCUA_MAX_NODES)
                break;

            cJSON *var_id  = cJSON_GetObjectItem(item, "var_id");
            cJSON *node_id = cJSON_GetObjectItem(item, "node_id");
            cJSON *type    = cJSON_GetObjectItem(item, "type");

            if (!var_id || !cJSON_IsString(var_id) || !node_id || !cJSON_IsString(node_id))
                continue;

            opcua_node_t *node = &output_nodes[output_count];
            strncpy(node->var_id, var_id->valuestring, OPCUA_MAX_NAME_LEN - 1);
            node->type    = type && cJSON_IsString(type) ?
                            var_table_type_from_string(type->valuestring) : TYPE_UINT16;
            node->node_id = parse_node_id(node_id->valuestring);

            var_table_add(node->var_id, node->type, DIR_OUTPUT);
            log_debug("opcua: output %s <- %s", node->var_id, node_id->valuestring);
            output_count++;
        }
    }

    return 0;
}

static int opcua_iface_start(void)
{
    cJSON *ep = cJSON_GetObjectItem(opcua_config, "endpoint");
    if (!ep || !cJSON_IsString(ep))
    {
        log_debug("opcua: missing 'endpoint' in config");
        return -1;
    }
    strncpy(endpoint, ep->valuestring, OPCUA_MAX_EP_LEN - 1);

    client = UA_Client_new();
    if (!client)
    {
        log_debug("opcua: failed to create client");
        return -1;
    }
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));

    cJSON *user = cJSON_GetObjectItem(opcua_config, "username");
    cJSON *pass = cJSON_GetObjectItem(opcua_config, "password");
    UA_StatusCode sc;

    if (user && cJSON_IsString(user) && pass && cJSON_IsString(pass))
        sc = UA_Client_connectUsername(client, endpoint, user->valuestring, pass->valuestring);
    else
        sc = UA_Client_connect(client, endpoint);

    if (!UA_StatusCode_isGood(sc))
        log_info("opcua: initial connect failed (%s), will retry", UA_StatusCode_name(sc));
    else
    {
        connected = true;
        log_info("opcua: connected to %s", endpoint);
    }

    running = true;
    if (thrd_create(&thread_process, thread_process_func, NULL) != thrd_success)
    {
        log_info("opcua: failed to create process thread");
        running = false;
        UA_Client_disconnect(client);
        UA_Client_delete(client);
        client = NULL;
        return -1;
    }
    log_info("opcua: process thread started");

    return 0;
}

static void opcua_iface_stop(void)
{
    if (!running)
        return;

    running   = false;
    connected = false;
    thrd_join(thread_process, NULL);

    if (client)
    {
        UA_Client_disconnect(client);
        UA_Client_delete(client);
        client = NULL;
    }

    // Free allocated memory for string node IDs
    for (int i = 0; i < input_count; i++)
        UA_NodeId_clear(&input_nodes[i].node_id);
    for (int i = 0; i < output_count; i++)
        UA_NodeId_clear(&output_nodes[i].node_id);

    input_count  = 0;
    output_count = 0;

    log_debug("opcua: stopped");
}

static interface_t opcua_interface = {
    .name  = "opcua",
    .init  = opcua_iface_init,
    .start = opcua_iface_start,
    .stop  = opcua_iface_stop
};

void opcua_iface_register(void)
{
    interface_register(&opcua_interface);
}
