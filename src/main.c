#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/api_server.h"
#include "api/events.h"
#include "cJSON.h"
#include "core/connections.h"
#include "core/var_table.h"
#include "util/cfg.h"
#include "util/log.h"

static atomic_bool running = true;

// Only async-signal-safe work here: the main loop notices the flag and shuts down
static void signal_handler(int sig)
{
    (void)sig;
    atomic_store(&running, false);
}

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage: ammio --config <file.json> --endpoint <url> [options]\n"
        "\n"
        "  --config <file>      connections file (JSON)\n"
        "  --endpoint <url>     nng endpoint for the JSON API, e.g. tcp://127.0.0.1:5555\n"
        "  --events <url>       optional nng PUB endpoint for change events, e.g. tcp://127.0.0.1:5556\n"
        "  --log-level <level>  debug | info | warn | error | off (default: info)\n"
        "  --check              validate the configuration and exit\n"
        "  --version            print the version and exit\n"
        "  --help               print this help and exit\n");
}

static cJSON *load_json_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        log_error("cannot open '%s'", path);
        return NULL;
    }

    char *data = NULL;
    size_t len = 0, cap = 0;
    for (;;)
    {
        if (cap - len < 4096)
        {
            size_t new_cap = cap ? cap * 2 : 16384;
            char *p = realloc(data, new_cap);
            if (!p)
            {
                free(data);
                fclose(f);
                log_error("out of memory reading '%s'", path);
                return NULL;
            }
            data = p;
            cap = new_cap;
        }
        size_t n = fread(data + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    bool read_error = ferror(f) != 0;
    fclose(f);
    if (read_error)
    {
        free(data);
        log_error("error reading '%s'", path);
        return NULL;
    }

    cJSON *json = cJSON_ParseWithLength(data, len);
    if (!json)
    {
        // Report the line of the syntax error
        const char *err = cJSON_GetErrorPtr();
        int line = 1;
        if (err && err >= data && err <= data + len)
        {
            for (const char *p = data; p < err; p++)
                line += *p == '\n';
            log_error("'%s' is not valid JSON (near line %d). Note: comments are not allowed in JSON.", path, line);
        }
        else
        {
            log_error("'%s' is not valid JSON", path);
        }
    }
    free(data);
    return json;
}

int main(int argc, char *argv[])
{
    const char *endpoint = NULL;
    const char *config_path = NULL;
    const char *events_endpoint = NULL;
    bool check_only = false;

    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        bool takes_value = strcmp(arg, "--config") == 0 || strcmp(arg, "--interface") == 0 ||
                           strcmp(arg, "--endpoint") == 0 || strcmp(arg, "--events") == 0 ||
                           strcmp(arg, "--log-level") == 0;

        if (takes_value && i + 1 >= argc)
        {
            fprintf(stderr, "ammio: %s needs a value\n", arg);
            return 2;
        }

        if (strcmp(arg, "--config") == 0)
            config_path = argv[++i];
        else if (strcmp(arg, "--interface") == 0)
        {
            config_path = argv[++i];
            fprintf(stderr, "ammio: --interface is deprecated, use --config\n");
        }
        else if (strcmp(arg, "--endpoint") == 0)
            endpoint = argv[++i];
        else if (strcmp(arg, "--events") == 0)
            events_endpoint = argv[++i];
        else if (strcmp(arg, "--log-level") == 0)
        {
            ammio_log_level_t level;
            const char *value = argv[++i];
            if (!log_level_from_string(value, &level))
            {
                fprintf(stderr, "ammio: invalid log level '%s'\n", value);
                return 2;
            }
            log_set_level(level);
        }
        else if (strcmp(arg, "--check") == 0)
            check_only = true;
        else if (strcmp(arg, "--version") == 0)
        {
            printf("ammio %s\n", AMMIO_VERSION);
            return 0;
        }
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            print_usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "ammio: unknown argument '%s'\n\n", arg);
            print_usage(stderr);
            return 2;
        }
    }

    if (!config_path || (!endpoint && !check_only))
    {
        print_usage(stderr);
        return 2;
    }

    if (var_table_init() != 0)
        return 1;

    cJSON *config = load_json_file(config_path);
    if (!config)
        return 1;

    cfg_t cfg = {0};
    int rc = connections_load(config, &cfg);
    if (rc != 0)
    {
        log_error("'%s': %d error(s), %d warning(s)", config_path, cfg.errors, cfg.warnings);
        cJSON_Delete(config);
        var_table_free();
        return 1;
    }
    log_info("'%s': %zu connection(s), %zu variable(s), %d warning(s)", config_path,
             connections_count(), var_table_count(), cfg.warnings);

    if (check_only)
    {
        connections_destroy();
        cJSON_Delete(config);
        var_table_free();
        printf("configuration OK\n");
        return 0;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef SIGBREAK
    signal(SIGBREAK, signal_handler);  // Ctrl+Break on Windows
#endif

    int exit_code = 1;
    if (events_endpoint)
    {
        if (events_open(events_endpoint) != 0)
            goto cleanup;
        api_server_set_events_endpoint(events_endpoint);
        log_info("Publishing change events on %s", events_endpoint);
    }

    if (api_server_open(endpoint) != 0)
        goto cleanup;
    log_info("JSON API listening on %s", endpoint);

    if (connections_start() != 0)
    {
        connections_stop();
        goto cleanup;
    }

    api_server_run(&running);
    log_info("Shutting down...");
    connections_stop();
    exit_code = 0;

cleanup:
    api_server_close();
    events_close();
    connections_destroy();
    cJSON_Delete(config);
    var_table_free();
    return exit_code;
}
