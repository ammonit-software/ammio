// TRDP process data (IEC 61375-2-3). Inputs are published telegrams,
// outputs are subscribed telegrams. Values are big-endian; bitset bits are
// numbered from the most significant bit of the first byte (bit 0 = 0x80).

#include "../json.h"
#include "../log.h"
#include "../protocol.h"

#include "trdp_if_light.h"
#include "vos_sock.h"
#include "vos_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define MAX_SELECT_US 10000 // upper bound of one cycle, i.e. input latency
#define TIMEOUT_PERIODS 3   // an output times out after 3 missed periods

typedef struct
{
    var_t *var;
    uint16_t offset; // byte offset in the payload
    int8_t bit;      // -1: whole value; 0..7: bit of a bitset (0 = MSB)
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
    void *handle; // TRDP_PUB_T or TRDP_SUB_T
    rx_state_t rx;
    bool size_mismatch;
} telegram_t;

typedef struct
{
    const char *conn;
    TRDP_IP_ADDR_T own_ip;
    TRDP_APP_SESSION_T session;
    telegram_t *tgs; // inputs first, then outputs
    size_t input_count, count;
} trdp_conn_t;

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
// Two TRDP sessions on the same address both bind UDP port 17224, and each
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

// --- payload <-> variables ---------------------------------------------------

static void encode(telegram_t *tg)
{
    memset(tg->buf, 0, tg->size);
    for (size_t i = 0; i < tg->field_count; i++)
    {
        const field_t *f = &tg->fields[i];
        value_t v = var_get(f->var, NULL);
        if (f->bit < 0)
            value_to_be(var_type(f->var), v, tg->buf + f->offset);
        else if (v.b)
            tg->buf[f->offset] |= (uint8_t)(0x80u >> f->bit);
    }
}

static void decode(telegram_t *tg, uint32_t received)
{
    for (size_t i = 0; i < tg->field_count; i++)
    {
        const field_t *f = &tg->fields[i];
        type_t type = var_type(f->var);
        if (f->offset + (f->bit < 0 ? type_size(type) : 1) > received)
            continue;

        value_t v;
        if (f->bit < 0)
            v = value_from_be(type, tg->buf + f->offset);
        else
            v.b = (tg->buf[f->offset] & (0x80u >> f->bit)) != 0;
        var_set(f->var, v);
    }
}

// --- config ------------------------------------------------------------------

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

