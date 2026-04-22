#include "trdp_iface.h"
#include "../var_table.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include "../compat/net.h"

#include "trdp_if_light.h"
#include "trdp_private.h"
#include "trdp_utils.h"
#include "vos_mem.h"
#include "vos_utils.h"
#include "vos_thread.h"
#include "vos_sock.h"
#include "iec61375-2-3.h"

#define MAX_CONTAINERS 64

// Message type discriminator for containers
typedef enum {
    TRDP_TYPE_PD,
    TRDP_TYPE_Mn,
    TRDP_TYPE_Mr,
    TRDP_TYPE_Mp,
    TRDP_TYPE_Mq,
    TRDP_TYPE_Mc,
    TRDP_TYPE_Me
} trdp_msg_type_t;

// MD container descriptor
typedef struct trdp_md_container_s {
    char name[TRDP_MAX_NAME_LEN];
    char enable_id[TRDP_MAX_NAME_LEN];
    trdp_msg_type_t msg_type;
    uint32_t comid;
    char pair_name[TRDP_MAX_NAME_LEN];  // for Mp: name of the paired Mr container
    char dest_ip[16];
    uint32_t size_bytes;
    uint8_t *buffer;
    trdp_var_mapping_t *variables;
    size_t var_count;
    void *listen_handle;                // TRDP_LIS_T, stored as void*
    uint8_t prev_enabled;               // previous enable value, for rising-edge detection (MD input)
    struct trdp_md_container_s *partner;
} trdp_md_container_t;

// Module state
static cJSON *trdp_config = NULL;
static char local_ip[16] = "0.0.0.0";
static UINT16 source_port = 0u;
static TRDP_APP_SESSION_T app_handle = NULL;

static trdp_container_t input_containers[MAX_CONTAINERS];
static size_t input_count = 0;

static trdp_container_t output_containers[MAX_CONTAINERS];
static size_t output_count = 0;

static trdp_md_container_t md_input_containers[MAX_CONTAINERS];
static size_t md_input_count = 0;

static trdp_md_container_t md_output_containers[MAX_CONTAINERS];
static size_t md_output_count = 0;

static thrd_t thread_process;
static atomic_bool running = false;

// Enable flag helpers — enable_id is the var_table key, read directly from the container's enable_id field
static void register_enable(const char *enable_id, uint8_t default_val)
{
    var_table_add(enable_id, TYPE_UINT8, DIR_INPUT);
    var_t v;
    if (var_table_get(enable_id, &v) == 0)
    {
        v.value.u8 = default_val;
        var_table_set(enable_id, &v);
    }
}

static uint8_t get_enable(const char *enable_id)
{
    var_t v;
    if (var_table_get(enable_id, &v) != 0) return 0;
    return v.value.u8;
}

static void set_enable(const char *enable_id, uint8_t val)
{
    var_t v;
    if (var_table_get(enable_id, &v) != 0) return;
    v.value.u8 = val;
    var_table_set(enable_id, &v);
}

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
            if (var_table_get(mapping->var_id, &var) != 0)
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
            var_table_set(mapping->var_id, &var);
            continue;
        }

        var_t var;
        if (var_table_get(mapping->var_id, &var) != 0)
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
            var_table_set(mapping->var_id, &var);
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

        var_table_set(mapping->var_id, &var);
    }
}

// Read values from var_table into MD container buffer (no bitset handling)
static void md_get_from_var_table(trdp_md_container_t *container)
{
    for (size_t i = 0; i < container->var_count; i++)
    {
        trdp_var_mapping_t *mapping = &container->variables[i];
        uint32_t byte_offset = mapping->offset_bits / 8;

        if (byte_offset >= container->size_bytes)
            continue;

        var_t var;
        if (var_table_get(mapping->var_id, &var) != 0)
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

// Write values from MD container buffer into var_table (no bitset handling)
static void md_set_to_var_table(trdp_md_container_t *container)
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

        var_table_set(mapping->var_id, &var);
    }
}

