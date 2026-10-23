#ifndef AMMIO_API_SERVER_H
#define AMMIO_API_SERVER_H

// Client-facing JSON API over nng REQ/REP. See README.md ("JSON API") for the protocol.

#include <stdatomic.h>
#include <stddef.h>

int api_server_open(const char *endpoint);

// Serves requests until *running becomes false
void api_server_run(const atomic_bool *running);

void api_server_close(void);

// Handles one request (not necessarily NUL-terminated) and returns the response JSON, which the
// caller frees with free(). Exposed for the unit tests.
char *api_handle_request(const char *data, size_t len);

// Endpoint of the event publisher, reported by the "info" command (NULL if disabled)
void api_server_set_events_endpoint(const char *endpoint);

#endif
