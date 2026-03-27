#include "modbus_iface.h"
#include "../var_table.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include <errno.h>

#include <modbus.h>

// Module state
static cJSON *modbus_config = NULL;
static modbus_t *ctx = NULL;

static modbus_reg_t input_regs[MODBUS_MAX_REGS];
static int input_count = 0;

static modbus_reg_t output_regs[MODBUS_MAX_REGS];
static int output_count = 0;

static thrd_t thread_process;
static atomic_bool running = false;
static bool connected = false;

static int reg_count_for_type(type_t type)
{
    switch (type)
    {
        case TYPE_UINT8:
        case TYPE_INT8:
        case TYPE_UINT16:
        case TYPE_INT16:
            return 1;
        case TYPE_UINT32:
        case TYPE_INT32:
        case TYPE_FLOAT32:
            return 2;
        case TYPE_FLOAT64:
            return 4;
        default:
            return 1;
    }
}

// Read values from var_table and write to Modbus slave (for inputs)
static void write_to_slave(modbus_reg_t *reg)
{
    var_t var;
    if (var_table_get(reg->var_id, &var) != 0)
        return;

    uint16_t regs[4] = {0};

    switch (reg->type)
    {
        case TYPE_UINT8:
            regs[0] = (uint16_t)var.value.u8;
            break;
        case TYPE_INT8:
            regs[0] = (uint16_t)(int16_t)var.value.i8;
            break;
        case TYPE_UINT16:
            regs[0] = var.value.u16;
            break;
        case TYPE_INT16:
            regs[0] = (uint16_t)var.value.i16;
            break;
        case TYPE_UINT32:
            regs[0] = (uint16_t)(var.value.u32 >> 16);
            regs[1] = (uint16_t)(var.value.u32 & 0xFFFF);
            break;
        case TYPE_INT32:
        {
            uint32_t u = (uint32_t)var.value.i32;
            regs[0] = (uint16_t)(u >> 16);
            regs[1] = (uint16_t)(u & 0xFFFF);
            break;
        }
        case TYPE_FLOAT32:
            modbus_set_float(var.value.f32, regs);
            break;
        case TYPE_FLOAT64:
        {
            uint64_t tmp;
            memcpy(&tmp, &var.value.f64, sizeof(tmp));
            regs[0] = (uint16_t)(tmp >> 48);
            regs[1] = (uint16_t)(tmp >> 32);
            regs[2] = (uint16_t)(tmp >> 16);
            regs[3] = (uint16_t)(tmp & 0xFFFF);
            break;
        }
    }

    if (modbus_write_registers(ctx, reg->address, reg->reg_count, regs) == -1)
    {
        log_debug("modbus: write failed for '%s' addr=%d: %s",
                  reg->var_id, reg->address, modbus_strerror(errno));
    }
}

// Read values from Modbus slave and write into var_table (for outputs)
static void read_from_slave(modbus_reg_t *reg)
{
    uint16_t regs[4] = {0};

    if (modbus_read_registers(ctx, reg->address, reg->reg_count, regs) == -1)
    {
        log_debug("modbus: read failed for '%s' addr=%d: %s",
                  reg->var_id, reg->address, modbus_strerror(errno));
        return;
    }

    var_t var;
    var.type = reg->type;

    switch (reg->type)
    {
        case TYPE_UINT8:
            var.value.u8 = (uint8_t)regs[0];
            break;
        case TYPE_INT8:
            var.value.i8 = (int8_t)regs[0];
            break;
        case TYPE_UINT16:
            var.value.u16 = regs[0];
            break;
        case TYPE_INT16:
            var.value.i16 = (int16_t)regs[0];
            break;
        case TYPE_UINT32:
            var.value.u32 = ((uint32_t)regs[0] << 16) | regs[1];
            break;
        case TYPE_INT32:
            var.value.i32 = (int32_t)(((uint32_t)regs[0] << 16) | regs[1]);
            break;
        case TYPE_FLOAT32:
            var.value.f32 = modbus_get_float(regs);
            break;
        case TYPE_FLOAT64:
        {
            uint64_t tmp = ((uint64_t)regs[0] << 48) | ((uint64_t)regs[1] << 32) |
                           ((uint64_t)regs[2] << 16) | (uint64_t)regs[3];
            memcpy(&var.value.f64, &tmp, sizeof(tmp));
            break;
        }
    }

    var_table_set(reg->var_id, &var);
}

