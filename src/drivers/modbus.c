// Modbus TCP/RTU client (master) driver.
//
// Inputs are written to the slave only once they have a value (from the client or an "initial"
// in the configuration) and, by default, only when that value was written again by a client
// ("write_mode": "on_change"). After a reconnect every input with a value is sent once more.
// Outputs are polled every period in batches of contiguous addresses.

#include "drivers/driver.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <modbus.h>

#include "util/clock.h"
#include "util/codec.h"
#include "util/log.h"

#define MB_MAX_READ_REGS   125   // protocol limits per request
#define MB_MAX_WRITE_REGS  123
#define MB_MAX_READ_BITS   2000
#define MB_MAX_WRITE_BITS  1968

typedef enum
{
    MB_AREA_HOLDING,
    MB_AREA_COIL,
    MB_AREA_INPUT_REGISTER,
    MB_AREA_DISCRETE_INPUT
} mb_area_t;

static const char *const area_names[] = {"holding", "coil", "input_register", "discrete_input", NULL};

typedef struct
{
    ammio_var_t *var;
    ammio_type_t type;
    mb_area_t area;
    uint16_t address;
    uint16_t count;             // registers (register areas) or 1 (bit areas)
    codec_byte_order_t order;
    // inputs only: last write_seq successfully sent
    uint32_t sent_seq;
    bool sent_valid;
    bool failing;               // the slave rejected the last request (logged once)
} mb_point_t;

typedef struct
{
    mb_area_t area;
    uint16_t start;
    uint16_t count;
    size_t first;               // index of the first point in the batch
    size_t n;                   // number of points
    bool failing;
} mb_batch_t;

typedef struct
{
    ammio_conn_t *conn;
    const char *name;

    // settings
    bool rtu;
    char *host;
    int port;
    char *device;
    int baud;
    char parity;
    int data_bits;
    int stop_bits;
    int slave_id;
    uint32_t period_ms;
    uint32_t timeout_ms;
    bool cyclic_writes;

    mb_point_t *inputs;         // sorted by area/address
    size_t n_inputs;
    mb_point_t *outputs;        // sorted by area/address
    size_t n_outputs;
    mb_batch_t *batches;        // read batches over outputs
    size_t n_batches;

    // scratch buffers used by the thread
    ammio_var_t **in_vars;
    ammio_var_snapshot_t *in_snaps;
    ammio_var_t **out_vars;
    ammio_value_t *out_vals;
    uint16_t regs[MB_MAX_READ_BITS];
    uint8_t bits[MB_MAX_READ_BITS];

    modbus_t *ctx;
    bool connected;
    bool connect_failure_logged;
    thrd_t thread;
    bool thread_started;
    atomic_bool running;
} mb_conn_t;

typedef enum
{
    MB_OK,
    MB_DEVICE_ERROR,  // the slave answered with an exception: the link is fine
    MB_LINK_ERROR     // timeout, broken connection, ...: reconnect
} mb_result_t;

static mb_result_t classify_errno(int err)
{
    // libmodbus reports protocol-level problems as MODBUS_ENOBASE + n
    return (err > MODBUS_ENOBASE && err < MODBUS_ENOBASE + 100) ? MB_DEVICE_ERROR : MB_LINK_ERROR;
}

static bool is_bit_area(mb_area_t area)
{
    return area == MB_AREA_COIL || area == MB_AREA_DISCRETE_INPUT;
}

