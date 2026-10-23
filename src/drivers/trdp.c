// TRDP (IEC 61375-2-3) driver: process data (PD) and message data (MD Mn / Mr / Mp).
//
// Each connection owns one TRDP session bound to its local_ip and one thread that runs the
// TCNopen loop. Datasets are encoded/decoded here in wire format (packed, big-endian); the
// TCNopen marshalling layer is not used.
//
// Variable ids are exactly the var_ids written in the configuration. Generated ids are only
// used where the configuration cannot name every variable: bitset members ("<var>.<bit>"), MD
// array entries ("<array>[i].<field>"), enable flags ("<container>.is_enabled" unless
// enable_id is given) and telegrams imported from XML ("<dataset>.<element>").

#include "drivers/driver.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "compat/net.h"
#include "drivers/trdp_xml.h"
#include "util/codec.h"
#include "util/log.h"

#include "trdp_if_light.h"
#include "vos_sock.h"

#define TRDP_MAX_CONTAINER_BYTES TRDP_MAX_PD_DATA_SIZE
#define TRDP_MAX_MD_BYTES        65388u
#define LOOP_MAX_WAIT_US         10000

typedef struct trdp_conn trdp_conn_t;

// ---------------------------------------------------------------------------------------------
// Process data
// ---------------------------------------------------------------------------------------------

typedef struct
{
    ammio_var_t *var;          // scalar, or the whole word of a bitset
    ammio_type_t type;
    uint32_t byte_offset;
    unsigned bitset_bytes;     // 0 for scalars, 1/2/4 for bitset8/16/32
    unsigned n_bits;           // named members of a bitset (bits 0..n_bits-1)
    size_t idx;                // index of var in the container's var list; members follow it
    uint32_t word_seq;         // inputs: last seen write_seq of the word and of each member
    uint32_t *bit_seq;
} pd_var_t;

typedef struct
{
    char *id;
    char *label;
    ammio_dir_t dir;
    uint32_t comid;
    uint32_t dest_ip;          // inputs: destination; outputs: multicast group to join (0 = none)
    uint32_t source_ip;        // outputs: source filter (0 = any)
    uint32_t period_ms;
    uint32_t timeout_ms;
    uint32_t size_bytes;
    uint8_t *buffer;           // TRDP_MAX_CONTAINER_BYTES long

    pd_var_t *vars;
    size_t n_vars;
    ammio_var_t **all;         // every ammio variable of the container (for snapshots)
    ammio_var_snapshot_t *snaps;
    ammio_value_t *vals;
    ammio_var_t **upd;
    size_t n_all;

    ammio_var_t *enable;
    TRDP_PUB_T pub;
    TRDP_SUB_T sub;
    bool published;
    bool publish_failed;
    bool received;             // at least one telegram arrived (a timeout before that is "no data")
    bool timed_out;
    bool size_warned;
} pd_container_t;

// ---------------------------------------------------------------------------------------------
// Message data
// ---------------------------------------------------------------------------------------------

typedef enum { MD_MN, MD_MR, MD_MP } md_kind_t;
static const char *const md_kind_names[] = {"Mn", "Mr", "Mp"};

typedef enum { MD_FIELD_SCALAR, MD_FIELD_ARRAY, MD_FIELD_PADDING } md_field_kind_t;

typedef struct
{
    bool is_padding;
    ammio_type_t type;
    uint32_t pad_bytes;
    char *var_id;
} md_entry_t;

typedef struct
{
    md_field_kind_t kind;
    ammio_type_t type;         // scalar type, or the array's count type
    uint32_t pad_bytes;
    size_t idx;                // scalar: var index; array: index of the count var
    uint32_t max_iter;
    md_entry_t *entries;       // copied from the struct definition
    size_t n_entries;
    size_t n_named;
    size_t base_idx;           // index of "<array>[0].<first named entry>"
} md_field_t;

typedef struct md_container
{
    trdp_conn_t *tc;
    char *id;
    char *label;
    ammio_dir_t dir;
    md_kind_t kind;
    uint32_t comid;
    uint32_t dest_ip;
    uint32_t mc_ip;
    char *reply_to;
    uint32_t reply_timeout_ms;
    struct md_container *partner;

    md_field_t *fields;
    size_t n_fields;
    uint32_t max_size;
    uint8_t *buffer;

    ammio_var_t **vars;
    size_t n_vars;
    size_t cap_vars;
    ammio_var_snapshot_t *snaps;
    ammio_value_t *vals;
    ammio_var_t **upd;

    ammio_var_t *enable;
    TRDP_LIS_T listener;
    bool truncated_warned;
} md_container_t;

// ---------------------------------------------------------------------------------------------

struct trdp_conn
{
    ammio_conn_t *conn;
    const char *name;
    bool lib_acquired;

    uint32_t local_ip;
    ammio_var_t *etb_topo_var;
    ammio_var_t *op_trn_topo_var;
    uint32_t etb_topo_cnt;
    uint32_t op_trn_topo_cnt;

    pd_container_t *pd;
    size_t n_pd;
    size_t cap_pd;
    md_container_t *md;
    size_t n_md;
    size_t cap_md;

    TRDP_APP_SESSION_T app;
    thrd_t thread;
    bool thread_started;
    atomic_bool running;
};

// ---------------------------------------------------------------------------------------------
// TCNopen library lifetime (one tlc_init per process, shared by all TRDP connections)
// ---------------------------------------------------------------------------------------------

static once_flag lib_once = ONCE_FLAG_INIT;
static mtx_t lib_mutex;
static int lib_refs = 0;

static void lib_init_mutex(void)
{
    mtx_init(&lib_mutex, mtx_plain);
}

static void trdp_log(void *ref, VOS_LOG_T category, const CHAR8 *time_str, const CHAR8 *file,
                     UINT16 line, const CHAR8 *msg)
{
    (void)ref; (void)time_str; (void)file; (void)line;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", msg ? msg : "");
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    // TCNopen's info/debug output (every allocation, socket dumps) would drown ammio's own log
    if (category == VOS_LOG_ERROR)
        log_warn("trdp: %s", buf);
    else if (category == VOS_LOG_WARNING)
        log_debug("trdp: %s", buf);
}

static bool lib_acquire(void)
{
    call_once(&lib_once, lib_init_mutex);
    mtx_lock(&lib_mutex);
    bool ok = true;
    if (lib_refs == 0)
    {
        TRDP_ERR_T err = tlc_init(trdp_log, NULL, NULL);
        if (err != TRDP_NO_ERR)
        {
            log_error("trdp: tlc_init failed: %d", err);
            ok = false;
        }
    }
    if (ok)
        lib_refs++;
    mtx_unlock(&lib_mutex);
    return ok;
}

