#include "trdp_iface.h"
#include "../var_table.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include "../compat/net.h"

#include "trdp_if_light.h"
#include "vos_mem.h"
#include "vos_utils.h"
#include "vos_thread.h"
#include "iec61375-2-3.h"

#define MAX_CONTAINERS 64
#define MAX_STRUCT_FIELDS 16
#define MAX_STRUCTS 8

typedef enum {
    TRDP_TYPE_PD,
    TRDP_TYPE_Mn,
    TRDP_TYPE_Mr,
    TRDP_TYPE_Mp,
    TRDP_TYPE_Mq,
    TRDP_TYPE_Mc,
    TRDP_TYPE_Me
} trdp_msg_type_t;

typedef enum {
    MD_FIELD_SCALAR,
    MD_FIELD_ARRAY,
    MD_FIELD_PADDING
} md_field_kind_t;

typedef struct {
    char var_id[TRDP_MAX_NAME_LEN];
    type_t type;
    uint32_t pad_bits;
} md_struct_field_t;

typedef struct {
    md_field_kind_t kind;
    char var_id[TRDP_MAX_NAME_LEN];
    type_t type;
    uint32_t pad_bits;
    char iterations_id[TRDP_MAX_NAME_LEN];
    type_t iterations_type;
    uint32_t max_iterations;
    md_struct_field_t *entry_fields;
    size_t entry_field_count;
    uint32_t entry_size_bits;
} md_field_t;

typedef struct trdp_md_container_s {
    char var_id[TRDP_MAX_NAME_LEN];
    char enable_id[TRDP_MAX_NAME_LEN];
    trdp_msg_type_t msg_type;
    uint32_t comid;
    char pair_var_id[TRDP_MAX_NAME_LEN];
    char dest_ip[16];
    TRDP_IP_ADDR_T dest_ip_addr;
    uint32_t max_size_bytes;
    uint8_t *buffer;
    md_field_t *fields;
    size_t field_count;
    void *listen_handle;
    uint8_t prev_enabled;
    struct trdp_md_container_s *partner;
} trdp_md_container_t;

static char local_ip[16] = "0.0.0.0";
static TRDP_APP_SESSION_T app_handle = NULL;

static trdp_container_t input_containers[MAX_CONTAINERS];
static size_t input_count = 0;

static trdp_container_t output_containers[MAX_CONTAINERS];
static size_t output_count = 0;

static trdp_md_container_t md_input_containers[MAX_CONTAINERS];
static size_t md_input_count = 0;

static trdp_md_container_t md_output_containers[MAX_CONTAINERS];
static size_t md_output_count = 0;

static char etb_topo_cnt_id[256] = "";
static char op_trn_topo_cnt_id[256] = "";
static uint32_t cached_etb_topo_cnt = 0;
static uint32_t cached_op_trn_topo_cnt = 0;

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

static TRDP_IP_ADDR_T parse_ip(const char *ip_str)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) == 1)
    {
        return ntohl(addr.s_addr);
    }
    return 0;
}

static cJSON *get_trdp_var_id(cJSON *json)
{
    cJSON *var_id = cJSON_GetObjectItem(json, "var_id");
    if (var_id && cJSON_IsString(var_id))
        return var_id;

    return NULL;
}

// Float64 big-endian byte swap (TRDP wire format)
static inline void swap64(const void *src, void *dst)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (int j = 0; j < 8; j++) d[j] = s[7 - j];
}

static void marshal_scalar_to_buffer(uint8_t *dest, const var_t *var)
{
    switch (var->type)
    {
        case TYPE_UINT8:
            *dest = var->value.u8;
            break;
        case TYPE_INT8:
            *(int8_t *)dest = var->value.i8;
            break;
        case TYPE_UINT16:
        {
            uint16_t val = htons(var->value.u16);
            memcpy(dest, &val, sizeof(val));
            break;
        }
        case TYPE_INT16:
        {
            int16_t val = (int16_t)htons((uint16_t)var->value.i16);
            memcpy(dest, &val, sizeof(val));
            break;
        }
        case TYPE_UINT32:
        {
            uint32_t val = htonl(var->value.u32);
            memcpy(dest, &val, sizeof(val));
            break;
        }
        case TYPE_INT32:
        {
            int32_t val = (int32_t)htonl((uint32_t)var->value.i32);
            memcpy(dest, &val, sizeof(val));
            break;
        }
        case TYPE_FLOAT32:
        {
            uint32_t tmp;
            memcpy(&tmp, &var->value.f32, sizeof(tmp));
            tmp = htonl(tmp);
            memcpy(dest, &tmp, sizeof(tmp));
            break;
        }
        case TYPE_FLOAT64:
            swap64(&var->value.f64, dest);
            break;
    }
}

