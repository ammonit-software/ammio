// TRDP (IEC 61375-2-3): process data (Pd) and message data (Mn, Mr, Mp).
//
// Process data: inputs are published telegrams, outputs subscribed ones.
// Variables sit at byte offsets; values are big-endian; bitset bits are
// numbered from the most significant bit of the first byte (bit 0 = 0x80).
//
// Message data: payloads are laid out sequentially (scalars, padding and
// arrays of structs). Inputs are sent by ammio: Mn/Mr when the client raises
// their enable flag, Mp in reply to an Mr from the SUT. Outputs are received.

#include "../json.h"
#include "../log.h"
#include "../protocol.h"

#include "iec61375-2-3.h"
#include "trdp_if_light.h"
#include "vos_sock.h"
#include "vos_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define MAX_SELECT_US 10000        // upper bound of one cycle, i.e. input latency
#define TIMEOUT_PERIODS 3          // a Pd output times out after 3 missed periods
#define DEFAULT_REPLY_TIMEOUT_MS 3000

typedef enum
{
    MSG_PD,
    MSG_MN, // notification
    MSG_MR, // request (expects an Mp)
    MSG_MP  // reply
} msg_t;

static const char *const msg_names[] = {"Pd", "Mn", "Mr", "Mp"};

typedef struct trdp_conn trdp_conn_t;

// --- process data ------------------------------------------------------------

typedef struct
{
    var_t *var;
    uint16_t offset; // byte offset in the payload
    int8_t bit;      // -1: whole value; 0..7: bit of a bitset (0 = MSB)
    bool mirror;     // input bitset aggregate: read-only copy of the assembled bits
} field_t;

typedef enum
{
    RX_WAITING,
    RX_OK,
    RX_TIMEOUT
} rx_state_t;

typedef struct
{
    char label[96]; // "comid 1000 (DOOR_CONTROL)" for logs
    uint32_t comid;
    TRDP_IP_ADDR_T dest_ip;
    uint32_t period_ms;
    uint32_t size;
    uint8_t *buf;
    field_t *fields;
    size_t field_count;
    var_t *enable; // optional: 0 stops publishing (inputs) / updating (outputs)
    void *handle;  // TRDP_PUB_T or TRDP_SUB_T
    rx_state_t rx;
    bool size_mismatch;
} pd_t;

// --- message data ------------------------------------------------------------

typedef struct
{
    const char *id; // NULL for padding
    type_t type;
    uint32_t pad; // padding bytes
} member_t;

typedef struct
{
    const char *name;
    member_t *members;
    size_t count;
    uint32_t size; // bytes per entry
} struct_def_t;

typedef enum
{
    F_SCALAR,
    F_PADDING,
    F_ARRAY
} fkind_t;

typedef struct
{
    fkind_t kind;
    var_t *var;      // scalar
    uint32_t pad;    // padding bytes
    var_t *count;    // array: number of entries
    uint32_t max;    // array: max entries
    const struct_def_t *def;
    var_t **items;   // array: items[i * def->count + m], NULL for padding members
} mfield_t;

typedef struct md md_t;
struct md
{
    char label[96];
    const char *id;
    msg_t kind;
    dir_t dir;
    uint32_t comid;
    TRDP_IP_ADDR_T dest_ip;
    uint32_t timeout_ms;
    const char *reply_to;
    md_t *partner; // Mr <-> Mp pairing
    var_t *enable; // inputs Mn/Mr: send trigger; otherwise optional gate
    struct_def_t *structs;
    size_t struct_count;
    mfield_t *fields;
    size_t field_count;
    uint32_t max_size;
    uint8_t *buf;
    TRDP_LIS_T listener;
    trdp_conn_t *owner;
};

struct trdp_conn
{
    const char *conn;
    TRDP_IP_ADDR_T own_ip;
    TRDP_APP_SESSION_T session;

    pd_t *pds; // inputs first, then outputs
    size_t pd_input_count, pd_count;
    md_t *mds;
    size_t md_count;

    var_t *etb_topo, *op_trn_topo; // optional topology counters
    uint32_t etb_sent, op_trn_sent;
    bool topo_sent;
};

static bool enabled(const var_t *enable)
{
    return !enable || var_get(enable, NULL).u8 != 0;
}

// --- library lifetime (tlc_init is process-wide, shared by all connections) --

static mtx_t lib_mutex;
static once_flag lib_once = ONCE_FLAG_INIT;
static int lib_users = 0;

static void lib_mutex_init(void)
{
    mtx_init(&lib_mutex, mtx_plain);
}

static void trdp_log(void *ref, VOS_LOG_T category, const CHAR8 *time, const CHAR8 *file,
                     UINT16 line, const CHAR8 *msg)
{
    (void)ref, (void)time, (void)file, (void)line;
    if (category <= VOS_LOG_WARNING && log_get_level() >= LOG_DEBUG)
    {
        size_t n = strlen(msg);
        while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
            n--;
        log_debug("trdp: %.*s", (int)n, msg);
    }
}

static TRDP_ERR_T lib_acquire(void)
{
    call_once(&lib_once, lib_mutex_init);
    mtx_lock(&lib_mutex);
    TRDP_ERR_T err = lib_users ? TRDP_NO_ERR : tlc_init(trdp_log, NULL, NULL);
    if (err == TRDP_NO_ERR)
        lib_users++;
    mtx_unlock(&lib_mutex);
    return err;
}

static void lib_release(void)
{
    mtx_lock(&lib_mutex);
    if (--lib_users == 0)
        tlc_terminate();
    mtx_unlock(&lib_mutex);
}

// --- one connection per local IP --------------------------------------------
// Two TRDP sessions on the same address both bind the TRDP UDP ports, and each
// datagram reaches only one of them. Refuse such configs instead of losing data.

#define MAX_INSTANCES 64
static const trdp_conn_t *instances[MAX_INSTANCES];
static size_t instance_count = 0;