static void lib_release(void)
{
    mtx_lock(&lib_mutex);
    if (lib_refs > 0 && --lib_refs == 0)
        tlc_terminate();
    mtx_unlock(&lib_mutex);
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static void *grow(void *arr, size_t *cap, size_t n, size_t elem)
{
    if (n < *cap)
        return arr;
    size_t new_cap = *cap ? *cap * 2 : 8;
    void *p = realloc(arr, new_cap * elem);
    if (!p)
        return NULL;
    memset((char *)p + *cap * elem, 0, (new_cap - *cap) * elem);
    *cap = new_cap;
    return p;
}

static char *ip_str(uint32_t ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

static bool is_multicast(uint32_t ip)
{
    return (ip >> 28) == 0xE;
}

// Container identity: "id" (or the pre-0.6 "var_id")
static const char *get_container_id(const cJSON *json, const char *path, cfg_t *cfg)
{
    const char *id = NULL;
    bool has_id = cfg_get_string(cfg, path, json, "id", false, &id);
    const char *legacy = NULL;
    if (cfg_get_string(cfg, path, json, "var_id", false, &legacy))
    {
        if (has_id)
        {
            cfg_error(cfg, path, "use either 'id' or the deprecated 'var_id', not both");
            return NULL;
        }
        cfg_warn(cfg, path, "'var_id' on a container is deprecated, rename it to 'id' (it is not a variable)");
        id = legacy;
    }
    if (!id && !cJSON_GetObjectItemCaseSensitive(json, "id") && !cJSON_GetObjectItemCaseSensitive(json, "var_id"))
        cfg_error(cfg, path, "missing required key 'id'");
    if (id && !cfg_is_valid_id(id))
    {
        cfg_error(cfg, path, "invalid id \"%s\"", id);
        return NULL;
    }
    return id;
}

// Registers "<id>.is_enabled" (or enable_id) as a bool input with the given default
static ammio_var_t *register_enable(trdp_conn_t *tc, const cJSON *json, const char *path, const char *id,
                                    bool default_value, cfg_t *cfg)
{
    char enable_id[300];
    const char *given = NULL;
    if (cfg_get_string(cfg, path, json, "enable_id", false, &given))
        snprintf(enable_id, sizeof(enable_id), "%s", given);
    else
        snprintf(enable_id, sizeof(enable_id), "%s.is_enabled", id);

    bool initial = default_value;
    cfg_get_bool(cfg, path, json, "enabled", false, &initial);

    ammio_var_t *var = conn_add_var(tc->conn, cfg, path, enable_id, AMMIO_TYPE_BOOL, AMMIO_DIR_INPUT);
    if (var)
    {
        ammio_value_t v = {.b = initial};
        var_table_set_value_silent(var, v);
    }
    return var;
}

static bool read_bool(ammio_var_t *var, bool fallback)
{
    if (!var)
        return fallback;
    ammio_var_snapshot_t s;
    var_table_read(var, &s);
    return s.has_value ? s.value.b : fallback;
}

// ---------------------------------------------------------------------------------------------
// PD configuration
// ---------------------------------------------------------------------------------------------

typedef struct
{
    uint32_t start;
    uint32_t end;
    const char *id;
} span_t;

static int span_cmp(const void *a, const void *b)
{
    const span_t *sa = a, *sb = b;
    return sa->start < sb->start ? -1 : sa->start > sb->start;
}

static bool pd_add_ref(pd_container_t *c, ammio_var_t *var, size_t *cap)
{
    if (c->n_all >= *cap)
    {
        size_t new_cap = *cap ? *cap * 2 : 16;
        ammio_var_t **p = realloc(c->all, new_cap * sizeof(*p));
        if (!p)
            return false;
        c->all = p;
        *cap = new_cap;
    }
    c->all[c->n_all++] = var;
    return true;
}

static bool pd_alloc_scratch(pd_container_t *c)
{
    size_t n = c->n_all ? c->n_all : 1;
    c->snaps = calloc(n, sizeof(*c->snaps));
    c->vals = calloc(n, sizeof(*c->vals));
    c->upd = calloc(n, sizeof(*c->upd));
    c->buffer = calloc(1, TRDP_MAX_CONTAINER_BYTES);
    return c->snaps && c->vals && c->upd && c->buffer;
}

static void pd_parse_variables(trdp_conn_t *tc, pd_container_t *c, const cJSON *variables,
                               const char *path, cfg_t *cfg)
{
    static const char *const input_keys[] = {"var_id", "name", "offset", "type", "bits", "initial", NULL};
    static const char *const output_keys[] = {"var_id", "name", "offset", "type", "bits", NULL};

    size_t n = (size_t)cJSON_GetArraySize(variables);
    c->vars = calloc(n ? n : 1, sizeof(*c->vars));
    span_t *spans = calloc(n ? n : 1, sizeof(*spans));
    size_t cap_all = 0;
    if (!c->vars || !spans)
    {
        free(spans);
        cfg_error(cfg, path, "out of memory");
        return;
    }

    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, variables)
    {
        char vpath[CFG_PATH_MAX];
        cfg_path_index(vpath, path, index++);
        if (!cfg_expect_object(cfg, vpath, item))
            continue;
        cfg_check_keys(cfg, vpath, item, c->dir == AMMIO_DIR_INPUT ? input_keys : output_keys);

        const char *id = NULL, *type_str = NULL;
        uint32_t offset = 0;
        bool ok = cfg_get_string(cfg, vpath, item, "var_id", true, &id);
        ok &= cfg_get_uint32(cfg, vpath, item, "offset", true, 0, TRDP_MAX_CONTAINER_BYTES * 8, &offset);
        ok &= cfg_get_string(cfg, vpath, item, "type", true, &type_str);
        if (!ok)
            continue;

        pd_var_t *v = &c->vars[c->n_vars];
        if (strcmp(type_str, "bitset8") == 0) v->bitset_bytes = 1;
        else if (strcmp(type_str, "bitset16") == 0) v->bitset_bytes = 2;
        else if (strcmp(type_str, "bitset32") == 0) v->bitset_bytes = 4;

        if (v->bitset_bytes)
            v->type = v->bitset_bytes == 1 ? AMMIO_TYPE_UINT8 : v->bitset_bytes == 2 ? AMMIO_TYPE_UINT16 : AMMIO_TYPE_UINT32;
        else if (!ammio_type_from_string(type_str, &v->type))
        {
            cfg_error(cfg, vpath, "'type' = \"%s\" is not valid (expected one of: %s, bitset8, bitset16, bitset32)",
                      type_str, ammio_type_list());
            continue;
        }

        if (offset % 8 != 0)
        {
            cfg_error(cfg, vpath, "'offset' = %u bits is not byte-aligned (use a bitset for single bits)", offset);
            continue;
        }
        uint32_t size = (uint32_t)ammio_type_size_bytes(v->type);
        if (offset / 8 + size > c->size_bytes)
        {
            cfg_error(cfg, vpath, "'%s' (%s at bit %u) ends at bit %u, beyond size_bits = %u", id, type_str,
                      offset, offset + size * 8, c->size_bytes * 8);
            continue;
        }
        v->byte_offset = offset / 8;

        const cJSON *bits = cJSON_GetObjectItemCaseSensitive(item, "bits");
        if (bits && !v->bitset_bytes)
        {
            cfg_error(cfg, vpath, "'bits' is only valid for bitset8/16/32");
            continue;
        }
        if (bits && !cfg_expect_array(cfg, vpath, bits))
            continue;

        v->var = conn_add_var(tc->conn, cfg, vpath, id, v->type, c->dir);
        if (!v->var)
            continue;
        v->idx = c->n_all;
        pd_add_ref(c, v->var, &cap_all);

        if (bits)
        {
            unsigned n_bits = (unsigned)cJSON_GetArraySize(bits);
            if (n_bits > v->bitset_bytes * 8)
            {
                cfg_error(cfg, vpath, "%u bit names for a %s", n_bits, type_str);
                continue;
            }
            v->bit_seq = calloc(n_bits ? n_bits : 1, sizeof(uint32_t));
            unsigned b = 0;
            const cJSON *bit;
            cJSON_ArrayForEach(bit, bits)
            {
                if (!cJSON_IsString(bit))
                {
                    cfg_error(cfg, vpath, "'bits' must contain only strings");
                    break;
                }
                char bit_id[600];
                if (bit->valuestring[0])
                    snprintf(bit_id, sizeof(bit_id), "%s.%s", id, bit->valuestring);
                else
                    snprintf(bit_id, sizeof(bit_id), "%s.bit%u", id, b);
                ammio_var_t *bv = conn_add_var(tc->conn, cfg, vpath, bit_id, AMMIO_TYPE_BOOL, c->dir);
                if (!bv)
                    break;
                pd_add_ref(c, bv, &cap_all);
                b++;
            }
            v->n_bits = b;
        }

        if (c->dir == AMMIO_DIR_INPUT)
        {
            ammio_value_t initial;
            if (cfg_get_value(cfg, vpath, item, "initial", false, v->type, &initial))
            {
                var_table_set_value_silent(v->var, initial);
                // Members follow the initial word value
                uint8_t tmp[4] = {0};
                codec_put_be(tmp, v->type, initial);
                for (unsigned b = 0; b < v->n_bits; b++)
                {
                    ammio_value_t bv = {.b = codec_bitset_get(tmp, b)};
                    var_table_set_value_silent(c->all[v->idx + 1 + b], bv);
                }
            }
        }

        spans[c->n_vars].start = v->byte_offset;
        spans[c->n_vars].end = v->byte_offset + size;
        spans[c->n_vars].id = var_id(v->var);
        c->n_vars++;
    }

    qsort(spans, c->n_vars, sizeof(*spans), span_cmp);
    for (size_t i = 1; i < c->n_vars; i++)
    {
        if (spans[i].start < spans[i - 1].end)
            cfg_error(cfg, path, "'%s' (bytes %u..%u) overlaps '%s' (bytes %u..%u)", spans[i].id,
                      spans[i].start, spans[i].end - 1, spans[i - 1].id, spans[i - 1].start, spans[i - 1].end - 1);
    }
    free(spans);
}

static void pd_parse(trdp_conn_t *tc, const cJSON *json, const char *path, ammio_dir_t dir, cfg_t *cfg)
{
    static const char *const input_keys[] = {
        "id", "var_id", "name", "type", "comid", "dest_ip", "multicast_ip", "period_ms", "size_bits",
        "enable_id", "enabled", "variables", NULL
    };
    static const char *const output_keys[] = {
        "id", "var_id", "name", "type", "comid", "multicast_ip", "source_ip", "period_ms", "timeout_ms",
        "size_bits", "enable_id", "enabled", "variables", NULL
    };
    cfg_check_keys(cfg, path, json, dir == AMMIO_DIR_INPUT ? input_keys : output_keys);

    const char *id = get_container_id(json, path, cfg);
    if (!id)
        return;

    pd_container_t *grown = grow(tc->pd, &tc->cap_pd, tc->n_pd, sizeof(*tc->pd));
    if (!grown)
    {
        cfg_error(cfg, path, "out of memory");
        return;
    }
    tc->pd = grown;
    pd_container_t *c = &tc->pd[tc->n_pd++];
    c->id = ammio_strdup(id);
    c->dir = dir;

    const char *label;
    if (cfg_get_string(cfg, path, json, "name", false, &label))
        c->label = ammio_strdup(label);

    cfg_get_uint32(cfg, path, json, "comid", true, 1, UINT32_MAX, &c->comid);

    if (dir == AMMIO_DIR_INPUT)
    {
        bool has_dest = cfg_get_ipv4(cfg, path, json, "dest_ip", false, &c->dest_ip);
        uint32_t legacy;
        if (cfg_get_ipv4(cfg, path, json, "multicast_ip", false, &legacy))
        {
            if (has_dest)
                cfg_error(cfg, path, "use either 'dest_ip' or 'multicast_ip', not both");
            c->dest_ip = legacy;
            has_dest = true;
        }
        if (!has_dest && !cJSON_GetObjectItemCaseSensitive(json, "dest_ip") &&
            !cJSON_GetObjectItemCaseSensitive(json, "multicast_ip"))
            cfg_error(cfg, path, "missing required key 'dest_ip' (unicast or multicast destination)");
    }
    else
    {
        if (cfg_get_ipv4(cfg, path, json, "multicast_ip", false, &c->dest_ip) && !is_multicast(c->dest_ip))
            cfg_error(cfg, path, "'multicast_ip' is not a multicast address (224.0.0.0/4)");
        cfg_get_ipv4(cfg, path, json, "source_ip", false, &c->source_ip);
    }

    c->period_ms = 100;
    cfg_get_uint32(cfg, path, json, "period_ms", false, 1, 3600000, &c->period_ms);
    c->timeout_ms = 3 * c->period_ms;
    if (dir == AMMIO_DIR_OUTPUT)
        cfg_get_uint32(cfg, path, json, "timeout_ms", false, 1, 3600000, &c->timeout_ms);

    uint32_t size_bits = 0;
    if (cfg_get_uint32(cfg, path, json, "size_bits", true, 8, TRDP_MAX_CONTAINER_BYTES * 8, &size_bits) &&
        size_bits % 8 != 0)
        cfg_error(cfg, path, "'size_bits' = %u is not a multiple of 8", size_bits);
    c->size_bytes = size_bits / 8;

    c->enable = register_enable(tc, json, path, id, true, cfg);

    const cJSON *variables;
    char vpath[CFG_PATH_MAX];
    if (cfg_get_array(cfg, path, json, "variables", true, &variables))
        pd_parse_variables(tc, c, variables, cfg_path(vpath, path, "variables"), cfg);

    if (!pd_alloc_scratch(c))
        cfg_error(cfg, path, "out of memory");
}

// Builds a PD container from a telegram imported from XML
static void pd_from_xml(trdp_conn_t *tc, const trdp_xml_telegram_t *t, const char *path, cfg_t *cfg)
{
    pd_container_t *grown = grow(tc->pd, &tc->cap_pd, tc->n_pd, sizeof(*tc->pd));
    if (!grown)
    {
        cfg_error(cfg, path, "out of memory");
        return;
    }
    tc->pd = grown;
    pd_container_t *c = &tc->pd[tc->n_pd++];
    c->id = ammio_strdup(t->id);
    c->dir = t->dir;
    c->comid = t->comid;
    c->dest_ip = t->dest_ip;
    c->source_ip = t->source_ip;
    c->period_ms = t->period_ms ? t->period_ms : 100;
    c->timeout_ms = t->timeout_ms ? t->timeout_ms : 3 * c->period_ms;
    c->size_bytes = t->size_bytes;
    if (c->size_bytes > TRDP_MAX_CONTAINER_BYTES)
        cfg_error(cfg, path, "dataset '%s' is %u bytes, more than the PD maximum of %u", t->id, c->size_bytes,
                  TRDP_MAX_CONTAINER_BYTES);

    char enable_id[300];
    snprintf(enable_id, sizeof(enable_id), "%s.is_enabled", t->id);
    c->enable = conn_add_var(tc->conn, cfg, path, enable_id, AMMIO_TYPE_BOOL, AMMIO_DIR_INPUT);
    if (c->enable)
    {
        ammio_value_t on = {.b = true};
        var_table_set_value_silent(c->enable, on);
    }

    size_t cap_all = 0;
    c->vars = calloc(t->n_vars ? t->n_vars : 1, sizeof(*c->vars));
    for (size_t i = 0; c->vars && i < t->n_vars; i++)
    {
        pd_var_t *v = &c->vars[c->n_vars];
        v->type = t->vars[i].type;
        v->byte_offset = t->vars[i].byte_offset;
        v->var = conn_add_var(tc->conn, cfg, path, t->vars[i].var_id, v->type, c->dir);
        if (!v->var)
            continue;
        v->idx = c->n_all;
        pd_add_ref(c, v->var, &cap_all);
        c->n_vars++;
    }
    if (!c->vars || !pd_alloc_scratch(c))
        cfg_error(cfg, path, "out of memory");
}

// ---------------------------------------------------------------------------------------------
// MD configuration
// ---------------------------------------------------------------------------------------------

typedef struct
{
    const char *name;
    md_entry_t *entries;
    size_t n;
} md_struct_t;

static size_t md_add_var(md_container_t *c, ammio_var_t *var)
{
    if (c->n_vars >= c->cap_vars)
    {
        size_t cap = c->cap_vars ? c->cap_vars * 2 : 16;
        ammio_var_t **p = realloc(c->vars, cap * sizeof(*p));
        if (!p)
            return SIZE_MAX;
        c->vars = p;
        c->cap_vars = cap;
    }
    c->vars[c->n_vars] = var;
    return c->n_vars++;
}

static bool parse_padding(const cJSON *json, const char *path, uint32_t *pad_bytes, cfg_t *cfg)
{
    uint32_t bits = 0;
    if (!cfg_get_uint32(cfg, path, json, "bits", true, 8, TRDP_MAX_MD_BYTES * 8, &bits))
        return false;
    if (bits % 8 != 0)
    {
        cfg_error(cfg, path, "padding of %u bits is not a multiple of 8", bits);
        return false;
    }
    *pad_bytes = bits / 8;
    return true;
}

static void free_structs(md_struct_t *structs, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        for (size_t k = 0; k < structs[i].n; k++)
            free(structs[i].entries[k].var_id);
        free(structs[i].entries);
    }
    free(structs);
}