static void unmarshal_scalar_from_buffer(var_t *var, const uint8_t *src)
{
    switch (var->type)
    {
        case TYPE_UINT8:
            var->value.u8 = *src;
            break;
        case TYPE_INT8:
            var->value.i8 = *(int8_t *)src;
            break;
        case TYPE_UINT16:
        {
            uint16_t val;
            memcpy(&val, src, sizeof(val));
            var->value.u16 = ntohs(val);
            break;
        }
        case TYPE_INT16:
        {
            int16_t val;
            memcpy(&val, src, sizeof(val));
            var->value.i16 = (int16_t)ntohs((uint16_t)val);
            break;
        }
        case TYPE_UINT32:
        {
            uint32_t val;
            memcpy(&val, src, sizeof(val));
            var->value.u32 = ntohl(val);
            break;
        }
        case TYPE_INT32:
        {
            int32_t val;
            memcpy(&val, src, sizeof(val));
            var->value.i32 = (int32_t)ntohl((uint32_t)val);
            break;
        }
        case TYPE_FLOAT32:
        {
            uint32_t tmp;
            memcpy(&tmp, src, sizeof(tmp));
            tmp = ntohl(tmp);
            memcpy(&var->value.f32, &tmp, sizeof(var->value.f32));
            break;
        }
        case TYPE_FLOAT64:
            swap64(src, &var->value.f64);
            break;
    }
}

static void get_from_var_table(trdp_container_t *container)
{
    for (size_t i = 0; i < container->var_count; i++)
    {
        trdp_var_mapping_t *mapping = &container->variables[i];
        uint32_t byte_offset = mapping->offset_bits / 8;

        if (byte_offset >= container->size_bytes)
            continue;

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

        marshal_scalar_to_buffer(container->buffer + byte_offset, &var);
    }
}

static void set_to_var_table(trdp_container_t *container)
{
    for (size_t i = 0; i < container->var_count; i++)
    {
        trdp_var_mapping_t *mapping = &container->variables[i];
        uint32_t byte_offset = mapping->offset_bits / 8;

        if (byte_offset >= container->size_bytes)
            continue;

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

        var_t var;
        var.type = mapping->type;
        unmarshal_scalar_from_buffer(&var, container->buffer + byte_offset);
        var_table_set(mapping->var_id, &var);
    }
}

static uint32_t md_serialize_to_buffer(trdp_md_container_t *container)
{
    uint8_t *buf = container->buffer;
    uint32_t offset = 0;

    memset(buf, 0, container->max_size_bytes);

    for (size_t i = 0; i < container->field_count; i++)
    {
        md_field_t *f = &container->fields[i];

        if (f->kind == MD_FIELD_PADDING)
        {
            offset += f->pad_bits / 8;
            continue;
        }

        if (f->kind == MD_FIELD_SCALAR)
        {
            var_t var;
            char full_id[512];
            snprintf(full_id, sizeof(full_id), "%s.%s", container->var_id, f->var_id);
            if (var_table_get(full_id, &var) == 0)
                marshal_scalar_to_buffer(buf + offset, &var);
            offset += (uint32_t)type_size_bits(f->type) / 8;
            continue;
        }

        // MD_FIELD_ARRAY
        char count_id[512];
        snprintf(count_id, sizeof(count_id), "%s.%s", container->var_id, f->iterations_id);
        var_t count_var;
        uint32_t count = 0;
        if (var_table_get(count_id, &count_var) == 0)
        {
            switch (f->iterations_type)
            {
                case TYPE_UINT8:  count = count_var.value.u8;  break;
                case TYPE_UINT16: count = count_var.value.u16; break;
                case TYPE_UINT32: count = count_var.value.u32; break;
                default:          count = count_var.value.u8;  break;
            }
        }
        if (count > f->max_iterations)
            count = f->max_iterations;

        var_t cv;
        cv.type = f->iterations_type;
        switch (f->iterations_type)
        {
            case TYPE_UINT8:  cv.value.u8  = (uint8_t)count;  break;
            case TYPE_UINT16: cv.value.u16 = (uint16_t)count; break;
            case TYPE_UINT32: cv.value.u32 = count;            break;
            default:          cv.value.u8  = (uint8_t)count;   break;
        }
        marshal_scalar_to_buffer(buf + offset, &cv);
        offset += (uint32_t)type_size_bits(f->iterations_type) / 8;

        for (uint32_t idx = 0; idx < count; idx++)
        {
            for (size_t fi = 0; fi < f->entry_field_count; fi++)
            {
                md_struct_field_t *sf = &f->entry_fields[fi];
                if (sf->var_id[0] == '\0')
                {
                    offset += sf->pad_bits / 8;
                    continue;
                }
                char entry_id[512];
                snprintf(entry_id, sizeof(entry_id), "%s.%s[%u].%s",
                         container->var_id, f->var_id, idx, sf->var_id);
                var_t var;
                if (var_table_get(entry_id, &var) == 0)
                    marshal_scalar_to_buffer(buf + offset, &var);
                offset += (uint32_t)type_size_bits(sf->type) / 8;
            }
        }
    }

    return offset;
}

