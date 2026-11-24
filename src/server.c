#include "server.h"
#include "cJSON.h"
#include "log.h"
#include "vars.h"

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define POLL_MS 100 // how often server_run() checks the stop flag

static nng_socket sock = NNG_SOCKET_INITIALIZER;

static char *reply(cJSON *obj)
{
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return str;
}

static char *error(api_error_t code, const char *fmt, ...) AMMIO_PRINTF(2, 3);
static char *error(api_error_t code, const char *fmt, ...)
{
    char msg[300];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    log_debug("api: error %d: %s", code, msg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "error", code);
    cJSON_AddStringToObject(obj, "message", msg);
    return reply(obj);
}

static const char *const error_names[ERR_COUNT] = {
    [ERR_NOT_FOUND] = "variable not found",
    [ERR_READ_ONLY] = "read-only variable",
    [ERR_INVALID_JSON] = "invalid json",
    [ERR_INVALID_CMD] = "invalid command",
    [ERR_MISSING_VALUE] = "missing value",
    [ERR_INVALID_VALUE] = "invalid value",
};

static char *handle_read(const var_t *var)
{
    uint64_t ts = 0;
    value_t value = var_get(var, &ts);

    // "type" is the numeric type code here, as in ammio v0.5 (list_vars gives names).
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "var_id", var_id(var));
    cJSON_AddNumberToObject(obj, "type", var_type(var));
    cJSON_AddStringToObject(obj, "dir", dir_name(var_dir(var)));
    if (var_type(var) == TYPE_BOOL)
        cJSON_AddBoolToObject(obj, "value", value.b);
    else
        cJSON_AddNumberToObject(obj, "value", value_to_double(var_type(var), value));
    cJSON_AddNumberToObject(obj, "timestamp", (double)ts);
    return reply(obj);
}

static char *handle_write(var_t *var, const cJSON *json_value)
{
    if (var_dir(var) == DIR_OUTPUT)
        return error(ERR_READ_ONLY, "'%s' is an output and cannot be written", var_id(var));
    if (!json_value)
        return error(ERR_MISSING_VALUE, "missing 'value'");

    double d;
    if (cJSON_IsBool(json_value))
        d = cJSON_IsTrue(json_value) ? 1 : 0;
    else if (cJSON_IsNumber(json_value))
        d = json_value->valuedouble;
    else
        return error(ERR_INVALID_VALUE, "'value' must be a number or a boolean");

    value_t value;
    if (!value_from_double(var_type(var), d, &value))
        return error(ERR_INVALID_VALUE, "%.17g does not fit '%s' (%s)",
                     d, var_id(var), type_name(var_type(var)));

    var_set(var, value);
    log_debug("api: %s = %.17g", var_id(var), d);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    return reply(obj);
}

static char *handle_list(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(obj, "vars");
    for (size_t i = 0; i < vars_count(); i++)
    {
        const var_t *var = vars_at(i);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "var_id", var_id(var));
        cJSON_AddStringToObject(item, "type", type_name(var_type(var)));
        cJSON_AddStringToObject(item, "dir", dir_name(var_dir(var)));
        cJSON_AddItemToArray(arr, item);
    }
    return reply(obj);
}

static char *handle_list_errors(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *errors = cJSON_AddObjectToObject(obj, "errors");
    cJSON_AddStringToObject(errors, "0", "ok");
    for (int code = 1; code < ERR_COUNT; code++)
    {
        char key[8];
        snprintf(key, sizeof(key), "%d", code);
        cJSON_AddStringToObject(errors, key, error_names[code]);
    }
    return reply(obj);
}

static char *dispatch(const cJSON *req)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    if (!cJSON_IsString(cmd))
        return error(ERR_INVALID_CMD, "missing 'cmd' (read, write, list_vars, list_errors)");

    if (strcmp(cmd->valuestring, "list_vars") == 0)
        return handle_list();
    if (strcmp(cmd->valuestring, "list_errors") == 0)
        return handle_list_errors();

    bool is_read = strcmp(cmd->valuestring, "read") == 0;
    bool is_write = strcmp(cmd->valuestring, "write") == 0;
    if (!is_read && !is_write)
        return error(ERR_INVALID_CMD, "unknown cmd '%s' (read, write, list_vars, list_errors)",
                     cmd->valuestring);

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "var_id");
    if (!cJSON_IsString(id))
        return error(ERR_INVALID_CMD, "missing 'var_id'");

    var_t *var = vars_find(id->valuestring);
    if (!var)
        return error(ERR_NOT_FOUND, "unknown variable '%s'", id->valuestring);

    return is_read ? handle_read(var)
                   : handle_write(var, cJSON_GetObjectItemCaseSensitive(req, "value"));
}

char *server_handle(const char *request, size_t len)
{
    cJSON *req = cJSON_ParseWithLength(request, len);
    char *resp = cJSON_IsObject(req) ? dispatch(req)
                                     : error(ERR_INVALID_JSON, "request is not a JSON object");
    cJSON_Delete(req);
    return resp;
}

int server_open(const char *url)
{
    int rv;
    if ((rv = nng_rep0_open(&sock)) != 0)
    {
        log_error("api: cannot open socket: %s", nng_strerror(rv));
        return -1;
    }
    if ((rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, POLL_MS)) != 0 ||
        (rv = nng_listen(sock, url, NULL, 0)) != 0)
    {
        log_error("api: cannot listen on %s: %s", url, nng_strerror(rv));
        nng_close(sock);
        sock = (nng_socket)NNG_SOCKET_INITIALIZER;
        return -1;
    }
    return 0;
}

void server_run(const atomic_bool *stop)
{
    while (!atomic_load(stop))
    {
        char *buf = NULL;
        size_t size = 0;
        int rv = nng_recv(sock, &buf, &size, NNG_FLAG_ALLOC);
        if (rv == NNG_ETIMEDOUT)
            continue;
        if (rv != 0)
        {
            if (rv == NNG_ECLOSED)
                break;
            log_warn("api: receive failed: %s", nng_strerror(rv));
            continue;
        }

        char *resp = server_handle(buf, size);
        nng_free(buf, size);
        if (!resp)
            continue; // out of memory: REQ client will retry

        if ((rv = nng_send(sock, resp, strlen(resp), 0)) != 0)
            log_warn("api: send failed: %s", nng_strerror(rv));
        cJSON_free(resp);
    }
}

void server_close(void)
{
    nng_close(sock);
    sock = (nng_socket)NNG_SOCKET_INITIALIZER;
}