// Main protocol loop: write inputs, read outputs, sleep
static int thread_process_func(void *arg)
{
    (void)arg;

    uint32_t period_ms = 100;
    cJSON *p = cJSON_GetObjectItem(modbus_config, "period_ms");
    if (p && cJSON_IsNumber(p))
        period_ms = (uint32_t)p->valuedouble;

    struct timespec ts = {
        .tv_sec  = (time_t)(period_ms / 1000),
        .tv_nsec = (long)((period_ms % 1000) * 1000000L)
    };

    while (running)
    {
        if (!connected)
        {
            if (modbus_connect(ctx) == -1)
            {
                log_debug("modbus: connect retry failed: %s", modbus_strerror(errno));
                thrd_sleep(&ts, NULL);
                continue;
            }
            connected = true;
            log_info("modbus: connected");
        }

        for (int i = 0; i < input_count; i++)
            write_to_slave(&input_regs[i]);

        for (int i = 0; i < output_count; i++)
            read_from_slave(&output_regs[i]);

        thrd_sleep(&ts, NULL);
    }

    return 0;
}

static int modbus_iface_init(cJSON *config)
{
    modbus_config = config;

    cJSON *registers = cJSON_GetObjectItem(config, "registers");
    if (!registers)
    {
        log_debug("modbus: no registers defined");
        return 0;
    }

    // Inputs: ammio writes to slave
    cJSON *inputs = cJSON_GetObjectItem(registers, "inputs");
    if (inputs && cJSON_IsArray(inputs))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, inputs)
        {
            if (input_count >= MODBUS_MAX_REGS)
                break;

            cJSON *var_id  = cJSON_GetObjectItem(item, "var_id");
            cJSON *address = cJSON_GetObjectItem(item, "address");
            cJSON *type    = cJSON_GetObjectItem(item, "type");

            if (!var_id || !cJSON_IsString(var_id))
                continue;

            modbus_reg_t *reg = &input_regs[input_count];
            strncpy(reg->var_id, var_id->valuestring, MODBUS_MAX_NAME_LEN - 1);
            reg->address   = address && cJSON_IsNumber(address) ? (int)address->valuedouble : 0;
            reg->type      = type && cJSON_IsString(type) ?
                             var_table_type_from_string(type->valuestring) : TYPE_UINT16;
            reg->reg_count = reg_count_for_type(reg->type);

            var_table_add(reg->var_id, reg->type, DIR_INPUT);
            log_debug("modbus: input '%s' addr=%d type=%s regs=%d",
                      reg->var_id, reg->address,
                      type ? type->valuestring : "uint16", reg->reg_count);
            input_count++;
        }
        log_debug("modbus: parsed %d input registers", input_count);
    }

    // Outputs: ammio reads from slave
    cJSON *outputs = cJSON_GetObjectItem(registers, "outputs");
    if (outputs && cJSON_IsArray(outputs))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, outputs)
        {
            if (output_count >= MODBUS_MAX_REGS)
                break;

            cJSON *var_id  = cJSON_GetObjectItem(item, "var_id");
            cJSON *address = cJSON_GetObjectItem(item, "address");
            cJSON *type    = cJSON_GetObjectItem(item, "type");

            if (!var_id || !cJSON_IsString(var_id))
                continue;

            modbus_reg_t *reg = &output_regs[output_count];
            strncpy(reg->var_id, var_id->valuestring, MODBUS_MAX_NAME_LEN - 1);
            reg->address   = address && cJSON_IsNumber(address) ? (int)address->valuedouble : 0;
            reg->type      = type && cJSON_IsString(type) ?
                             var_table_type_from_string(type->valuestring) : TYPE_UINT16;
            reg->reg_count = reg_count_for_type(reg->type);

            var_table_add(reg->var_id, reg->type, DIR_OUTPUT);
            log_debug("modbus: output '%s' addr=%d type=%s regs=%d",
                      reg->var_id, reg->address,
                      type ? type->valuestring : "uint16", reg->reg_count);
            output_count++;
        }
        log_debug("modbus: parsed %d output registers", output_count);
    }

    return 0;
}

