#ifndef TRDP_H
#define TRDP_H

#include "protocol.h"
#include "../var_table.h"
#include <stdint.h>
#include <stddef.h>

#define TRDP_MAX_VARS_PER_CONTAINER 256
#define TRDP_MAX_NAME_LEN 256

typedef struct
{
    char name[TRDP_MAX_NAME_LEN];
    uint32_t offset_bits;
    type_t type;
} trdp_var_mapping_t;

typedef struct
{
    char name[TRDP_MAX_NAME_LEN];
    uint32_t comid;
    char multicast_ip[16];
    uint32_t period_ms;
    uint32_t size_bits;
    uint32_t size_bytes;
    uint8_t *buffer;
    trdp_var_mapping_t variables[TRDP_MAX_VARS_PER_CONTAINER];
    size_t var_count;
    void *handle;  // TRDP_PUB_T or TRDP_SUB_T
} trdp_container_t;

void trdp_register(void);

#endif
