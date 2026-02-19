#include "trdp.h"
#include "../var_table.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include "../compat/net.h"

#include "trdp_if_light.h"
#include "vos_thread.h"
#include "vos_sock.h"

#define MAX_CONTAINERS 64

// Module state
static cJSON *trdp_config = NULL;
static char local_ip[16] = "0.0.0.0";
static TRDP_APP_SESSION_T app_handle = NULL;

static trdp_container_t input_containers[MAX_CONTAINERS];
static size_t input_count = 0;

static trdp_container_t output_containers[MAX_CONTAINERS];
static size_t output_count = 0;

static thrd_t thread_process;
static atomic_bool running = false;

// Get byte size for a type
static size_t type_size_bits(type_t type)
{
    switch (type)
    {
        case TYPE_UINT8:
        case TYPE_INT8:
            return 8;
        case TYPE_UINT16:
        case TYPE_INT16:
            return 16;
        case TYPE_UINT32:
        case TYPE_INT32:
        case TYPE_FLOAT32:
            return 32;
        case TYPE_FLOAT64:
            return 64;
        default:
            return 8;
    }
}

// Parse IP address string to TRDP_IP_ADDR_T
static TRDP_IP_ADDR_T parse_ip(const char *ip_str)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) == 1)
    {
        return ntohl(addr.s_addr);
    }
    return 0;
}

// Read values from var_table into packet buffer (for publishing inputs)
static void get_from_var_table(trdp_container_t *container)
{
    for (size_t i = 0; i < container->var_count; i++)
    {
        trdp_var_mapping_t *mapping = &container->variables[i];
        uint32_t byte_offset = mapping->offset_bits / 8;

        if (byte_offset >= container->size_bytes)
            continue;

        var_t var;
        if (var_table_get(mapping->name, &var) != 0)
            continue;

        uint8_t *dest = container->buffer + byte_offset;

        switch (mapping->type)
        {
            case TYPE_UINT8:
                *dest = var.value.u8;
                break;
            case TYPE_INT8:
                *(int8_t *)dest = var.value.i8;
                break;
            case TYPE_UINT16:
            {
                uint16_t val = htons(var.value.u16);
                memcpy(dest, &val, sizeof(val));
                break;
            }
            case TYPE_INT16:
            {
                int16_t val = (int16_t)htons((uint16_t)var.value.i16);
                memcpy(dest, &val, sizeof(val));
                break;
            }
            case TYPE_UINT32:
            {
                uint32_t val = htonl(var.value.u32);
                memcpy(dest, &val, sizeof(val));
                break;
            }
            case TYPE_INT32:
            {
                int32_t val = (int32_t)htonl((uint32_t)var.value.i32);
                memcpy(dest, &val, sizeof(val));
                break;
            }
            case TYPE_FLOAT32:
            {
                uint32_t tmp;
                memcpy(&tmp, &var.value.f32, sizeof(tmp));
                tmp = htonl(tmp);
                memcpy(dest, &tmp, sizeof(tmp));
                break;
            }
            case TYPE_FLOAT64:
            {
                uint64_t tmp;
                memcpy(&tmp, &var.value.f64, sizeof(tmp));
                // Convert to network byte order (big endian)
                uint8_t *p = (uint8_t *)&tmp;
                uint8_t swapped[8];
                for (int j = 0; j < 8; j++)
                    swapped[j] = p[7 - j];
                memcpy(dest, swapped, sizeof(swapped));
                break;
            }
        }
    }
}