static void md_deserialize_from_buffer(trdp_md_container_t *container, uint32_t data_size)
{
    const uint8_t *buf = container->buffer;
    uint32_t offset = 0;

    for (size_t i = 0; i < container->field_count; i++)
    {
        md_field_t *f = &container->fields[i];

        if (offset >= data_size)
            break;

        if (f->kind == MD_FIELD_PADDING)
        {
            offset += f->pad_bits / 8;
            continue;
        }

        if (f->kind == MD_FIELD_SCALAR)
        {
            char full_id[512];
            snprintf(full_id, sizeof(full_id), "%s.%s", container->var_id, f->var_id);
            var_t var;
            var.type = f->type;
            unmarshal_scalar_from_buffer(&var, buf + offset);
            var_table_set(full_id, &var);
            offset += (uint32_t)type_size_bits(f->type) / 8;
            continue;
        }

        // MD_FIELD_ARRAY
        var_t count_var;
        count_var.type = f->iterations_type;
        unmarshal_scalar_from_buffer(&count_var, buf + offset);
        offset += (uint32_t)type_size_bits(f->iterations_type) / 8;

        uint32_t count = 0;
        switch (f->iterations_type)
        {
            case TYPE_UINT8:  count = count_var.value.u8;  break;
            case TYPE_UINT16: count = count_var.value.u16; break;
            case TYPE_UINT32: count = count_var.value.u32; break;
            default:          count = count_var.value.u8;  break;
        }
        if (count > f->max_iterations)
            count = f->max_iterations;

        char count_id[512];
        snprintf(count_id, sizeof(count_id), "%s.%s", container->var_id, f->iterations_id);
        var_table_set(count_id, &count_var);

        for (uint32_t idx = 0; idx < count; idx++)
        {
            if (offset >= data_size)
                break;
            for (size_t fi = 0; fi < f->entry_field_count; fi++)
            {
                md_struct_field_t *sf = &f->entry_fields[fi];
                if (sf->var_id[0] == '\0')
                {
                    offset += sf->pad_bits / 8;
                    continue;
                }
                char entry_id[512];
                snprintf(entry_id, sizeof(entry_id), "%s.%s[%u].%s",
                         container->var_id, f->var_id, idx, sf->var_id);
                var_t var;
                var.type = sf->type;
                unmarshal_scalar_from_buffer(&var, buf + offset);
                var_table_set(entry_id, &var);
                offset += (uint32_t)type_size_bits(sf->type) / 8;
            }
        }
    }
}

static int parse_container(cJSON *json, trdp_container_t *container, dir_t dir)
{
    cJSON *var_id_c  = get_trdp_var_id(json);
    cJSON *enable_id = cJSON_GetObjectItem(json, "enable_id");
    cJSON *comid     = cJSON_GetObjectItem(json, "comid");
    cJSON *multicast_ip = cJSON_GetObjectItem(json, "multicast_ip");
    cJSON *period_ms = cJSON_GetObjectItem(json, "period_ms");
    cJSON *size_bits = cJSON_GetObjectItem(json, "size_bits");
    cJSON *variables = cJSON_GetObjectItem(json, "variables");

    if (!var_id_c || !cJSON_IsString(var_id_c))
    {
        log_error("trdp: container missing var_id");
        return -1;
    }
    if (!enable_id || !cJSON_IsString(enable_id))
    {
        log_error("trdp: container '%s' missing enable_id", var_id_c->valuestring);
        return -1;
    }

    strncpy(container->var_id,      var_id_c->valuestring,      TRDP_MAX_NAME_LEN - 1);
    strncpy(container->enable_id, enable_id->valuestring, TRDP_MAX_NAME_LEN - 1);
    container->comid = comid && cJSON_IsNumber(comid) ? (uint32_t)comid->valuedouble : 0;

    if (multicast_ip && cJSON_IsString(multicast_ip))
    {
        strncpy(container->multicast_ip, multicast_ip->valuestring, 15);
    }

    container->period_ms = period_ms && cJSON_IsNumber(period_ms) ? (uint32_t)period_ms->valuedouble : 100;
    container->size_bits = size_bits && cJSON_IsNumber(size_bits) ? (uint32_t)size_bits->valuedouble : 0;
    container->size_bytes = (container->size_bits + 7) / 8;

    container->buffer = calloc(1, container->size_bytes);
    if (!container->buffer)
    {
        log_error("trdp: failed to allocate buffer for container %s", container->var_id);
        return -1;
    }

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
                log_error("trdp: failed to allocate variables for container %s", container->var_id);
                free(container->buffer);
                return -1;
            }
        }

        cJSON *var;
        cJSON_ArrayForEach(var, variables)
        {
            cJSON *var_id_json = get_trdp_var_id(var);
            cJSON *var_offset = cJSON_GetObjectItem(var, "offset");
            cJSON *var_type = cJSON_GetObjectItem(var, "type");
            cJSON *var_bits = cJSON_GetObjectItem(var, "bits");

            if (!var_id_json || !cJSON_IsString(var_id_json))
            {
                log_error("trdp: variable in container '%s' missing var_id", container->var_id);
                continue;
            }

            const char *type_str = (var_type && cJSON_IsString(var_type)) ? var_type->valuestring : "uint8";
            uint32_t offset = (var_offset && cJSON_IsNumber(var_offset)) ? (uint32_t)var_offset->valuedouble : 0;

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

static uint32_t calc_struct_entry_bits(md_struct_field_t *fields, size_t count)
{
    uint32_t bits = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (fields[i].var_id[0] == '\0')
            bits += fields[i].pad_bits;
        else
            bits += (uint32_t)type_size_bits(fields[i].type);
    }
    return bits;
}