static const char *ip_owner(TRDP_IP_ADDR_T ip)
{
    for (size_t i = 0; i < instance_count; i++)
        if (instances[i]->own_ip == ip)
            return instances[i]->conn;
    return NULL;
}

static void instance_remove(const trdp_conn_t *t)
{
    for (size_t i = 0; i < instance_count; i++)
        if (instances[i] == t)
            instances[i] = instances[--instance_count];
}

// --- Pd payload <-> variables ------------------------------------------------

static void pd_encode(pd_t *pd)
{
    memset(pd->buf, 0, pd->size);
    for (size_t i = 0; i < pd->field_count; i++)
    {
        const field_t *f = &pd->fields[i];
        if (f->mirror)
            continue;
        value_t v = var_get(f->var, NULL);
        if (f->bit < 0)
            value_to_be(var_type(f->var), v, pd->buf + f->offset);
        else if (v.b)
            pd->buf[f->offset] |= (uint8_t)(0x80u >> f->bit);
    }
    for (size_t i = 0; i < pd->field_count; i++)
    {
        const field_t *f = &pd->fields[i];
        if (f->mirror)
        {
            value_t now = value_from_be(var_type(f->var), pd->buf + f->offset);
            value_t was = var_get(f->var, NULL);
            if (memcmp(&now, &was, sizeof(now)) != 0)
                var_set(f->var, now);
        }
    }
}

static void pd_decode(pd_t *pd, uint32_t received)
{
    for (size_t i = 0; i < pd->field_count; i++)
    {
        const field_t *f = &pd->fields[i];
        type_t type = var_type(f->var);
        if (f->offset + (f->bit < 0 ? type_size(type) : 1) > received)
            continue;

        value_t v;
        if (f->bit < 0)
            v = value_from_be(type, pd->buf + f->offset);
        else
            v.b = (pd->buf[f->offset] & (0x80u >> f->bit)) != 0;
        var_set(f->var, v);
    }
}

// --- Md payload <-> variables ------------------------------------------------

static uint32_t md_count_value(const mfield_t *f)
{
    double d = value_to_double(var_type(f->count), var_get(f->count, NULL));
    return d < 0 ? 0 : d > f->max ? f->max : (uint32_t)d;
}

// Returns the payload size.
static uint32_t md_serialize(md_t *md)
{
    uint8_t *buf = md->buf;
    uint32_t off = 0;
    memset(buf, 0, md->max_size);

    for (size_t i = 0; i < md->field_count; i++)
    {
        const mfield_t *f = &md->fields[i];
        if (f->kind == F_PADDING)
        {
            off += f->pad;
        }
        else if (f->kind == F_SCALAR)
        {
            value_to_be(var_type(f->var), var_get(f->var, NULL), buf + off);
            off += (uint32_t)type_size(var_type(f->var));
        }
        else
        {
            uint32_t n = md_count_value(f);
            value_t count = {0};
            value_from_double(var_type(f->count), n, &count);
            value_to_be(var_type(f->count), count, buf + off);
            off += (uint32_t)type_size(var_type(f->count));
            for (uint32_t e = 0; e < n; e++)
            {
                for (size_t m = 0; m < f->def->count; m++)
                {
                    const member_t *mb = &f->def->members[m];
                    var_t *item = f->items[e * f->def->count + m];
                    if (item)
                        value_to_be(mb->type, var_get(item, NULL), buf + off);
                    off += item ? (uint32_t)type_size(mb->type) : mb->pad;
                }
            }
        }
    }
    return off;
}

// Stores every field that is fully inside the received payload.
static void md_deserialize(md_t *md, const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;
    for (size_t i = 0; i < md->field_count; i++)
    {
        const mfield_t *f = &md->fields[i];
        if (f->kind == F_PADDING)
        {
            off += f->pad;
            continue;
        }

        var_t *head = f->kind == F_SCALAR ? f->var : f->count;
        uint32_t size = (uint32_t)type_size(var_type(head));
        if (off + size > len)
            return;
        value_t v = value_from_be(var_type(head), data + off);
        var_set(head, v);
        off += size;
        if (f->kind == F_SCALAR)
            continue;

        double count = value_to_double(var_type(f->count), v);
        uint32_t n = count > f->max ? f->max : (uint32_t)count;
        for (uint32_t e = 0; e < n; e++)
        {
            for (size_t m = 0; m < f->def->count; m++)
            {
                const member_t *mb = &f->def->members[m];
                var_t *item = f->items[e * f->def->count + m];
                if (!item)
                {
                    off += mb->pad;
                    continue;
                }
                uint32_t isize = (uint32_t)type_size(mb->type);
                if (off + isize > len)
                    return;
                var_set(item, value_from_be(mb->type, data + off));
                off += isize;
            }
        }
    }
}

// --- config helpers ----------------------------------------------------------