// Write values from packet buffer into var_table (for received outputs)
static void set_to_var_table(trdp_container_t *container)
{
    for (size_t i = 0; i < container->var_count; i++)
    {
        trdp_var_mapping_t *mapping = &container->variables[i];
        uint32_t byte_offset = mapping->offset_bits / 8;

        if (byte_offset >= container->size_bytes)
            continue;

        uint8_t *src = container->buffer + byte_offset;
        var_t var;
        var.type = mapping->type;

        switch (mapping->type)
        {
            case TYPE_UINT8:
                var.value.u8 = *src;
                break;
            case TYPE_INT8:
                var.value.i8 = *(int8_t *)src;
                break;
            case TYPE_UINT16:
            {
                uint16_t val;
                memcpy(&val, src, sizeof(val));
                var.value.u16 = ntohs(val);
                break;
            }
            case TYPE_INT16:
            {
                int16_t val;
                memcpy(&val, src, sizeof(val));
                var.value.i16 = (int16_t)ntohs((uint16_t)val);
                break;
            }
            case TYPE_UINT32:
            {
                uint32_t val;
                memcpy(&val, src, sizeof(val));
                var.value.u32 = ntohl(val);
                break;
            }
            case TYPE_INT32:
            {
                int32_t val;
                memcpy(&val, src, sizeof(val));
                var.value.i32 = (int32_t)ntohl((uint32_t)val);
                break;
            }
            case TYPE_FLOAT32:
            {
                uint32_t tmp;
                memcpy(&tmp, src, sizeof(tmp));
                tmp = ntohl(tmp);
                memcpy(&var.value.f32, &tmp, sizeof(var.value.f32));
                break;
            }
            case TYPE_FLOAT64:
            {
                uint8_t swapped[8];
                for (int j = 0; j < 8; j++)
                    swapped[j] = src[7 - j];
                memcpy(&var.value.f64, swapped, sizeof(var.value.f64));
                break;
            }
        }

        var_table_set(mapping->name, &var);
    }
}

// Parse container from JSON and populate structure
static int parse_container(cJSON *json, trdp_container_t *container, dir_t dir)
{
    cJSON *name = cJSON_GetObjectItem(json, "name");
    cJSON *comid = cJSON_GetObjectItem(json, "comid");
    cJSON *multicast_ip = cJSON_GetObjectItem(json, "multicast_ip");
    cJSON *period_ms = cJSON_GetObjectItem(json, "period_ms");
    cJSON *size_bits = cJSON_GetObjectItem(json, "size_bits");
    cJSON *variables = cJSON_GetObjectItem(json, "variables");

    if (!name || !cJSON_IsString(name))
    {
        log_debug("trdp: container missing name");
        return -1;
    }

    strncpy(container->name, name->valuestring, TRDP_MAX_NAME_LEN - 1);
    container->comid = comid && cJSON_IsNumber(comid) ? (uint32_t)comid->valuedouble : 0;

    if (multicast_ip && cJSON_IsString(multicast_ip))
    {
        strncpy(container->multicast_ip, multicast_ip->valuestring, 15);
    }

    container->period_ms = period_ms && cJSON_IsNumber(period_ms) ? (uint32_t)period_ms->valuedouble : 100;
    container->size_bits = size_bits && cJSON_IsNumber(size_bits) ? (uint32_t)size_bits->valuedouble : 0;
    container->size_bytes = (container->size_bits + 7) / 8;

    // Allocate buffer
    container->buffer = calloc(1, container->size_bytes);
    if (!container->buffer)
    {
        log_debug("trdp: failed to allocate buffer for container %s", container->name);
        return -1;
    }

    // Parse variables
    container->var_count = 0;
    container->variables = NULL;
    if (variables && cJSON_IsArray(variables))
    {
        int num_vars = cJSON_GetArraySize(variables);
        if (num_vars > 0)
        {
            container->variables = calloc((size_t)num_vars, sizeof(trdp_var_mapping_t));
            if (!container->variables)
            {
                log_debug("trdp: failed to allocate variables for container %s", container->name);
                free(container->buffer);
                return -1;
            }
        }

        cJSON *var;
        cJSON_ArrayForEach(var, variables)
        {
            cJSON *var_name = cJSON_GetObjectItem(var, "name");
            cJSON *var_offset = cJSON_GetObjectItem(var, "offset");
            cJSON *var_type = cJSON_GetObjectItem(var, "type");

            if (!var_name || !cJSON_IsString(var_name))
                continue;

            trdp_var_mapping_t *mapping = &container->variables[container->var_count];
            strncpy(mapping->name, var_name->valuestring, TRDP_MAX_NAME_LEN - 1);
            mapping->offset_bits = var_offset && cJSON_IsNumber(var_offset) ? (uint32_t)var_offset->valuedouble : 0;
            mapping->type = var_type && cJSON_IsString(var_type) ?
                var_table_type_from_string(var_type->valuestring) : TYPE_UINT8;

            // Add to var_table
            var_table_add(mapping->name, mapping->type, dir);
            log_debug("trdp: added variable %s (offset=%u, type=%s)",
                     mapping->name, mapping->offset_bits, var_type ? var_type->valuestring : "uint8");

            container->var_count++;
        }
    }

    container->handle = NULL;
    return 0;
}

