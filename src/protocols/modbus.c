// Modbus TCP / RTU master. Inputs are written to holding registers,
// outputs are read from holding registers, once per period.

#include "../json.h"
#include "../log.h"
#include "../protocol.h"

#include <modbus.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    var_t *var;
    uint16_t address;
    uint16_t count; // registers used by the type
    bool failing;
} reg_t;

typedef struct
{
    const char *conn;
    modbus_t *ctx;
    bool little_words; // word order for multi-register values
    reg_t *regs;       // inputs first, then outputs
    size_t input_count, count;
} modbus_conn_t;

static uint16_t reg_count(type_t type)
{
    return type_size(type) <= 2 ? 1 : (uint16_t)(type_size(type) / 2);
}

// --- value <-> registers -----------------------------------------------------

static void encode(const modbus_conn_t *m, const reg_t *r, uint16_t *regs)
{
    type_t type = var_type(r->var);
    value_t v = var_get(r->var, NULL);

    switch (type)
    {
        case TYPE_BOOL:  regs[0] = v.b ? 1 : 0; return;
        case TYPE_UINT8: regs[0] = v.u8; return;
        case TYPE_INT8:  regs[0] = (uint16_t)(int16_t)v.i8; return; // sign-extended
        default: break;
    }

    uint8_t bytes[8];
    value_to_be(type, v, bytes);
    for (uint16_t i = 0; i < r->count; i++)
    {
        uint16_t word = (uint16_t)(bytes[2 * i] << 8 | bytes[2 * i + 1]);
        regs[m->little_words ? r->count - 1 - i : i] = word;
    }
}

static value_t decode(const modbus_conn_t *m, const reg_t *r, const uint16_t *regs)
{
    type_t type = var_type(r->var);
    value_t v = {0};

    switch (type)
    {
        case TYPE_BOOL:  v.b = regs[0] != 0; return v;
        case TYPE_UINT8: v.u8 = (uint8_t)regs[0]; return v;
        case TYPE_INT8:  v.i8 = (int8_t)(uint8_t)regs[0]; return v;
        default: break;
    }

    uint8_t bytes[8];
    for (uint16_t i = 0; i < r->count; i++)
    {
        uint16_t word = regs[m->little_words ? r->count - 1 - i : i];
        bytes[2 * i] = (uint8_t)(word >> 8);
        bytes[2 * i + 1] = (uint8_t)word;
    }
    return value_from_be(type, bytes);
}

// --- config ------------------------------------------------------------------

static bool parse_regs(modbus_conn_t *m, const cJSON *arr, const char *key, dir_t dir)
{
    static const char *const keys[] = {"var_id", "name", "address", "type", NULL};
    bool ok = true;
    int index = 0;
    const cJSON *item;

    cJSON_ArrayForEach(item, arr)
    {
        char ctx[256];
        snprintf(ctx, sizeof(ctx), "%s.%s[%d]", m->conn, key, index++);

        const char *id = NULL, *label = NULL;
        uint32_t address = 0;
        type_t type = TYPE_UINT16;
        if (!cJSON_IsObject(item))
        {
            log_error("%s: must be an object", ctx);
            ok = false;
            continue;
        }
        bool item_ok = json_check_keys(item, ctx, keys);
        item_ok &= json_need_string(item, "var_id", ctx, &id);
        item_ok &= json_get_string(item, "name", ctx, &label); // optional description
        (void)label;
        item_ok &= json_need_uint(item, "address", ctx, 0, 65535, &address);
        item_ok &= json_need_type(item, ctx, &type);
        if (!item_ok)
        {
            ok = false;
            continue;
        }

        uint16_t count = reg_count(type);
        if (address + count > 65536)
        {
            log_error("%s: %s at address %u goes past register 65535", ctx, type_name(type), address);
            ok = false;
            continue;
        }

        var_t *var = conn_add_var(ctx, id, type, dir);
        if (!var)
        {
            ok = false;
            continue;
        }
        m->regs[m->count++] = (reg_t){.var = var, .address = (uint16_t)address, .count = count};
    }
    return ok;
}