static int point_cmp(const void *a, const void *b)
{
    const mb_point_t *pa = a, *pb = b;
    if (pa->area != pb->area)
        return (int)pa->area - (int)pb->area;
    return (int)pa->address - (int)pb->address;
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

static bool parse_point(mb_conn_t *mb, const cJSON *item, const char *path, ammio_dir_t dir,
                        codec_byte_order_t default_order, mb_point_t *p, cfg_t *cfg)
{
    static const char *const input_keys[] = {"var_id", "address", "type", "area", "byte_order", "initial", NULL};
    static const char *const output_keys[] = {"var_id", "address", "type", "area", "byte_order", NULL};

    if (!cfg_expect_object(cfg, path, item))
        return false;
    cfg_check_keys(cfg, path, item, dir == AMMIO_DIR_INPUT ? input_keys : output_keys);

    const char *id = NULL;
    uint32_t address = 0;
    bool ok = cfg_get_string(cfg, path, item, "var_id", true, &id);
    ok &= cfg_get_uint32(cfg, path, item, "address", true, 0, 65535, &address);
    ok &= cfg_get_type(cfg, path, item, "type", true, &p->type);

    int area = MB_AREA_HOLDING;
    if (cJSON_GetObjectItemCaseSensitive(item, "area"))
        ok &= cfg_get_enum(cfg, path, item, "area", true, area_names, &area);
    p->area = (mb_area_t)area;
    if (!ok)
        return false;

    if (dir == AMMIO_DIR_INPUT && (p->area == MB_AREA_INPUT_REGISTER || p->area == MB_AREA_DISCRETE_INPUT))
    {
        cfg_error(cfg, path, "area \"%s\" is read-only in Modbus; inputs can use \"holding\" or \"coil\"",
                  area_names[p->area]);
        return false;
    }
    if (is_bit_area(p->area) && p->type != AMMIO_TYPE_BOOL)
    {
        cfg_error(cfg, path, "area \"%s\" holds single bits, so 'type' must be \"bool\"", area_names[p->area]);
        return false;
    }

    p->order = default_order;
    const char *order_str;
    if (cfg_get_string(cfg, path, item, "byte_order", false, &order_str))
    {
        if (is_bit_area(p->area))
        {
            cfg_error(cfg, path, "'byte_order' has no meaning for area \"%s\"", area_names[p->area]);
            return false;
        }
        if (!codec_byte_order_from_string(order_str, &p->order))
        {
            cfg_error(cfg, path, "'byte_order' = \"%s\" is not valid (expected ABCD, CDAB, BADC or DCBA)", order_str);
            return false;
        }
    }

    p->address = (uint16_t)address;
    p->count = is_bit_area(p->area) ? 1 : (uint16_t)codec_reg_count(p->type);
    if (address + p->count > 65536u)
    {
        cfg_error(cfg, path, "a %s at address %u does not fit in the 65536-entry address space",
                  ammio_type_name(p->type), address);
        return false;
    }

    p->var = conn_add_var(mb->conn, cfg, path, id, p->type, dir);
    if (!p->var)
        return false;

    if (dir == AMMIO_DIR_INPUT)
    {
        ammio_value_t initial;
        if (cfg_get_value(cfg, path, item, "initial", false, p->type, &initial))
            var_table_set_value_silent(p->var, initial);
    }
    return true;
}

static bool parse_points(mb_conn_t *mb, const cJSON *arr, const char *path, ammio_dir_t dir,
                         codec_byte_order_t order, mb_point_t **out, size_t *count, cfg_t *cfg)
{
    size_t n = (size_t)cJSON_GetArraySize(arr);
    *out = calloc(n ? n : 1, sizeof(mb_point_t));
    *count = 0;
    if (!*out)
        return false;

    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        char p[CFG_PATH_MAX];
        cfg_path_index(p, path, index++);
        if (parse_point(mb, item, p, dir, order, &(*out)[*count], cfg))
            (*count)++;
    }
    qsort(*out, *count, sizeof(mb_point_t), point_cmp);
    return true;
}

// Rejects two variables that share an address in the same area (inputs and outputs together)
static void check_overlaps(mb_conn_t *mb, const char *path, cfg_t *cfg)
{
    size_t n = mb->n_inputs + mb->n_outputs;
    mb_point_t *all = calloc(n ? n : 1, sizeof(*all));
    if (!all)
        return;
    if (mb->n_inputs)
        memcpy(all, mb->inputs, mb->n_inputs * sizeof(*all));
    if (mb->n_outputs)
        memcpy(all + mb->n_inputs, mb->outputs, mb->n_outputs * sizeof(*all));
    qsort(all, n, sizeof(*all), point_cmp);

    for (size_t i = 1; i < n; i++)
    {
        const mb_point_t *prev = &all[i - 1], *cur = &all[i];
        if (prev->area == cur->area && (uint32_t)prev->address + prev->count > cur->address)
        {
            cfg_error(cfg, path, "'%s' (%s %u..%u) overlaps '%s' (%s %u)",
                      var_id(prev->var), area_names[prev->area], prev->address,
                      prev->address + prev->count - 1, var_id(cur->var), area_names[cur->area], cur->address);
        }
    }
    free(all);
}

