#ifndef MODBUS_IFACE_H
#define MODBUS_IFACE_H

#include "interface.h"
#include "../var_table.h"
#include <stdint.h>

#define MODBUS_MAX_NAME_LEN 256
#define MODBUS_MAX_REGS 128

typedef struct {
    char name[MODBUS_MAX_NAME_LEN];
    int address;
    type_t type;
    int reg_count;
} modbus_reg_t;

void modbus_iface_register(void);

#endif
