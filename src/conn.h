#ifndef AMMIO_CONN_H
#define AMMIO_CONN_H

#include "cJSON.h"

#include <stddef.h>

// Validates the whole config and creates one connection per entry.
// Reports every error found; on failure nothing is left allocated.
int conns_load(const cJSON *root);

// Starts one thread per connection.
int conns_start(void);

// Stops all threads, disconnects and frees every connection.
void conns_stop(void);

#endif