static int modbus_iface_start(void)
{
    cJSON *mode = cJSON_GetObjectItem(modbus_config, "mode");
    const char *mode_str = mode && cJSON_IsString(mode) ? mode->valuestring : "tcp";

    if (strcmp(mode_str, "rtu") == 0)
    {
        cJSON *device    = cJSON_GetObjectItem(modbus_config, "device");
        cJSON *baud      = cJSON_GetObjectItem(modbus_config, "baud");
        cJSON *parity    = cJSON_GetObjectItem(modbus_config, "parity");
        cJSON *data_bits = cJSON_GetObjectItem(modbus_config, "data_bits");
        cJSON *stop_bits = cJSON_GetObjectItem(modbus_config, "stop_bits");

        if (!device || !cJSON_IsString(device))
        {
            log_debug("modbus rtu: missing device");
            return -1;
        }

        ctx = modbus_new_rtu(
            device->valuestring,
            baud      && cJSON_IsNumber(baud)      ? (int)baud->valuedouble      : 9600,
            parity    && cJSON_IsString(parity)    ? parity->valuestring[0]      : 'N',
            data_bits && cJSON_IsNumber(data_bits) ? (int)data_bits->valuedouble : 8,
            stop_bits && cJSON_IsNumber(stop_bits) ? (int)stop_bits->valuedouble : 1
        );
    }
    else
    {
        cJSON *host = cJSON_GetObjectItem(modbus_config, "host");
        cJSON *port = cJSON_GetObjectItem(modbus_config, "port");

        ctx = modbus_new_tcp(
            host && cJSON_IsString(host) ? host->valuestring    : "127.0.0.1",
            port && cJSON_IsNumber(port) ? (int)port->valuedouble : 502
        );
    }

    if (!ctx)
    {
        log_debug("modbus: failed to create context");
        return -1;
    }

    cJSON *slave_id = cJSON_GetObjectItem(modbus_config, "slave_id");
    int sid = slave_id && cJSON_IsNumber(slave_id) ? (int)slave_id->valuedouble : 1;
    modbus_set_slave(ctx, sid);

    // Auto-reconnect on link errors
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK);

    if (modbus_connect(ctx) == -1)
    {
        log_info("modbus: initial connect failed (%s), will retry", modbus_strerror(errno));
    }
    else
    {
        connected = true;
        log_info("modbus: connected (slave_id=%d)", sid);
    }

    running = true;
    if (thrd_create(&thread_process, thread_process_func, NULL) != thrd_success)
    {
        log_info("modbus: failed to create process thread");
        running = false;
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = NULL;
        return -1;
    }
    log_info("modbus: process thread started");

    return 0;
}

static void modbus_iface_stop(void)
{
    if (!running)
        return;

    running = false;
    connected = false;
    thrd_join(thread_process, NULL);
    log_debug("modbus: process thread stopped");

    if (ctx)
    {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = NULL;
    }
    log_debug("modbus: disconnected");
}

static interface_t modbus_interface = {
    .name  = "modbus",
    .init  = modbus_iface_init,
    .start = modbus_iface_start,
    .stop  = modbus_iface_stop
};

void modbus_iface_register(void)
{
    interface_register(&modbus_interface);
}