static bool parse_ip(const char *str, TRDP_IP_ADDR_T *out)
{
    unsigned a, b, c, d;
    char extra;
    if (sscanf(str, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255)
        return false;
    *out = (TRDP_IP_ADDR_T)(a << 24 | b << 16 | c << 8 | d);
    return true;
}

static bool get_ip(const cJSON *obj, const char *key, const char *ctx, bool required,
                   TRDP_IP_ADDR_T *out)
{
    const char *str = NULL;
    if (!(required ? json_need_string(obj, key, ctx, &str) : json_get_string(obj, key, ctx, &str)))
        return false;
    if (str && !parse_ip(str, out))
    {
        log_error("%s.%s: invalid IPv4 address '%s'", ctx, key, str);
        return false;
    }
    return true;
}

static bool forbid(const cJSON *obj, const char *key, const char *ctx, const char *why)
{
    if (!cJSON_GetObjectItemCaseSensitive(obj, key))
        return true;
    log_error("%s.%s: %s", ctx, key, why);
    return false;
}

// Optional enable flag: a uint8 input with the given default.
static bool get_enable(const cJSON *obj, const char *ctx, bool required, uint8_t initial,
                       var_t **out)
{
    const char *id = NULL;
    if (!(required ? json_need_string(obj, "enable_id", ctx, &id)
                   : json_get_string(obj, "enable_id", ctx, &id)))
        return false;
    if (!id)
        return true;
    if (!(*out = conn_add_var(ctx, id, TYPE_UINT8, DIR_INPUT)))
        return false;
    value_t v = {.u8 = initial};
    var_set(*out, v);
    return true;
}

static void make_label(char *label, size_t len, uint32_t comid, const char *id)
{
    if (id)
        snprintf(label, len, "comid %u (%s)", comid, id);
    else
        snprintf(label, len, "comid %u", comid);
}

// Marks payload bits as used; reports overlaps and fields past the end.
static bool claim(uint8_t *used, uint32_t size, const char *ctx, const char *id,
                  uint32_t offset, uint32_t bytes)
{
    if (offset + bytes > size)
    {
        log_error("%s: '%s' (bytes %u..%u) does not fit in size %u", ctx, id, offset,
                  offset + bytes - 1, size);
        return false;
    }
    for (uint32_t i = offset; i < offset + bytes; i++)
    {
        if (used[i])
        {
            log_error("%s: '%s' overlaps another variable at byte %u", ctx, id, i);
            return false;
        }
        used[i] = 1;
    }
    return true;
}

static int bitset_bits(const char *type)
{
    if (strcmp(type, "bitset8") == 0) return 8;
    if (strcmp(type, "bitset16") == 0) return 16;
    if (strcmp(type, "bitset32") == 0) return 32;
    return 0;
}

// --- Pd config ---------------------------------------------------------------

static bool parse_pd_field(pd_t *pd, const cJSON *item, const char *ctx, dir_t dir, uint8_t *used)
{
    static const char *const keys[] = {"var_id", "name", "offset", "type", "bits", NULL};
    const char *id = NULL, *label = NULL, *type_str = NULL;
    uint32_t offset = 0;

    if (!cJSON_IsObject(item))
    {
        log_error("%s: must be an object", ctx);
        return false;
    }
    bool ok = json_check_keys(item, ctx, keys);
    ok &= json_need_string(item, "var_id", ctx, &id);
    ok &= json_get_string(item, "name", ctx, &label);
    ok &= json_need_uint(item, "offset", ctx, 0, TRDP_MAX_PD_DATA_SIZE - 1, &offset);
    ok &= json_need_string(item, "type", ctx, &type_str);
    if (!ok)
        return false;

    int nbits = bitset_bits(type_str);
    const cJSON *bits = cJSON_GetObjectItemCaseSensitive(item, "bits");

    if (nbits == 0)
    {
        type_t type;
        if (!type_parse(type_str, &type))
        {
            log_error("%s.type: unknown type '%s' (expected: %s, bitset8, bitset16, bitset32)",
                      ctx, type_str, type_names());
            return false;
        }
        if (!forbid(item, "bits", ctx, "only valid for bitset types") ||
            !claim(used, pd->size, ctx, id, offset, (uint32_t)type_size(type)))
            return false;
        var_t *var = conn_add_var(ctx, id, type, dir);
        if (!var)
            return false;
        pd->fields[pd->field_count++] = (field_t){var, (uint16_t)offset, -1, false};
        return true;
    }

    // Bitset: the whole value as "<var_id>" (uint8/16/32, read-only for inputs)
    // plus one bool "<var_id>.<bit name>" per bit ("<var_id>.bit<N>" if unnamed).
    if (!cJSON_IsArray(bits) || cJSON_GetArraySize(bits) > nbits)
    {
        log_error("%s.bits: must be an array of up to %d bit names", ctx, nbits);
        return false;
    }
    if (!claim(used, pd->size, ctx, id, offset, (uint32_t)nbits / 8))
        return false;

    type_t whole_type = nbits == 8 ? TYPE_UINT8 : nbits == 16 ? TYPE_UINT16 : TYPE_UINT32;
    var_t *whole = conn_add_var(ctx, id, whole_type, dir == DIR_INPUT ? DIR_OUTPUT : dir);
    if (!whole)
        return false;
    pd->fields[pd->field_count++] = (field_t){whole, (uint16_t)offset, -1, dir == DIR_INPUT};

    int index = 0;
    const cJSON *bit;
    cJSON_ArrayForEach(bit, bits)
    {
        int b = index++;
        if (!cJSON_IsNull(bit) && !cJSON_IsString(bit))
        {
            log_error("%s.bits[%d]: must be a string or null", ctx, b);
            ok = false;
            continue;
        }
        char full[300];
        if (cJSON_IsString(bit) && bit->valuestring[0])
            snprintf(full, sizeof(full), "%s.%s", id, bit->valuestring);
        else
            snprintf(full, sizeof(full), "%s.bit%d", id, b);

        var_t *var = conn_add_var(ctx, full, TYPE_BOOL, dir);
        if (!var)
        {
            ok = false;
            continue;
        }
        pd->fields[pd->field_count++] =
            (field_t){var, (uint16_t)(offset + (uint32_t)b / 8), (int8_t)(b % 8), false};
    }
    return ok;
}

static bool parse_pd(trdp_conn_t *t, pd_t *pd, const cJSON *item, const char *ctx, dir_t dir)
{
    static const char *const keys[] = {"type", "var_id", "name", "enable_id", "comid", "dest_ip",
                                       "period_ms", "size", "variables", NULL};
    const char *id = NULL, *label = NULL;
    const cJSON *vars = NULL;
    pd->period_ms = 100;

    bool ok = json_check_keys(item, ctx, keys);
    ok &= json_get_string(item, "var_id", ctx, &id);
    ok &= json_get_string(item, "name", ctx, &label);
    bool comid_ok = json_need_uint(item, "comid", ctx, 1, UINT32_MAX, &pd->comid);
    ok &= comid_ok;
    ok &= get_ip(item, "dest_ip", ctx, dir == DIR_INPUT, &pd->dest_ip);
    ok &= json_get_uint(item, "period_ms", ctx, 1, 3600000, &pd->period_ms);
    ok &= get_enable(item, ctx, false, 1, &pd->enable);
    // Variables can still be checked after other errors, as long as the size is known.
    if (!json_need_uint(item, "size", ctx, 1, TRDP_MAX_PD_DATA_SIZE, &pd->size) ||
        !json_get_array(item, "variables", ctx, &vars))
        return false;

    make_label(pd->label, sizeof(pd->label), pd->comid, id ? id : label);

    // Pd telegrams of this direction parsed so far: inputs come first.
    for (size_t i = dir == DIR_INPUT ? 0 : t->pd_input_count; comid_ok && i < t->pd_count; i++)
    {
        if (&t->pds[i] != pd && t->pds[i].comid == pd->comid)
        {
            log_error("%s: comid %u is used twice", ctx, pd->comid);
            ok = false;
        }
    }

    // Upper bound: a bitset expands to at most 33 fields.
    size_t max_fields = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, vars)
        max_fields += cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(v, "bits")) ? 33 : 1;

    pd->buf = calloc(1, pd->size);
    pd->fields = calloc(max_fields ? max_fields : 1, sizeof(*pd->fields));
    uint8_t *used = calloc(1, pd->size);
    if (!pd->buf || !pd->fields || !used)
    {
        free(used);
        return false;
    }

    int index = 0;
    cJSON_ArrayForEach(v, vars)
    {
        char vctx[300];
        snprintf(vctx, sizeof(vctx), "%s.variables[%d]", ctx, index++);
        ok &= parse_pd_field(pd, v, vctx, dir, used);
    }
    free(used);
    return ok;
}