// Marks payload bits as used; reports overlaps and fields past the end.
static bool claim(uint8_t *used, uint32_t size, const char *ctx, const char *name,
                  uint32_t offset, uint32_t bytes, uint8_t mask)
{
    if (offset + bytes > size)
    {
        log_error("%s: '%s' (bytes %u..%u) does not fit in size %u", ctx, name, offset,
                  offset + bytes - 1, size);
        return false;
    }
    for (uint32_t i = offset; i < offset + bytes; i++)
    {
        if (used[i] & mask)
        {
            log_error("%s: '%s' overlaps another variable at byte %u", ctx, name, i);
            return false;
        }
        used[i] |= mask;
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

static bool parse_field(telegram_t *tg, const cJSON *item, const char *ctx, dir_t dir,
                        uint8_t *used)
{
    static const char *const keys[] = {"name", "offset", "type", "bits", NULL};
    const char *name = NULL, *type_str = NULL;
    uint32_t offset = 0;

    if (!cJSON_IsObject(item))
    {
        log_error("%s: must be an object", ctx);
        return false;
    }
    bool ok = json_check_keys(item, ctx, keys);
    ok &= json_need_string(item, "name", ctx, &name);
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
        if (bits)
        {
            log_error("%s.bits: only valid for bitset types", ctx);
            return false;
        }
        if (!claim(used, tg->size, ctx, name, offset, (uint32_t)type_size(type), 0xFF))
            return false;
        var_t *var = conn_add_var(ctx, name, type, dir);
        if (!var)
            return false;
        tg->fields[tg->field_count++] = (field_t){var, (uint16_t)offset, -1};
        return true;
    }

    // Bitset: one bool variable "<name>.<bit name>" per named bit.
    if (!cJSON_IsArray(bits) || cJSON_GetArraySize(bits) > nbits)
    {
        log_error("%s.bits: must be an array of up to %d bit names (\"\" or null = unused)",
                  ctx, nbits);
        return false;
    }
    int b = 0;
    const cJSON *bit;
    cJSON_ArrayForEach(bit, bits)
    {
        int index = b++;
        if (cJSON_IsNull(bit) || (cJSON_IsString(bit) && bit->valuestring[0] == '\0'))
            continue;
        if (!cJSON_IsString(bit))
        {
            log_error("%s.bits[%d]: must be a string or null", ctx, index);
            ok = false;
            continue;
        }

        char full[300];
        snprintf(full, sizeof(full), "%s.%s", name, bit->valuestring);
        uint32_t byte = offset + (uint32_t)index / 8;
        uint8_t mask = (uint8_t)(0x80u >> (index % 8));
        var_t *var = NULL;
        if (claim(used, tg->size, ctx, full, byte, 1, mask) &&
            (var = conn_add_var(ctx, full, TYPE_BOOL, dir)))
            tg->fields[tg->field_count++] = (field_t){var, (uint16_t)byte, (int8_t)(index % 8)};
        else
            ok = false;
    }
    return ok;
}

static bool parse_telegram(trdp_conn_t *t, telegram_t *tg, const cJSON *item, const char *ctx,
                           dir_t dir)
{
    static const char *const keys[] = {
        "name", "comid", "dest_ip", "period_ms", "size", "variables", NULL};
    const char *name = NULL;
    const cJSON *vars = NULL;
    tg->period_ms = 100;

    if (!cJSON_IsObject(item))
    {
        log_error("%s: must be an object", ctx);
        return false;
    }
    bool ok = json_check_keys(item, ctx, keys);
    ok &= json_get_string(item, "name", ctx, &name);
    bool comid_ok = json_need_uint(item, "comid", ctx, 1, UINT32_MAX, &tg->comid);
    ok &= comid_ok;
    ok &= get_ip(item, "dest_ip", ctx, dir == DIR_INPUT, &tg->dest_ip);
    ok &= json_get_uint(item, "period_ms", ctx, 1, 3600000, &tg->period_ms);
    // Variables can still be checked after other errors, as long as the size is known.
    if (!json_need_uint(item, "size", ctx, 1, TRDP_MAX_PD_DATA_SIZE, &tg->size) ||
        !json_get_array(item, "variables", ctx, &vars))
        return false;

    if (name)
        snprintf(tg->label, sizeof(tg->label), "comid %u (%s)", tg->comid, name);
    else
        snprintf(tg->label, sizeof(tg->label), "comid %u", tg->comid);

    // Telegrams of this direction parsed so far: inputs start at 0, outputs after the inputs.
    for (size_t i = dir == DIR_INPUT ? 0 : t->input_count; comid_ok && i < t->count; i++)
    {
        if (t->tgs[i].comid == tg->comid)
        {
            log_error("%s: comid %u is used twice", ctx, tg->comid);
            ok = false;
        }
    }

    // Upper bound: a bitset expands to at most 32 fields.
    size_t max_fields = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, vars)
        max_fields += cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(v, "bits")) ? 32 : 1;

    tg->buf = calloc(1, tg->size);
    tg->fields = calloc(max_fields ? max_fields : 1, sizeof(*tg->fields));
    uint8_t *used = calloc(1, tg->size);
    if (!tg->buf || !tg->fields || !used)
    {
        free(used);
        return false;
    }

    int index = 0;
    cJSON_ArrayForEach(v, vars)
    {
        char vctx[300];
        snprintf(vctx, sizeof(vctx), "%s.variables[%d]", ctx, index++);
        ok &= parse_field(tg, v, vctx, dir, used);
    }
    free(used);
    return ok;
}

static void trdp_destroy(void *self)
{
    trdp_conn_t *t = self;
    instance_remove(t);
    for (size_t i = 0; i < t->count; i++)
    {
        free(t->tgs[i].buf);
        free(t->tgs[i].fields);
    }
    free(t->tgs);
    free(t);
}

static void *trdp_create(const char *conn, const cJSON *spec, uint32_t *period_ms)
{
    static const char *const keys[] = {"protocol", "local_ip", "inputs", "outputs", NULL};

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
    ok &= json_get_array(spec, "inputs", conn, &inputs);
    ok &= json_get_array(spec, "outputs", conn, &outputs);

    // Keep validating after an error so that one run reports every problem.
    size_t total = (size_t)cJSON_GetArraySize(inputs) + (size_t)cJSON_GetArraySize(outputs);
    if (total == 0)
    {
        log_error("%s: no inputs or outputs defined", conn);
        ok = false;
    }
    else if (!(t->tgs = calloc(total, sizeof(*t->tgs))))
    {
        ok = false;
    }

    const cJSON *lists[2] = {inputs, outputs};
    for (int d = 0; t->tgs && d < 2; d++)
    {
        int index = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, lists[d])
        {
            char ctx[256];
            snprintf(ctx, sizeof(ctx), "%s.%s[%d]", conn, d == 0 ? "inputs" : "outputs", index++);
            // Count the telegram even on failure so destroy() frees it.
            telegram_t *tg = &t->tgs[t->count];
            ok &= parse_telegram(t, tg, item, ctx, d == 0 ? DIR_INPUT : DIR_OUTPUT);
            t->count++;
        }
        if (d == 0)
            t->input_count = t->count;
    }

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

