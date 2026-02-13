#ifndef CONFIG_H
#define CONFIG_H

#include "cJSON.h"

int config_load(const char *path);
cJSON *config_get(const char *path);

#endif