static md_struct_t *parse_structs(const cJSON *json, const char *path, size_t *count, cfg_t *cfg)
{
    *count = 0;
    if (!json)
        return NULL;
    size_t n = (size_t)cJSON_GetArraySize(json);
    md_struct_t *structs = calloc(n ? n : 1, sizeof(*structs));
    if (!structs)
        return NULL;

    const cJSON *s;
    cJSON_ArrayForEach(s, json)
    {
        char spath[CFG_PATH_MAX];
        cfg_path(spath, path, s->string);
        ammio_type_t dummy;
        if (ammio_type_from_string(s->string, &dummy) || strcmp(s->string, "padding") == 0)
        {
            cfg_error(cfg, spath, "struct name '%s' clashes with a built-in type", s->string);
            continue;
        }
        if (!cfg_expect_array(cfg, spath, s))
            continue;

        md_struct_t *st = &structs[*count];
        st->name = s->string;
        size_t nf = (size_t)cJSON_GetArraySize(s);
        st->entries = calloc(nf ? nf : 1, sizeof(md_entry_t));
        if (!st->entries)
            continue;

        int index = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, s)
        {
            static const char *const keys[] = {"var_id", "name", "type", "bits", NULL};
            char fpath[CFG_PATH_MAX];
            cfg_path_index(fpath, spath, index++);
            if (!cfg_expect_object(cfg, fpath, f))
                continue;
            cfg_check_keys(cfg, fpath, f, keys);

            md_entry_t *e = &st->entries[st->n];
            const char *type_str;
            if (!cfg_get_string(cfg, fpath, f, "type", true, &type_str))
                continue;
            if (strcmp(type_str, "padding") == 0)
            {
                e->is_padding = true;
                if (parse_padding(f, fpath, &e->pad_bytes, cfg))
                    st->n++;
                continue;
            }
            const char *id;
            if (!cfg_get_string(cfg, fpath, f, "var_id", true, &id))
                continue;
            if (!ammio_type_from_string(type_str, &e->type))
            {
                cfg_error(cfg, fpath, "'type' = \"%s\" is not valid in a struct (expected one of: %s, padding)",
                          type_str, ammio_type_list());
                continue;
            }
            for (size_t k = 0; k < st->n; k++)
            {
                if (st->entries[k].var_id && strcmp(st->entries[k].var_id, id) == 0)
                    cfg_error(cfg, fpath, "duplicate field '%s' in struct '%s'", id, st->name);
            }
            e->var_id = ammio_strdup(id);
            st->n++;
        }
        (*count)++;
    }
    return structs;
}

