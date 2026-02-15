#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "../config.h"

typedef struct {
    const char *name;
    int (*init)(cJSON *config);
    int (*start)(void);
    void (*stop)(void);
} protocol_t;

void protocol_register(protocol_t *proto);
int protocols_init_with(cJSON *interface_config);
int protocols_start(void);
void protocols_stop(void);

#endif