static bool build_read_batches(mb_conn_t *mb)
{
    mb->batches = calloc(mb->n_outputs ? mb->n_outputs : 1, sizeof(mb_batch_t));
    if (!mb->batches)
        return false;
    mb->n_batches = 0;

    for (size_t i = 0; i < mb->n_outputs; i++)
    {
        const mb_point_t *p = &mb->outputs[i];
        mb_batch_t *b = mb->n_batches ? &mb->batches[mb->n_batches - 1] : NULL;
        unsigned max = is_bit_area(p->area) ? MB_MAX_READ_BITS : MB_MAX_READ_REGS;

        if (b && b->area == p->area && (uint32_t)b->start + b->count == p->address &&
            (unsigned)b->count + p->count <= max)
        {
            b->count = (uint16_t)(b->count + p->count);
            b->n++;
            continue;
        }

        b = &mb->batches[mb->n_batches++];
        b->area = p->area;
        b->start = p->address;
        b->count = p->count;
        b->first = i;
        b->n = 1;
    }
    return true;
}

static void mb_destroy(void *ctx);

static void *mb_create(ammio_conn_t *conn, const cJSON *settings, const char *path, cfg_t *cfg)
{
    static const char *const keys[] = {
        "mode", "host", "port", "device", "baud", "parity", "data_bits", "stop_bits", "slave_id",
        "period_ms", "timeout_ms", "byte_order", "write_mode", "registers", NULL
    };
    static const char *const modes[] = {"tcp", "rtu", NULL};
    static const char *const parities[] = {"N", "E", "O", NULL};
    static const char *const write_modes[] = {"on_change", "cyclic", NULL};

    mb_conn_t *mb = calloc(1, sizeof(*mb));
    if (!mb)
        return NULL;
    mb->conn = conn;
    mb->name = conn_name(conn);
    atomic_init(&mb->running, false);

    cfg_check_keys(cfg, path, settings, keys);

    int mode = 0;
    cfg_get_enum(cfg, path, settings, "mode", false, modes, &mode);
    mb->rtu = mode == 1;

    const char *str;
    if (mb->rtu)
    {
        if (cfg_get_string(cfg, path, settings, "device", true, &str))
            mb->device = ammio_strdup(str);
        int64_t v = 9600;
        cfg_get_int(cfg, path, settings, "baud", false, 50, 4000000, &v);
        mb->baud = (int)v;
        int parity = 0;
        cfg_get_enum(cfg, path, settings, "parity", false, parities, &parity);
        mb->parity = parities[parity][0];
        v = 8;
        cfg_get_int(cfg, path, settings, "data_bits", false, 5, 8, &v);
        mb->data_bits = (int)v;
        v = 1;
        cfg_get_int(cfg, path, settings, "stop_bits", false, 1, 2, &v);
        mb->stop_bits = (int)v;
        if (cJSON_GetObjectItemCaseSensitive(settings, "host") || cJSON_GetObjectItemCaseSensitive(settings, "port"))
            cfg_error(cfg, path, "'host'/'port' are only valid with \"mode\": \"tcp\"");
    }
    else
    {
        if (cfg_get_string(cfg, path, settings, "host", true, &str))
            mb->host = ammio_strdup(str);
        int64_t v = 502;
        cfg_get_int(cfg, path, settings, "port", false, 1, 65535, &v);
        mb->port = (int)v;
        static const char *const rtu_only[] = {"device", "baud", "parity", "data_bits", "stop_bits", NULL};
        for (const char *const *k = rtu_only; *k; k++)
        {
            if (cJSON_GetObjectItemCaseSensitive(settings, *k))
                cfg_error(cfg, path, "'%s' is only valid with \"mode\": \"rtu\"", *k);
        }
    }

    int64_t slave = 1;
    cfg_get_int(cfg, path, settings, "slave_id", false, 0, 255, &slave);
    mb->slave_id = (int)slave;

    mb->period_ms = 100;
    cfg_get_uint32(cfg, path, settings, "period_ms", false, 1, 3600000, &mb->period_ms);
    mb->timeout_ms = 500;
    cfg_get_uint32(cfg, path, settings, "timeout_ms", false, 1, 60000, &mb->timeout_ms);

    codec_byte_order_t order = CODEC_ORDER_ABCD;
    if (cfg_get_string(cfg, path, settings, "byte_order", false, &str) &&
        !codec_byte_order_from_string(str, &order))
        cfg_error(cfg, path, "'byte_order' = \"%s\" is not valid (expected ABCD, CDAB, BADC or DCBA)", str);

    int write_mode = 0;
    cfg_get_enum(cfg, path, settings, "write_mode", false, write_modes, &write_mode);
    mb->cyclic_writes = write_mode == 1;

    const cJSON *registers = NULL;
    char rpath[CFG_PATH_MAX];
    cfg_path(rpath, path, "registers");
    if (cfg_get_object(cfg, path, settings, "registers", true, &registers))
    {
        static const char *const reg_keys[] = {"inputs", "outputs", NULL};
        cfg_check_keys(cfg, rpath, registers, reg_keys);

        const cJSON *arr;
        char apath[CFG_PATH_MAX];
        if (cfg_get_array(cfg, rpath, registers, "inputs", false, &arr))
            parse_points(mb, arr, cfg_path(apath, rpath, "inputs"), AMMIO_DIR_INPUT, order,
                         &mb->inputs, &mb->n_inputs, cfg);
        if (cfg_get_array(cfg, rpath, registers, "outputs", false, &arr))
            parse_points(mb, arr, cfg_path(apath, rpath, "outputs"), AMMIO_DIR_OUTPUT, order,
                         &mb->outputs, &mb->n_outputs, cfg);

        if (mb->n_inputs + mb->n_outputs == 0 && cfg->errors == 0)
            cfg_error(cfg, rpath, "no inputs or outputs defined");
        check_overlaps(mb, rpath, cfg);
    }

    if (!build_read_batches(mb))
    {
        mb_destroy(mb);
        return NULL;
    }

    size_t n = mb->n_inputs > mb->n_outputs ? mb->n_inputs : mb->n_outputs;
    mb->in_vars = calloc(mb->n_inputs ? mb->n_inputs : 1, sizeof(*mb->in_vars));
    mb->in_snaps = calloc(mb->n_inputs ? mb->n_inputs : 1, sizeof(*mb->in_snaps));
    mb->out_vars = calloc(n ? n : 1, sizeof(*mb->out_vars));
    mb->out_vals = calloc(n ? n : 1, sizeof(*mb->out_vals));
    if (!mb->in_vars || !mb->in_snaps || !mb->out_vars || !mb->out_vals)
    {
        mb_destroy(mb);
        return NULL;
    }
    for (size_t i = 0; i < mb->n_inputs; i++)
        mb->in_vars[i] = mb->inputs[i].var;

    log_debug("%s: %zu input(s), %zu output(s) in %zu read batch(es)", mb->name, mb->n_inputs,
              mb->n_outputs, mb->n_batches);
    return mb;
}