static int parse_md_container(cJSON *json, trdp_md_container_t *container, dir_t dir, trdp_msg_type_t msg_type)
{
    cJSON *var_id_c  = get_trdp_var_id(json);
    cJSON *enable_id = cJSON_GetObjectItem(json, "enable_id");
    cJSON *comid     = cJSON_GetObjectItem(json, "comid");
    cJSON *dest_ip   = cJSON_GetObjectItem(json, "dest_ip");
    cJSON *reply_to  = cJSON_GetObjectItem(json, "reply_to");
    cJSON *structs   = cJSON_GetObjectItem(json, "structs");
    cJSON *variables = cJSON_GetObjectItem(json, "variables");

    if (!var_id_c || !cJSON_IsString(var_id_c))
    {
        log_error("trdp md: container missing var_id");
        return -1;
    }
    if (!enable_id || !cJSON_IsString(enable_id))
    {
        log_error("trdp md: container '%s' missing enable_id", var_id_c->valuestring);
        return -1;
    }

    memset(container, 0, sizeof(*container));
    strncpy(container->var_id,      var_id_c->valuestring,      TRDP_MAX_NAME_LEN - 1);
    strncpy(container->enable_id, enable_id->valuestring, TRDP_MAX_NAME_LEN - 1);
    container->msg_type = msg_type;
    container->comid = comid && cJSON_IsNumber(comid) ? (uint32_t)comid->valuedouble : 0;

    if (dest_ip && cJSON_IsString(dest_ip))
    {
        strncpy(container->dest_ip, dest_ip->valuestring, 15);
        container->dest_ip_addr = parse_ip(dest_ip->valuestring);
    }

    if (reply_to && cJSON_IsString(reply_to))
        strncpy(container->pair_var_id, reply_to->valuestring, TRDP_MAX_NAME_LEN - 1);

    container->prev_enabled = 0;
    container->partner = NULL;
    container->listen_handle = NULL;

    // Parse struct definitions
    char struct_names[MAX_STRUCTS][TRDP_MAX_NAME_LEN];
    md_struct_field_t struct_fields[MAX_STRUCTS][MAX_STRUCT_FIELDS];
    size_t struct_field_counts[MAX_STRUCTS];
    size_t struct_count = 0;

    if (structs && cJSON_IsObject(structs))
    {
        cJSON *s;
        cJSON_ArrayForEach(s, structs)
        {
            if (struct_count >= MAX_STRUCTS || !cJSON_IsArray(s))
                continue;
            strncpy(struct_names[struct_count], s->string, TRDP_MAX_NAME_LEN - 1);
            struct_field_counts[struct_count] = 0;

            cJSON *sf;
            cJSON_ArrayForEach(sf, s)
            {
                if (struct_field_counts[struct_count] >= MAX_STRUCT_FIELDS)
                    break;
                md_struct_field_t *field = &struct_fields[struct_count][struct_field_counts[struct_count]];
                memset(field, 0, sizeof(*field));

                cJSON *sf_type = cJSON_GetObjectItem(sf, "type");
                const char *sf_type_str = (sf_type && cJSON_IsString(sf_type)) ? sf_type->valuestring : "uint8";

                if (strcmp(sf_type_str, "padding") == 0)
                {
                    cJSON *bits = cJSON_GetObjectItem(sf, "bits");
                    field->pad_bits = (bits && cJSON_IsNumber(bits)) ? (uint32_t)bits->valuedouble : 0;
                }
                else
                {
                    cJSON *sf_var_id = cJSON_GetObjectItem(sf, "var_id");
                    if (sf_var_id && cJSON_IsString(sf_var_id))
                        strncpy(field->var_id, sf_var_id->valuestring, TRDP_MAX_NAME_LEN - 1);
                    field->type = var_table_type_from_string(sf_type_str);
                }
                struct_field_counts[struct_count]++;
            }
            struct_count++;
        }
    }

    // Parse variables into md_field_t array
    container->field_count = 0;
    container->fields = NULL;

    if (variables && cJSON_IsArray(variables))
    {
        int num_vars = cJSON_GetArraySize(variables);
        if (num_vars > 0)
        {
            container->fields = calloc((size_t)num_vars, sizeof(md_field_t));
            if (!container->fields)
            {
                log_error("trdp md: failed to allocate fields for %s", container->var_id);
                return -1;
            }

            cJSON *var;
            cJSON_ArrayForEach(var, variables)
            {
                md_field_t *f = &container->fields[container->field_count];
                memset(f, 0, sizeof(*f));

                cJSON *var_type = cJSON_GetObjectItem(var, "type");
                const char *type_str = (var_type && cJSON_IsString(var_type)) ? var_type->valuestring : "uint8";

                if (strcmp(type_str, "padding") == 0)
                {
                    f->kind = MD_FIELD_PADDING;
                    cJSON *bits = cJSON_GetObjectItem(var, "bits");
                    f->pad_bits = (bits && cJSON_IsNumber(bits)) ? (uint32_t)bits->valuedouble : 0;
                    container->field_count++;
                    continue;
                }

                cJSON *var_id_json = get_trdp_var_id(var);
                if (!var_id_json || !cJSON_IsString(var_id_json))
                {
                    log_error("trdp md: variable in container '%s' missing var_id", container->var_id);
                    continue;
                }

                strncpy(f->var_id, var_id_json->valuestring, TRDP_MAX_NAME_LEN - 1);

                // Check if type matches a struct name → ARRAY
                size_t si;
                for (si = 0; si < struct_count; si++)
                {
                    if (strcmp(type_str, struct_names[si]) == 0)
                        break;
                }

                if (si < struct_count)
                {
                    f->kind = MD_FIELD_ARRAY;

                    cJSON *iter_id   = cJSON_GetObjectItem(var, "iterations_id");
                    cJSON *max_iter  = cJSON_GetObjectItem(var, "max_iterations");
                    cJSON *iter_type = cJSON_GetObjectItem(var, "iterations_type");

                    if (iter_id && cJSON_IsString(iter_id))
                        strncpy(f->iterations_id, iter_id->valuestring, TRDP_MAX_NAME_LEN - 1);

                    f->max_iterations = (max_iter && cJSON_IsNumber(max_iter)) ? (uint32_t)max_iter->valuedouble : 1;
                    f->iterations_type = (iter_type && cJSON_IsString(iter_type))
                        ? var_table_type_from_string(iter_type->valuestring)
                        : TYPE_UINT8;

                    size_t fc = struct_field_counts[si];
                    f->entry_fields = calloc(fc, sizeof(md_struct_field_t));
                    if (!f->entry_fields)
                    {
                        log_error("trdp md: failed to allocate entry fields for %s", f->var_id);
                        continue;
                    }
                    memcpy(f->entry_fields, struct_fields[si], fc * sizeof(md_struct_field_t));
                    f->entry_field_count = fc;
                    f->entry_size_bits = calc_struct_entry_bits(f->entry_fields, fc);

                    // Register iterations count variable
                    char full_id[512];
                    snprintf(full_id, sizeof(full_id), "%s.%s", container->var_id, f->iterations_id);
                    var_table_add(full_id, f->iterations_type, dir);
                    log_debug("trdp md: added iterations var %s (type=%s)",
                              full_id, var_table_type_to_string(f->iterations_type));

                    // Register entry field variables for all possible iterations
                    for (uint32_t idx = 0; idx < f->max_iterations; idx++)
                    {
                        for (size_t fi = 0; fi < fc; fi++)
                        {
                            if (f->entry_fields[fi].var_id[0] == '\0')
                                continue;
                            snprintf(full_id, sizeof(full_id), "%s.%s[%u].%s",
                                     container->var_id, f->var_id, idx, f->entry_fields[fi].var_id);
                            var_table_add(full_id, f->entry_fields[fi].type, dir);
                            log_debug("trdp md: added array var %s", full_id);
                        }
                    }
                }
                else
                {
                    f->kind = MD_FIELD_SCALAR;
                    f->type = var_table_type_from_string(type_str);

                    char full_id[512];
                    snprintf(full_id, sizeof(full_id), "%s.%s", container->var_id, f->var_id);
                    var_table_add(full_id, f->type, dir);
                    log_debug("trdp md: added scalar var %s (type=%s)", full_id, type_str);
                }

                container->field_count++;
            }
        }
    }

    // Calculate max buffer size (all arrays at max_iterations)
    uint32_t max_bits = 0;
    for (size_t i = 0; i < container->field_count; i++)
    {
        md_field_t *f = &container->fields[i];
        if (f->kind == MD_FIELD_PADDING)
            max_bits += f->pad_bits;
        else if (f->kind == MD_FIELD_SCALAR)
            max_bits += (uint32_t)type_size_bits(f->type);
        else
        {
            max_bits += (uint32_t)type_size_bits(f->iterations_type);
            max_bits += f->max_iterations * f->entry_size_bits;
        }
    }

    container->max_size_bytes = (max_bits + 7) / 8;
    if (container->max_size_bytes == 0)
        container->max_size_bytes = 1;

    container->buffer = calloc(1, container->max_size_bytes);
    if (!container->buffer)
    {
        log_error("trdp md: failed to allocate buffer for %s", container->var_id);
        free(container->fields);
        container->fields = NULL;
        return -1;
    }

    register_enable(container->enable_id, dir == DIR_INPUT ? 0 : 1);
    return 0;
}

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

