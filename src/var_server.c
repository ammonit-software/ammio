#include "var_server.h"
#include "log.h"
#include "var_table.h"
#include "cJSON.h"
#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef enum {
    ERR_OK = 0,
    ERR_NOT_FOUND = 1,
    ERR_READ_ONLY = 2,
    ERR_INVALID_JSON = 3,
    ERR_INVALID_CMD = 4,
    ERR_MISSING_VALUE = 5
} var_error_t;

static const char *var_error_str(var_error_t code)
{
    switch (code) {
        case ERR_OK:            return "ok";
        case ERR_NOT_FOUND:     return "variable not found";
        case ERR_READ_ONLY:     return "read-only variable";
        case ERR_INVALID_JSON:  return "invalid json";
        case ERR_INVALID_CMD:   return "invalid command";
        case ERR_MISSING_VALUE: return "missing value";
        default:                return "unknown error";
    }
}

static char *make_error(var_error_t code)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "error", code);
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

static nng_socket sock;
static bool running = false;

static char *handle_read(const char *name)
{
    var_t var;
    if (var_table_get(name, &var) != 0) {
        log_debug("read: %s (not found)", name);
        return make_error(ERR_NOT_FOUND);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "name", var.name);
    cJSON_AddNumberToObject(resp, "type", var.type);
    cJSON_AddNumberToObject(resp, "timestamp", (double)var.timestamp);

    switch (var.type) {
        case TYPE_UINT8:
            log_debug("read: %s = %u", name, var.value.u8);
            cJSON_AddNumberToObject(resp, "value", var.value.u8);
            break;
        case TYPE_INT8:
            log_debug("read: %s = %d", name, var.value.i8);
            cJSON_AddNumberToObject(resp, "value", var.value.i8);
            break;
        case TYPE_UINT16:
            log_debug("read: %s = %u", name, var.value.u16);
            cJSON_AddNumberToObject(resp, "value", var.value.u16);
            break;
        case TYPE_INT16:
            log_debug("read: %s = %d", name, var.value.i16);
            cJSON_AddNumberToObject(resp, "value", var.value.i16);
            break;
        case TYPE_UINT32:
            log_debug("read: %s = %u", name, var.value.u32);
            cJSON_AddNumberToObject(resp, "value", var.value.u32);
            break;
        case TYPE_INT32:
            log_debug("read: %s = %d", name, var.value.i32);
            cJSON_AddNumberToObject(resp, "value", var.value.i32);
            break;
        case TYPE_FLOAT32:
            log_debug("read: %s = %f", name, var.value.f32);
            cJSON_AddNumberToObject(resp, "value", var.value.f32);
            break;
        case TYPE_FLOAT64:
            log_debug("read: %s = %f", name, var.value.f64);
            cJSON_AddNumberToObject(resp, "value", var.value.f64);
            break;
    }

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

static char *handle_write(const char *name, double value)
{
    var_t var;
    if (var_table_get(name, &var) != 0) {
        log_debug("write: %s (not found)", name);
        return make_error(ERR_NOT_FOUND);
    }

    if (var.dir == DIR_OUTPUT) {
        log_debug("write: %s (read-only)", name);
        return make_error(ERR_READ_ONLY);
    }

    switch (var.type)
    {
    case TYPE_UINT8:
        var.value.u8 = (uint8_t)value;
        log_debug("write: %s = %u", name, var.value.u8);
        break;
    case TYPE_INT8:
        var.value.i8 = (int8_t)value;
        log_debug("write: %s = %d", name, var.value.i8);
        break;
    case TYPE_UINT16:
        var.value.u16 = (uint16_t)value;
        log_debug("write: %s = %u", name, var.value.u16);
        break;
    case TYPE_INT16:
        var.value.i16 = (int16_t)value;
        log_debug("write: %s = %d", name, var.value.i16);
        break;
    case TYPE_UINT32:
        var.value.u32 = (uint32_t)value;
        log_debug("write: %s = %u", name, var.value.u32);
        break;
    case TYPE_INT32:
        var.value.i32 = (int32_t)value;
        log_debug("write: %s = %d", name, var.value.i32);
        break;
    case TYPE_FLOAT32:
        var.value.f32 = (float)value;
        log_debug("write: %s = %f", name, var.value.f32);
        break;
    case TYPE_FLOAT64:
        var.value.f64 = value;
        log_debug("write: %s = %f", name, var.value.f64);
        break;
    }

    var_table_set(name, &var);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

static char *handle_list_vars(void)
{
    var_t *vars = NULL;
    size_t count = 0;

    if (var_table_get_all(&vars, &count) != 0) {
        return make_error(ERR_INVALID_CMD);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", vars[i].name);
        cJSON_AddStringToObject(item, "type", var_table_type_to_string(vars[i].type));
        cJSON_AddStringToObject(item, "dir", var_table_dir_to_string(vars[i].dir));
        cJSON_AddItemToArray(arr, item);
    }

    free(vars);
    cJSON_AddItemToObject(resp, "vars", arr);
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

static char *handle_list_errors(void)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *errors = cJSON_CreateObject();

    cJSON_AddStringToObject(errors, "0", var_error_str(ERR_OK));
    cJSON_AddStringToObject(errors, "1", var_error_str(ERR_NOT_FOUND));
    cJSON_AddStringToObject(errors, "2", var_error_str(ERR_READ_ONLY));
    cJSON_AddStringToObject(errors, "3", var_error_str(ERR_INVALID_JSON));
    cJSON_AddStringToObject(errors, "4", var_error_str(ERR_INVALID_CMD));
    cJSON_AddStringToObject(errors, "5", var_error_str(ERR_MISSING_VALUE));

    cJSON_AddItemToObject(resp, "errors", errors);
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

static char *process_request(const char *msg)
{
    cJSON *req = cJSON_Parse(msg);
    if (!req) {
        return make_error(ERR_INVALID_JSON);
    }

    cJSON *cmd = cJSON_GetObjectItem(req, "cmd");
    char *response = NULL;

    if (cmd && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "list_errors") == 0) {
        response = handle_list_errors();
        cJSON_Delete(req);
        return response;
    }

    if (cmd && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "list_vars") == 0) {
        response = handle_list_vars();
        cJSON_Delete(req);
        return response;
    }

    cJSON *name = cJSON_GetObjectItem(req, "name");

    if (cmd && cJSON_IsString(cmd) && name && cJSON_IsString(name)) {
        if (strcmp(cmd->valuestring, "read") == 0) {
            response = handle_read(name->valuestring);
        } else if (strcmp(cmd->valuestring, "write") == 0) {
            cJSON *value = cJSON_GetObjectItem(req, "value");
            if (value && cJSON_IsNumber(value)) {
                response = handle_write(name->valuestring, value->valuedouble);
            } else {
                response = make_error(ERR_MISSING_VALUE);
            }
        }
    }

    if (!response) {
        response = make_error(ERR_INVALID_CMD);
    }

    cJSON_Delete(req);
    return response;
}

int var_server_init(const char *endpoint)
{
    int rv;

    if ((rv = nng_rep0_open(&sock)) != 0) {
        fprintf(stderr, "nng_rep0_open: %s\n", nng_strerror(rv));
        return -1;
    }

    if ((rv = nng_listen(sock, endpoint, NULL, 0)) != 0) {
        fprintf(stderr, "nng_listen: %s\n", nng_strerror(rv));
        return -1;
    }

    return 0;
}

int var_server_run(void)
{
    running = true;

    while (running) {
        char *buf = NULL;
        size_t sz;
        int rv;

        if ((rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC)) != 0) {
            if (rv == NNG_ECLOSED) break;
            fprintf(stderr, "nng_recv: %s\n", nng_strerror(rv));
            continue;
        }

        char *response = process_request(buf);
        nng_free(buf, sz);

        if ((rv = nng_send(sock, response, strlen(response), 0)) != 0) {
            fprintf(stderr, "nng_send: %s\n", nng_strerror(rv));
        }

        free(response);
    }

    return 0;
}

void var_server_stop(void)
{
    running = false;
    nng_close(sock);
}