// ---------------------------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------------------------

static void mb_disconnect(mb_conn_t *mb)
{
    if (mb->connected)
    {
        modbus_close(mb->ctx);
        mb->connected = false;
        conn_set_state(mb->conn, AMMIO_CONN_CONNECTING);
    }
}

static bool mb_connect(mb_conn_t *mb)
{
    if (modbus_connect(mb->ctx) == -1)
    {
        if (!mb->connect_failure_logged)
        {
            log_warn("%s: cannot connect to %s: %s (retrying every %u ms)", mb->name,
                     mb->rtu ? mb->device : mb->host, modbus_strerror(errno), mb->period_ms);
            mb->connect_failure_logged = true;
        }
        return false;
    }
    mb->connected = true;
    mb->connect_failure_logged = false;

    // Re-send every input that has a value: the slave may have restarted
    for (size_t i = 0; i < mb->n_inputs; i++)
        mb->inputs[i].sent_valid = false;

    conn_set_state(mb->conn, AMMIO_CONN_CONNECTED);
    return true;
}

// Writes points [first, first+n) of the inputs as one request
static mb_result_t write_batch(mb_conn_t *mb, size_t first, size_t n)
{
    const mb_point_t *p0 = &mb->inputs[first];
    int rc;

    if (p0->area == MB_AREA_COIL)
    {
        for (size_t i = 0; i < n; i++)
            mb->bits[i] = mb->in_snaps[first + i].value.b ? 1 : 0;
        rc = n == 1 ? modbus_write_bit(mb->ctx, p0->address, mb->bits[0])
                    : modbus_write_bits(mb->ctx, p0->address, (int)n, mb->bits);
    }
    else
    {
        int count = 0;
        for (size_t i = 0; i < n; i++)
        {
            const mb_point_t *p = &mb->inputs[first + i];
            codec_to_regs(p->type, mb->in_snaps[first + i].value, p->order, &mb->regs[count]);
            count += p->count;
        }
        // FC06 for a single register: every slave supports it, not every slave supports FC16
        rc = count == 1 ? modbus_write_register(mb->ctx, p0->address, mb->regs[0])
                        : modbus_write_registers(mb->ctx, p0->address, count, mb->regs);
    }

    if (rc == -1)
    {
        int err = errno;
        mb_result_t res = classify_errno(err);
        if (res == MB_DEVICE_ERROR)
        {
            if (!mb->inputs[first].failing)
                log_warn("%s: slave rejected write of '%s'%s (%s %u): %s", mb->name,
                         var_id(p0->var), n > 1 ? " and following" : "", area_names[p0->area],
                         p0->address, modbus_strerror(err));
            for (size_t i = 0; i < n; i++)
            {
                mb_point_t *p = &mb->inputs[first + i];
                p->failing = true;
                // Do not retry until the client writes again
                p->sent_seq = mb->in_snaps[first + i].write_seq;
                p->sent_valid = true;
            }
            var_table_set_quality_many(&mb->in_vars[first], n, AMMIO_QUALITY_BAD);
        }
        else
        {
            log_warn("%s: write failed: %s", mb->name, modbus_strerror(err));
        }
        return res;
    }

    bool was_failing = false;
    for (size_t i = 0; i < n; i++)
    {
        mb_point_t *p = &mb->inputs[first + i];
        was_failing |= p->failing;
        p->failing = false;
        p->sent_seq = mb->in_snaps[first + i].write_seq;
        p->sent_valid = true;
    }
    if (was_failing)
        var_table_set_quality_many(&mb->in_vars[first], n, AMMIO_QUALITY_GOOD);
    return MB_OK;
}