static void link_md_partners(void)
{
    // outputs.Mp → inputs.Mr (ammio is Caller: sends Mr, receives Mp)
    for (size_t i = 0; i < md_output_count; i++)
    {
        trdp_md_container_t *mp = &md_output_containers[i];
        if (mp->msg_type != TRDP_TYPE_Mp || mp->pair_var_id[0] == '\0')
            continue;

        for (size_t j = 0; j < md_input_count; j++)
        {
            trdp_md_container_t *mr = &md_input_containers[j];
            if (mr->msg_type == TRDP_TYPE_Mr && strcmp(mr->var_id, mp->pair_var_id) == 0)
            {
                mp->partner = mr;
                mr->partner = mp;
                log_debug("trdp md: linked Mr '%s' <-> Mp '%s'", mr->var_id, mp->var_id);
                break;
            }
        }
    }

    // inputs.Mp → outputs.Mr (SUT is Caller: sends Mr, ammio replies with Mp)
    for (size_t i = 0; i < md_input_count; i++)
    {
        trdp_md_container_t *mp = &md_input_containers[i];
        if (mp->msg_type != TRDP_TYPE_Mp || mp->pair_var_id[0] == '\0')
            continue;

        for (size_t j = 0; j < md_output_count; j++)
        {
            trdp_md_container_t *mr = &md_output_containers[j];
            if (mr->msg_type == TRDP_TYPE_Mr && strcmp(mr->var_id, mp->pair_var_id) == 0)
            {
                mp->partner = mr;
                mr->partner = mp;
                log_debug("trdp md: linked output Mr '%s' <-> input Mp '%s'", mr->var_id, mp->var_id);
                break;
            }
        }
    }
}

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
            uint32_t copy_size = dataSize < container->max_size_bytes ? dataSize : container->max_size_bytes;
            memcpy(container->buffer, pData, copy_size);
        }
        md_deserialize_from_buffer(container, dataSize);
        log_debug("trdp md: received %s for '%s' (comid=%u)",
                  pMsg->msgType == TRDP_MSG_MN ? "Mn" : "Mp", container->var_id, pMsg->comId);
    }
    else if (pMsg->msgType == TRDP_MSG_MR)
    {
        // Received request from SUT: read reply data and send Mp back
        trdp_md_container_t *reply_container = container->partner;
        if (!reply_container)
        {
            log_debug("trdp md: received Mr for '%s' but no Mp partner configured", container->var_id);
            return;
        }

        uint32_t send_size = md_serialize_to_buffer(reply_container);

        TRDP_ERR_T err = tlm_reply(
            app_handle,
            &pMsg->sessionId,
            reply_container->comid,
            0,
            NULL,
            reply_container->buffer,
            send_size,
            NULL);

        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp md: tlm_reply failed for '%s': %d", reply_container->var_id, err);
        }
        else
        {
            log_debug("trdp md: replied Mp '%s' to Mr '%s'", reply_container->var_id, container->var_id);
        }
    }
}