// Parse container from JSON and populate structure
static int parse_container(cJSON *json, trdp_container_t *container, dir_t dir)
{
    cJSON *name      = cJSON_GetObjectItem(json, "name");
    cJSON *enable_id = cJSON_GetObjectItem(json, "enable_id");
    cJSON *comid     = cJSON_GetObjectItem(json, "comid");
    cJSON *multicast_ip = cJSON_GetObjectItem(json, "multicast_ip");
    cJSON *period_ms = cJSON_GetObjectItem(json, "period_ms");
    cJSON *size_bits = cJSON_GetObjectItem(json, "size_bits");
    cJSON *variables = cJSON_GetObjectItem(json, "variables");

    if (!name || !cJSON_IsString(name))
    {
        log_debug("trdp: container missing name");
        return -1;
    }
    if (!enable_id || !cJSON_IsString(enable_id))
    {
        log_debug("trdp: container '%s' missing enable_id", name->valuestring);
        return -1;
    }

    strncpy(container->name,      name->valuestring,      TRDP_MAX_NAME_LEN - 1);
    strncpy(container->enable_id, enable_id->valuestring, TRDP_MAX_NAME_LEN - 1);
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
            cJSON *var_id_json = cJSON_GetObjectItem(var, "var_id");
            cJSON *var_offset = cJSON_GetObjectItem(var, "offset");
            cJSON *var_type = cJSON_GetObjectItem(var, "type");
            cJSON *var_bits = cJSON_GetObjectItem(var, "bits");

            if (!var_id_json || !cJSON_IsString(var_id_json))
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
                        snprintf(mapping->var_id, TRDP_MAX_NAME_LEN, "%s.%s", var_id_json->valuestring, bit_name);
                    else
                        snprintf(mapping->var_id, TRDP_MAX_NAME_LEN, "%s.bit%d", var_id_json->valuestring, b);

                    mapping->offset_bits = offset;
                    mapping->type = TYPE_UINT8;
                    mapping->bit_index = b;
                    mapping->bitset_bytes = bitset_bytes;

                    var_table_add(mapping->var_id, TYPE_UINT8, dir);
                    log_debug("trdp: added bit member %s (offset=%u, bit=%d)",
                             mapping->var_id, offset, b);
                    container->var_count++;
                }

                // Full bitset var: added last so get_from_var_table reads the assembled buffer value
                type_t full_type = TYPE_UINT8;
                if (bitset_bytes == 2) full_type = TYPE_UINT16;
                else if (bitset_bytes == 4) full_type = TYPE_UINT32;

                trdp_var_mapping_t *full = &container->variables[container->var_count];
                strncpy(full->var_id, var_id_json->valuestring, TRDP_MAX_NAME_LEN - 1);
                full->offset_bits = offset;
                full->type = full_type;
                full->bit_index = -1;
                full->bitset_bytes = bitset_bytes;
                var_table_add(full->var_id, full_type, dir);
                log_debug("trdp: added bitset var %s (offset=%u, type=%s)", full->var_id, offset, type_str);
                container->var_count++;
            }
            else
            {
                trdp_var_mapping_t *mapping = &container->variables[container->var_count];
                strncpy(mapping->var_id, var_id_json->valuestring, TRDP_MAX_NAME_LEN - 1);
                mapping->offset_bits = offset;
                mapping->type = var_table_type_from_string(type_str);
                mapping->bit_index = -1;
                mapping->bitset_bytes = 0;

                var_table_add(mapping->var_id, mapping->type, dir);
                log_debug("trdp: added variable %s (offset=%u, type=%s)",
                         mapping->var_id, offset, type_str);
                container->var_count++;
            }
        }
    }

    container->handle = NULL;
    register_enable(container->enable_id, 1);
    return 0;
}

// Parse containers from JSON array, skipping non-PD containers
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

        // Skip non-PD containers (MD types)
        cJSON *type_field = cJSON_GetObjectItem(item, "type");
        if (type_field && cJSON_IsString(type_field) && strcmp(type_field->valuestring, "Pd") != 0)
            continue;

        if (parse_container(item, &containers[*count], dir) == 0)
        {
            (*count)++;
        }
    }
    return 0;
}

