#include <signal.h>
#include <stdio.h>
#include "config.h"
#include "log.h"
#include "var_table.h"
#include "var_server.h"
#include "interfaces/interface.h"
#include "interfaces/trdp.h"

static void signal_handler(int sig)
{
    (void)sig;
    log_info("Shutting down...");
    interfaces_stop();
    var_server_stop();
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: ammio <config.json> <interface.json>\n");
        return 1;
    }

    const char *config_path = argv[1];
    const char *interface_path = argv[2];

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
    trdp_register();

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