static TRDP_ERR_T send_md_notification(trdp_md_container_t *container, TRDP_IP_ADDR_T dest)
{
    uint32_t send_size = md_serialize_to_buffer(container);

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
        send_size,
        NULL,
        NULL);
}

static TRDP_ERR_T send_md_request(trdp_md_container_t *container, TRDP_IP_ADDR_T dest)
{
    trdp_md_container_t *reply_container = container->partner;
    TRDP_UUID_T session_id;

    if (!reply_container)
    {
        log_debug("trdp md: Mr '%s' has no Mp partner, skipping", container->var_id);
        return TRDP_NO_ERR;
    }

    uint32_t send_size = md_serialize_to_buffer(container);

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
        send_size,
        NULL,
        NULL);
}

static int thread_process_func(void *arg)
{
    (void)arg;
    TRDP_ERR_T err;
    TRDP_FDS_T rfds;
    TRDP_TIME_T tv;
    INT32 noOfDesc;

    while (running)
    {
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
                    log_debug("trdp: tlp_put failed for %s: %d", container->var_id, err);
                }
            }
        }

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
                log_debug("trdp md: no dest_ip for '%s', skipping send", container->var_id);
                continue;
            }

            if (container->msg_type == TRDP_TYPE_Mn)
            {
                err = send_md_notification(container, container->dest_ip_addr);
                if (err != TRDP_NO_ERR)
                    log_debug("trdp md: tlm_notify failed for '%s': %d", container->var_id, err);
                else
                    log_debug("trdp md: sent Mn '%s' (comid=%u)", container->var_id, container->comid);
            }
            else if (container->msg_type == TRDP_TYPE_Mr)
            {
                err = send_md_request(container, container->dest_ip_addr);
                if (err != TRDP_NO_ERR)
                    log_debug("trdp md: tlm_request failed for '%s': %d", container->var_id, err);
                else if (container->partner)
                    log_debug("trdp md: sent Mr '%s' (comid=%u)", container->var_id, container->comid);
            }
        }

        if (etb_topo_cnt_id[0] != '\0')
        {
            var_t v;
            if (var_table_get(etb_topo_cnt_id, &v) == 0 && v.value.u32 != cached_etb_topo_cnt)
            {
                cached_etb_topo_cnt = v.value.u32;
                tlc_setETBTopoCount(app_handle, cached_etb_topo_cnt);
                log_debug("trdp: etb_topo_cnt updated to %u", cached_etb_topo_cnt);
            }
        }
        if (op_trn_topo_cnt_id[0] != '\0')
        {
            var_t v;
            if (var_table_get(op_trn_topo_cnt_id, &v) == 0 && v.value.u32 != cached_op_trn_topo_cnt)
            {
                cached_op_trn_topo_cnt = v.value.u32;
                tlc_setOpTrainTopoCount(app_handle, cached_op_trn_topo_cnt);
                log_debug("trdp: op_trn_topo_cnt updated to %u", cached_op_trn_topo_cnt);
            }
        }

        FD_ZERO(&rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms
        noOfDesc = 0;

        err = tlc_getInterval(app_handle, &tv, &rfds, &noOfDesc);
        if (err != TRDP_NO_ERR)
        {
            log_debug("trdp: tlc_getInterval failed: %d", err);
        }

        noOfDesc = (INT32)vos_select((VOS_SOCK_T)noOfDesc, &rfds, NULL, NULL, &tv);

        err = tlc_process(app_handle, &rfds, &noOfDesc);
        if (err != TRDP_NO_ERR && err != TRDP_NODATA_ERR)
        {
            log_debug("trdp: tlc_process failed: %d", err);
        }

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
                    log_debug("trdp: tlp_get failed for %s: %d", container->var_id, err);
                }
            }
        }

    }

    return 0;
}