// --- Md config ---------------------------------------------------------------

static bool parse_padding(const cJSON *item, const char *ctx, uint32_t *pad)
{
    uint32_t bits = 0;
    if (!json_need_uint(item, "bits", ctx, 8, TRDP_MAX_MD_DATA_SIZE * 8, &bits))
        return false;
    if (bits % 8)
    {
        log_error("%s.bits: padding must be a multiple of 8 bits", ctx);
        return false;
    }
    *pad = bits / 8;
    return forbid(item, "var_id", ctx, "padding has no var_id");
}

static bool parse_structs(md_t *md, const cJSON *structs, const char *ctx)
{
    static const char *const keys[] = {"var_id", "name", "type", "bits", NULL};
    if (!structs)
        return true;
    if (!cJSON_IsObject(structs))
    {
        log_error("%s.structs: must be an object of struct definitions", ctx);
        return false;
    }

    md->struct_count = (size_t)cJSON_GetArraySize(structs);
    md->structs = calloc(md->struct_count ? md->struct_count : 1, sizeof(*md->structs));
    if (!md->structs)
        return false;

    bool ok = true;
    size_t si = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, structs)
    {
        struct_def_t *def = &md->structs[si++];
        def->name = s->string;
        char sctx[300];
        snprintf(sctx, sizeof(sctx), "%s.structs.%s", ctx, s->string);

        type_t ignored;
        if (type_parse(s->string, &ignored) || bitset_bits(s->string) || strcmp(s->string, "padding") == 0)
        {
            log_error("%s: '%s' is a built-in type name", sctx, s->string);
            ok = false;
            continue;
        }
        if (!cJSON_IsArray(s) || cJSON_GetArraySize(s) == 0)
        {
            log_error("%s: must be a non-empty array of members", sctx);
            ok = false;
            continue;
        }

        def->members = calloc((size_t)cJSON_GetArraySize(s), sizeof(*def->members));
        if (!def->members)
            return false;
        int index = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, s)
        {
            char mctx[340];
            snprintf(mctx, sizeof(mctx), "%s[%d]", sctx, index++);
            member_t *mb = &def->members[def->count++];
            const char *type_str = NULL, *label = NULL;
            if (!cJSON_IsObject(m) || !json_check_keys(m, mctx, keys) ||
                !json_get_string(m, "name", mctx, &label) ||
                !json_need_string(m, "type", mctx, &type_str))
            {
                ok = false;
                continue;
            }
            if (strcmp(type_str, "padding") == 0)
            {
                ok &= parse_padding(m, mctx, &mb->pad);
                def->size += mb->pad;
                continue;
            }
            if (!json_need_string(m, "var_id", mctx, &mb->id) || !json_need_type(m, mctx, &mb->type) ||
                !forbid(m, "bits", mctx, "only valid for padding"))
            {
                mb->id = NULL;
                ok = false;
                continue;
            }
            def->size += (uint32_t)type_size(mb->type);
        }
    }
    return ok;
}

static const struct_def_t *find_struct(const md_t *md, const char *name)
{
    for (size_t i = 0; i < md->struct_count; i++)
        if (strcmp(md->structs[i].name, name) == 0)
            return &md->structs[i];
    return NULL;
}

