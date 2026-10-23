#include "api/api_server.h"

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/connections.h"
#include "core/var_table.h"
#include "util/log.h"

#define RECV_POLL_MS 100

static nng_socket sock = NNG_SOCKET_INITIALIZER;
static bool sock_open = false;
static const char *events_endpoint = NULL;

// Error codes are strings so responses are self-describing
#define ERR_INVALID_JSON    "INVALID_JSON"
#define ERR_INVALID_REQUEST "INVALID_REQUEST"
#define ERR_UNKNOWN_CMD     "UNKNOWN_CMD"
#define ERR_NOT_FOUND       "NOT_FOUND"
#define ERR_READ_ONLY       "READ_ONLY"
#define ERR_INVALID_VALUE   "INVALID_VALUE"
#define ERR_OUT_OF_RANGE    "OUT_OF_RANGE"
#define ERR_INTERNAL        "INTERNAL"

static cJSON *make_ok(void)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "ok");
    return resp;
}

static cJSON *make_error(const char *code, const char *var_id, const char *fmt, ...) AMMIO_PRINTF(3, 4);

static cJSON *make_error(const char *code, const char *var_id, const char *fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddFalseToObject(resp, "ok");
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    if (var_id)
        cJSON_AddStringToObject(err, "var_id", var_id);
    return resp;
}

static void add_timestamp(cJSON *obj, uint64_t ns)
{
    // Raw so the 64-bit nanosecond count is not rounded through a double
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)ns);
    cJSON_AddRawToObject(obj, "timestamp_ns", buf);
}

static void add_var_fields(cJSON *obj, const ammio_var_snapshot_t *snap)
{
    cJSON_AddStringToObject(obj, "var_id", snap->var_id);
    if (snap->has_value)
        cJSON_AddItemToObject(obj, "value", ammio_value_to_json(snap->type, snap->value));
    else
        cJSON_AddNullToObject(obj, "value");
    cJSON_AddStringToObject(obj, "type", ammio_type_name(snap->type));
    cJSON_AddStringToObject(obj, "dir", ammio_dir_name(snap->dir));
    cJSON_AddStringToObject(obj, "quality", ammio_quality_name(snap->quality));
    add_timestamp(obj, snap->timestamp_ns);
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// ---- read -----------------------------------------------------------------------------------

static cJSON *handle_read(const cJSON *req)
{
    const cJSON *var_id = cJSON_GetObjectItemCaseSensitive(req, "var_id");
    const cJSON *var_ids = cJSON_GetObjectItemCaseSensitive(req, "var_ids");

    if (var_id && var_ids)
        return make_error(ERR_INVALID_REQUEST, NULL, "use either 'var_id' or 'var_ids', not both");

    if (var_id)
    {
        if (!cJSON_IsString(var_id))
            return make_error(ERR_INVALID_REQUEST, NULL, "'var_id' must be a string");
        ammio_var_t *var = var_table_find(var_id->valuestring);
        if (!var)
            return make_error(ERR_NOT_FOUND, var_id->valuestring, "variable '%s' not found", var_id->valuestring);

        ammio_var_snapshot_t snap;
        var_table_read(var, &snap);
        cJSON *resp = make_ok();
        add_var_fields(resp, &snap);
        return resp;
    }

    if (!cJSON_IsArray(var_ids))
        return make_error(ERR_INVALID_REQUEST, NULL, "'read' needs 'var_id' (string) or 'var_ids' (array)");

    size_t n = (size_t)cJSON_GetArraySize(var_ids);
    ammio_var_t **vars = calloc(n ? n : 1, sizeof(*vars));
    ammio_var_snapshot_t *snaps = calloc(n ? n : 1, sizeof(*snaps));
    if (!vars || !snaps)
    {
        free(vars);
        free(snaps);
        return make_error(ERR_INTERNAL, NULL, "out of memory");
    }

    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, var_ids)
    {
        if (!cJSON_IsString(item))
        {
            free(vars);
            free(snaps);
            return make_error(ERR_INVALID_REQUEST, NULL, "'var_ids' must contain only strings");
        }
        vars[i] = var_table_find(item->valuestring);
        if (!vars[i])
        {
            cJSON *err = make_error(ERR_NOT_FOUND, item->valuestring, "variable '%s' not found", item->valuestring);
            free(vars);
            free(snaps);
            return err;
        }
        i++;
    }

    // One lock for all of them: the values form a consistent snapshot
    var_table_read_many(vars, n, snaps);

    cJSON *resp = make_ok();
    cJSON *arr = cJSON_AddArrayToObject(resp, "vars");
    for (i = 0; i < n; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        add_var_fields(obj, &snaps[i]);
        cJSON_AddItemToArray(arr, obj);
    }
    free(vars);
    free(snaps);
    return resp;
}