static void md_parse_variables(trdp_conn_t *tc, md_container_t *c, const cJSON *variables, md_struct_t *structs,
                               size_t n_structs, const char *path, cfg_t *cfg)
{
    size_t n = (size_t)cJSON_GetArraySize(variables);
    c->fields = calloc(n ? n : 1, sizeof(*c->fields));
    if (!c->fields)
    {
        cfg_error(cfg, path, "out of memory");
        return;
    }

    uint64_t max_size = 0;
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, variables)
    {
        static const char *const keys[] = {
            "var_id", "name", "type", "bits", "iterations_id", "iterations_type", "max_iterations", NULL
        };
        char vpath[CFG_PATH_MAX];
        cfg_path_index(vpath, path, index++);
        if (!cfg_expect_object(cfg, vpath, item))
            continue;
        cfg_check_keys(cfg, vpath, item, keys);

        md_field_t *f = &c->fields[c->n_fields];
        const char *type_str;
        if (!cfg_get_string(cfg, vpath, item, "type", true, &type_str))
            continue;

        if (strcmp(type_str, "padding") == 0)
        {
            f->kind = MD_FIELD_PADDING;
            if (parse_padding(item, vpath, &f->pad_bytes, cfg))
            {
                max_size += f->pad_bytes;
                c->n_fields++;
            }
            continue;
        }

        const char *id;
        if (!cfg_get_string(cfg, vpath, item, "var_id", true, &id))
            continue;

        if (ammio_type_from_string(type_str, &f->type))
        {
            f->kind = MD_FIELD_SCALAR;
            ammio_var_t *var = conn_add_var(tc->conn, cfg, vpath, id, f->type, c->dir);
            if (!var)
                continue;
            f->idx = md_add_var(c, var);
            max_size += ammio_type_size_bytes(f->type);
            c->n_fields++;
            continue;
        }

        md_struct_t *st = NULL;
        for (size_t s = 0; s < n_structs; s++)
        {
            if (strcmp(structs[s].name, type_str) == 0)
                st = &structs[s];
        }
        if (!st)
        {
            cfg_error(cfg, vpath, "'type' = \"%s\" is neither a scalar type, \"padding\" nor a struct defined in 'structs'",
                      type_str);
            continue;
        }

        // Array of structs, preceded on the wire by its element count
        f->kind = MD_FIELD_ARRAY;
        const char *count_id;
        bool ok = cfg_get_string(cfg, vpath, item, "iterations_id", true, &count_id);
        ok &= cfg_get_uint32(cfg, vpath, item, "max_iterations", true, 1, 65535, &f->max_iter);
        f->type = AMMIO_TYPE_UINT8;
        if (cJSON_GetObjectItemCaseSensitive(item, "iterations_type"))
        {
            ok &= cfg_get_type(cfg, vpath, item, "iterations_type", true, &f->type);
            if (f->type != AMMIO_TYPE_UINT8 && f->type != AMMIO_TYPE_UINT16 && f->type != AMMIO_TYPE_UINT32)
            {
                cfg_error(cfg, vpath, "'iterations_type' must be uint8, uint16 or uint32");
                ok = false;
            }
        }
        if (!ok)
            continue;
        if ((f->type == AMMIO_TYPE_UINT8 && f->max_iter > UINT8_MAX) ||
            (f->type == AMMIO_TYPE_UINT16 && f->max_iter > UINT16_MAX))
        {
            cfg_error(cfg, vpath, "max_iterations = %u does not fit iterations_type %s", f->max_iter,
                      ammio_type_name(f->type));
            continue;
        }

        ammio_var_t *count_var = conn_add_var(tc->conn, cfg, vpath, count_id, f->type, c->dir);
        if (!count_var)
            continue;
        f->idx = md_add_var(c, count_var);

        f->entries = calloc(st->n ? st->n : 1, sizeof(md_entry_t));
        if (!f->entries)
            continue;
        f->n_entries = st->n;
        uint64_t entry_size = 0;
        for (size_t k = 0; k < st->n; k++)
        {
            f->entries[k] = st->entries[k];
            f->entries[k].var_id = st->entries[k].var_id ? ammio_strdup(st->entries[k].var_id) : NULL;
            entry_size += st->entries[k].is_padding ? st->entries[k].pad_bytes : ammio_type_size_bytes(st->entries[k].type);
            if (!st->entries[k].is_padding)
                f->n_named++;
        }

        f->base_idx = c->n_vars;
        for (uint32_t it = 0; it < f->max_iter; it++)
        {
            for (size_t k = 0; k < f->n_entries; k++)
            {
                if (f->entries[k].is_padding)
                    continue;
                char entry_id[700];
                snprintf(entry_id, sizeof(entry_id), "%s[%u].%s", id, it, f->entries[k].var_id);
                ammio_var_t *ev = conn_add_var(tc->conn, cfg, vpath, entry_id, f->entries[k].type, c->dir);
                md_add_var(c, ev);  // keeps indices aligned even if registration failed
            }
        }
        max_size += ammio_type_size_bytes(f->type) + (uint64_t)f->max_iter * entry_size;
        c->n_fields++;
    }

    if (max_size > TRDP_MAX_MD_BYTES)
    {
        cfg_error(cfg, path, "maximum message size %llu bytes exceeds the MD limit of %u",
                  (unsigned long long)max_size, TRDP_MAX_MD_BYTES);
        max_size = TRDP_MAX_MD_BYTES;
    }
    c->max_size = (uint32_t)max_size;
}

