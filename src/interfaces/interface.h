#ifndef INTERFACE_H
#define INTERFACE_H

#include "cJSON.h"

typedef struct {
    const char *name;
    int (*init)(cJSON *config);
    int (*start)(void);
    void (*stop)(void);
} interface_t;

void interface_register(interface_t *iface);
int interfaces_init_with(cJSON *interface_config);
int interfaces_start(void);
void interfaces_stop(void);

#endif
