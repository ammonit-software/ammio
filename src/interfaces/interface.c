#include "interface.h"
#include "../log.h"
#include <string.h>

#define MAX_INTERFACES 16

static interface_t *interfaces[MAX_INTERFACES];
static int interface_count = 0;

void interface_register(interface_t *iface)
{
    if (interface_count < MAX_INTERFACES)
    {
        interfaces[interface_count++] = iface;
    }
}

int interfaces_init_with(cJSON *interface_config)
{
    if (!interface_config)
    {
        log_info("No interface configured");
        return 0;
    }

    for (int i = 0; i < interface_count; i++)
    {
        cJSON *iface_config = cJSON_GetObjectItem(interface_config, interfaces[i]->name);
        if (iface_config)
        {
            log_info("Initializing interface: %s", interfaces[i]->name);
            if (interfaces[i]->init(iface_config) != 0)
            {
                log_info("Failed to initialize interface: %s", interfaces[i]->name);
                return -1;
            }
        }
    }

    return 0;
}

int interfaces_start(void)
{
    for (int i = 0; i < interface_count; i++)
    {
        if (interfaces[i]->start)
        {
            log_info("Starting interface: %s", interfaces[i]->name);
            if (interfaces[i]->start() != 0)
            {
                return -1;
            }
        }
    }
    return 0;
}

void interfaces_stop(void)
{
    for (int i = 0; i < interface_count; i++)
    {
        if (interfaces[i]->stop)
        {
            log_info("Stopping interface: %s", interfaces[i]->name);
            interfaces[i]->stop();
        }
    }
}