static bool parse_md_field(md_t *md, mfield_t *f, const cJSON *item, const char *ctx)
{
    static const char *const keys[] = {"var_id", "name", "type", "bits", "iterations_id",
                                       "iterations_type", "max_iterations", NULL};
    const char *type_str = NULL, *id = NULL, *label = NULL;

    if (!cJSON_IsObject(item))
    {
        log_error("%s: must be an object", ctx);
        return false;
    }
    if (!json_check_keys(item, ctx, keys) || !json_get_string(item, "name", ctx, &label) ||
        !json_need_string(item, "type", ctx, &type_str))
        return false;

    if (strcmp(type_str, "padding") == 0)
    {
        f->kind = F_PADDING;
        return parse_padding(item, ctx, &f->pad) &&
               forbid(item, "iterations_id", ctx, "only valid for arrays");
    }
    if (!json_need_string(item, "var_id", ctx, &id) || !forbid(item, "bits", ctx, "only valid for padding"))
        return false;

    char full[300];
    snprintf(full, sizeof(full), "%s.%s", md->id, id);
    const struct_def_t *def = find_struct(md, type_str);

    if (!def)
    {
        type_t type;
        if (!type_parse(type_str, &type))
        {
            log_error("%s.type: unknown type '%s' (expected: %s, padding, or a struct name)",
                      ctx, type_str, type_names());
            return false;
        }
        f->kind = F_SCALAR;
        bool ok = forbid(item, "iterations_id", ctx, "only valid for arrays of structs") &&
                  forbid(item, "max_iterations", ctx, "only valid for arrays of structs") &&
                  forbid(item, "iterations_type", ctx, "only valid for arrays of structs");
        return ok && (f->var = conn_add_var(ctx, full, type, md->dir)) != NULL;
    }

    // Array of structs, preceded on the wire by its entry count.
    const char *iter_id = NULL, *iter_type_str = "uint8";
    f->kind = F_ARRAY;
    f->def = def;
    f->max = 1;
    bool ok = json_need_string(item, "iterations_id", ctx, &iter_id);
    ok &= json_get_string(item, "iterations_type", ctx, &iter_type_str);
    ok &= json_get_uint(item, "max_iterations", ctx, 1, 65535, &f->max);
    type_t iter_type = TYPE_UINT8;
    if (ok && (!type_parse(iter_type_str, &iter_type) ||
               (iter_type != TYPE_UINT8 && iter_type != TYPE_UINT16 && iter_type != TYPE_UINT32)))
    {
        log_error("%s.iterations_type: must be uint8, uint16 or uint32", ctx);
        ok = false;
    }
    value_t fits;
    if (ok && !value_from_double(iter_type, f->max, &fits))
    {
        log_error("%s.max_iterations: %u entries cannot be counted in %s", ctx, f->max, type_name(iter_type));
        ok = false;
    }
    if (!ok)
        return false;

    char count_id[300];
    snprintf(count_id, sizeof(count_id), "%s.%s", md->id, iter_id);
    if (!(f->count = conn_add_var(ctx, count_id, iter_type, md->dir)))
        return false;

    f->items = calloc((size_t)f->max * def->count, sizeof(*f->items));
    if (!f->items)
        return false;
    for (uint32_t e = 0; e < f->max; e++)
    {
        for (size_t m = 0; m < def->count; m++)
        {
            const member_t *mb = &def->members[m];
            if (!mb->id)
                continue;
            char item_id[400];
            snprintf(item_id, sizeof(item_id), "%s.%s[%u].%s", md->id, id, e, mb->id);
            if (!(f->items[e * def->count + m] = conn_add_var(ctx, item_id, mb->type, md->dir)))
                return false;
        }
    }
    return true;
}

static bool parse_md(trdp_conn_t *t, md_t *md, const cJSON *item, const char *ctx, dir_t dir,
                     msg_t kind)
{
    static const char *const keys[] = {"type", "var_id", "name", "enable_id", "comid", "dest_ip",
                                       "reply_to", "timeout_ms", "structs", "variables", NULL};
    const char *label = NULL;
    const cJSON *vars = NULL;
    md->kind = kind;
    md->dir = dir;
    md->owner = t;
    md->timeout_ms = DEFAULT_REPLY_TIMEOUT_MS;

    bool sends = dir == DIR_INPUT && kind != MSG_MP; // Mn/Mr sent on request
    bool ok = json_check_keys(item, ctx, keys);
    ok &= json_get_string(item, "name", ctx, &label);
    bool id_ok = json_need_string(item, "var_id", ctx, &md->id);
    ok &= id_ok;
    ok &= json_need_uint(item, "comid", ctx, 1, UINT32_MAX, &md->comid);
    ok &= get_ip(item, "dest_ip", ctx, sends, &md->dest_ip);
    if (!sends)
        ok &= forbid(item, "dest_ip", ctx, "only valid for Mn and Mr inputs (replies go back to the requester)");

    if (kind == MSG_MP)
        ok &= json_need_string(item, "reply_to", ctx, &md->reply_to);
    else
        ok &= forbid(item, "reply_to", ctx, "only valid for Mp");

    if (kind == MSG_MR && dir == DIR_INPUT)
        ok &= json_get_uint(item, "timeout_ms", ctx, 1, 3600000, &md->timeout_ms);
    else
        ok &= forbid(item, "timeout_ms", ctx, "only valid for Mr inputs");

    // Inputs Mn/Mr: writing 1 sends once, then ammio resets it to 0. Otherwise 0 ignores the telegram.
    if (id_ok)
        ok &= get_enable(item, ctx, sends, sends ? 0 : 1, &md->enable);

    ok &= json_get_array(item, "variables", ctx, &vars);
    if (!id_ok || !ok)
        return false;

    make_label(md->label, sizeof(md->label), md->comid, md->id);

    if (dir == DIR_OUTPUT && kind != MSG_MP)
    {
        for (size_t i = 0; i < t->md_count; i++)
        {
            const md_t *o = &t->mds[i];
            if (o != md && o->dir == DIR_OUTPUT && o->kind != MSG_MP && o->comid == md->comid)
            {
                log_error("%s: comid %u is received twice", ctx, md->comid);
                ok = false;
            }
        }
    }

    ok &= parse_structs(md, cJSON_GetObjectItemCaseSensitive(item, "structs"), ctx);

    size_t n = (size_t)cJSON_GetArraySize(vars);
    md->fields = calloc(n ? n : 1, sizeof(*md->fields));
    if (!md->fields)
        return false;
    int index = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, vars)
    {
        char vctx[300];
        snprintf(vctx, sizeof(vctx), "%s.variables[%d]", ctx, index++);
        ok &= parse_md_field(md, &md->fields[md->field_count++], v, vctx);
    }
    if (!ok)
        return false;

    uint64_t size = 0;
    for (size_t i = 0; i < md->field_count; i++)
    {
        const mfield_t *f = &md->fields[i];
        size += f->kind == F_PADDING ? f->pad
              : f->kind == F_SCALAR  ? type_size(var_type(f->var))
              : type_size(var_type(f->count)) + (uint64_t)f->max * f->def->size;
    }
    if (size > TRDP_MAX_MD_DATA_SIZE)
    {
        log_error("%s: payload can reach %llu bytes, more than TRDP's %u", ctx,
                  (unsigned long long)size, TRDP_MAX_MD_DATA_SIZE);
        return false;
    }
    md->max_size = (uint32_t)size;
    return (md->buf = calloc(1, size ? size : 1)) != NULL;
}

