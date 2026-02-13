#include "protocol.h"
#include "../log.h"
#include <string.h>

#define MAX_PROTOCOLS 16

static protocol_t *protocols[MAX_PROTOCOLS];
static int protocol_count = 0;

void protocol_register(protocol_t *proto)
{
    if (protocol_count < MAX_PROTOCOLS)
    {
        protocols[protocol_count++] = proto;
    }
}

int protocols_init(void)
{
    cJSON *interface_config = config_get("interface");
    if (!interface_config)
    {
        log_info("No interface configured");
        return 0;
    }

    for (int i = 0; i < protocol_count; i++)
    {
        cJSON *proto_config = cJSON_GetObjectItem(interface_config, protocols[i]->name);
        if (proto_config)
        {
            log_info("Initializing protocol: %s", protocols[i]->name);
            if (protocols[i]->init(proto_config) != 0)
            {
                log_info("Failed to initialize protocol: %s", protocols[i]->name);
                return -1;
            }
        }
    }

    return 0;
}

int protocols_start(void)
{
    for (int i = 0; i < protocol_count; i++)
    {
        if (protocols[i]->start)
        {
            log_info("Starting protocol: %s", protocols[i]->name);
            if (protocols[i]->start() != 0)
            {
                return -1;
            }
        }
    }
    return 0;
}

void protocols_stop(void)
{
    for (int i = 0; i < protocol_count; i++)
    {
        if (protocols[i]->stop)
        {
            log_info("Stopping protocol: %s", protocols[i]->name);
            protocols[i]->stop();
        }
    }
}