static void md_parse(trdp_conn_t *tc, const cJSON *json, const char *path, ammio_dir_t dir, md_kind_t kind, cfg_t *cfg)
{
    // Allowed keys depend on who sends what
    static const char *const send_keys[] = {   // inputs Mn / Mr: ammio sends
        "id", "var_id", "name", "type", "comid", "dest_ip", "reply_timeout_ms", "enable_id", "enabled",
        "structs", "variables", NULL
    };
    static const char *const listen_keys[] = { // outputs Mn / Mr: ammio listens
        "id", "var_id", "name", "type", "comid", "multicast_ip", "enable_id", "enabled", "structs", "variables", NULL
    };
    static const char *const reply_keys[] = {  // Mp in either direction
        "id", "var_id", "name", "type", "comid", "reply_to", "enable_id", "enabled", "structs", "variables", NULL
    };
    const char *const *keys = kind == MD_MP ? reply_keys : dir == AMMIO_DIR_INPUT ? send_keys : listen_keys;
    cfg_check_keys(cfg, path, json, keys);
    if (kind == MD_MN && cJSON_GetObjectItemCaseSensitive(json, "reply_timeout_ms"))
        cfg_error(cfg, path, "'reply_timeout_ms' is only valid for Mr");

    const char *id = get_container_id(json, path, cfg);
    if (!id)
        return;

    md_container_t *grown = grow(tc->md, &tc->cap_md, tc->n_md, sizeof(*tc->md));
    if (!grown)
    {
        cfg_error(cfg, path, "out of memory");
        return;
    }
    tc->md = grown;
    md_container_t *c = &tc->md[tc->n_md++];
    c->tc = tc;
    c->id = ammio_strdup(id);
    c->dir = dir;
    c->kind = kind;

    const char *str;
    if (cfg_get_string(cfg, path, json, "name", false, &str))
        c->label = ammio_strdup(str);
    cfg_get_uint32(cfg, path, json, "comid", true, 1, UINT32_MAX, &c->comid);

    if (kind != MD_MP && dir == AMMIO_DIR_INPUT)
    {
        cfg_get_ipv4(cfg, path, json, "dest_ip", true, &c->dest_ip);
        c->reply_timeout_ms = 3000;
        if (kind == MD_MR)
            cfg_get_uint32(cfg, path, json, "reply_timeout_ms", false, 1, 3600000, &c->reply_timeout_ms);
    }
    if (kind != MD_MP && dir == AMMIO_DIR_OUTPUT &&
        cfg_get_ipv4(cfg, path, json, "multicast_ip", false, &c->mc_ip) && !is_multicast(c->mc_ip))
        cfg_error(cfg, path, "'multicast_ip' is not a multicast address (224.0.0.0/4)");
    if (kind == MD_MP && cfg_get_string(cfg, path, json, "reply_to", true, &str))
        c->reply_to = ammio_strdup(str);

    // Inputs Mn/Mr: writing true sends once and resets the flag. Everything else: true = active.
    bool trigger = dir == AMMIO_DIR_INPUT && kind != MD_MP;
    c->enable = register_enable(tc, json, path, id, !trigger, cfg);

    const cJSON *structs_json = NULL;
    char spath[CFG_PATH_MAX];
    cfg_get_object(cfg, path, json, "structs", false, &structs_json);
    size_t n_structs = 0;
    md_struct_t *structs = parse_structs(structs_json, cfg_path(spath, path, "structs"), &n_structs, cfg);

    const cJSON *variables = NULL;
    char vpath[CFG_PATH_MAX];
    if (cfg_get_array(cfg, path, json, "variables", false, &variables))
        md_parse_variables(tc, c, variables, structs, n_structs, cfg_path(vpath, path, "variables"), cfg);
    free_structs(structs, n_structs);

    size_t nv = c->n_vars ? c->n_vars : 1;
    c->snaps = calloc(nv, sizeof(*c->snaps));
    c->vals = calloc(nv, sizeof(*c->vals));
    c->upd = calloc(nv, sizeof(*c->upd));
    c->buffer = calloc(1, c->max_size ? c->max_size : 1);
    if (!c->snaps || !c->vals || !c->upd || !c->buffer)
        cfg_error(cfg, path, "out of memory");
}

// Pairs every Mp with the Mr it answers (in the opposite direction)
static void md_link(trdp_conn_t *tc, const char *path, cfg_t *cfg)
{
    for (size_t i = 0; i < tc->n_md; i++)
    {
        md_container_t *mp = &tc->md[i];
        if (mp->kind != MD_MP || !mp->reply_to)
            continue;
        md_container_t *mr = NULL;
        for (size_t j = 0; j < tc->n_md; j++)
        {
            if (tc->md[j].kind == MD_MR && tc->md[j].dir != mp->dir && strcmp(tc->md[j].id, mp->reply_to) == 0)
                mr = &tc->md[j];
        }
        if (!mr)
        {
            cfg_error(cfg, path, "Mp '%s': reply_to = \"%s\" does not name an Mr in %s", mp->id, mp->reply_to,
                      mp->dir == AMMIO_DIR_INPUT ? "outputs" : "inputs");
            continue;
        }
        if (mr->partner)
        {
            cfg_error(cfg, path, "Mr '%s' is answered by both '%s' and '%s'", mr->id, mr->partner->id, mp->id);
            continue;
        }
        mr->partner = mp;
        mp->partner = mr;
    }

    for (size_t i = 0; i < tc->n_md; i++)
    {
        md_container_t *mr = &tc->md[i];
        if (mr->kind == MD_MR && !mr->partner)
            cfg_error(cfg, path, "Mr '%s' has no Mp (in %s) with \"reply_to\": \"%s\"", mr->id,
                      mr->dir == AMMIO_DIR_INPUT ? "outputs" : "inputs", mr->id);
    }
}

// ---------------------------------------------------------------------------------------------
// Connection configuration
// ---------------------------------------------------------------------------------------------

static void parse_containers(trdp_conn_t *tc, const cJSON *arr, const char *path, ammio_dir_t dir, cfg_t *cfg)
{
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        char cpath[CFG_PATH_MAX];
        cfg_path_index(cpath, path, index++);
        if (!cfg_expect_object(cfg, cpath, item))
            continue;

        const char *type = "Pd";
        if (cJSON_GetObjectItemCaseSensitive(item, "type") && !cfg_get_string(cfg, cpath, item, "type", true, &type))
            continue;

        if (strcmp(type, "Pd") == 0 || strcmp(type, "PD") == 0)
            pd_parse(tc, item, cpath, dir, cfg);
        else if (strcmp(type, "Mn") == 0)
            md_parse(tc, item, cpath, dir, MD_MN, cfg);
        else if (strcmp(type, "Mr") == 0)
            md_parse(tc, item, cpath, dir, MD_MR, cfg);
        else if (strcmp(type, "Mp") == 0)
            md_parse(tc, item, cpath, dir, MD_MP, cfg);
        else
            cfg_error(cfg, cpath, "'type' = \"%s\" is not supported (expected \"Pd\", \"Mn\", \"Mr\" or \"Mp\")", type);
    }
}

static void check_pd_duplicates(trdp_conn_t *tc, const char *path, cfg_t *cfg)
{
    for (size_t i = 0; i < tc->n_pd; i++)
    {
        for (size_t j = i + 1; j < tc->n_pd; j++)
        {
            const pd_container_t *a = &tc->pd[i], *b = &tc->pd[j];
            if (a->dir == b->dir && a->comid == b->comid && a->dest_ip == b->dest_ip && a->source_ip == b->source_ip)
                cfg_error(cfg, path, "'%s' and '%s' are the same telegram (comid %u, same addresses)", a->id, b->id, a->comid);
        }
    }
}

static void trdp_destroy(void *ctx);