static int trdp_init(cJSON *config)
{
    cJSON *local_ip_json = cJSON_GetObjectItem(config, "local_ip");
    if (local_ip_json && cJSON_IsString(local_ip_json))
    {
        strncpy(local_ip, local_ip_json->valuestring, 15);
    }
    log_debug("trdp: local_ip = %s", local_ip);

    cJSON *etb_topo = cJSON_GetObjectItem(config, "etb_topo_cnt_id");
    if (etb_topo && cJSON_IsString(etb_topo))
    {
        strncpy(etb_topo_cnt_id, etb_topo->valuestring, sizeof(etb_topo_cnt_id) - 1);
        var_table_add(etb_topo_cnt_id, TYPE_UINT32, DIR_INPUT);
        log_debug("trdp: etb_topo_cnt_id = %s", etb_topo_cnt_id);
    }

    cJSON *op_trn_topo = cJSON_GetObjectItem(config, "op_trn_topo_cnt_id");
    if (op_trn_topo && cJSON_IsString(op_trn_topo))
    {
        strncpy(op_trn_topo_cnt_id, op_trn_topo->valuestring, sizeof(op_trn_topo_cnt_id) - 1);
        var_table_add(op_trn_topo_cnt_id, TYPE_UINT32, DIR_INPUT);
        log_debug("trdp: op_trn_topo_cnt_id = %s", op_trn_topo_cnt_id);
    }

    cJSON *containers = cJSON_GetObjectItem(config, "containers");
    if (!containers)
    {
        log_debug("trdp: no containers defined");
        return 0;
    }

    cJSON *inputs = cJSON_GetObjectItem(containers, "inputs");
    if (inputs)
    {
        parse_containers(inputs, input_containers, &input_count, MAX_CONTAINERS, DIR_INPUT);
        log_debug("trdp: parsed %zu PD input containers", input_count);
    }

    cJSON *outputs = cJSON_GetObjectItem(containers, "outputs");
    if (outputs)
    {
        parse_containers(outputs, output_containers, &output_count, MAX_CONTAINERS, DIR_OUTPUT);
        log_debug("trdp: parsed %zu PD output containers", output_count);
    }

    if (inputs)
    {
        parse_md_containers(inputs, md_input_containers, &md_input_count, MAX_CONTAINERS, DIR_INPUT);
        log_debug("trdp: parsed %zu MD input containers", md_input_count);
    }

    if (outputs)
    {
        parse_md_containers(outputs, md_output_containers, &md_output_count, MAX_CONTAINERS, DIR_OUTPUT);
        log_debug("trdp: parsed %zu MD output containers", md_output_count);
    }

    link_md_partners();

    return 0;
}