static void trdp_disconnect(void *self)
{
    trdp_conn_t *t = self;
    for (size_t i = 0; i < t->count; i++)
    {
        telegram_t *tg = &t->tgs[i];
        if (!tg->handle)
            continue;
        if (i < t->input_count)
            tlp_unpublish(t->session, (TRDP_PUB_T)tg->handle);
        else
            tlp_unsubscribe(t->session, (TRDP_SUB_T)tg->handle);
        tg->handle = NULL;
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

    for (size_t i = 0; i < t->count; i++)
    {
        telegram_t *tg = &t->tgs[i];
        if (i < t->input_count)
        {
            TRDP_PUB_T pub = NULL;
            encode(tg);
            rc = tlp_publish(t->session, &pub, NULL, NULL, 0, tg->comid, 0, 0, t->own_ip,
                             tg->dest_ip, tg->period_ms * 1000, 0, TRDP_FLAGS_DEFAULT, NULL,
                             tg->buf, tg->size);
            tg->handle = pub;
        }
        else
        {
            TRDP_SUB_T sub = NULL;
            rc = tlp_subscribe(t->session, &sub, NULL, NULL, 0, tg->comid, 0, 0, 0, 0,
                               tg->dest_ip, TRDP_FLAGS_DEFAULT, NULL,
                               tg->period_ms * 1000 * TIMEOUT_PERIODS, TRDP_TO_KEEP_LAST_VALUE);
            tg->handle = sub;
            tg->rx = RX_WAITING;
        }
        if (rc != TRDP_NO_ERR)
        {
            snprintf(err, err_len, "cannot %s %s (error %d)",
                     i < t->input_count ? "publish" : "subscribe to", tg->label, rc);
            tg->handle = NULL;
            trdp_disconnect(t);
            return -1;
        }
    }
    return 0;
}

static void receive(trdp_conn_t *t, telegram_t *tg)
{
    TRDP_PD_INFO_T info;
    UINT32 received = tg->size;
    TRDP_ERR_T rc = tlp_get(t->session, (TRDP_SUB_T)tg->handle, &info, tg->buf, &received);

    if (rc == TRDP_NO_ERR)
    {
        if (tg->rx != RX_OK)
            log_info("%s: receiving %s", t->conn, tg->label);
        tg->rx = RX_OK;

        char problem[96];
        snprintf(problem, sizeof(problem), "received %u bytes, expected %u", received, tg->size);
        conn_report(t->conn, tg->label, &tg->size_mismatch, received != tg->size ? problem : NULL);
        decode(tg, received);
    }
    else if (rc == TRDP_TIMEOUT_ERR && tg->rx == RX_OK)
    {
        log_warn("%s: %s: no data for %u ms, keeping last values", t->conn, tg->label,
                 tg->period_ms * TIMEOUT_PERIODS);
        tg->rx = RX_TIMEOUT;
    }
}

static int trdp_cycle(void *self, char *err, size_t err_len)
{
    (void)err, (void)err_len;
    trdp_conn_t *t = self;

    for (size_t i = 0; i < t->input_count; i++)
    {
        telegram_t *tg = &t->tgs[i];
        encode(tg);
        tlp_put(t->session, (TRDP_PUB_T)tg->handle, tg->buf, tg->size);
    }

    TRDP_FDS_T rfds;
    TRDP_SOCK_T highest = 0; // TRDP_SOCK_T, not INT32: it is 64-bit on Win64
    TRDP_TIME_T tv = {0, 0}, max = {0, MAX_SELECT_US};
    VOS_FD_ZERO(&rfds);
    tlc_getInterval(t->session, &tv, &rfds, &highest);
    if (vos_cmpTime(&tv, &max) > 0)
        tv = max;
    INT32 ready = vos_select(highest, &rfds, NULL, NULL, &tv);
    tlc_process(t->session, &rfds, &ready);

    for (size_t i = t->input_count; i < t->count; i++)
        receive(t, &t->tgs[i]);
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
