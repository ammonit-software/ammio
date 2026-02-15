#ifndef CONFIG_H
#define CONFIG_H

#include "cJSON.h"

int config_load(const char *path);
cJSON *config_get(const char *path);
cJSON *config_load_json(const char *path);

#endif