// Map type string to trdp_msg_type_t
static trdp_msg_type_t parse_msg_type_str(const char *s)
{
    if (strcmp(s, "Mn") == 0) return TRDP_TYPE_Mn;
    if (strcmp(s, "Mr") == 0) return TRDP_TYPE_Mr;
    if (strcmp(s, "Mp") == 0) return TRDP_TYPE_Mp;
    if (strcmp(s, "Mq") == 0) return TRDP_TYPE_Mq;
    if (strcmp(s, "Mc") == 0) return TRDP_TYPE_Mc;
    if (strcmp(s, "Me") == 0) return TRDP_TYPE_Me;
    return TRDP_TYPE_PD;
}

// Parse a single MD container from JSON
static int parse_md_container(cJSON *json, trdp_md_container_t *container, dir_t dir, trdp_msg_type_t msg_type)
{
    cJSON *name      = cJSON_GetObjectItem(json, "name");
    cJSON *enable_id = cJSON_GetObjectItem(json, "enable_id");
    cJSON *comid     = cJSON_GetObjectItem(json, "comid");
    cJSON *dest_ip   = cJSON_GetObjectItem(json, "dest_ip");
    cJSON *reply_to  = cJSON_GetObjectItem(json, "reply_to");
    cJSON *variables = cJSON_GetObjectItem(json, "variables");

    if (!name || !cJSON_IsString(name))
    {
        log_debug("trdp md: container missing name");
        return -1;
    }
    if (!enable_id || !cJSON_IsString(enable_id))
    {
        log_debug("trdp md: container '%s' missing enable_id", name->valuestring);
        return -1;
    }

    memset(container, 0, sizeof(*container));
    strncpy(container->name,      name->valuestring,      TRDP_MAX_NAME_LEN - 1);
    strncpy(container->enable_id, enable_id->valuestring, TRDP_MAX_NAME_LEN - 1);
    container->msg_type = msg_type;
    container->comid = comid && cJSON_IsNumber(comid) ? (uint32_t)comid->valuedouble : 0;

    if (dest_ip && cJSON_IsString(dest_ip))
        strncpy(container->dest_ip, dest_ip->valuestring, 15);

    if (reply_to && cJSON_IsString(reply_to))
        strncpy(container->pair_name, reply_to->valuestring, TRDP_MAX_NAME_LEN - 1);

    container->prev_enabled = 0;
    container->partner = NULL;
    container->listen_handle = NULL;

    // Calculate size_bytes from variables: max(offset_bits + type_size_bits) / 8
    uint32_t max_end_bits = 0;
    container->var_count = 0;
    container->variables = NULL;
    container->size_bytes = 0;

    if (variables && cJSON_IsArray(variables))
    {
        int num_vars = cJSON_GetArraySize(variables);
        if (num_vars > 0)
        {
            container->variables = calloc((size_t)num_vars, sizeof(trdp_var_mapping_t));
            if (!container->variables)
            {
                log_debug("trdp md: failed to allocate variables for %s", container->name);
                return -1;
            }

            cJSON *var;
            cJSON_ArrayForEach(var, variables)
            {
                cJSON *var_id_json = cJSON_GetObjectItem(var, "var_id");
                cJSON *var_offset  = cJSON_GetObjectItem(var, "offset");
                cJSON *var_type    = cJSON_GetObjectItem(var, "type");

                if (!var_id_json || !cJSON_IsString(var_id_json))
                    continue;

                const char *type_str = (var_type && cJSON_IsString(var_type)) ? var_type->valuestring : "uint8";
                uint32_t offset = (var_offset && cJSON_IsNumber(var_offset)) ? (uint32_t)var_offset->valuedouble : 0;

                type_t vtype = var_table_type_from_string(type_str);
                size_t vbits = type_size_bits(vtype);
                uint32_t end_bits = offset + (uint32_t)vbits;
                if (end_bits > max_end_bits)
                    max_end_bits = end_bits;

                trdp_var_mapping_t *mapping = &container->variables[container->var_count];
                strncpy(mapping->var_id, var_id_json->valuestring, TRDP_MAX_NAME_LEN - 1);
                mapping->offset_bits = offset;
                mapping->type = vtype;
                mapping->bit_index = -1;
                mapping->bitset_bytes = 0;

                var_table_add(mapping->var_id, vtype, dir);
                log_debug("trdp md: added variable %s (offset=%u, type=%s)", mapping->var_id, offset, type_str);
                container->var_count++;
            }
        }
    }

    container->size_bytes = (max_end_bits + 7) / 8;
    if (container->size_bytes == 0)
        container->size_bytes = 1;

    container->buffer = calloc(1, container->size_bytes);
    if (!container->buffer)
    {
        log_debug("trdp md: failed to allocate buffer for %s", container->name);
        free(container->variables);
        container->variables = NULL;
        return -1;
    }

    // DIR_INPUT = ammio sends (Mn/Mr trigger): start disabled
    // DIR_OUTPUT = ammio receives: start enabled so var_table is always updated
    register_enable(container->enable_id, dir == DIR_INPUT ? 0 : 1);
    return 0;
}

