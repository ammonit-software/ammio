#ifndef AMMIO_VAR_TABLE_H
#define AMMIO_VAR_TABLE_H

// Central variable store shared by the API server (clients) and the protocol drivers.
//
// Variables are registered while the configuration is loaded and never removed while ammio
// runs, so the ammio_var_t handles returned by var_table_add()/var_table_find() stay valid
// and drivers can keep them instead of doing a hash lookup every cycle.
//
// All functions are thread-safe. The *_many functions apply to all variables under a single
// lock, which gives clients atomic multi-variable writes and drivers consistent snapshots.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

// Direction is named from the point of view of the System Under Test:
//   INPUT  - ammio -> SUT. Clients write it, a driver sends it to the SUT.
//   OUTPUT - SUT -> ammio. A driver receives it, clients can only read it.
typedef enum
{
    AMMIO_DIR_INPUT,
    AMMIO_DIR_OUTPUT
} ammio_dir_t;

typedef enum
{
    AMMIO_QUALITY_NO_DATA,      // no value yet (never received / never written)
    AMMIO_QUALITY_GOOD,         // current value
    AMMIO_QUALITY_STALE,        // last known value, but the source stopped updating it (timeout)
    AMMIO_QUALITY_DISCONNECTED, // the connection to the SUT is down
    AMMIO_QUALITY_BAD           // the SUT reported an error or sent an unconvertible value
} ammio_quality_t;

typedef struct ammio_var ammio_var_t;

typedef struct
{
    const char *var_id;      // owned by the table, valid for its whole lifetime
    const char *connection;  // name of the connection that owns the variable
    ammio_type_t type;
    ammio_dir_t dir;
    ammio_value_t value;
    bool has_value;
    ammio_quality_t quality;
    uint64_t timestamp_ns;   // wall-clock time of the last update (or 0)
    uint32_t write_seq;      // incremented on every client write; drivers use it for on-change sends
} ammio_var_snapshot_t;

typedef void (*var_change_hook_t)(const ammio_var_snapshot_t *snap, void *ctx);

int var_table_init(void);
void var_table_free(void);

// Returns NULL if the id is already registered (or on allocation failure)
ammio_var_t *var_table_add(const char *var_id, ammio_type_t type, ammio_dir_t dir, const char *connection);
ammio_var_t *var_table_find(const char *var_id);
size_t var_table_count(void);

const char *var_id(const ammio_var_t *var);
ammio_type_t var_type(const ammio_var_t *var);
ammio_dir_t var_dir(const ammio_var_t *var);

void var_table_read(const ammio_var_t *var, ammio_var_snapshot_t *out);
void var_table_read_many(ammio_var_t *const *vars, size_t count, ammio_var_snapshot_t *out);

// Snapshot of every variable in registration order; caller frees *out
int var_table_list(ammio_var_snapshot_t **out, size_t *count);

// Client writes (API): sets the values and bumps write_seq. Values must already be converted
// to each variable's type; direction checks are the caller's job.
void var_table_client_write_many(ammio_var_t *const *vars, const ammio_value_t *values, size_t count);

// Driver updates of received data
void var_table_update(ammio_var_t *var, ammio_value_t value, ammio_quality_t quality);
void var_table_update_many(ammio_var_t *const *vars, const ammio_value_t *values, size_t count,
                           ammio_quality_t quality);

// Sets the value without touching write_seq (initial values, keeping linked variables in sync)
void var_table_set_value_silent(ammio_var_t *var, ammio_value_t value);

// Changes the quality and keeps the value
void var_table_set_quality_many(ammio_var_t *const *vars, size_t count, ammio_quality_t quality);

// Applies a connection state change to every variable of a connection:
//   disconnected -> all variables DISCONNECTED
//   connected    -> variables without a value NO_DATA, inputs with a value GOOD; outputs with an
//                   old value stay DISCONNECTED until fresh data arrives from the SUT
void var_table_mark_connection(const char *connection, bool connected);

// Called after every change of value or quality, outside the table lock
void var_table_set_change_hook(var_change_hook_t hook, void *ctx);

const char *ammio_quality_name(ammio_quality_t quality);
const char *ammio_dir_name(ammio_dir_t dir);

#endif