static mb_result_t write_inputs(mb_conn_t *mb)
{
    if (mb->n_inputs == 0)
        return MB_OK;

    var_table_read_many(mb->in_vars, mb->n_inputs, mb->in_snaps);

    size_t i = 0;
    while (i < mb->n_inputs)
    {
        const mb_point_t *p = &mb->inputs[i];
        const ammio_var_snapshot_t *s = &mb->in_snaps[i];
        bool dirty = s->has_value && (mb->cyclic_writes || !p->sent_valid || p->sent_seq != s->write_seq);
        if (!dirty)
        {
            i++;
            continue;
        }

        // Extend over following dirty points that are contiguous in the same area
        size_t n = 1;
        unsigned count = p->count;
        unsigned max = p->area == MB_AREA_COIL ? MB_MAX_WRITE_BITS : MB_MAX_WRITE_REGS;
        while (i + n < mb->n_inputs)
        {
            const mb_point_t *q = &mb->inputs[i + n];
            const ammio_var_snapshot_t *qs = &mb->in_snaps[i + n];
            bool q_dirty = qs->has_value && (mb->cyclic_writes || !q->sent_valid || q->sent_seq != qs->write_seq);
            if (!q_dirty || q->area != p->area || q->address != p->address + count || count + q->count > max)
                break;
            count += q->count;
            n++;
        }

        if (write_batch(mb, i, n) == MB_LINK_ERROR)
            return MB_LINK_ERROR;
        i += n;
    }
    return MB_OK;
}

