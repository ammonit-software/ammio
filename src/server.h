#ifndef AMMIO_SERVER_H
#define AMMIO_SERVER_H

// Client API: JSON requests over nng REQ/REP.
//
//   {"cmd": "read",  "name": "x"}            -> {"name", "type", "dir", "value", "timestamp"}
//   {"cmd": "write", "name": "x", "value": 1} -> {"status": "ok"}
//   {"cmd": "list_vars"}                      -> {"vars": [{"name", "type", "dir"}, ...]}
//   any error                                 -> {"error": <code>, "message": "..."}

#include <stdatomic.h>
#include <stddef.h>

typedef enum
{
    ERR_NOT_FOUND = 1,     // unknown variable
    ERR_READ_ONLY = 2,     // writing an output
    ERR_INVALID_JSON = 3,  // request is not a JSON object
    ERR_INVALID_CMD = 4,   // unknown command or missing "cmd"/"name"
    ERR_MISSING_VALUE = 5, // write without "value"
    ERR_INVALID_VALUE = 6, // value not representable in the variable's type
} api_error_t;

int server_open(const char *url);
void server_run(const atomic_bool *stop); // serves until *stop becomes true
void server_close(void);

// Handles one request; the result must be released with cJSON_free().
char *server_handle(const char *request, size_t len);

#endif
