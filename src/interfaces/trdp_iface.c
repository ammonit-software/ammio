#include "trdp_iface.h"
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

// Returns bitset field size in bytes for "bitset8/16/32", or 0 if not a bitset type.
static int bitset_size_bytes(const char *type_str)
{
    if (strcmp(type_str, "bitset8")  == 0) return 1;
    if (strcmp(type_str, "bitset16") == 0) return 2;
    if (strcmp(type_str, "bitset32") == 0) return 4;
    return 0;
}

// Byte offset of a bit within a TRDP bitset field.
// TRDP bit numbering: bit 0 is the MSB of the first byte, bit 7 is the LSB of the first byte,
// bit 8 is the MSB of the second byte, etc.
static inline uint32_t bitset_target_byte(uint32_t base, int bit_index)
{
    return base + (uint32_t)(bit_index / 8);
}

static inline int bitset_bit_in_byte(int bit_index)
{
    return 7 - (bit_index % 8);
}

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

        // Bitset member: apply bit value into buffer
        if (mapping->bit_index >= 0)
        {
            var_t var;
            if (var_table_get(mapping->name, &var) != 0)
                continue;

            uint32_t target_byte = bitset_target_byte(byte_offset, mapping->bit_index);
            if (target_byte >= container->size_bytes)
                continue;

            int bit_in_byte = bitset_bit_in_byte(mapping->bit_index);
            if (var.value.u8)
                container->buffer[target_byte] |= (uint8_t)(1u << bit_in_byte);
            else
                container->buffer[target_byte] &= (uint8_t)~(1u << bit_in_byte);
            continue;
        }

        // Full bitset var: bits were already applied above; sync VARNAME to the assembled buffer value
        if (mapping->bitset_bytes > 0)
        {
            uint8_t *src = container->buffer + byte_offset;
            var_t var;
            var.type = mapping->type;
            switch (mapping->type)
            {
                case TYPE_UINT8:  var.value.u8  = *src; break;
                case TYPE_UINT16: { uint16_t v; memcpy(&v, src, 2); var.value.u16 = ntohs(v); break; }
                case TYPE_UINT32: { uint32_t v; memcpy(&v, src, 4); var.value.u32 = ntohl(v); break; }
                default: break;
            }
            var_table_set(mapping->name, &var);
            continue;
        }

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

        // Bitset member: extract the bit and store as uint8 (0 or 1)
        if (mapping->bit_index >= 0)
        {
            uint32_t target_byte = bitset_target_byte(byte_offset, mapping->bit_index);

            if (target_byte >= container->size_bytes)
                continue;

            var_t var;
            var.type = TYPE_UINT8;
            var.value.u8 = (container->buffer[target_byte] >> bitset_bit_in_byte(mapping->bit_index)) & 1u;
            var_table_set(mapping->name, &var);
            continue;
        }

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
            // Count exact number of mappings (bitsets expand to one entry per bit)
            int total_mappings = 0;
            cJSON *v;
            cJSON_ArrayForEach(v, variables)
            {
                cJSON *t = cJSON_GetObjectItem(v, "type");
                cJSON *b = cJSON_GetObjectItem(v, "bits");
                const char *ts = (t && cJSON_IsString(t)) ? t->valuestring : "uint8";
                int bs = bitset_size_bytes(ts);
                if (bs > 0 && b && cJSON_IsArray(b))
                    total_mappings += 1 + cJSON_GetArraySize(b);  // 1 for full bitset + N for bits
                else
                    total_mappings++;
            }

            container->variables = calloc((size_t)total_mappings, sizeof(trdp_var_mapping_t));
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
            cJSON *var_bits = cJSON_GetObjectItem(var, "bits");

            if (!var_name || !cJSON_IsString(var_name))
                continue;

            const char *type_str = (var_type && cJSON_IsString(var_type)) ? var_type->valuestring : "uint8";
            uint32_t offset = (var_offset && cJSON_IsNumber(var_offset)) ? (uint32_t)var_offset->valuedouble : 0;

            // Bitset types: one entry per bit (VARNAME.BITNAME) + one full var (VARNAME as uint8/16/32)
            // Bits are added first so get_from_var_table assembles the buffer before the full var reads it back.
            int bitset_bytes = bitset_size_bytes(type_str);
            if (bitset_bytes > 0 && var_bits && cJSON_IsArray(var_bits))
            {
                int max_bits = bitset_bytes * 8;
                int bit_count = cJSON_GetArraySize(var_bits);
                if (bit_count > max_bits)
                    bit_count = max_bits;

                // Individual bit vars: VARNAME.BITNAME
                for (int b = 0; b < bit_count; b++)
                {
                    cJSON *bit_item = cJSON_GetArrayItem(var_bits, b);
                    const char *bit_name = (bit_item && cJSON_IsString(bit_item)) ? bit_item->valuestring : NULL;

                    trdp_var_mapping_t *mapping = &container->variables[container->var_count];
                    if (bit_name && bit_name[0])
                        snprintf(mapping->name, TRDP_MAX_NAME_LEN, "%s.%s", var_name->valuestring, bit_name);
                    else
                        snprintf(mapping->name, TRDP_MAX_NAME_LEN, "%s.bit%d", var_name->valuestring, b);

                    mapping->offset_bits = offset;
                    mapping->type = TYPE_UINT8;
                    mapping->bit_index = b;
                    mapping->bitset_bytes = bitset_bytes;

                    var_table_add(mapping->name, TYPE_UINT8, dir);
                    log_debug("trdp: added bit member %s (offset=%u, bit=%d)",
                             mapping->name, offset, b);
                    container->var_count++;
                }

                // Full bitset var: added last so get_from_var_table reads the assembled buffer value
                type_t full_type = TYPE_UINT8;
                if (bitset_bytes == 2) full_type = TYPE_UINT16;
                else if (bitset_bytes == 4) full_type = TYPE_UINT32;

                trdp_var_mapping_t *full = &container->variables[container->var_count];
                strncpy(full->name, var_name->valuestring, TRDP_MAX_NAME_LEN - 1);
                full->offset_bits = offset;
                full->type = full_type;
                full->bit_index = -1;
                full->bitset_bytes = bitset_bytes;
                var_table_add(full->name, full_type, dir);
                log_debug("trdp: added bitset var %s (offset=%u, type=%s)", full->name, offset, type_str);
                container->var_count++;
            }
            else
            {
                trdp_var_mapping_t *mapping = &container->variables[container->var_count];
                strncpy(mapping->name, var_name->valuestring, TRDP_MAX_NAME_LEN - 1);
                mapping->offset_bits = offset;
                mapping->type = var_table_type_from_string(type_str);
                mapping->bit_index = -1;
                mapping->bitset_bytes = 0;

                var_table_add(mapping->name, mapping->type, dir);
                log_debug("trdp: added variable %s (offset=%u, type=%s)",
                         mapping->name, offset, type_str);
                container->var_count++;
            }
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

static interface_t trdp_interface = {
    .name = "trdp",
    .init = trdp_init,
    .start = trdp_start,
    .stop = trdp_stop
};

void trdp_iface_register(void)
{
    interface_register(&trdp_interface);
}