static void *trdp_create(ammio_conn_t *conn, const cJSON *settings, const char *path, cfg_t *cfg)
{
    static const char *const keys[] = {"local_ip", "etb_topo_cnt_id", "op_trn_topo_cnt_id", "xml", "containers", NULL};

    trdp_conn_t *tc = calloc(1, sizeof(*tc));
    if (!tc)
        return NULL;
    tc->conn = conn;
    tc->name = conn_name(conn);
    atomic_init(&tc->running, false);

    if (!lib_acquire())
    {
        cfg_error(cfg, path, "cannot initialise the TRDP library");
        free(tc);
        return NULL;
    }
    tc->lib_acquired = true;

    cfg_check_keys(cfg, path, settings, keys);

    tc->local_ip = 0;
    if (cfg_get_ipv4(cfg, path, settings, "local_ip", false, &tc->local_ip) && (tc->local_ip >> 24) == 127)
        cfg_warn(cfg, path, "TRDP multicast does not work on the loopback interface; use a real NIC address or 0.0.0.0");

    const char *str;
    if (cfg_get_string(cfg, path, settings, "etb_topo_cnt_id", false, &str))
        tc->etb_topo_var = conn_add_var(conn, cfg, path, str, AMMIO_TYPE_UINT32, AMMIO_DIR_INPUT);
    if (cfg_get_string(cfg, path, settings, "op_trn_topo_cnt_id", false, &str))
        tc->op_trn_topo_var = conn_add_var(conn, cfg, path, str, AMMIO_TYPE_UINT32, AMMIO_DIR_INPUT);

    const cJSON *xml = NULL;
    char xpath[CFG_PATH_MAX];
    cfg_path(xpath, path, "xml");
    if (cfg_get_object(cfg, path, settings, "xml", false, &xml))
    {
        static const char *const xml_keys[] = {"file", "interface", "perspective", NULL};
        static const char *const perspectives[] = {"sut", "ammio", NULL};
        cfg_check_keys(cfg, xpath, xml, xml_keys);
        const char *file = NULL, *ifname = NULL;
        int perspective = 0;
        bool ok = cfg_get_string(cfg, xpath, xml, "file", true, &file);
        ok &= cfg_get_string(cfg, xpath, xml, "interface", true, &ifname);
        cfg_get_enum(cfg, xpath, xml, "perspective", false, perspectives, &perspective);

        trdp_xml_import_t import;
        if (ok && trdp_xml_import(file, ifname, perspective == 0, xpath, cfg, &import) == 0)
        {
            for (size_t i = 0; i < import.n_telegrams; i++)
                pd_from_xml(tc, &import.telegrams[i], xpath, cfg);
            if (!cJSON_GetObjectItemCaseSensitive(settings, "local_ip") && perspective == 1)
                tc->local_ip = import.host_ip;
            log_debug("%s: imported %zu telegram(s) from %s", tc->name, import.n_telegrams, file);
            trdp_xml_import_free(&import);
        }
    }

    const cJSON *containers = NULL;
    char cpath[CFG_PATH_MAX];
    cfg_path(cpath, path, "containers");
    if (cfg_get_object(cfg, path, settings, "containers", !xml, &containers))
    {
        static const char *const ckeys[] = {"inputs", "outputs", NULL};
        cfg_check_keys(cfg, cpath, containers, ckeys);
        const cJSON *arr;
        char apath[CFG_PATH_MAX];
        if (cfg_get_array(cfg, cpath, containers, "inputs", false, &arr))
            parse_containers(tc, arr, cfg_path(apath, cpath, "inputs"), AMMIO_DIR_INPUT, cfg);
        if (cfg_get_array(cfg, cpath, containers, "outputs", false, &arr))
            parse_containers(tc, arr, cfg_path(apath, cpath, "outputs"), AMMIO_DIR_OUTPUT, cfg);
    }

    md_link(tc, cpath, cfg);
    check_pd_duplicates(tc, cpath, cfg);
    if (tc->n_pd + tc->n_md == 0 && cfg->errors == 0)
        cfg_error(cfg, path, "no containers defined");

    return tc;
}

// ---------------------------------------------------------------------------------------------
// PD runtime
// ---------------------------------------------------------------------------------------------

// Keeps a bitset word and its member bits consistent: whichever was written last by a client wins
static void pd_sync_bitset(pd_container_t *c, pd_var_t *v)
{
    ammio_var_snapshot_t *word = &c->snaps[v->idx];
    ammio_var_snapshot_t *bits = &c->snaps[v->idx + 1];
    uint8_t tmp[4] = {0};

    if (word->write_seq != v->word_seq)
    {
        codec_put_be(tmp, v->type, word->value);
        for (unsigned b = 0; b < v->n_bits; b++)
        {
            bool bv = codec_bitset_get(tmp, b);
            if (!bits[b].has_value || bits[b].value.b != bv)
            {
                ammio_value_t val = {.b = bv};
                var_table_set_value_silent(c->all[v->idx + 1 + b], val);
                bits[b].value = val;
                bits[b].has_value = true;
            }
        }
    }
    else
    {
        bool changed = false;
        for (unsigned b = 0; b < v->n_bits; b++)
            changed |= bits[b].write_seq != v->bit_seq[b];
        if (!changed)
            return;

        if (word->has_value)
            codec_put_be(tmp, v->type, word->value);
        for (unsigned b = 0; b < v->n_bits; b++)
        {
            if (bits[b].has_value)
                codec_bitset_set(tmp, b, bits[b].value.b);
        }
        ammio_value_t w = codec_get_be(tmp, v->type);
        var_table_set_value_silent(v->var, w);
        word->value = w;
        word->has_value = true;
    }

    v->word_seq = word->write_seq;
    for (unsigned b = 0; b < v->n_bits; b++)
        v->bit_seq[b] = bits[b].write_seq;
}

static void pd_publish_cycle(trdp_conn_t *tc, pd_container_t *c)
{
    bool enabled = read_bool(c->enable, true);
    var_table_read_many(c->all, c->n_all, c->snaps);

    bool any_value = false;
    for (size_t i = 0; i < c->n_all; i++)
        any_value |= c->snaps[i].has_value;

    for (size_t i = 0; i < c->n_vars; i++)
    {
        if (c->vars[i].n_bits)
            pd_sync_bitset(c, &c->vars[i]);
    }

    memset(c->buffer, 0, c->size_bytes);
    for (size_t i = 0; i < c->n_vars; i++)
    {
        const pd_var_t *v = &c->vars[i];
        const ammio_var_snapshot_t *s = &c->snaps[v->idx];
        if (s->has_value)
            codec_put_be(c->buffer + v->byte_offset, v->type, s->value);
    }

    // Nothing is put on the bus until at least one value is known, and enable=false stops sending
    bool want = enabled && any_value;
    char ip[16];
    if (want && !c->published)
    {
        TRDP_ERR_T err = tlp_publish(tc->app, &c->pub, NULL, NULL, 0, c->comid, 0, 0, tc->local_ip, c->dest_ip,
                                     c->period_ms * 1000u, 0, TRDP_FLAGS_DEFAULT, NULL, c->buffer, c->size_bytes);
        if (err != TRDP_NO_ERR)
        {
            if (!c->publish_failed)
                log_error("%s: cannot publish '%s' (comid %u to %s): TRDP error %d", tc->name, c->id, c->comid,
                          ip_str(c->dest_ip, ip), err);
            c->publish_failed = true;
            return;
        }
        c->published = true;
        c->publish_failed = false;
        log_debug("%s: publishing '%s' (comid %u to %s every %u ms)", tc->name, c->id, c->comid,
                  ip_str(c->dest_ip, ip), c->period_ms);
    }
    else if (!want && c->published)
    {
        tlp_unpublish(tc->app, c->pub);
        c->published = false;
        log_debug("%s: stopped publishing '%s'", tc->name, c->id);
    }

    if (c->published)
    {
        TRDP_ERR_T err = tlp_put(tc->app, c->pub, c->buffer, c->size_bytes);
        if (err != TRDP_NO_ERR)
            log_debug("%s: tlp_put '%s' failed: %d", tc->name, c->id, err);
    }
}

static void pd_receive_cycle(trdp_conn_t *tc, pd_container_t *c)
{
    if (!c->sub)
        return;

    TRDP_PD_INFO_T info;
    UINT32 size = TRDP_MAX_CONTAINER_BYTES;
    TRDP_ERR_T err = tlp_get(tc->app, c->sub, &info, c->buffer, &size);

    if (err == TRDP_TIMEOUT_ERR)
    {
        if (c->received && !c->timed_out)
        {
            log_warn("%s: '%s' (comid %u) not received for %u ms", tc->name, c->id, c->comid, c->timeout_ms);
            var_table_set_quality_many(c->all, c->n_all, AMMIO_QUALITY_STALE);
            c->timed_out = true;
        }
        return;
    }
    if (err != TRDP_NO_ERR)
    {
        if (err != TRDP_NODATA_ERR)
            log_debug("%s: tlp_get '%s' failed: %d", tc->name, c->id, err);
        return;
    }

    if (c->timed_out)
        log_info("%s: '%s' (comid %u) received again", tc->name, c->id, c->comid);
    c->timed_out = false;
    c->received = true;

    if (size != c->size_bytes && !c->size_warned)
    {
        log_warn("%s: '%s' (comid %u): received %u bytes, configured size is %u", tc->name, c->id, c->comid,
                 size, c->size_bytes);
        c->size_warned = true;
    }

    if (!read_bool(c->enable, true))
        return;

    size_t n_good = 0;
    for (size_t i = 0; i < c->n_vars; i++)
    {
        const pd_var_t *v = &c->vars[i];
        bool fits = v->byte_offset + ammio_type_size_bytes(v->type) <= size;
        if (!fits)
        {
            // Telegram shorter than the configured layout
            var_table_set_quality_many(&c->all[v->idx], 1 + v->n_bits, AMMIO_QUALITY_BAD);
            continue;
        }
        const uint8_t *src = c->buffer + v->byte_offset;
        c->upd[n_good] = v->var;
        c->vals[n_good++] = codec_get_be(src, v->type);
        for (unsigned b = 0; b < v->n_bits; b++)
        {
            c->upd[n_good] = c->all[v->idx + 1 + b];
            memset(&c->vals[n_good], 0, sizeof(c->vals[n_good]));
            c->vals[n_good++].b = codec_bitset_get(src, b);
        }
    }
    var_table_update_many(c->upd, c->vals, n_good, AMMIO_QUALITY_GOOD);
}