// Pairs every Mp with the Mr it answers (or is answered by): inputs Mp reply to
// outputs Mr (the SUT asks), outputs Mp receive replies to inputs Mr (ammio asks).
static bool link_replies(trdp_conn_t *t)
{
    bool ok = true;
    for (size_t i = 0; i < t->md_count; i++)
    {
        md_t *mp = &t->mds[i];
        if (mp->kind != MSG_MP)
            continue;
        md_t *mr = NULL;
        for (size_t j = 0; j < t->md_count && !mr; j++)
            if (t->mds[j].kind == MSG_MR && t->mds[j].dir != mp->dir && strcmp(t->mds[j].id, mp->reply_to) == 0)
                mr = &t->mds[j];
        if (!mr)
        {
            log_error("%s: Mp '%s': reply_to '%s' is not an Mr in %s", t->conn, mp->id, mp->reply_to,
                      mp->dir == DIR_INPUT ? "outputs" : "inputs");
            ok = false;
        }
        else if (mr->partner)
        {
            log_error("%s: Mr '%s' has two replies ('%s' and '%s')", t->conn, mr->id, mr->partner->id, mp->id);
            ok = false;
        }
        else
        {
            mr->partner = mp;
            mp->partner = mr;
        }
    }
    for (size_t i = 0; i < t->md_count; i++)
    {
        const md_t *mr = &t->mds[i];
        if (mr->kind == MSG_MR && !mr->partner && ok)
        {
            log_error("%s: Mr '%s' needs an Mp in %s with \"reply_to\": \"%s\"", t->conn, mr->id,
                      mr->dir == DIR_INPUT ? "outputs" : "inputs", mr->id);
            ok = false;
        }
    }
    return ok;
}

static void trdp_destroy(void *self)
{
    trdp_conn_t *t = self;
    instance_remove(t);
    for (size_t i = 0; i < t->pd_count; i++)
    {
        free(t->pds[i].buf);
        free(t->pds[i].fields);
    }
    for (size_t i = 0; i < t->md_count; i++)
    {
        md_t *md = &t->mds[i];
        for (size_t f = 0; f < md->field_count; f++)
            free(md->fields[f].items);
        for (size_t s = 0; s < md->struct_count; s++)
            free(md->structs[s].members);
        free(md->structs);
        free(md->fields);
        free(md->buf);
    }
    free(t->pds);
    free(t->mds);
    free(t);
}

static bool get_msg_type(const cJSON *item, const char *ctx, msg_t *out)
{
    const char *str = "Pd";
    if (!json_get_string(item, "type", ctx, &str))
        return false;
    for (int k = 0; k < 4; k++)
    {
        if (strcmp(str, msg_names[k]) == 0)
        {
            *out = (msg_t)k;
            return true;
        }
    }
    log_error("%s.type: unknown telegram type '%s' (expected: Pd, Mn, Mr, Mp)", ctx, str);
    return false;
}

static bool get_topo(trdp_conn_t *t, const cJSON *spec, const char *key, var_t **out)
{
    const char *id = NULL;
    if (!json_get_string(spec, key, t->conn, &id))
        return false;
    return !id || (*out = conn_add_var(t->conn, id, TYPE_UINT32, DIR_INPUT)) != NULL;
}

static void *trdp_create(const char *conn, const cJSON *spec, uint32_t *period_ms)
{
    static const char *const keys[] = {"protocol", "local_ip", "etb_topo_cnt_id", "op_trn_topo_cnt_id",
                                       "inputs", "outputs", NULL};

    trdp_conn_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->conn = conn;
    *period_ms = 0; // cycle() waits on the sockets itself

    const cJSON *inputs = NULL, *outputs = NULL;
    bool ok = json_check_keys(spec, conn, keys);
    ok &= get_ip(spec, "local_ip", conn, false, &t->own_ip);
    const char *owner = ip_owner(t->own_ip);
    if (owner)
    {
        log_error("%s.local_ip: already used by '%s'; put all telegrams of one local IP "
                  "in one connection", conn, owner);
        ok = false;
    }
    ok &= get_topo(t, spec, "etb_topo_cnt_id", &t->etb_topo);
    ok &= get_topo(t, spec, "op_trn_topo_cnt_id", &t->op_trn_topo);
    ok &= json_get_array(spec, "inputs", conn, &inputs);
    ok &= json_get_array(spec, "outputs", conn, &outputs);

    // Keep validating after an error so that one run reports every problem.
    size_t total = (size_t)cJSON_GetArraySize(inputs) + (size_t)cJSON_GetArraySize(outputs);
    if (total == 0)
    {
        log_error("%s: no inputs or outputs defined", conn);
        ok = false;
    }
    else if (!(t->pds = calloc(total, sizeof(*t->pds))) || !(t->mds = calloc(total, sizeof(*t->mds))))
    {
        ok = false;
    }

    const cJSON *lists[2] = {inputs, outputs};
    for (int d = 0; t->pds && t->mds && d < 2; d++)
    {
        dir_t dir = d == 0 ? DIR_INPUT : DIR_OUTPUT;
        int index = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, lists[d])
        {
            char ctx[256];
            snprintf(ctx, sizeof(ctx), "%s.%s[%d]", conn, d == 0 ? "inputs" : "outputs", index++);
            msg_t kind = MSG_PD;
            if (!cJSON_IsObject(item))
            {
                log_error("%s: must be an object", ctx);
                ok = false;
            }
            else if (!get_msg_type(item, ctx, &kind))
                ok = false;
            else if (kind == MSG_PD)
                ok &= parse_pd(t, &t->pds[t->pd_count++], item, ctx, dir); // counted even on failure, for destroy()
            else
                ok &= parse_md(t, &t->mds[t->md_count++], item, ctx, dir, kind);
        }
        if (d == 0)
            t->pd_input_count = t->pd_count;
    }
    ok = ok && link_replies(t);

    if (!ok)
    {
        trdp_destroy(t);
        return NULL;
    }
    if (instance_count < MAX_INSTANCES)
        instances[instance_count++] = t;
    return t;
}

