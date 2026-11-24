#ifndef AMMIO_PROTOCOL_H
#define AMMIO_PROTOCOL_H

// Contract between ammio and a protocol adapter.
//
// An adapter only parses its config and moves data; ammio owns the thread,
// pacing, reconnection and logging of link state. Each config entry gets its
// own instance (`self`), so the same protocol can be used any number of times.
//
// Lifecycle (connect/cycle/disconnect run on the connection's own thread):
//
//   create -> [connect -> cycle, cycle, ... -> disconnect]* -> destroy
//
// To add a protocol: implement these five functions in src/protocols/<name>.c,
// declare it below and add it to the table in conn.c.

#include "cJSON.h"
#include "vars.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char *name; // value of "protocol" in the config

    // Validate `spec` (the whole connection object), register its variables
    // with conn_add_var() and set *period_ms (0 = cycle() paces itself).
    // Logs every problem found and returns NULL on error.
    void *(*create)(const char *conn, const cJSON *spec, uint32_t *period_ms);

    // Bring the link up. On failure clean up, describe why in `err`, return -1.
    int (*connect)(void *self, char *err, size_t err_len);

    // One exchange: send inputs to the SUT, store outputs from it.
    // Return -1 only when the link is lost (reason in `err`); problems with
    // single variables are the adapter's to log.
    int (*cycle)(void *self, char *err, size_t err_len);

    void (*disconnect)(void *self);
    void (*destroy)(void *self);
} protocol_t;

extern const protocol_t modbus_protocol;
extern const protocol_t opcua_protocol;
extern const protocol_t trdp_protocol;

// Registers a variable; logs and returns NULL if the name is already taken.
var_t *conn_add_var(const char *ctx, const char *name, type_t type, dir_t dir);

// Per-variable problem reporting without log spam: logs once when a problem
// appears and once when it clears (problem == NULL).
// `state` is the adapter's per-variable bool.
void conn_report(const char *conn, const char *var, bool *state, const char *problem);

#endif