// ---- write ----------------------------------------------------------------------------------

// Resolves and converts one value; returns an error response or NULL on success
static cJSON *prepare_write(const char *id, const cJSON *json_value, ammio_var_t **var_out, ammio_value_t *value_out)
{
    ammio_var_t *var = var_table_find(id);
    if (!var)
        return make_error(ERR_NOT_FOUND, id, "variable '%s' not found", id);
    if (var_dir(var) != AMMIO_DIR_INPUT)
        return make_error(ERR_READ_ONLY, id, "variable '%s' is an output of the SUT and cannot be written", id);

    ammio_type_t type = var_type(var);
    switch (ammio_value_from_json(type, json_value, value_out))
    {
        case AMMIO_VALUE_OK:
            *var_out = var;
            return NULL;
        case AMMIO_VALUE_OUT_OF_RANGE:
            return make_error(ERR_OUT_OF_RANGE, id, "value out of range for '%s' (type %s)", id, ammio_type_name(type));
        default:
            return make_error(ERR_INVALID_VALUE, id, "invalid value for '%s': expected %s%s", id,
                              ammio_type_is_integer(type) ? "an integer " : "", ammio_type_name(type));
    }
}

static cJSON *handle_write(const cJSON *req)
{
    const cJSON *var_id = cJSON_GetObjectItemCaseSensitive(req, "var_id");
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(req, "values");

    if (var_id && values)
        return make_error(ERR_INVALID_REQUEST, NULL, "use either 'var_id'/'value' or 'values', not both");

    if (var_id)
    {
        if (!cJSON_IsString(var_id))
            return make_error(ERR_INVALID_REQUEST, NULL, "'var_id' must be a string");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(req, "value");
        if (!value)
            return make_error(ERR_INVALID_REQUEST, var_id->valuestring, "missing 'value'");

        ammio_var_t *var;
        ammio_value_t v;
        cJSON *err = prepare_write(var_id->valuestring, value, &var, &v);
        if (err)
            return err;
        var_table_client_write_many(&var, &v, 1);
        return make_ok();
    }

    if (!cJSON_IsObject(values))
        return make_error(ERR_INVALID_REQUEST, NULL, "'write' needs 'var_id' + 'value' or 'values' (object)");

    size_t n = (size_t)cJSON_GetArraySize(values);
    ammio_var_t **vars = calloc(n ? n : 1, sizeof(*vars));
    ammio_value_t *vals = calloc(n ? n : 1, sizeof(*vals));
    if (!vars || !vals)
    {
        free(vars);
        free(vals);
        return make_error(ERR_INTERNAL, NULL, "out of memory");
    }

    // Validate everything first: either all values are written or none
    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, values)
    {
        cJSON *err = prepare_write(item->string, item, &vars[i], &vals[i]);
        if (err)
        {
            free(vars);
            free(vals);
            return err;
        }
        i++;
    }

    var_table_client_write_many(vars, vals, n);
    free(vars);
    free(vals);
    return make_ok();
}

// ---- listings -------------------------------------------------------------------------------

static cJSON *handle_list_vars(void)
{
    ammio_var_snapshot_t *snaps = NULL;
    size_t n = 0;
    if (var_table_list(&snaps, &n) != 0)
        return make_error(ERR_INTERNAL, NULL, "out of memory");

    cJSON *resp = make_ok();
    cJSON *arr = cJSON_AddArrayToObject(resp, "vars");
    for (size_t i = 0; i < n; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "var_id", snaps[i].var_id);
        cJSON_AddStringToObject(obj, "type", ammio_type_name(snaps[i].type));
        cJSON_AddStringToObject(obj, "dir", ammio_dir_name(snaps[i].dir));
        cJSON_AddStringToObject(obj, "access", snaps[i].dir == AMMIO_DIR_INPUT ? "rw" : "r");
        cJSON_AddStringToObject(obj, "connection", snaps[i].connection);
        cJSON_AddItemToArray(arr, obj);
    }
    free(snaps);
    return resp;
}

