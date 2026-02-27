#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
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

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    const char *interface_path = NULL;

    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--config") == 0)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--interface") == 0)
            interface_path = argv[++i];
    }

    if (!config_path || !interface_path)
    {
        fprintf(stderr, "Usage: ammio --config <config.json> --interface <interface.json>\n");
        return 1;
    }

    if (config_load(config_path) != 0)
    {
        return 1;
    }

    cJSON *log_level = config_get("log_level");
    if (log_level && cJSON_IsNumber(log_level))
    {
        log_set_level(log_level->valueint);
    }

    log_info("Config loaded: %s", config_path);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Register available protocols
    trdp_iface_register();
    modbus_iface_register();
    opcua_iface_register();

    var_table_init();

    // Load interface config and initialize protocols
    cJSON *interface_config = config_load_json(interface_path);
    if (!interface_config)
    {
        return 1;
    }
    log_info("Interface loaded: %s", interface_path);

    if (interfaces_init_with(interface_config) != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    log_info("Variable table initialized");

    cJSON *endpoint = config_get("ammio_endpoint");
    if (!endpoint || !cJSON_IsString(endpoint))
    {
        log_info("Missing ammio_endpoint in config");
        cJSON_Delete(interface_config);
        return 1;
    }

    if (var_server_init(endpoint->valuestring) != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    log_info("Server started on %s", endpoint->valuestring);

    // Start protocol communication threads
    if (interfaces_start() != 0)
    {
        cJSON_Delete(interface_config);
        return 1;
    }

    var_server_run();

    cJSON_Delete(interface_config);
    return 0;
}
