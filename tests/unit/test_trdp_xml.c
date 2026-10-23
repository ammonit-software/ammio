#include <stdio.h>
#include <stdlib.h>

#include "core/connections.h"
#include "drivers/trdp_xml.h"
#include "test.h"
#include "trdp_if_light.h"

#define XML_FILE AMMIO_TEST_DATA_DIR "/trdp_device.xml"

static const trdp_xml_telegram_t *find_telegram(const trdp_xml_import_t *imp, uint32_t comid)
{
    for (size_t i = 0; i < imp->n_telegrams; i++)
    {
        if (imp->telegrams[i].comid == comid)
            return &imp->telegrams[i];
    }
    return NULL;
}

static const trdp_xml_var_t *find_var(const trdp_xml_telegram_t *t, const char *id)
{
    for (size_t i = 0; t && i < t->n_vars; i++)
    {
        if (strcmp(t->vars[i].var_id, id) == 0)
            return &t->vars[i];
    }
    return NULL;
}

TEST(test_trdp_xml_import)
{
    CHECK(tlc_init(NULL, NULL, NULL) == TRDP_NO_ERR);

    cfg_t cfg = {0};
    trdp_xml_import_t imp;
    CHECK(trdp_xml_import(XML_FILE, "eth0", true, "xml", &cfg, &imp) == 0);
    CHECK_EQ_INT(cfg.errors, 0);
    CHECK_EQ_INT(imp.host_ip, 0x0A000005);
    CHECK_EQ_INT(imp.n_telegrams, 3);

    // Device source -> SUT output: ammio subscribes, joining the multicast group
    const trdp_xml_telegram_t *status = find_telegram(&imp, 3001);
    CHECK(status != NULL);
    if (status)
    {
        CHECK_EQ_STR(status->id, "DoorStatus");
        CHECK_EQ_INT(status->dir, AMMIO_DIR_OUTPUT);
        CHECK_EQ_INT(status->dest_ip, 0xEF0A0001);
        CHECK_EQ_INT(status->source_ip, 0x0A000005);
        CHECK_EQ_INT(status->period_ms, 100);
        CHECK_EQ_INT(status->timeout_ms, 300);
        // lifeSign(2) + 2 * (open(1) + force(2)) + speed(4) + since(4 + 4)
        CHECK_EQ_INT(status->size_bytes, 2 + 2 * 3 + 4 + 8);

        const trdp_xml_var_t *v = find_var(status, "DoorStatus.door[1].force");
        CHECK(v && v->byte_offset == 2 + 3 + 1 && v->type == AMMIO_TYPE_INT16);
        v = find_var(status, "DoorStatus.speed");
        CHECK(v && v->byte_offset == 8 && v->type == AMMIO_TYPE_FLOAT32);
        v = find_var(status, "DoorStatus.since.microseconds");
        CHECK(v && v->byte_offset == 16 && v->type == AMMIO_TYPE_INT32);
        CHECK(find_var(status, "DoorStatus.door[0].open") != NULL);
    }

    // Device sink -> SUT input: ammio publishes to the destination
    const trdp_xml_telegram_t *cmd = find_telegram(&imp, 3002);
    CHECK(cmd != NULL);
    if (cmd)
    {
        CHECK_EQ_INT(cmd->dir, AMMIO_DIR_INPUT);
        CHECK_EQ_INT(cmd->dest_ip, 0xEF0A0002);
        CHECK_EQ_INT(cmd->period_ms, 50);
        CHECK_EQ_INT(cmd->size_bytes, 1 + 3 + 8);
        const trdp_xml_var_t *v = find_var(cmd, "DoorCommand.text[2]");
        CHECK(v && v->byte_offset == 3 && v->type == AMMIO_TYPE_UINT8);
        v = find_var(cmd, "DoorCommand.counter");
        CHECK(v && v->byte_offset == 4 && v->type == AMMIO_TYPE_UINT64);
    }

    // No type attribute: the source is the device itself, so it is a device source
    const trdp_xml_telegram_t *diag = find_telegram(&imp, 3003);
    CHECK(diag && diag->dir == AMMIO_DIR_OUTPUT);
    CHECK(diag && find_var(diag, "Diag.t48.ticks") && diag->size_bytes == 4 + 6);
    trdp_xml_import_free(&imp);

    // From the device's own perspective the directions flip
    CHECK(trdp_xml_import(XML_FILE, "eth0", false, "xml", &cfg, &imp) == 0);
    status = find_telegram(&imp, 3001);
    CHECK(status && status->dir == AMMIO_DIR_INPUT && status->dest_ip == 0xEF0A0001);
    cmd = find_telegram(&imp, 3002);
    CHECK(cmd && cmd->dir == AMMIO_DIR_OUTPUT && cmd->source_ip == 0x0A000001);
    diag = find_telegram(&imp, 3003);
    CHECK(diag && diag->dir == AMMIO_DIR_INPUT && diag->dest_ip == 0x0A000001);
    trdp_xml_import_free(&imp);

    // Unknown interface / file
    int errors = cfg.errors;
    CHECK(trdp_xml_import(XML_FILE, "eth9", true, "xml", &cfg, &imp) != 0);
    CHECK(trdp_xml_import(AMMIO_TEST_DATA_DIR "/missing.xml", "eth0", true, "xml", &cfg, &imp) != 0);
    CHECK_EQ_INT(cfg.errors, errors + 2);

    tlc_terminate();
}

TEST(test_trdp_xml_connection)
{
    var_table_init();
    cJSON *root = cJSON_Parse(
        "{\"train\": {\"protocol\": \"trdp\", \"settings\": {\"local_ip\": \"0.0.0.0\","
        " \"xml\": {\"file\": \"" XML_FILE "\", \"interface\": \"eth0\"}}}}");
    cfg_t cfg = {0};
    CHECK(connections_load(root, &cfg) == 0);
    CHECK_EQ_INT(cfg.errors, 0);

    ammio_var_t *v = var_table_find("DoorStatus.door[1].force");
    CHECK(v && var_dir(v) == AMMIO_DIR_OUTPUT);
    v = var_table_find("DoorCommand.cmd");
    CHECK(v && var_dir(v) == AMMIO_DIR_INPUT);
    CHECK(var_table_find("DoorCommand.is_enabled") != NULL);
    CHECK(var_table_find("train.connected") != NULL);

    connections_destroy();
    cJSON_Delete(root);
    var_table_free();
}
