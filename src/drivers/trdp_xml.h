#ifndef AMMIO_TRDP_XML_H
#define AMMIO_TRDP_XML_H

// Imports process-data telegrams from a TRDP XML device configuration (IEC 61375-2-3 / TCNopen
// trdp-config.xsd), so datasets do not have to be re-typed by hand as JSON offsets.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/types.h"
#include "core/var_table.h"
#include "util/cfg.h"

typedef struct
{
    char *var_id;          // "<telegram id>.<element path>", e.g. "DoorStatus.door[2].open"
    ammio_type_t type;
    uint32_t byte_offset;  // within the packed (wire format) dataset
} trdp_xml_var_t;

typedef struct
{
    char *id;              // dataset name (unique within the import)
    ammio_dir_t dir;       // from the SUT's point of view
    uint32_t comid;
    uint32_t dest_ip;      // inputs: destination; outputs: multicast group to join (0 = none)
    uint32_t source_ip;    // outputs: optional source filter (0 = any)
    uint32_t period_ms;
    uint32_t timeout_ms;   // outputs (0 = default)
    uint32_t size_bytes;
    trdp_xml_var_t *vars;
    size_t n_vars;
} trdp_xml_telegram_t;

typedef struct
{
    uint32_t host_ip;      // IP of the bus interface in the XML (0 if not given)
    trdp_xml_telegram_t *telegrams;
    size_t n_telegrams;
} trdp_xml_import_t;

// sut_perspective: the XML describes the SUT (its "source" telegrams are SUT outputs that
// ammio subscribes to, its "sink" telegrams are SUT inputs that ammio publishes). If false the
// XML describes the device ammio stands in for, and the directions are swapped.
// Requires the TRDP library to be initialised (tlc_init). Returns 0 on success.
int trdp_xml_import(const char *file, const char *interface_name, bool sut_perspective,
                    const char *path, cfg_t *cfg, trdp_xml_import_t *out);

void trdp_xml_import_free(trdp_xml_import_t *import);

#endif
