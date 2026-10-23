#ifndef AMMIO_DRIVER_H
#define AMMIO_DRIVER_H

// A driver implements one protocol. Every entry of the configuration file is a *connection*:
// a named instance of a driver with its own settings, state and thread. Several connections
// may use the same driver (e.g. two Modbus PLCs).

#include "cJSON.h"
#include "core/connections.h"
#include "util/cfg.h"

typedef struct
{
    const char *protocol;  // value of the "protocol" key, e.g. "modbus"

    // Parses and validates the settings and registers the connection's variables. Must not
    // touch the network. Reports problems through cfg and returns NULL if there were any.
    void *(*create)(ammio_conn_t *conn, const cJSON *settings, const char *path, cfg_t *cfg);

    // Opens sockets/sessions and starts the connection's thread. Failing to reach the SUT is
    // not an error (the driver keeps retrying); failing to set up local resources is.
    int (*start)(void *ctx);

    // Stops the thread and closes the network resources. Must be safe to call if start failed.
    void (*stop)(void *ctx);

    // Frees everything create() allocated
    void (*destroy)(void *ctx);
} ammio_driver_t;

extern const ammio_driver_t modbus_driver;
extern const ammio_driver_t opcua_driver;
extern const ammio_driver_t trdp_driver;

#endif