static bool check_overlaps(const modbus_conn_t *m)
{
    bool ok = true;
    for (size_t i = 0; i < m->count; i++)
    {
        for (size_t j = i + 1; j < m->count; j++)
        {
            const reg_t *a = &m->regs[i], *b = &m->regs[j];
            if (a->address < b->address + b->count && b->address < a->address + a->count)
            {
                log_error("%s: '%s' (registers %u..%u) overlaps '%s' (registers %u..%u)", m->conn,
                          var_id(a->var), a->address, a->address + a->count - 1,
                          var_id(b->var), b->address, b->address + b->count - 1);
                ok = false;
            }
        }
    }
    return ok;
}

static void modbus_destroy(void *self)
{
    modbus_conn_t *m = self;
    if (m->ctx)
        modbus_free(m->ctx);
    free(m->regs);
    free(m);
}

static modbus_t *new_context(const char *conn, const cJSON *spec)
{
    const char *host = NULL, *device = NULL;
    bool ok = json_get_string(spec, "host", conn, &host);
    ok &= json_get_string(spec, "device", conn, &device);
    if (!ok)
        return NULL;
    if (!host == !device)
    {
        log_error("%s: set either 'host' (Modbus TCP) or 'device' (Modbus RTU)", conn);
        return NULL;
    }

    static const char *const serial_keys[] = {"baud", "parity", "data_bits", "stop_bits"};
    for (size_t i = 0; i < 4; i++)
    {
        if (host && cJSON_GetObjectItemCaseSensitive(spec, serial_keys[i]))
        {
            log_error("%s.%s: only valid with 'device' (Modbus RTU)", conn, serial_keys[i]);
            ok = false;
        }
    }
    if (device && cJSON_GetObjectItemCaseSensitive(spec, "port"))
    {
        log_error("%s.port: only valid with 'host' (Modbus TCP)", conn);
        ok = false;
    }

    modbus_t *ctx = NULL;
    if (host)
    {
        uint32_t port = 502;
        ok &= json_get_uint(spec, "port", conn, 1, 65535, &port);
        if (!ok)
            return NULL;
        char service[8];
        snprintf(service, sizeof(service), "%u", port);
        ctx = modbus_new_tcp_pi(host, service);
    }
    else
    {
        uint32_t baud = 9600, data_bits = 8, stop_bits = 1;
        const char *parity = "N";
        ok &= json_get_uint(spec, "baud", conn, 1, 4000000, &baud);
        ok &= json_get_uint(spec, "data_bits", conn, 5, 8, &data_bits);
        ok &= json_get_uint(spec, "stop_bits", conn, 1, 2, &stop_bits);
        ok &= json_get_string(spec, "parity", conn, &parity);
        if (ok && strcmp(parity, "N") != 0 && strcmp(parity, "E") != 0 && strcmp(parity, "O") != 0)
        {
            log_error("%s.parity: must be \"N\", \"E\" or \"O\"", conn);
            ok = false;
        }
        if (!ok)
            return NULL;
        ctx = modbus_new_rtu(device, (int)baud, parity[0], (int)data_bits, (int)stop_bits);
    }

    if (!ctx)
        log_error("%s: %s", conn, modbus_strerror(errno));
    return ctx;
}

