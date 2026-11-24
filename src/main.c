#include "cJSON.h"
#include "conn.h"
#include "json.h"
#include "log.h"
#include "server.h"
#include "vars.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef AMMIO_VERSION
#define AMMIO_VERSION "dev"
#endif

#define DEFAULT_ENDPOINT "tcp://127.0.0.1:5555"

static atomic_bool stop_requested = false;

// Only sets a lock-free flag: everything else is unsafe inside a signal handler.
static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&stop_requested, true);
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: ammio [options] <config.json>\n"
            "\n"
            "  -i, --interface <file>  the config file (same as the positional argument)\n"
            "  -e, --endpoint <url>    where clients connect (default: " DEFAULT_ENDPOINT ")\n"
            "  -c, --check             validate the config and exit\n"
            "  -v, --verbose           log debug details (same as --log-level 0)\n"
            "      --log-level <n>     0 = debug, 1 = info (default), 2 = errors only\n"
            "  -h, --help              show this help\n"
            "      --version           show the version\n");
}

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    const char *endpoint = DEFAULT_ENDPOINT;
    bool check_only = false;

    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "--version") == 0)
        {
            printf("ammio %s\n", AMMIO_VERSION);
            return 0;
        }
        bool has_value = i + 1 < argc;
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
            log_set_level(LOG_DEBUG);
        else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0)
            check_only = true;
        else if ((strcmp(arg, "-e") == 0 || strcmp(arg, "--endpoint") == 0) && has_value)
            endpoint = argv[++i];
        else if ((strcmp(arg, "-i") == 0 || strcmp(arg, "--interface") == 0) && has_value && !config_path)
            config_path = argv[++i];
        else if (strcmp(arg, "--log-level") == 0 && has_value)
        {
            // v0.5 numbering
            const char *n = argv[++i];
            if (strcmp(n, "0") == 0)
                log_set_level(LOG_DEBUG);
            else if (strcmp(n, "1") == 0)
                log_set_level(LOG_INFO);
            else if (strcmp(n, "2") == 0)
                log_set_level(LOG_ERROR);
            else
            {
                fprintf(stderr, "ammio: --log-level must be 0, 1 or 2\n");
                return 2;
            }
        }
        else if (arg[0] != '-' && !config_path)
            config_path = arg;
        else
        {
            fprintf(stderr, "ammio: unexpected argument '%s'\n\n", arg);
            usage(stderr);
            return 2;
        }
    }

    if (!config_path)
    {
        usage(stderr);
        return 2;
    }

    cJSON *config = json_load_file(config_path);
    if (!config)
        return 1;

    if (conns_load(config) != 0)
    {
        log_error("%s: invalid config", config_path);
        cJSON_Delete(config);
        return 1;
    }

    int status = 1;
    if (check_only)
    {
        printf("%s: OK (%zu variables)\n", config_path, vars_count());
        status = 0;
    }
    else if (server_open(endpoint) == 0)
    {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        if (conns_start() == 0)
        {
            log_info("ammio %s ready on %s (%zu variables)", AMMIO_VERSION, endpoint, vars_count());
            server_run(&stop_requested);
            log_info("shutting down");
            status = 0;
        }
        server_close();
    }

    conns_stop();
    vars_clear();
    cJSON_Delete(config);
    return status;
}