static int trdp_start(void)
{
    TRDP_ERR_T err;

    err = tlc_init(NULL, NULL, NULL);
    if (err != TRDP_NO_ERR)
    {
        log_error("trdp: tlc_init failed: %d", err);
        return -1;
    }

    TRDP_IP_ADDR_T own_ip = parse_ip(local_ip);
    err = tlc_openSession(&app_handle, own_ip, 0, NULL, NULL, NULL, NULL);
    if (err != TRDP_NO_ERR)
    {
        log_error("trdp: tlc_openSession failed: %d", err);
        tlc_terminate();
        return -1;
    }
    log_debug("trdp: session opened");

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
            log_error("trdp: tlp_publish failed for %s (comid=%u): %d",
                    container->var_id, container->comid, err);
        }
        else
        {
            container->handle = pub_handle;
            log_debug("trdp: publishing %s (comid=%u, dest=%s, period=%ums)",
                    container->var_id, container->comid, container->multicast_ip, container->period_ms);
        }
    }

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
            log_error("trdp: tlp_subscribe failed for %s (comid=%u): %d",
                    container->var_id, container->comid, err);
        }
        else
        {
            container->handle = sub_handle;
            log_debug("trdp: subscribed to %s (comid=%u, src=%s)",
                    container->var_id, container->comid, container->multicast_ip);
        }
    }

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
            log_error("trdp md: tlm_addListener failed for '%s' (comid=%u): %d",
                      container->var_id, container->comid, err);
        }
        else
        {
            container->listen_handle = lis_handle;
            log_debug("trdp md: listening for %s '%s' (comid=%u)",
                      container->msg_type == TRDP_TYPE_Mn ? "Mn" : "Mr",
                      container->var_id, container->comid);
        }
    }

    running = true;

    if (thrd_create(&thread_process, thread_process_func, NULL) != thrd_success)
    {
        log_error("trdp: failed to create process thread");
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

    running = false;

    thrd_join(thread_process, NULL);
    log_debug("trdp: process thread stopped");

    for (size_t i = 0; i < md_output_count; i++)
    {
        if (md_output_containers[i].listen_handle)
        {
            tlm_delListener(app_handle, (TRDP_LIS_T)md_output_containers[i].listen_handle);
            md_output_containers[i].listen_handle = NULL;
        }
    }

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
    input_count = 0;

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
    output_count = 0;

    for (size_t i = 0; i < md_input_count; i++)
    {
        free(md_input_containers[i].buffer);
        md_input_containers[i].buffer = NULL;
        if (md_input_containers[i].fields)
        {
            for (size_t j = 0; j < md_input_containers[i].field_count; j++)
                free(md_input_containers[i].fields[j].entry_fields);
            free(md_input_containers[i].fields);
            md_input_containers[i].fields = NULL;
        }
    }
    md_input_count = 0;

    for (size_t i = 0; i < md_output_count; i++)
    {
        free(md_output_containers[i].buffer);
        md_output_containers[i].buffer = NULL;
        if (md_output_containers[i].fields)
        {
            for (size_t j = 0; j < md_output_containers[i].field_count; j++)
                free(md_output_containers[i].fields[j].entry_fields);
            free(md_output_containers[i].fields);
            md_output_containers[i].fields = NULL;
        }
    }
    md_output_count = 0;

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
