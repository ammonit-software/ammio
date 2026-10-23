#include "api/events.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/var_table.h"
#include "util/log.h"

static nng_socket pub_sock = NNG_SOCKET_INITIALIZER;
static bool pub_open = false;

static void on_change(const ammio_var_snapshot_t *snap, void *ctx)
{
    (void)ctx;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "var_id", snap->var_id);
    if (snap->has_value)
        cJSON_AddItemToObject(msg, "value", ammio_value_to_json(snap->type, snap->value));
    else
        cJSON_AddNullToObject(msg, "value");
    cJSON_AddStringToObject(msg, "quality", ammio_quality_name(snap->quality));
    char ts[32];
    snprintf(ts, sizeof(ts), "%llu", (unsigned long long)snap->timestamp_ns);
    cJSON_AddRawToObject(msg, "timestamp_ns", ts);

    char *str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!str)
        return;

    // PUB never blocks on slow subscribers; NONBLOCK also covers a full send queue
    int rv = nng_send(pub_sock, str, strlen(str), NNG_FLAG_NONBLOCK);
    if (rv != 0 && rv != NNG_EAGAIN)
        log_debug("events: send failed: %s", nng_strerror(rv));
    free(str);
}

int events_open(const char *endpoint)
{
    int rv;
    if ((rv = nng_pub0_open(&pub_sock)) != 0)
    {
        log_error("events: cannot open socket: %s", nng_strerror(rv));
        return -1;
    }
    pub_open = true;

    if ((rv = nng_listen(pub_sock, endpoint, NULL, 0)) != 0)
    {
        log_error("events: cannot listen on %s: %s", endpoint, nng_strerror(rv));
        events_close();
        return -1;
    }

    var_table_set_change_hook(on_change, NULL);
    return 0;
}

void events_close(void)
{
    if (!pub_open)
        return;
    var_table_set_change_hook(NULL, NULL);
    nng_close(pub_sock);
    pub_open = false;
}
