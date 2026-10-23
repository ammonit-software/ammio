#ifndef AMMIO_CONNECTIONS_H
#define AMMIO_CONNECTIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "core/var_table.h"
#include "util/cfg.h"

typedef struct ammio_conn ammio_conn_t;

typedef enum
{
    AMMIO_CONN_STOPPED,
    AMMIO_CONN_CONNECTING,
    AMMIO_CONN_CONNECTED
} ammio_conn_state_t;

typedef struct
{
    const char *name;
    const char *protocol;
    ammio_conn_state_t state;
} ammio_conn_info_t;

// Reads every connection of the configuration file, validating it completely. Returns 0 if the
// whole file is valid; otherwise every problem has been logged and the connections are freed.
int connections_load(const cJSON *root, cfg_t *cfg);

int connections_start(void);
void connections_stop(void);
void connections_destroy(void);

size_t connections_count(void);
// Fills up to max entries; returns the number of connections
size_t connections_list(ammio_conn_info_t *out, size_t max);
const char *ammio_conn_state_name(ammio_conn_state_t state);

// ---- For drivers ----------------------------------------------------------------------------

const char *conn_name(const ammio_conn_t *conn);

// Registers a variable owned by this connection. Reports duplicates (across all connections)
// and invalid ids through cfg; returns NULL in that case.
ammio_var_t *conn_add_var(ammio_conn_t *conn, cfg_t *cfg, const char *path, const char *var_id,
                          ammio_type_t type, ammio_dir_t dir);

// Updates the connection state, its "<name>.connected" variable and the quality of all of its
// variables. Logs transitions.
void conn_set_state(ammio_conn_t *conn, ammio_conn_state_t state);

#endif