// Parse MD containers from JSON array (skip PD containers)
static int parse_md_containers(cJSON *json_array, trdp_md_container_t *containers, size_t *count, size_t max, dir_t dir)
{
    if (!json_array || !cJSON_IsArray(json_array))
        return 0;

    cJSON *item;
    cJSON_ArrayForEach(item, json_array)
    {
        if (*count >= max)
            break;

        cJSON *type_field = cJSON_GetObjectItem(item, "type");
        if (!type_field || !cJSON_IsString(type_field))
            continue;

        const char *type_str = type_field->valuestring;
        if (strcmp(type_str, "Pd") == 0)
            continue;

        trdp_msg_type_t msg_type = parse_msg_type_str(type_str);
        if (parse_md_container(item, &containers[*count], dir, msg_type) == 0)
        {
            (*count)++;
        }
    }
    return 0;
}

// Link Mp<->Mr partners after parsing
static void link_md_partners(void)
{
    // outputs.Mp → inputs.Mr (ammio is Caller: sends Mr, receives Mp)
    for (size_t i = 0; i < md_output_count; i++)
    {
        trdp_md_container_t *mp = &md_output_containers[i];
        if (mp->msg_type != TRDP_TYPE_Mp || mp->pair_name[0] == '\0')
            continue;

        for (size_t j = 0; j < md_input_count; j++)
        {
            trdp_md_container_t *mr = &md_input_containers[j];
            if (mr->msg_type == TRDP_TYPE_Mr && strcmp(mr->name, mp->pair_name) == 0)
            {
                mp->partner = mr;
                mr->partner = mp;
                log_debug("trdp md: linked Mr '%s' <-> Mp '%s'", mr->name, mp->name);
                break;
            }
        }
    }

    // inputs.Mp → outputs.Mr (SUT is Caller: sends Mr, ammio replies with Mp)
    for (size_t i = 0; i < md_input_count; i++)
    {
        trdp_md_container_t *mp = &md_input_containers[i];
        if (mp->msg_type != TRDP_TYPE_Mp || mp->pair_name[0] == '\0')
            continue;

        for (size_t j = 0; j < md_output_count; j++)
        {
            trdp_md_container_t *mr = &md_output_containers[j];
            if (mr->msg_type == TRDP_TYPE_Mr && strcmp(mr->name, mp->pair_name) == 0)
            {
                mp->partner = mr;
                mr->partner = mp;
                log_debug("trdp md: linked output Mr '%s' <-> input Mp '%s'", mr->name, mp->name);
                break;
            }
        }
    }
}