// ---------------------------------------------------------------------------------------------
// MD runtime
// ---------------------------------------------------------------------------------------------

static uint64_t snap_uint(const ammio_var_snapshot_t *s)
{
    if (!s->has_value)
        return 0;
    double d = ammio_value_to_double(s->type, s->value);
    return d > 0 ? (uint64_t)d : 0;
}

static uint32_t md_encode(md_container_t *c)
{
    var_table_read_many(c->vars, c->n_vars, c->snaps);
    memset(c->buffer, 0, c->max_size ? c->max_size : 1);

    uint32_t off = 0;
    for (size_t i = 0; i < c->n_fields; i++)
    {
        const md_field_t *f = &c->fields[i];
        if (f->kind == MD_FIELD_PADDING)
        {
            off += f->pad_bytes;
            continue;
        }

        const ammio_var_snapshot_t *s = &c->snaps[f->idx];
        if (f->kind == MD_FIELD_SCALAR)
        {
            if (s->has_value)
                codec_put_be(c->buffer + off, f->type, s->value);
            off += (uint32_t)ammio_type_size_bytes(f->type);
            continue;
        }

        uint64_t count = snap_uint(s);
        if (count > f->max_iter)
        {
            log_warn("%s: '%s': %s = %llu exceeds max_iterations %u, sending %u", c->tc->name, c->id,
                     s->var_id, (unsigned long long)count, f->max_iter, f->max_iter);
            count = f->max_iter;
        }
        ammio_value_t cv;
        ammio_value_from_uint64(f->type, count, &cv);
        codec_put_be(c->buffer + off, f->type, cv);
        off += (uint32_t)ammio_type_size_bytes(f->type);

        for (uint64_t it = 0; it < count; it++)
        {
            size_t named = 0;
            for (size_t k = 0; k < f->n_entries; k++)
            {
                const md_entry_t *e = &f->entries[k];
                if (e->is_padding)
                {
                    off += e->pad_bytes;
                    continue;
                }
                const ammio_var_snapshot_t *es = &c->snaps[f->base_idx + it * f->n_named + named++];
                if (es->has_value)
                    codec_put_be(c->buffer + off, e->type, es->value);
                off += (uint32_t)ammio_type_size_bytes(e->type);
            }
        }
    }
    return off;
}

static void md_decode(md_container_t *c, const uint8_t *data, uint32_t size)
{
    size_t n = 0;
    uint32_t off = 0;
    bool truncated = false;

#define NEED(bytes) do { if ((uint64_t)off + (bytes) > size) { truncated = true; goto done; } } while (0)

    for (size_t i = 0; i < c->n_fields; i++)
    {
        const md_field_t *f = &c->fields[i];
        if (f->kind == MD_FIELD_PADDING)
        {
            NEED(f->pad_bytes);
            off += f->pad_bytes;
            continue;
        }

        size_t sz = ammio_type_size_bytes(f->type);
        NEED(sz);
        ammio_value_t v = codec_get_be(data + off, f->type);
        off += (uint32_t)sz;
        if (c->vars[f->idx])
        {
            c->upd[n] = c->vars[f->idx];
            c->vals[n++] = v;
        }
        if (f->kind == MD_FIELD_SCALAR)
            continue;

        uint64_t count = (uint64_t)ammio_value_to_double(f->type, v);
        if (count > f->max_iter)
        {
            log_warn("%s: '%s' received %llu array entries, only %u are configured", c->tc->name, c->id,
                     (unsigned long long)count, f->max_iter);
            count = f->max_iter;
        }
        for (uint64_t it = 0; it < count; it++)
        {
            size_t named = 0;
            for (size_t k = 0; k < f->n_entries; k++)
            {
                const md_entry_t *e = &f->entries[k];
                if (e->is_padding)
                {
                    NEED(e->pad_bytes);
                    off += e->pad_bytes;
                    continue;
                }
                size_t esz = ammio_type_size_bytes(e->type);
                NEED(esz);
                ammio_var_t *ev = c->vars[f->base_idx + it * f->n_named + named++];
                if (ev)
                {
                    c->upd[n] = ev;
                    c->vals[n++] = codec_get_be(data + off, e->type);
                }
                off += (uint32_t)esz;
            }
        }
    }
#undef NEED

done:
    if (truncated && !c->truncated_warned)
    {
        log_warn("%s: '%s' (comid %u): message of %u bytes is shorter than its layout", c->tc->name, c->id,
                 c->comid, size);
        c->truncated_warned = true;
    }
    var_table_update_many(c->upd, c->vals, n, AMMIO_QUALITY_GOOD);
}

static void md_callback(void *ref, TRDP_APP_SESSION_T app, const TRDP_MD_INFO_T *msg, UINT8 *data, UINT32 size)
{
    (void)ref;
    if (!msg || !msg->pUserRef)
        return;
    md_container_t *c = msg->pUserRef;

    if (msg->resultCode != TRDP_NO_ERR)
    {
        if (msg->resultCode == TRDP_REPLYTO_ERR || msg->resultCode == TRDP_TIMEOUT_ERR)
        {
            log_warn("%s: no reply '%s' to request '%s' within %u ms", c->tc->name, c->id,
                     c->partner ? c->partner->id : "?", c->partner ? c->partner->reply_timeout_ms : 0);
            var_table_set_quality_many(c->vars, c->n_vars, AMMIO_QUALITY_STALE);
        }
        else
        {
            log_debug("%s: MD error %d for '%s'", c->tc->name, msg->resultCode, c->id);
        }
        return;
    }

    if (msg->msgType == TRDP_MSG_MN || msg->msgType == TRDP_MSG_MP)
    {
        if (read_bool(c->enable, true))
            md_decode(c, data ? data : (const UINT8 *)"", data ? size : 0);
        return;
    }

    if (msg->msgType == TRDP_MSG_MR)
    {
        // Request from the SUT: store its payload, then answer with the paired Mp
        if (read_bool(c->enable, true))
            md_decode(c, data ? data : (const UINT8 *)"", data ? size : 0);

        md_container_t *reply = c->partner;
        if (!reply || !read_bool(reply->enable, true))
        {
            log_debug("%s: request '%s' received, reply disabled", c->tc->name, c->id);
            return;
        }
        uint32_t len = md_encode(reply);
        TRDP_ERR_T err = tlm_reply(app, &msg->sessionId, reply->comid, 0, NULL, len ? reply->buffer : NULL, len, NULL);
        if (err != TRDP_NO_ERR)
            log_warn("%s: reply '%s' failed: TRDP error %d", c->tc->name, reply->id, err);
    }
}

