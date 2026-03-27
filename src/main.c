#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "log.h"
#include "var_table.h"
#include "var_server.h"
#include "interfaces/interface.h"
#include "interfaces/trdp_iface.h"
#include "interfaces/modbus_iface.h"
#include "interfaces/opcua_iface.h"

static void signal_handler(int sig)
{
    (void)sig;
    log_info("Shutting down...");
    interfaces_stop();
    var_server_stop();
}

static cJSON *load_json_file(const char *path)
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

int main(int argc, char *argv[])
{
    const char *endpoint = NULL;
    const char *interface_path = NULL;
    int log_level = -1;

    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--endpoint") == 0)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--interface") == 0)
            interface_path = argv[++i];
        else if (strcmp(argv[i], "--log-level") == 0)
            log_level = atoi(argv[++i]);
    }

    if (!endpoint || !interface_path)
    {
        fprintf(stderr, "Usage: ammio --endpoint <url> --interface <interface.json> [--log-level <N>]\n");
        return 1;
    }

    if (log_level >= 0)
        log_set_level(log_level);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Register available protocols
    trdp_iface_register();
    modbus_iface_register();
    opcua_iface_register();

    var_table_init();

    cJSON *interface_config = load_json_file(interface_path);
    if (!interface_config)
        return 1;

    log_info("Interface loaded: %s", interface_path);

    if (interfaces_init_with(interface_config) != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    log_info("Variable table initialized");

    if (var_server_init(endpoint) != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    log_info("Server started on %s", endpoint);

    if (interfaces_start() != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    var_server_run();

    cJSON_Delete(interface_config);
    return 0;
}
