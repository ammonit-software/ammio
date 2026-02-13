#include <signal.h>
#include "config.h"
#include "log.h"
#include "var_table.h"
#include "var_server.h"
#include "protocols/protocol.h"
#include "protocols/trdp.h"

static void signal_handler(int sig)
{
    (void)sig;
    log_info("Shutting down...");
    protocols_stop();
    var_server_stop();
}

int main(int argc, char *argv[])
{
    const char *config_path;
    cJSON *log_level;
    cJSON *endpoint;

    if (argc < 2)
    {
        return 1;
    }
    config_path = argv[1];

    if (config_load(config_path) != 0)
    {
        return 1;
    }

    log_level = config_get("traces.log_level");
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

    // Initialize configured protocols (populates var_table)
    if (protocols_init() != 0)
    {
        return 1;
    }

    log_info("Variable table initialized");

    endpoint = config_get("connection.endpoint");
    if (!endpoint || !cJSON_IsString(endpoint))
    {
        log_info("Missing connection.endpoint in config");
        return 1;
    }

    if (var_server_init(endpoint->valuestring) != 0)
    {
        return 1;
    }

    log_info("Server started on %s", endpoint->valuestring);

    // Start protocol communication threads
    if (protocols_start() != 0)
    {
        return 1;
    }

    var_server_run();

    return 0;
}
