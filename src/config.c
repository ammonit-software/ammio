#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *config = NULL;

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open config file: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(len + 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);

    config = cJSON_Parse(data);
    free(data);

    if (!config) {
        fprintf(stderr, "Failed to parse config JSON\n");
        return -1;
    }

    return 0;
}

cJSON *config_get(const char *path)
{
    if (!config || !path) return NULL;

    char *path_copy = strdup(path);
    char *token = strtok(path_copy, ".");
    cJSON *current = config;

    while (token && current) {
        current = cJSON_GetObjectItem(current, token);
        token = strtok(NULL, ".");
    }

    free(path_copy);
    return current;
}