// --- runtime -----------------------------------------------------------------

static void md_callback(void *ref, TRDP_APP_SESSION_T app, const TRDP_MD_INFO_T *msg, UINT8 *data,
                        UINT32 size)
{
    (void)ref;
    md_t *md = msg ? (md_t *)msg->pUserRef : NULL;
    if (!md)
        return;
    const char *conn = md->owner->conn;

    if (msg->resultCode != TRDP_NO_ERR)
    {
        // For a request, md is the Mp output that should have received the reply.
        if (msg->resultCode == TRDP_REPLYTO_ERR || msg->resultCode == TRDP_TIMEOUT_ERR)
            log_warn("%s: no reply to %s within %u ms", conn, md->partner ? md->partner->label : md->label,
                     md->partner ? md->partner->timeout_ms : 0);
        else
            log_debug("%s: %s: message data error %d", conn, md->label, msg->resultCode);
        return;
    }

    if (enabled(md->enable))
    {
        md_deserialize(md, data, data ? size : 0);
        log_debug("%s: received %s %s (%u bytes)", conn, msg_names[md->kind], md->label, size);
    }

    if (msg->msgType == TRDP_MSG_MR && md->partner && enabled(md->partner->enable))
    {
        md_t *reply = md->partner; // input Mp
        UINT32 n = md_serialize(reply);
        TRDP_ERR_T rc = tlm_reply(app, &msg->sessionId, reply->comid, 0, NULL, reply->buf, n, NULL);
        if (rc != TRDP_NO_ERR)
            log_warn("%s: cannot reply to %s with %s (error %d)", conn, md->label, reply->label, rc);
        else
            log_debug("%s: replied to %s with %s", conn, md->label, reply->label);
    }
}

static TRDP_ERR_T pd_publish(trdp_conn_t *t, pd_t *pd)
{
    TRDP_PUB_T pub = NULL;
    pd_encode(pd);
    TRDP_ERR_T rc = tlp_publish(t->session, &pub, NULL, NULL, 0, pd->comid, 0, 0, t->own_ip, pd->dest_ip,
                                pd->period_ms * 1000, 0, TRDP_FLAGS_DEFAULT, NULL, pd->buf, pd->size);
    pd->handle = rc == TRDP_NO_ERR ? pub : NULL;
    return rc;
}

static void trdp_disconnect(void *self)
{
    trdp_conn_t *t = self;
    for (size_t i = 0; i < t->pd_count; i++)
    {
        pd_t *pd = &t->pds[i];
        if (!pd->handle)
            continue;
        if (i < t->pd_input_count)
            tlp_unpublish(t->session, (TRDP_PUB_T)pd->handle);
        else
            tlp_unsubscribe(t->session, (TRDP_SUB_T)pd->handle);
        pd->handle = NULL;
    }
    for (size_t i = 0; i < t->md_count; i++)
    {
        if (t->mds[i].listener)
            tlm_delListener(t->session, t->mds[i].listener);
        t->mds[i].listener = NULL;
    }
    tlc_closeSession(t->session);
    t->session = NULL;
    lib_release();
}

static int trdp_connect(void *self, char *err, size_t err_len)
{
    trdp_conn_t *t = self;

    TRDP_ERR_T rc = lib_acquire();
    if (rc != TRDP_NO_ERR)
    {
        snprintf(err, err_len, "TRDP initialisation failed (error %d)", rc);
        return -1;
    }
    rc = tlc_openSession(&t->session, t->own_ip, 0, NULL, NULL, NULL, NULL);
    if (rc != TRDP_NO_ERR)
    {
        snprintf(err, err_len, "cannot open a session on the local IP (error %d)", rc);
        t->session = NULL;
        lib_release();
        return -1;
    }
    t->topo_sent = false;

    const char *what = NULL;
    const char *label = NULL;
    for (size_t i = 0; i < t->pd_count && !what; i++)
    {
        pd_t *pd = &t->pds[i];
        label = pd->label;
        if (i < t->pd_input_count)
        {
            if (enabled(pd->enable) && (rc = pd_publish(t, pd)) != TRDP_NO_ERR)
                what = "publish";
        }
        else
        {
            TRDP_SUB_T sub = NULL;
            rc = tlp_subscribe(t->session, &sub, NULL, NULL, 0, pd->comid, 0, 0, 0, 0, pd->dest_ip,
                               TRDP_FLAGS_DEFAULT, NULL, pd->period_ms * 1000 * TIMEOUT_PERIODS,
                               TRDP_TO_KEEP_LAST_VALUE);
            pd->handle = rc == TRDP_NO_ERR ? sub : NULL;
            pd->rx = RX_WAITING;
            if (rc != TRDP_NO_ERR)
                what = "subscribe to";
        }
    }
    for (size_t i = 0; i < t->md_count && !what; i++)
    {
        md_t *md = &t->mds[i];
        if (md->dir != DIR_OUTPUT || md->kind == MSG_MP) // replies arrive through the request
            continue;
        label = md->label;
        rc = tlm_addListener(t->session, &md->listener, md, md_callback, TRUE, md->comid, 0, 0, 0, 0, 0,
                             TRDP_FLAGS_DEFAULT, NULL, NULL);
        if (rc != TRDP_NO_ERR)
        {
            md->listener = NULL;
            what = "listen for";
        }
    }

    if (what)
    {
        snprintf(err, err_len, "cannot %s %s (error %d)", what, label, rc);
        trdp_disconnect(t);
        return -1;
    }
    return 0;
}