static mb_result_t read_batch(mb_conn_t *mb, mb_batch_t *b)
{
    int rc;
    switch (b->area)
    {
        case MB_AREA_HOLDING:        rc = modbus_read_registers(mb->ctx, b->start, b->count, mb->regs); break;
        case MB_AREA_INPUT_REGISTER: rc = modbus_read_input_registers(mb->ctx, b->start, b->count, mb->regs); break;
        case MB_AREA_COIL:           rc = modbus_read_bits(mb->ctx, b->start, b->count, mb->bits); break;
        default:                     rc = modbus_read_input_bits(mb->ctx, b->start, b->count, mb->bits); break;
    }

    for (size_t i = 0; i < b->n; i++)
        mb->out_vars[i] = mb->outputs[b->first + i].var;

    if (rc == -1)
    {
        int err = errno;
        mb_result_t res = classify_errno(err);
        if (res == MB_DEVICE_ERROR)
        {
            if (!b->failing)
                log_warn("%s: slave rejected read of %s %u..%u: %s", mb->name, area_names[b->area],
                         b->start, b->start + b->count - 1, modbus_strerror(err));
            b->failing = true;
            var_table_set_quality_many(mb->out_vars, b->n, AMMIO_QUALITY_BAD);
        }
        else
        {
            log_warn("%s: read failed: %s", mb->name, modbus_strerror(err));
        }
        return res;
    }

    if (b->failing)
    {
        log_info("%s: read of %s %u..%u recovered", mb->name, area_names[b->area], b->start,
                 b->start + b->count - 1);
        b->failing = false;
    }

    for (size_t i = 0; i < b->n; i++)
    {
        const mb_point_t *p = &mb->outputs[b->first + i];
        unsigned off = p->address - b->start;
        if (is_bit_area(p->area))
        {
            memset(&mb->out_vals[i], 0, sizeof(mb->out_vals[i]));
            mb->out_vals[i].b = mb->bits[off] != 0;
        }
        else
        {
            mb->out_vals[i] = codec_from_regs(p->type, &mb->regs[off], p->order);
        }
    }
    var_table_update_many(mb->out_vars, mb->out_vals, b->n, AMMIO_QUALITY_GOOD);
    return MB_OK;
}

static mb_result_t read_outputs(mb_conn_t *mb)
{
    for (size_t i = 0; i < mb->n_batches; i++)
    {
        if (read_batch(mb, &mb->batches[i]) == MB_LINK_ERROR)
            return MB_LINK_ERROR;
    }
    return MB_OK;
}

static int mb_thread(void *arg)
{
    mb_conn_t *mb = arg;
    cycle_timer_t cycle;
    cycle_init(&cycle, mb->period_ms);

    while (atomic_load(&mb->running))
    {
        if (mb->connected || mb_connect(mb))
        {
            if (write_inputs(mb) == MB_LINK_ERROR || read_outputs(mb) == MB_LINK_ERROR)
                mb_disconnect(mb);
        }
        cycle_wait(&cycle, &mb->running);
    }

    if (cycle.overruns > 0)
        log_debug("%s: %llu cycle overrun(s)", mb->name, (unsigned long long)cycle.overruns);
    return 0;
}

static int mb_start(void *ctx)
{
    mb_conn_t *mb = ctx;

    if (mb->rtu)
    {
        mb->ctx = modbus_new_rtu(mb->device, mb->baud, mb->parity, mb->data_bits, mb->stop_bits);
    }
    else
    {
        char port[8];
        snprintf(port, sizeof(port), "%d", mb->port);
        mb->ctx = modbus_new_tcp_pi(mb->host, port);
    }
    if (!mb->ctx)
    {
        log_error("%s: cannot create Modbus context: %s", mb->name, modbus_strerror(errno));
        return -1;
    }
    if (modbus_set_slave(mb->ctx, mb->slave_id) == -1)
    {
        log_error("%s: invalid slave_id %d: %s", mb->name, mb->slave_id, modbus_strerror(errno));
        return -1;
    }
    modbus_set_response_timeout(mb->ctx, mb->timeout_ms / 1000, (mb->timeout_ms % 1000) * 1000);

    atomic_store(&mb->running, true);
    if (thrd_create(&mb->thread, mb_thread, mb) != thrd_success)
    {
        log_error("%s: cannot create thread", mb->name);
        atomic_store(&mb->running, false);
        return -1;
    }
    mb->thread_started = true;
    return 0;
}

static void mb_stop(void *ctx)
{
    mb_conn_t *mb = ctx;

    atomic_store(&mb->running, false);
    if (mb->thread_started)
    {
        thrd_join(mb->thread, NULL);
        mb->thread_started = false;
    }
    if (mb->ctx)
    {
        if (mb->connected)
            modbus_close(mb->ctx);
        mb->connected = false;
        modbus_free(mb->ctx);
        mb->ctx = NULL;
    }
}

static void mb_destroy(void *ctx)
{
    mb_conn_t *mb = ctx;
    if (!mb)
        return;
    free(mb->host);
    free(mb->device);
    free(mb->inputs);
    free(mb->outputs);
    free(mb->batches);
    free(mb->in_vars);
    free(mb->in_snaps);
    free(mb->out_vars);
    free(mb->out_vals);
    free(mb);
}

const ammio_driver_t modbus_driver = {
    .protocol = "modbus",
    .create   = mb_create,
    .start    = mb_start,
    .stop     = mb_stop,
    .destroy  = mb_destroy,
};
