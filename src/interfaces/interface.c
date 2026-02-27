#include "interface.h"
#include "../log.h"
#include <string.h>
#include <stdbool.h>

#define MAX_INTERFACES 16

static interface_t *interfaces[MAX_INTERFACES];
static bool initialized[MAX_INTERFACES];
static int interface_count = 0;

void interface_register(interface_t *iface)
{
    if (interface_count < MAX_INTERFACES)
    {
        interfaces[interface_count] = iface;
        initialized[interface_count] = false;
        interface_count++;
    }
}

int interfaces_init_with(cJSON *interface_config)
{
    if (!interface_config)
    {
        log_info("No interface configured");
        return 0;
    }

    int result = 0;

    cJSON *entry;
    cJSON_ArrayForEach(entry, interface_config)
    {
        cJSON *iface_type = cJSON_GetObjectItem(entry, "interface");
        if (!iface_type || !cJSON_IsString(iface_type))
        {
            log_info("Entry '%s' missing 'interface' field, skipping", entry->string);
            continue;
        }

        cJSON *spec = cJSON_GetObjectItem(entry, "specification");
        if (!spec)
        {
            log_info("Entry '%s' missing 'specification' field, skipping", entry->string);
            continue;
        }

        const char *type_name = iface_type->valuestring;
        bool found = false;

        for (int i = 0; i < interface_count; i++)
        {
            if (strcmp(interfaces[i]->name, type_name) != 0)
                continue;

            found = true;
            log_info("Initializing '%s' (interface: %s)", entry->string, type_name);

            if (interfaces[i]->init(spec) != 0)
            {
                log_info("Failed to initialize '%s'", entry->string);
                result = -1;
            }
            else
            {
                initialized[i] = true;
            }
            break;
        }

        if (!found)
            log_info("No registered interface for type '%s' (entry '%s'), skipping", type_name, entry->string);
    }

    return result;
}

int interfaces_start(void)
{
    for (int i = 0; i < interface_count; i++)
    {
        if (!initialized[i])
            continue;

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
        if (!initialized[i])
            continue;

        if (interfaces[i]->stop)
        {
            log_info("Stopping interface: %s", interfaces[i]->name);
            interfaces[i]->stop();
        }
    }
}