static cJSON *handle_list_connections(void)
{
    size_t n = connections_count();
    ammio_conn_info_t *infos = calloc(n ? n : 1, sizeof(*infos));
    if (!infos)
        return make_error(ERR_INTERNAL, NULL, "out of memory");
    connections_list(infos, n);

    cJSON *resp = make_ok();
    cJSON *arr = cJSON_AddArrayToObject(resp, "connections");
    for (size_t i = 0; i < n; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", infos[i].name);
        cJSON_AddStringToObject(obj, "protocol", infos[i].protocol);
        cJSON_AddStringToObject(obj, "state", ammio_conn_state_name(infos[i].state));
        cJSON_AddItemToArray(arr, obj);
    }
    free(infos);
    return resp;
}

static cJSON *handle_info(void)
{
    cJSON *resp = make_ok();
    cJSON_AddStringToObject(resp, "name", "ammio");
    cJSON_AddStringToObject(resp, "version", AMMIO_VERSION);
    cJSON *protocols = cJSON_AddArrayToObject(resp, "protocols");
    cJSON_AddItemToArray(protocols, cJSON_CreateString("modbus"));
    cJSON_AddItemToArray(protocols, cJSON_CreateString("opcua"));
    cJSON_AddItemToArray(protocols, cJSON_CreateString("trdp"));
    if (events_endpoint)
        cJSON_AddStringToObject(resp, "events", events_endpoint);
    else
        cJSON_AddNullToObject(resp, "events");
    return resp;
}

// ---------------------------------------------------------------------------------------------

// Parses exactly len bytes (requests are not NUL-terminated) and rejects trailing garbage
static cJSON *parse_request(const char *data, size_t len)
{
    const char *end = NULL;
    cJSON *req = cJSON_ParseWithLengthOpts(data, len, &end, false);
    if (!req)
        return NULL;
    while (end && end < data + len && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == '\0'))
        end++;
    if (!end || end != data + len)
    {
        cJSON_Delete(req);
        return NULL;
    }
    return req;
}

char *api_handle_request(const char *data, size_t len)
{
    cJSON *resp;
    cJSON *req = parse_request(data, len);

    if (!req)
    {
        resp = make_error(ERR_INVALID_JSON, NULL, "request is not valid JSON");
    }
    else if (!cJSON_IsObject(req))
    {
        resp = make_error(ERR_INVALID_REQUEST, NULL, "request must be a JSON object");
    }
    else
    {
        const char *cmd = json_string(req, "cmd");
        if (!cmd)
            resp = make_error(ERR_INVALID_REQUEST, NULL, "missing 'cmd' (string)");
        else if (strcmp(cmd, "read") == 0)
            resp = handle_read(req);
        else if (strcmp(cmd, "write") == 0)
            resp = handle_write(req);
        else if (strcmp(cmd, "list_vars") == 0)
            resp = handle_list_vars();
        else if (strcmp(cmd, "list_connections") == 0)
            resp = handle_list_connections();
        else if (strcmp(cmd, "info") == 0)
            resp = handle_info();
        else
            resp = make_error(ERR_UNKNOWN_CMD, NULL, "unknown command '%s'", cmd);

        // Echo a client-chosen request id so responses can be correlated
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
        if (id)
            cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return out;
}

int api_server_open(const char *endpoint)
{
    int rv;
    if ((rv = nng_rep0_open(&sock)) != 0)
    {
        log_error("api: cannot open socket: %s", nng_strerror(rv));
        return -1;
    }
    sock_open = true;
    nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, RECV_POLL_MS);

    if ((rv = nng_listen(sock, endpoint, NULL, 0)) != 0)
    {
        log_error("api: cannot listen on %s: %s", endpoint, nng_strerror(rv));
        api_server_close();
        return -1;
    }
    return 0;
}

void api_server_run(const atomic_bool *running)
{
    while (atomic_load(running))
    {
        char *buf = NULL;
        size_t sz = 0;
        int rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC);
        if (rv == NNG_ETIMEDOUT)
            continue;
        if (rv == NNG_ECLOSED)
            break;
        if (rv != 0)
        {
            log_warn("api: receive failed: %s", nng_strerror(rv));
            continue;
        }

        char *response = api_handle_request(buf, sz);
        nng_free(buf, sz);
        if (!response)
        {
            log_error("api: out of memory building response");
            continue;
        }

        log_debug("api: %s", response);
        if ((rv = nng_send(sock, response, strlen(response), 0)) != 0)
            log_warn("api: send failed: %s", nng_strerror(rv));
        free(response);
    }
}

void api_server_close(void)
{
    if (sock_open)
    {
        nng_close(sock);
        sock_open = false;
    }
}

void api_server_set_events_endpoint(const char *endpoint)
{
    events_endpoint = endpoint;
}