static void *modbus_create(const char *conn, const cJSON *spec, uint32_t *period_ms)
{
    static const char *const keys[] = {
        "protocol", "host", "port", "device", "baud", "parity", "data_bits", "stop_bits",
        "slave_id", "period_ms", "word_order", "inputs", "outputs", NULL};

    modbus_conn_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->conn = conn;

    const cJSON *inputs = NULL, *outputs = NULL;
    const char *word_order = "big";
    uint32_t slave_id = 1;
    *period_ms = 100;

    bool ok = json_check_keys(spec, conn, keys);
    ok &= json_get_uint(spec, "slave_id", conn, 0, 255, &slave_id);
    ok &= json_get_uint(spec, "period_ms", conn, 1, 3600000, period_ms);
    ok &= json_get_string(spec, "word_order", conn, &word_order);
    ok &= json_get_array(spec, "inputs", conn, &inputs);
    ok &= json_get_array(spec, "outputs", conn, &outputs);

    if (strcmp(word_order, "big") != 0 && strcmp(word_order, "little") != 0)
    {
        log_error("%s.word_order: must be \"big\" (high word first) or \"little\"", conn);
        ok = false;
    }
    m->little_words = strcmp(word_order, "little") == 0;

    m->ctx = new_context(conn, spec);
    ok &= m->ctx != NULL;
    if (m->ctx && modbus_set_slave(m->ctx, (int)slave_id) != 0)
    {
        log_error("%s.slave_id: %u is not valid here", conn, slave_id);
        ok = false;
    }

    // Keep validating after an error so that one run reports every problem.
    size_t total = (size_t)cJSON_GetArraySize(inputs) + (size_t)cJSON_GetArraySize(outputs);
    if (total == 0)
    {
        log_error("%s: no inputs or outputs defined", conn);
        ok = false;
    }
    else if (!(m->regs = calloc(total, sizeof(*m->regs))))
    {
        ok = false;
    }
    else
    {
        ok &= parse_regs(m, inputs, "inputs", DIR_INPUT);
        m->input_count = m->count;
        ok &= parse_regs(m, outputs, "outputs", DIR_OUTPUT);
        ok &= check_overlaps(m);
    }

    if (!ok)
    {
        modbus_destroy(m);
        return NULL;
    }
    return m;
}

// --- runtime -----------------------------------------------------------------

static int modbus_connect_(void *self, char *err, size_t err_len)
{
    modbus_conn_t *m = self;
    if (modbus_connect(m->ctx) != 0)
    {
        snprintf(err, err_len, "%s", modbus_strerror(errno));
        return -1;
    }
    return 0;
}

// Modbus exceptions are answers from a live device (e.g. illegal address):
// a problem with that variable, not with the link.
static bool is_exception(int e)
{
    return e >= EMBXILFUN && e <= EMBXGTAR;
}

static int modbus_cycle(void *self, char *err, size_t err_len)
{
    modbus_conn_t *m = self;

    for (size_t i = 0; i < m->count; i++)
    {
        reg_t *r = &m->regs[i];
        uint16_t regs[4] = {0};
        int rc;

        if (i < m->input_count)
        {
            encode(m, r, regs);
            rc = modbus_write_registers(m->ctx, r->address, r->count, regs);
        }
        else
        {
            rc = modbus_read_registers(m->ctx, r->address, r->count, regs);
        }

        if (rc != r->count)
        {
            int e = errno;
            if (!is_exception(e))
            {
                snprintf(err, err_len, "%s", modbus_strerror(e));
                return -1;
            }
            char problem[160];
            snprintf(problem, sizeof(problem), "%s register %u: %s",
                     i < m->input_count ? "cannot write" : "cannot read", r->address, modbus_strerror(e));
            conn_report(m->conn, var_id(r->var), &r->failing, problem);
            continue;
        }

        if (i >= m->input_count)
            var_set(r->var, decode(m, r, regs));
        conn_report(m->conn, var_id(r->var), &r->failing, NULL);
    }
    return 0;
}

static void modbus_disconnect(void *self)
{
    modbus_close(((modbus_conn_t *)self)->ctx);
}

const protocol_t modbus_protocol = {
    .name = "modbus",
    .create = modbus_create,
    .connect = modbus_connect_,
    .cycle = modbus_cycle,
    .disconnect = modbus_disconnect,
    .destroy = modbus_destroy,
};