static void md_send_cycle(trdp_conn_t *tc, md_container_t *c)
{
    if (c->dir != AMMIO_DIR_INPUT || c->kind == MD_MP || !read_bool(c->enable, false))
        return;

    // One message per rising edge: reset the trigger before sending
    ammio_value_t off = {.b = false};
    var_table_set_value_silent(c->enable, off);

    uint32_t len = md_encode(c);
    const UINT8 *payload = len ? c->buffer : NULL;
    char ip[16];
    TRDP_ERR_T err;
    if (c->kind == MD_MN)
    {
        err = tlm_notify(tc->app, NULL, NULL, c->comid, 0, 0, tc->local_ip, c->dest_ip, TRDP_FLAGS_DEFAULT, NULL,
                         payload, len, NULL, NULL);
    }
    else
    {
        TRDP_UUID_T session;
        err = tlm_request(tc->app, c->partner, md_callback, &session, c->comid, 0, 0, tc->local_ip, c->dest_ip,
                          TRDP_FLAGS_DEFAULT, 1, c->reply_timeout_ms * 1000u, NULL, payload, len, NULL, NULL);
    }

    if (err != TRDP_NO_ERR)
        log_warn("%s: sending %s '%s' to %s failed: TRDP error %d", tc->name, md_kind_names[c->kind], c->id,
                 ip_str(c->dest_ip, ip), err);
    else
        log_debug("%s: sent %s '%s' (comid %u, %u bytes) to %s", tc->name, md_kind_names[c->kind], c->id,
                  c->comid, len, ip_str(c->dest_ip, ip));
}

// ---------------------------------------------------------------------------------------------
// Thread and lifecycle
// ---------------------------------------------------------------------------------------------

static void update_topo_counts(trdp_conn_t *tc)
{
    ammio_var_snapshot_t s;
    if (tc->etb_topo_var)
    {
        var_table_read(tc->etb_topo_var, &s);
        if (s.has_value && s.value.u32 != tc->etb_topo_cnt)
        {
            tc->etb_topo_cnt = s.value.u32;
            tlc_setETBTopoCount(tc->app, tc->etb_topo_cnt);
        }
    }
    if (tc->op_trn_topo_var)
    {
        var_table_read(tc->op_trn_topo_var, &s);
        if (s.has_value && s.value.u32 != tc->op_trn_topo_cnt)
        {
            tc->op_trn_topo_cnt = s.value.u32;
            tlc_setOpTrainTopoCount(tc->app, tc->op_trn_topo_cnt);
        }
    }
}

static int trdp_thread(void *arg)
{
    trdp_conn_t *tc = arg;

    while (atomic_load(&tc->running))
    {
        update_topo_counts(tc);
        for (size_t i = 0; i < tc->n_pd; i++)
        {
            if (tc->pd[i].dir == AMMIO_DIR_INPUT)
                pd_publish_cycle(tc, &tc->pd[i]);
        }
        for (size_t i = 0; i < tc->n_md; i++)
            md_send_cycle(tc, &tc->md[i]);

        TRDP_FDS_T rfds;
        TRDP_TIME_T tv;
        TRDP_SOCK_T n_desc = 0;
        FD_ZERO((fd_set *)&rfds);
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_MAX_WAIT_US;
        tlc_getInterval(tc->app, &tv, &rfds, &n_desc);
        if (tv.tv_sec > 0 || tv.tv_usec > LOOP_MAX_WAIT_US)
        {
            tv.tv_sec = 0;
            tv.tv_usec = LOOP_MAX_WAIT_US;
        }

        INT32 ready = vos_select(n_desc, &rfds, NULL, NULL, &tv);
        if (ready < 0)
            ready = 0;
        TRDP_ERR_T err = tlc_process(tc->app, &rfds, &ready);
        if (err != TRDP_NO_ERR && err != TRDP_NODATA_ERR)
            log_debug("%s: tlc_process: %d", tc->name, err);

        for (size_t i = 0; i < tc->n_pd; i++)
        {
            if (tc->pd[i].dir == AMMIO_DIR_OUTPUT)
                pd_receive_cycle(tc, &tc->pd[i]);
        }
    }
    return 0;
}

static int trdp_start(void *ctx)
{
    trdp_conn_t *tc = ctx;
    char ip[16];

    TRDP_ERR_T err = tlc_openSession(&tc->app, tc->local_ip, 0, NULL, NULL, NULL, NULL);
    if (err != TRDP_NO_ERR)
    {
        log_error("%s: cannot open a TRDP session on %s: TRDP error %d", tc->name, ip_str(tc->local_ip, ip), err);
        tc->app = NULL;
        return -1;
    }

    for (size_t i = 0; i < tc->n_pd; i++)
    {
        pd_container_t *c = &tc->pd[i];
        if (c->dir != AMMIO_DIR_OUTPUT)
            continue;
        err = tlp_subscribe(tc->app, &c->sub, NULL, NULL, 0, c->comid, 0, 0, c->source_ip, 0, c->dest_ip,
                            TRDP_FLAGS_DEFAULT, NULL, c->timeout_ms * 1000u, TRDP_TO_KEEP_LAST_VALUE);
        if (err != TRDP_NO_ERR)
        {
            log_error("%s: cannot subscribe to '%s' (comid %u): TRDP error %d", tc->name, c->id, c->comid, err);
            c->sub = NULL;
            return -1;
        }
    }

    for (size_t i = 0; i < tc->n_md; i++)
    {
        md_container_t *c = &tc->md[i];
        if (c->dir != AMMIO_DIR_OUTPUT || c->kind == MD_MP)
            continue;
        err = tlm_addListener(tc->app, &c->listener, c, md_callback, TRUE, c->comid, 0, 0, 0, 0, c->mc_ip,
                              TRDP_FLAGS_DEFAULT, NULL, NULL);
        if (err != TRDP_NO_ERR)
        {
            log_error("%s: cannot listen for %s '%s' (comid %u): TRDP error %d", tc->name, md_kind_names[c->kind],
                      c->id, c->comid, err);
            c->listener = NULL;
            return -1;
        }
    }

    // TRDP is connectionless: once the session is open the link is considered up
    conn_set_state(tc->conn, AMMIO_CONN_CONNECTED);

    atomic_store(&tc->running, true);
    if (thrd_create(&tc->thread, trdp_thread, tc) != thrd_success)
    {
        log_error("%s: cannot create thread", tc->name);
        atomic_store(&tc->running, false);
        return -1;
    }
    tc->thread_started = true;
    return 0;
}

static void trdp_stop(void *ctx)
{
    trdp_conn_t *tc = ctx;

    atomic_store(&tc->running, false);
    if (tc->thread_started)
    {
        thrd_join(tc->thread, NULL);
        tc->thread_started = false;
    }
    if (!tc->app)
        return;

    for (size_t i = 0; i < tc->n_md; i++)
    {
        if (tc->md[i].listener)
        {
            tlm_delListener(tc->app, tc->md[i].listener);
            tc->md[i].listener = NULL;
        }
    }
    for (size_t i = 0; i < tc->n_pd; i++)
    {
        pd_container_t *c = &tc->pd[i];
        if (c->published)
            tlp_unpublish(tc->app, c->pub);
        c->published = false;
        if (c->sub)
            tlp_unsubscribe(tc->app, c->sub);
        c->sub = NULL;
    }
    tlc_closeSession(tc->app);
    tc->app = NULL;
}

static void trdp_destroy(void *ctx)
{
    trdp_conn_t *tc = ctx;
    if (!tc)
        return;

    for (size_t i = 0; i < tc->n_pd; i++)
    {
        pd_container_t *c = &tc->pd[i];
        for (size_t k = 0; k < c->n_vars; k++)
            free(c->vars[k].bit_seq);
        free(c->vars);
        free(c->all);
        free(c->snaps);
        free(c->vals);
        free(c->upd);
        free(c->buffer);
        free(c->id);
        free(c->label);
    }
    free(tc->pd);

    for (size_t i = 0; i < tc->n_md; i++)
    {
        md_container_t *c = &tc->md[i];
        for (size_t k = 0; k < c->n_fields; k++)
        {
            for (size_t e = 0; e < c->fields[k].n_entries; e++)
                free(c->fields[k].entries[e].var_id);
            free(c->fields[k].entries);
        }
        free(c->fields);
        free(c->vars);
        free(c->snaps);
        free(c->vals);
        free(c->upd);
        free(c->buffer);
        free(c->id);
        free(c->label);
        free(c->reply_to);
    }
    free(tc->md);

    if (tc->lib_acquired)
        lib_release();
    free(tc);
}

const ammio_driver_t trdp_driver = {
    .protocol = "trdp",
    .create   = trdp_create,
    .start    = trdp_start,
    .stop     = trdp_stop,
    .destroy  = trdp_destroy,
};