static void pd_receive(trdp_conn_t *t, pd_t *pd)
{
    TRDP_PD_INFO_T info;
    UINT32 received = pd->size;
    TRDP_ERR_T rc = tlp_get(t->session, (TRDP_SUB_T)pd->handle, &info, pd->buf, &received);

    if (rc == TRDP_NO_ERR)
    {
        if (pd->rx != RX_OK)
            log_info("%s: receiving %s", t->conn, pd->label);
        pd->rx = RX_OK;

        char problem[96];
        snprintf(problem, sizeof(problem), "received %u bytes, expected %u", received, pd->size);
        conn_report(t->conn, pd->label, &pd->size_mismatch, received != pd->size ? problem : NULL);
        if (enabled(pd->enable))
            pd_decode(pd, received);
    }
    else if (rc == TRDP_TIMEOUT_ERR && pd->rx == RX_OK)
    {
        log_warn("%s: %s: no data for %u ms, keeping last values", t->conn, pd->label,
                 pd->period_ms * TIMEOUT_PERIODS);
        pd->rx = RX_TIMEOUT;
    }
}

static void pd_send(trdp_conn_t *t, pd_t *pd)
{
    bool on = enabled(pd->enable);
    if (on && !pd->handle)
    {
        TRDP_ERR_T rc = pd_publish(t, pd);
        if (rc == TRDP_NO_ERR)
            log_info("%s: publishing %s", t->conn, pd->label);
        else
            log_warn("%s: cannot publish %s (error %d)", t->conn, pd->label, rc);
    }
    else if (!on && pd->handle)
    {
        tlp_unpublish(t->session, (TRDP_PUB_T)pd->handle);
        pd->handle = NULL;
        log_info("%s: stopped publishing %s", t->conn, pd->label);
    }
    if (pd->handle)
    {
        pd_encode(pd);
        tlp_put(t->session, (TRDP_PUB_T)pd->handle, pd->buf, pd->size);
    }
}

// Inputs Mn/Mr are sent once each time the client sets their enable flag.
static void md_send(trdp_conn_t *t, md_t *md)
{
    if (var_get(md->enable, NULL).u8 == 0)
        return;
    value_t off = {.u8 = 0};
    var_set(md->enable, off);

    UINT32 size = md_serialize(md);
    TRDP_ERR_T rc;
    if (md->kind == MSG_MN)
    {
        rc = tlm_notify(t->session, NULL, NULL, md->comid, 0, 0, 0, md->dest_ip, TRDP_FLAGS_DEFAULT, NULL,
                        md->buf, size, NULL, NULL);
    }
    else
    {
        TRDP_UUID_T session_id;
        rc = tlm_request(t->session, md->partner, md_callback, &session_id, md->comid, 0, 0, 0,
                         md->dest_ip, TRDP_FLAGS_DEFAULT, 1, md->timeout_ms * 1000, NULL, md->buf, size,
                         NULL, NULL);
    }
    if (rc != TRDP_NO_ERR)
        log_warn("%s: cannot send %s %s (error %d)", t->conn, msg_names[md->kind], md->label, rc);
    else
        log_debug("%s: sent %s %s (%u bytes)", t->conn, msg_names[md->kind], md->label, size);
}

static void update_topology(trdp_conn_t *t)
{
    if (t->etb_topo)
    {
        uint32_t v = var_get(t->etb_topo, NULL).u32;
        if (!t->topo_sent || v != t->etb_sent)
            tlc_setETBTopoCount(t->session, t->etb_sent = v);
    }
    if (t->op_trn_topo)
    {
        uint32_t v = var_get(t->op_trn_topo, NULL).u32;
        if (!t->topo_sent || v != t->op_trn_sent)
            tlc_setOpTrainTopoCount(t->session, t->op_trn_sent = v);
    }
    t->topo_sent = true;
}

static int trdp_cycle(void *self, char *err, size_t err_len)
{
    (void)err, (void)err_len;
    trdp_conn_t *t = self;

    update_topology(t);
    for (size_t i = 0; i < t->pd_input_count; i++)
        pd_send(t, &t->pds[i]);
    for (size_t i = 0; i < t->md_count; i++)
        if (t->mds[i].dir == DIR_INPUT && t->mds[i].kind != MSG_MP)
            md_send(t, &t->mds[i]);

    TRDP_FDS_T rfds;
    TRDP_SOCK_T highest = 0; // TRDP_SOCK_T, not INT32: it is 64-bit on Win64
    TRDP_TIME_T tv = {0, 0}, max = {0, MAX_SELECT_US};
    VOS_FD_ZERO(&rfds);
    tlc_getInterval(t->session, &tv, &rfds, &highest);
    if (vos_cmpTime(&tv, &max) > 0)
        tv = max;
    INT32 ready = vos_select(highest, &rfds, NULL, NULL, &tv);
    tlc_process(t->session, &rfds, &ready); // also runs the message data callbacks

    for (size_t i = t->pd_input_count; i < t->pd_count; i++)
        pd_receive(t, &t->pds[i]);
    return 0;
}

const protocol_t trdp_protocol = {
    .name = "trdp",
    .create = trdp_create,
    .connect = trdp_connect,
    .cycle = trdp_cycle,
    .disconnect = trdp_disconnect,
    .destroy = trdp_destroy,
};