// Parse containers from JSON array
static int parse_containers(cJSON *json_array, trdp_container_t *containers, size_t *count, size_t max, dir_t dir)
{
    *count = 0;
    if (!json_array || !cJSON_IsArray(json_array))
        return 0;

    cJSON *item;
    cJSON_ArrayForEach(item, json_array)
    {
        if (*count >= max)
            break;

        if (parse_container(item, &containers[*count], dir) == 0)
        {
            (*count)++;
        }
    }
    return 0;
}

// Main protocol loop
static int thread_process_func(void *arg)
{
    (void)arg;
    TRDP_ERR_T err;
    TRDP_FDS_T rfds;
    TRDP_TIME_T tv;
    INT32 noOfDesc;

    while (running)
    {
        // Send: get values from var_table and publish to protocol bus
        for (size_t i = 0; i < input_count; i++)
        {
            trdp_container_t *container = &input_containers[i];
            if (container->handle)
            {
                get_from_var_table(container);
                err = tlp_put(app_handle, (TRDP_PUB_T)container->handle,
                             container->buffer, container->size_bytes);
                if (err != TRDP_NO_ERR)
                {
                    log_debug("trdp: tlp_put failed for %s: %d", container->name, err);
                }
            }
        }

        // TRDP housekeeping
        FD_ZERO(&rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms
        noOfDesc = 0;

        err = tlc_getInterval(app_handle, &tv, &rfds, &noOfDesc);
        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp: tlc_getInterval failed: %d", err);
        }

        // Wait for network data or timeout
        (void)vos_select((VOS_SOCK_T)noOfDesc, &rfds, NULL, NULL, &tv);

        err = tlc_process(app_handle, &rfds, &noOfDesc);
        if (err != TRDP_NO_ERR && err != TRDP_NODATA_ERR)
        {
            log_debug("trdp: tlc_process failed: %d", err);
        }

        // Receive: get values from protocol bus and set into var_table
        for (size_t i = 0; i < output_count; i++)
        {
            trdp_container_t *container = &output_containers[i];
            if (container->handle)
            {
                TRDP_PD_INFO_T pd_info;
                UINT32 recv_size = container->size_bytes;

                err = tlp_get(app_handle, (TRDP_SUB_T)container->handle,
                             &pd_info, container->buffer, &recv_size);
                if (err == TRDP_NO_ERR)
                {
                    set_to_var_table(container);
                }
                else if (err != TRDP_NODATA_ERR && err != TRDP_TIMEOUT_ERR)
                {
                    log_debug("trdp: tlp_get failed for %s: %d", container->name, err);
                }
            }
        }

    }

    return 0;
}

static int trdp_init(cJSON *config)
{
    trdp_config = config;

    // Parse local_ip
    cJSON *local_ip_json = cJSON_GetObjectItem(config, "local_ip");
    if (local_ip_json && cJSON_IsString(local_ip_json))
    {
        strncpy(local_ip, local_ip_json->valuestring, 15);
    }
    log_debug("trdp: local_ip = %s", local_ip);

    // Parse containers
    cJSON *containers = cJSON_GetObjectItem(config, "containers");
    if (!containers)
    {
        log_debug("trdp: no containers defined");
        return 0;
    }

    // Parse inputs (ammio publishes to SUT)
    cJSON *inputs = cJSON_GetObjectItem(containers, "inputs");
    if (inputs)
    {
        parse_containers(inputs, input_containers, &input_count, MAX_CONTAINERS, DIR_INPUT);
        log_debug("trdp: parsed %zu input containers", input_count);
    }

    // Parse outputs (ammio subscribes from SUT)
    cJSON *outputs = cJSON_GetObjectItem(containers, "outputs");
    if (outputs)
    {
        parse_containers(outputs, output_containers, &output_count, MAX_CONTAINERS, DIR_OUTPUT);
        log_debug("trdp: parsed %zu output containers", output_count);
    }

    return 0;
}