// MD message callback — called by TRDP library for received MD messages
static void md_callback(
    void *pRefCon,
    TRDP_APP_SESSION_T appHandle,
    const TRDP_MD_INFO_T *pMsg,
    UINT8 *pData,
    UINT32 dataSize)
{
    (void)appHandle;

    if (!pMsg || !pMsg->pUserRef)
        return;

    log_debug("trdp md: callback fired msgType=0x%04x comid=%u resultCode=%d userRef=%p",
              pMsg->msgType, pMsg->comId, pMsg->resultCode, pMsg->pUserRef);

    if (pMsg->resultCode != TRDP_NO_ERR)
    {
        log_debug("trdp md: callback error %d for comid=%u", pMsg->resultCode, pMsg->comId);
        return;
    }

    trdp_md_container_t *container = (trdp_md_container_t *)pMsg->pUserRef;

    if (pMsg->msgType == TRDP_MSG_MN || pMsg->msgType == TRDP_MSG_MP)
    {
        if (!get_enable(container->enable_id))
            return;

        // Received notification or reply: copy data into buffer and update var_table
        if (pData && dataSize > 0 && container->buffer)
        {
            uint32_t copy_size = dataSize < container->size_bytes ? dataSize : container->size_bytes;
            memcpy(container->buffer, pData, copy_size);
        }
        md_set_to_var_table(container);
        log_debug("trdp md: received %s for '%s' (comid=%u)",
                  pMsg->msgType == TRDP_MSG_MN ? "Mn" : "Mp", container->name, pMsg->comId);
    }
    else if (pMsg->msgType == TRDP_MSG_MR)
    {
        // Received request from SUT: read reply data and send Mp back
        trdp_md_container_t *reply_container = container->partner;
        if (!reply_container)
        {
            log_debug("trdp md: received Mr for '%s' but no Mp partner configured", container->name);
            return;
        }

        md_get_from_var_table(reply_container);

        TRDP_ERR_T err = tlm_reply(
            app_handle,
            &pMsg->sessionId,
            reply_container->comid,
            0,
            NULL,
            reply_container->buffer,
            reply_container->size_bytes,
            NULL);

        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp md: tlm_reply failed for '%s': %d", reply_container->name, err);
        }
        else
        {
            log_debug("trdp md: replied Mp '%s' to Mr '%s'", reply_container->name, container->name);
        }
    }
}

static TRDP_ERR_T send_md_notification(trdp_md_container_t *container, TRDP_IP_ADDR_T dest)
{
    return tlm_notify(
        app_handle,
        NULL,
        NULL,
        container->comid,
        0,
        0,
        0,
        dest,
        TRDP_FLAGS_DEFAULT,
        NULL,
        container->buffer,
        container->size_bytes,
        NULL,
        NULL);
}

