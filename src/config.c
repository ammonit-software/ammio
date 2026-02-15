#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *config = NULL;

// Read and parse a JSON file, caller owns the returned cJSON
cJSON *config_load_json(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open file: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(len + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }

    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);

    cJSON *json = cJSON_Parse(data);
    free(data);

    if (!json) {
        fprintf(stderr, "Failed to parse JSON: %s\n", path);
        return NULL;
    }

    return json;
}

int config_load(const char *path)
{
    config = config_load_json(path);
    return config ? 0 : -1;
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