static int trdp_start(void)
{
    TRDP_ERR_T err;

    // Initialize TRDP library
    err = tlc_init(NULL, NULL, NULL);
    if (err != TRDP_NO_ERR)
    {
        log_debug("trdp: tlc_init failed: %d", err);
        return -1;
    }

    // Open session
    TRDP_IP_ADDR_T own_ip = parse_ip(local_ip);
    err = tlc_openSession(&app_handle, own_ip, 0, NULL, NULL, NULL, NULL);
    if (err != TRDP_NO_ERR)
    {
        log_debug("trdp: tlc_openSession failed: %d", err);
        tlc_terminate();
        return -1;
    }
    log_debug("trdp: session opened");

    // Create publishers for input containers
    for (size_t i = 0; i < input_count; i++)
    {
        trdp_container_t *container = &input_containers[i];
        TRDP_PUB_T pub_handle;
        TRDP_IP_ADDR_T dest_ip = parse_ip(container->multicast_ip);

        err = tlp_publish(app_handle, &pub_handle, NULL, NULL, 0,
                         container->comid, 0, 0, own_ip, dest_ip,
                         container->period_ms * 1000,  // microseconds
                         0, TRDP_FLAGS_DEFAULT, NULL,
                         container->buffer, container->size_bytes);

        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp: tlp_publish failed for %s (comid=%u): %d",
                    container->name, container->comid, err);
        }
        else
        {
            container->handle = pub_handle;
            log_debug("trdp: publishing %s (comid=%u, dest=%s, period=%ums)",
                    container->name, container->comid, container->multicast_ip, container->period_ms);
        }
    }

    // Create subscribers for output containers
    for (size_t i = 0; i < output_count; i++)
    {
        trdp_container_t *container = &output_containers[i];
        TRDP_SUB_T sub_handle;
        TRDP_IP_ADDR_T mc_ip = parse_ip(container->multicast_ip);

        err = tlp_subscribe(app_handle, &sub_handle, NULL, NULL, 0,
                           container->comid, 0, 0, 0, 0, mc_ip,
                           TRDP_FLAGS_DEFAULT, NULL,
                           container->period_ms * 3 * 1000,  // timeout in microseconds
                           TRDP_TO_SET_TO_ZERO);

        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp: tlp_subscribe failed for %s (comid=%u): %d",
                    container->name, container->comid, err);
        }
        else
        {
            container->handle = sub_handle;
            log_debug("trdp: subscribed to %s (comid=%u, src=%s)",
                    container->name, container->comid, container->multicast_ip);
        }
    }

    // Start process thread
    running = true;

    if (thrd_create(&thread_process, thread_process_func, NULL) != thrd_success)
    {
        log_info("trdp: failed to create process thread");
        running = false;
        tlc_closeSession(app_handle);
        tlc_terminate();
        return -1;
    }
    log_info("trdp: process thread started");

    return 0;
}

static void trdp_stop(void)
{
    if (!running)
        return;

    // Signal threads to stop
    running = false;

    // Wait for thread
    thrd_join(thread_process, NULL);
    log_debug("trdp: process thread stopped");

    // Unpublish all input containers
    for (size_t i = 0; i < input_count; i++)
    {
        if (input_containers[i].handle)
        {
            tlp_unpublish(app_handle, (TRDP_PUB_T)input_containers[i].handle);
            input_containers[i].handle = NULL;
        }
        free(input_containers[i].buffer);
        input_containers[i].buffer = NULL;
        free(input_containers[i].variables);
        input_containers[i].variables = NULL;
    }

    // Unsubscribe all output containers
    for (size_t i = 0; i < output_count; i++)
    {
        if (output_containers[i].handle)
        {
            tlp_unsubscribe(app_handle, (TRDP_SUB_T)output_containers[i].handle);
            output_containers[i].handle = NULL;
        }
        free(output_containers[i].buffer);
        output_containers[i].buffer = NULL;
        free(output_containers[i].variables);
        output_containers[i].variables = NULL;
    }

    // Close session and terminate
    if (app_handle)
    {
        tlc_closeSession(app_handle);
        app_handle = NULL;
    }
    tlc_terminate();
    log_debug("trdp: terminated");
}

static protocol_t trdp_protocol = {
    .name = "trdp",
    .init = trdp_init,
    .start = trdp_start,
    .stop = trdp_stop
};

void trdp_register(void)
{
    protocol_register(&trdp_protocol);
}
