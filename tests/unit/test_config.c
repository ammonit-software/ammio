#include <stdlib.h>

#include "api/api_server.h"
#include "core/connections.h"
#include "test.h"

// Loads a configuration from JSON text; returns the number of errors
static int load(const char *json)
{
    var_table_init();
    cJSON *root = cJSON_Parse(json);
    cfg_t cfg = {0};
    int rc = connections_load(root, &cfg);
    if (rc != 0 && cfg.errors == 0)
        cfg.errors = 1;
    cJSON_Delete(root);
    return cfg.errors;
}

static void unload(void)
{
    connections_destroy();
    var_table_free();
}

#define MODBUS(regs) \
    "{\"plc\": {\"protocol\": \"modbus\", \"settings\": {\"host\": \"127.0.0.1\", \"registers\": " regs "}}}"

TEST(test_config_two_connections_of_one_protocol)
{
    // Before 0.6 the second connection silently replaced the first one's settings
    CHECK_EQ_INT(load("{"
        "\"a\": {\"protocol\": \"modbus\", \"settings\": {\"host\": \"10.0.0.1\", \"registers\": {"
        "  \"inputs\": [{\"var_id\": \"a_in\", \"address\": 0, \"type\": \"uint16\"}]}}},"
        "\"b\": {\"protocol\": \"modbus\", \"settings\": {\"host\": \"10.0.0.2\", \"registers\": {"
        "  \"inputs\": [{\"var_id\": \"b_in\", \"address\": 0, \"type\": \"uint16\"}]}}}}"), 0);
    CHECK_EQ_INT(connections_count(), 2);
    ammio_conn_info_t info[2];
    connections_list(info, 2);
    CHECK_EQ_STR(info[0].name, "a");
    CHECK_EQ_STR(info[1].name, "b");
    ammio_var_snapshot_t s;
    var_table_read(var_table_find("a_in"), &s);
    CHECK_EQ_STR(s.connection, "a");
    var_table_read(var_table_find("b_in"), &s);
    CHECK_EQ_STR(s.connection, "b");
    unload();
}

TEST(test_config_rejects_mistakes)
{
    CHECK(load(MODBUS("{\"inputs\": [{\"var_id\": \"x\", \"address\": 0, \"type\": \"uint16\", \"adress\": 1}]}")) > 0);
    unload();
    CHECK(load(MODBUS("{\"inputs\": [{\"var_id\": \"x\", \"address\": 0}]}")) > 0);
    unload();
    CHECK(load(MODBUS("{\"inputs\": [{\"var_id\": \"x\", \"address\": 0, \"type\": \"uint16\"},"
                      " {\"var_id\": \"x\", \"address\": 5, \"type\": \"uint16\"}]}")) > 0);
    unload();
    CHECK(load("{\"x\": {\"protocol\": \"modbus\"}}") > 0);
    unload();
    CHECK(load("{}") > 0);
    unload();
    // Comments via "description" / "_..." keys are allowed
    CHECK_EQ_INT(load(MODBUS("{\"_comment\": \"ok\", \"inputs\": [{\"var_id\": \"x\", \"address\": 0,"
                             " \"type\": \"uint16\", \"description\": \"speed\"}]}")), 0);
    unload();
}

TEST(test_config_trdp_layout_checks)
{
    const char *base =
        "{\"t\": {\"protocol\": \"trdp\", \"settings\": {\"containers\": {\"inputs\": [{\"id\": \"C\","
        " \"comid\": 1, \"dest_ip\": \"239.0.0.1\", \"size_bits\": 32, \"variables\": [%s]}]}}}}";
    char buf[1024];

    snprintf(buf, sizeof(buf), base, "{\"var_id\": \"v\", \"offset\": 0, \"type\": \"uint32\"}");
    CHECK_EQ_INT(load(buf), 0);
    CHECK(var_table_find("C.is_enabled") != NULL);
    unload();

    // Ends past size_bits: before 0.6 this overflowed the telegram buffer
    snprintf(buf, sizeof(buf), base, "{\"var_id\": \"v\", \"offset\": 8, \"type\": \"uint32\"}");
    CHECK(load(buf) > 0);
    unload();

    snprintf(buf, sizeof(buf), base, "{\"var_id\": \"v\", \"offset\": 4, \"type\": \"uint8\"}");
    CHECK(load(buf) > 0);
    unload();

    snprintf(buf, sizeof(buf), base,
             "{\"var_id\": \"f\", \"offset\": 0, \"type\": \"bitset16\", \"bits\": [\"a\", \"\", \"c\"]}");
    CHECK_EQ_INT(load(buf), 0);
    CHECK(var_table_find("f.a") && var_table_find("f.bit1") && var_table_find("f.c"));
    CHECK(var_type(var_table_find("f")) == AMMIO_TYPE_UINT16);
    unload();
}

// The request is in an exactly-sized heap buffer with no NUL terminator: under ASan any read past
// the end (the pre-0.6 cJSON_Parse bug) is reported.
static cJSON *request(const char *text)
{
    size_t len = strlen(text);
    char *buf = malloc(len);
    memcpy(buf, text, len);
    char *resp = api_handle_request(buf, len);
    free(buf);
    cJSON *json = cJSON_Parse(resp);
    free(resp);
    return json;
}

static const char *error_code(const cJSON *resp)
{
    const cJSON *err = cJSON_GetObjectItem(resp, "error");
    const cJSON *code = err ? cJSON_GetObjectItem(err, "code") : NULL;
    return code ? code->valuestring : "(none)";
}

TEST(test_config_api_requests)
{
    CHECK_EQ_INT(load(MODBUS("{\"inputs\": [{\"var_id\": \"in\", \"address\": 0, \"type\": \"int16\"}],"
                             " \"outputs\": [{\"var_id\": \"out\", \"address\": 1, \"type\": \"uint16\"}]}")), 0);

    cJSON *r = request("{\"cmd\":\"write\",\"var_id\":\"in\",\"value\":-3}");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(r);

    r = request("{\"cmd\":\"read\",\"var_id\":\"in\"}");
    CHECK(cJSON_GetObjectItem(r, "value")->valuedouble == -3);
    CHECK_EQ_STR(cJSON_GetObjectItem(r, "type")->valuestring, "int16");
    cJSON_Delete(r);

    r = request("{\"cmd\":\"write\",\"var_id\":\"out\",\"value\":1}");
    CHECK_EQ_STR(error_code(r), "READ_ONLY");
    cJSON_Delete(r);

    r = request("{\"cmd\":\"write\",\"values\":{\"in\":1,\"nope\":2}}");
    CHECK_EQ_STR(error_code(r), "NOT_FOUND");
    cJSON_Delete(r);

    r = request("{\"cmd\":\"read\"");  // truncated
    CHECK_EQ_STR(error_code(r), "INVALID_JSON");
    cJSON_Delete(r);

    r = request("{\"cmd\":\"info\"} ");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(r);

    unload();
}