static TRDP_ERR_T send_md_request(trdp_md_container_t *container, TRDP_IP_ADDR_T dest)
{
    trdp_md_container_t *reply_container = container->partner;
    TRDP_UUID_T session_id;

    if (!reply_container)
    {
        log_debug("trdp md: Mr '%s' has no Mp partner, skipping", container->name);
        return TRDP_NO_ERR;
    }

    return tlm_request(
        app_handle,
        reply_container,
        md_callback,
        &session_id,
        container->comid,
        0,
        0,
        0,
        dest,
        TRDP_FLAGS_DEFAULT,
        1,
        3000000,
        NULL,
        container->buffer,
        container->size_bytes,
        NULL,
        NULL);
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
        // Send PD: get values from var_table and publish to protocol bus
        for (size_t i = 0; i < input_count; i++)
        {
            trdp_container_t *container = &input_containers[i];
            if (container->handle && get_enable(container->enable_id))
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

        // Send MD: fire on rising edge of enable_id flag, then auto-reset
        for (size_t i = 0; i < md_input_count; i++)
        {
            trdp_md_container_t *container = &md_input_containers[i];

            uint8_t current = get_enable(container->enable_id);
            uint8_t prev    = container->prev_enabled;
            container->prev_enabled = current;

            if (!prev && current)
                set_enable(container->enable_id, 0);  // auto-reset before sending
            else
                continue;

            if (container->dest_ip[0] == '\0')
            {
                log_debug("trdp md: no dest_ip for '%s', skipping send", container->name);
                continue;
            }

            md_get_from_var_table(container);
            TRDP_IP_ADDR_T dest = parse_ip(container->dest_ip);

            if (container->msg_type == TRDP_TYPE_Mn)
            {
                err = send_md_notification(container, dest);
                if (err != TRDP_NO_ERR)
                    log_debug("trdp md: tlm_notify failed for '%s': %d", container->name, err);
                else
                    log_debug("trdp md: sent Mn '%s' (comid=%u)", container->name, container->comid);
            }
            else if (container->msg_type == TRDP_TYPE_Mr)
            {
                err = send_md_request(container, dest);
                if (err != TRDP_NO_ERR)
                    log_debug("trdp md: tlm_request failed for '%s': %d", container->name, err);
                else if (container->partner)
                    log_debug("trdp md: sent Mr '%s' (comid=%u)", container->name, container->comid);
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

        // Receive PD: get values from protocol bus and set into var_table
        for (size_t i = 0; i < output_count; i++)
        {
            trdp_container_t *container = &output_containers[i];
            if (container->handle && get_enable(container->enable_id))
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

    // Parse source_port (optional — 0 means OS-assigned)
    cJSON *source_port_json = cJSON_GetObjectItem(config, "source_port");
    if (source_port_json && cJSON_IsNumber(source_port_json))
    {
        source_port = (UINT16)source_port_json->valueint;
    }
    log_debug("trdp: source_port = %u", (unsigned)source_port);

    // Parse containers
    cJSON *containers = cJSON_GetObjectItem(config, "containers");
    if (!containers)
    {
        log_debug("trdp: no containers defined");
        return 0;
    }

    // Parse PD inputs (ammio publishes to SUT)
    cJSON *inputs = cJSON_GetObjectItem(containers, "inputs");
    if (inputs)
    {
        parse_containers(inputs, input_containers, &input_count, MAX_CONTAINERS, DIR_INPUT);
        log_debug("trdp: parsed %zu PD input containers", input_count);
    }

    // Parse PD outputs (ammio subscribes from SUT)
    cJSON *outputs = cJSON_GetObjectItem(containers, "outputs");
    if (outputs)
    {
        parse_containers(outputs, output_containers, &output_count, MAX_CONTAINERS, DIR_OUTPUT);
        log_debug("trdp: parsed %zu PD output containers", output_count);
    }

    // Parse MD input containers (ammio sends MD to SUT)
    if (inputs)
    {
        parse_md_containers(inputs, md_input_containers, &md_input_count, MAX_CONTAINERS, DIR_INPUT);
        log_debug("trdp: parsed %zu MD input containers", md_input_count);
    }

    // Parse MD output containers (SUT sends MD to ammio)
    if (outputs)
    {
        parse_md_containers(outputs, md_output_containers, &md_output_count, MAX_CONTAINERS, DIR_OUTPUT);
        log_debug("trdp: parsed %zu MD output containers", md_output_count);
    }

    // Link Mr<->Mp partners
    link_md_partners();

    return 0;
}

/* Pre-bind a send socket to a fixed source port.
 * Must be called BEFORE any tlp_publish / tlm_addListener so that trdp_requestSocket
 * finds this socket and reuses it instead of creating a new one with port 0.
 *
 * Strategy: call trdp_requestSocket to let TCNopen create and register the socket
 * the normal way (so the iface table and high-water mark are updated correctly).
 * Then close the OS socket it created (which was bound to port 0) and replace it
 * with a new socket bound to src_port. On Windows, sockets can't be rebound, so
 * the replacement is necessary. */
static void prebind_send_socket(TRDP_SOCKETS_T iface[],
                                TRDP_SOCK_TYPE_T type,
                                UINT16 trdp_port,
                                const TRDP_SEND_PARAM_T *sendParam,
                                TRDP_IP_ADDR_T bind_addr,
                                TRDP_OPTION_T options,
                                UINT16 src_port)
{
    INT32 sock_idx = TRDP_INVALID_SOCKET_INDEX;
    TRDP_ERR_T err = trdp_requestSocket(iface, trdp_port, sendParam,
                                        bind_addr, 0u, type, options,
                                        FALSE, VOS_INVALID_SOCKET,
                                        &sock_idx, 0u);
    if (err != TRDP_NO_ERR || sock_idx == TRDP_INVALID_SOCKET_INDEX)
    {
        log_debug("trdp: prebind: trdp_requestSocket failed (%d)", (int)err);
        return;
    }

    /* Close the socket TCNopen just created (bound to port 0) */
    vos_sockClose(iface[sock_idx].sock);
    iface[sock_idx].sock = VOS_INVALID_SOCKET;

    /* Open a new socket with the same options */
    VOS_SOCK_OPT_T opts;
    memset(&opts, 0, sizeof(opts));
    opts.qos           = sendParam->qos;
    opts.ttl           = sendParam->ttl;
    opts.ttl_multicast = sendParam->ttl;
    opts.reuseAddrPort = (options & TRDP_OPTION_NO_REUSE_ADDR) ? FALSE : TRUE;
    opts.nonBlocking   = (type == TRDP_SOCK_MD_UDP) ? TRUE
                         : ((options & TRDP_OPTION_BLOCK) ? FALSE : TRUE);

    VOS_SOCK_T new_sock;
    if (vos_sockOpenUDP(&new_sock, &opts) != VOS_NO_ERR)
    {
        log_debug("trdp: prebind: vos_sockOpenUDP failed");
        return;
    }

    if (vos_sockBind(new_sock, bind_addr, src_port) != VOS_NO_ERR)
    {
        log_debug("trdp: prebind: vos_sockBind to port %u failed", (unsigned)src_port);
        vos_sockClose(new_sock);
        return;
    }

    if (bind_addr != 0u)
    {
        (void)vos_sockSetMulticastIf(new_sock, bind_addr);
    }

    /* Install the pre-bound socket into the iface table slot TCNopen reserved */
    iface[sock_idx].sock = new_sock;

    log_debug("trdp: pre-bound %s send socket (slot %d) to port %u",
              (type == TRDP_SOCK_PD) ? "PD" : "MD", (int)sock_idx, (unsigned)src_port);
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

    // Pre-bind send sockets to a fixed source port before any pub/sub is created.
    // trdp_requestSocket reuses a pre-existing socket when parameters match,
    // so this guarantees the desired source port on all outgoing packets.
    if (source_port != 0u)
    {
        TRDP_SESSION_PT s = (TRDP_SESSION_PT)app_handle;
        prebind_send_socket(s->ifacePD, TRDP_SOCK_PD,
                            s->pdDefault.port, &s->pdDefault.sendParam,
                            own_ip, s->option, source_port);
#if MD_SUPPORT
        prebind_send_socket(s->ifaceMD, TRDP_SOCK_MD_UDP,
                            s->mdDefault.udpPort, &s->mdDefault.sendParam,
                            own_ip, s->option, source_port);
#endif
    }

    // Create publishers for PD input containers
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

    // Create subscribers for PD output containers
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

    // Register MD listeners for output containers that receive Mn or Mr from SUT
    for (size_t i = 0; i < md_output_count; i++)
    {
        trdp_md_container_t *container = &md_output_containers[i];

        if (container->msg_type != TRDP_TYPE_Mn && container->msg_type != TRDP_TYPE_Mr)
            continue;

        TRDP_LIS_T lis_handle;
        err = tlm_addListener(
            app_handle,
            &lis_handle,
            container,
            md_callback,
            TRUE,
            container->comid,
            0, 0, 0, 0, 0,
            TRDP_FLAGS_DEFAULT,
            NULL,
            NULL);

        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp md: tlm_addListener failed for '%s' (comid=%u): %d",
                      container->name, container->comid, err);
        }
        else
        {
            container->listen_handle = lis_handle;
            log_debug("trdp md: listening for %s '%s' (comid=%u)",
                      container->msg_type == TRDP_TYPE_Mn ? "Mn" : "Mr",
                      container->name, container->comid);
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

    // Remove MD listeners for output containers
    for (size_t i = 0; i < md_output_count; i++)
    {
        if (md_output_containers[i].listen_handle)
        {
            tlm_delListener(app_handle, (TRDP_LIS_T)md_output_containers[i].listen_handle);
            md_output_containers[i].listen_handle = NULL;
        }
    }

    // Unpublish all PD input containers
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

    // Unsubscribe all PD output containers
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

    // Free MD input containers
    for (size_t i = 0; i < md_input_count; i++)
    {
        free(md_input_containers[i].buffer);
        md_input_containers[i].buffer = NULL;
        free(md_input_containers[i].variables);
        md_input_containers[i].variables = NULL;
    }
    md_input_count = 0;

    // Free MD output containers
    for (size_t i = 0; i < md_output_count; i++)
    {
        free(md_output_containers[i].buffer);
        md_output_containers[i].buffer = NULL;
        free(md_output_containers[i].variables);
        md_output_containers[i].variables = NULL;
    }
    md_output_count = 0;

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
    .stop = trdp_stop,
};

void trdp_iface_register(void)
{
    interface_register(&trdp_interface);
}
