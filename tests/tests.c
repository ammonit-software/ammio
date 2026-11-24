// ammio test suite: values, config validation, client API and end-to-end
// runs against real in-process Modbus TCP and OPC UA servers (POSIX only).

#include "cJSON.h"
#include "conn.h"
#include "log.h"
#include "server.h"
#include "vars.h"

#include <modbus.h>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

// --- mini test framework -----------------------------------------------------

static int checks = 0, failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        checks++;                                                            \
        if (!(cond))                                                         \
        {                                                                    \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    thrd_sleep(&ts, NULL);
}

// Polls `cond` for up to 3 s.
#define EVENTUALLY(cond)                          \
    do                                            \
    {                                             \
        for (int _i = 0; _i < 300 && !(cond); _i++) \
            sleep_ms(10);                         \
        CHECK(cond);                              \
    } while (0)

static log_level_t base_level = LOG_WARN; // AMMIO_TEST_DEBUG=1 for debug logs

static void quiet(bool on)
{
    // Some tests produce errors on purpose; hide them.
    log_set_level(on ? (log_level_t)-1 : base_level);
}

// --- helpers -----------------------------------------------------------------

static cJSON *config_root = NULL;

static bool load(const char *json)
{
    config_root = cJSON_Parse(json);
    if (!config_root)
    {
        fprintf(stderr, "test config is not valid JSON: %s\n", json);
        return false;
    }
    if (conns_load(config_root) != 0)
    {
        cJSON_Delete(config_root);
        config_root = NULL;
        return false;
    }
    return true;
}

static void unload(void)
{
    conns_stop();
    vars_clear();
    cJSON_Delete(config_root);
    config_root = NULL;
}

// Sends one request through the API handler and returns the parsed reply.
static cJSON *api(const char *request)
{
    char *resp = server_handle(request, strlen(request));
    cJSON *json = cJSON_Parse(resp);
    cJSON_free(resp);
    return json;
}

static int api_error(const char *request)
{
    cJSON *r = api(request);
    const cJSON *e = cJSON_GetObjectItem(r, "error");
    int code = cJSON_IsNumber(e) ? e->valueint : 0;
    CHECK(code == 0 || cJSON_IsString(cJSON_GetObjectItem(r, "message")));
    cJSON_Delete(r);
    return code;
}

static bool api_write(const char *name, double value)
{
    char req[256];
    snprintf(req, sizeof(req), "{\"cmd\":\"write\",\"var_id\":\"%s\",\"value\":%.17g}", name, value);
    return api_error(req) == 0;
}

static double api_read(const char *name, uint64_t *timestamp)
{
    char req[256];
    snprintf(req, sizeof(req), "{\"cmd\":\"read\",\"var_id\":\"%s\"}", name);
    cJSON *r = api(req);
    const cJSON *v = cJSON_GetObjectItem(r, "value");
    double d = cJSON_IsBool(v) ? (cJSON_IsTrue(v) ? 1 : 0) : cJSON_IsNumber(v) ? v->valuedouble : NAN;
    if (timestamp)
    {
        const cJSON *ts = cJSON_GetObjectItem(r, "timestamp");
        *timestamp = cJSON_IsNumber(ts) ? (uint64_t)ts->valuedouble : 0;
    }
    cJSON_Delete(r);
    return d;
}

static uint64_t api_timestamp(const char *name)
{
    uint64_t ts = 0;
    api_read(name, &ts);
    return ts;
}

// --- values ------------------------------------------------------------------

static void test_values(void)
{
    type_t t;
    CHECK(type_parse("float32", &t) && t == TYPE_FLOAT32);
    CHECK(type_parse("bool", &t) && t == TYPE_BOOL);
    CHECK(!type_parse("unit16", &t));

    value_t v;
    CHECK(value_from_double(TYPE_UINT8, 255, &v) && v.u8 == 255);
    CHECK(!value_from_double(TYPE_UINT8, 256, &v));
    CHECK(!value_from_double(TYPE_UINT8, -1, &v));
    CHECK(!value_from_double(TYPE_UINT16, 1.5, &v));
    CHECK(value_from_double(TYPE_INT8, -128, &v) && v.i8 == -128);
    CHECK(!value_from_double(TYPE_INT8, -129, &v));
    CHECK(value_from_double(TYPE_BOOL, 1, &v) && v.b);
    CHECK(!value_from_double(TYPE_BOOL, 2, &v));
    CHECK(value_from_double(TYPE_UINT32, 4294967295.0, &v) && v.u32 == UINT32_MAX);
    CHECK(!value_from_double(TYPE_UINT32, 4294967296.0, &v));
    CHECK(value_from_double(TYPE_INT32, -2147483648.0, &v) && v.i32 == INT32_MIN);
    CHECK(value_from_double(TYPE_FLOAT32, 3.4e38, &v));
    CHECK(!value_from_double(TYPE_FLOAT32, 1e39, &v));
    CHECK(value_from_double(TYPE_FLOAT64, -1e308, &v) && v.f64 == -1e308);
    CHECK(!value_from_double(TYPE_FLOAT64, NAN, &v));

    uint8_t b[8];
    v.i16 = -2;
    value_to_be(TYPE_INT16, v, b);
    CHECK(b[0] == 0xFF && b[1] == 0xFE);
    CHECK(value_from_be(TYPE_INT16, b).i16 == -2);

    v.f32 = 1.0f;
    value_to_be(TYPE_FLOAT32, v, b);
    CHECK(b[0] == 0x3F && b[1] == 0x80 && b[2] == 0 && b[3] == 0);

    v.u32 = 0x01020304;
    value_to_be(TYPE_UINT32, v, b);
    CHECK(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4);

    v.f64 = -123.456;
    value_to_be(TYPE_FLOAT64, v, b);
    CHECK(value_from_be(TYPE_FLOAT64, b).f64 == -123.456);
    CHECK(b[0] == 0xC0); // sign bit first
}

// --- config validation -------------------------------------------------------

#define MB "\"protocol\":\"modbus\",\"host\":\"127.0.0.1\""
#define TR "\"protocol\":\"trdp\""

static void test_config(void)
{
    static const char *const bad[] = {
        "[]",
        "{}",
        "{\"a\":1}",
        "{\"a\":{\"protocol\":\"can\"}}",
        "{\"a\":{\"host\":\"x\"}}",
        // modbus
        "{\"a\":{\"protocol\":\"modbus\",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"device\":\"/dev/ttyS0\",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB "}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"adress\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"unit16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":65536,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":65534,\"type\":\"float64\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint32\"},"
        "{\"var_id\":\"y\",\"address\":1,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"},"
        "{\"var_id\":\"x\",\"address\":1,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"word_order\":\"middle\",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"baud\":9600,\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"period_ms\":0,\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":-1,\"type\":\"uint16\"}]}}",
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0.5,\"type\":\"uint16\"}]}}",
        // same connection name twice
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]},"
        " \"a\":{" MB ",\"outputs\":[{\"var_id\":\"y\",\"address\":0,\"type\":\"uint16\"}]}}",
        // same variable name in two connections
        "{\"a\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]},"
        " \"b\":{" MB ",\"outputs\":[{\"var_id\":\"x\",\"address\":0,\"type\":\"uint16\"}]}}",
        // opcua
        "{\"a\":{\"protocol\":\"opcua\",\"outputs\":[{\"var_id\":\"x\",\"node_id\":\"ns=1;i=1\",\"type\":\"bool\"}]}}",
        "{\"a\":{\"protocol\":\"opcua\",\"endpoint\":\"opc.tcp://h:4840\",\"username\":\"u\","
        "\"outputs\":[{\"var_id\":\"x\",\"node_id\":\"ns=1;i=1\",\"type\":\"bool\"}]}}",
        "{\"a\":{\"protocol\":\"opcua\",\"endpoint\":\"opc.tcp://h:4840\","
        "\"outputs\":[{\"var_id\":\"x\",\"node_id\":\"what\",\"type\":\"bool\"}]}}",
        // trdp
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"dest_ip\":\"239.0.0.1\",\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0\",\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":2,\"dest_ip\":\"239.0.0.1\","
        "\"variables\":[{\"var_id\":\"x\",\"offset\":1,\"type\":\"uint16\"}]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0.1\","
        "\"variables\":[{\"var_id\":\"x\",\"offset\":0,\"type\":\"uint16\"},{\"var_id\":\"y\",\"offset\":1,\"type\":\"uint8\"}]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0.1\","
        "\"variables\":[{\"var_id\":\"x\",\"offset\":0,\"type\":\"bitset8\",\"bits\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0.1\","
        "\"variables\":[{\"var_id\":\"x\",\"offset\":0,\"type\":\"uint8\",\"bits\":[\"a\"]}]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0.1\","
        "\"variables\":[{\"var_id\":\"x\",\"offset\":0,\"type\":\"bitset8\",\"bits\":[\"a\"]},"
        "{\"var_id\":\"y\",\"offset\":0,\"type\":\"uint8\"}]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"comid\":7,\"size\":4,\"variables\":[]},{\"comid\":7,\"size\":4,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"comid\":7,\"size\":1433,\"variables\":[]}]}}",
        // v0.5 format
        "{\"a\":{\"interface\":\"modbus\",\"specification\":{}}}",
        // TRDP message data
        "{\"a\":{" TR ",\"inputs\":[{\"type\":\"Mq\",\"var_id\":\"m\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"enable_id\":\"m.go\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"inputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"dest_ip\":\"10.0.0.1\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"dest_ip\":\"10.0.0.1\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"comid\":1,"
        "\"variables\":[{\"type\":\"padding\",\"bits\":4}]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"comid\":1,"
        "\"variables\":[{\"var_id\":\"x\",\"type\":\"entry\",\"iterations_id\":\"n\"}]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"comid\":1,"
        "\"structs\":{\"e\":[{\"var_id\":\"v\",\"type\":\"uint8\"}]},"
        "\"variables\":[{\"var_id\":\"x\",\"type\":\"e\",\"iterations_id\":\"n\",\"max_iterations\":300}]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mn\",\"var_id\":\"m\",\"comid\":1,\"variables\":[]},"
        "{\"type\":\"Mn\",\"var_id\":\"k\",\"comid\":1,\"variables\":[]}]}}",
        // Mp without reply_to, Mp answering nothing, Mr without its Mp
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mp\",\"var_id\":\"m\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mp\",\"var_id\":\"m\",\"reply_to\":\"r\",\"comid\":1,\"variables\":[]}]}}",
        "{\"a\":{" TR ",\"outputs\":[{\"type\":\"Mr\",\"var_id\":\"r\",\"comid\":1,\"variables\":[]}]}}",
        // two TRDP connections on the same local IP
        "{\"a\":{" TR ",\"outputs\":[{\"comid\":7,\"size\":1,\"variables\":[]}]},"
        " \"b\":{" TR ",\"outputs\":[{\"comid\":8,\"size\":1,\"variables\":[]}]}}",
    };

    quiet(true);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        bool loaded = load(bad[i]);
        if (loaded)
        {
            fprintf(stderr, "accepted invalid config #%zu: %s\n", i, bad[i]);
            unload();
        }
        CHECK(!loaded);
        CHECK(vars_count() == 0); // nothing left behind
    }
    quiet(false);

    // A valid TRDP config: bitsets expand to "<name>.<bit>" bools, unused bits are skipped.
    CHECK(load("{\"t\":{" TR ",\"local_ip\":\"0.0.0.0\","
               "\"inputs\":[{\"var_id\":\"ST\",\"comid\":1,\"size\":4,\"dest_ip\":\"239.0.0.1\","
               "\"variables\":[{\"var_id\":\"st\",\"offset\":0,\"type\":\"bitset16\",\"bits\":[\"a\",\"\",null,\"d\"]},"
               "{\"var_id\":\"v\",\"offset\":2,\"type\":\"int16\"}]}],"
               "\"outputs\":[{\"comid\":1,\"size\":1,\"variables\":[{\"var_id\":\"o\",\"offset\":0,\"type\":\"bool\"}]}]}}"));
    CHECK(vars_count() == 7); // st, st.a, st.bit1, st.bit2, st.d, v, o
    CHECK(vars_find("st.a") && var_type(vars_find("st.a")) == TYPE_BOOL);
    CHECK(vars_find("st.bit1") && vars_find("st.bit2") && vars_find("st.d"));
    CHECK(vars_find("st") && var_type(vars_find("st")) == TYPE_UINT16);
    CHECK(var_dir(vars_find("st")) == DIR_OUTPUT); // read-only copy of the bits
    CHECK(vars_find("o") && var_dir(vars_find("o")) == DIR_OUTPUT);
    unload();
}

// --- client API --------------------------------------------------------------

static atomic_bool api_stop = false;

static int api_thread(void *arg)
{
    (void)arg;
    server_run(&api_stop);
    return 0;
}

static void test_api(void)
{
    CHECK(load("{\"plc\":{" MB ",\"port\":1,"
               "\"inputs\":[{\"var_id\":\"speed\",\"address\":0,\"type\":\"uint16\"},"
               "{\"var_id\":\"on\",\"address\":1,\"type\":\"bool\"},"
               "{\"var_id\":\"t\",\"address\":2,\"type\":\"float32\"}],"
               "\"outputs\":[{\"var_id\":\"state\",\"address\":10,\"type\":\"int16\"}]}}"));

    cJSON *r = api("{\"cmd\":\"list_vars\"}");
    const cJSON *list = cJSON_GetObjectItem(r, "vars");
    CHECK(cJSON_GetArraySize(list) == 4);
    const cJSON *first = cJSON_GetArrayItem(list, 0);
    CHECK(strcmp(cJSON_GetObjectItem(first, "var_id")->valuestring, "speed") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(first, "type")->valuestring, "uint16") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 3), "dir")->valuestring, "output") == 0);
    cJSON_Delete(r);

    CHECK(api_timestamp("speed") == 0); // never written
    CHECK(api_write("speed", 1234));
    CHECK(api_read("speed", NULL) == 1234);
    CHECK(api_timestamp("speed") > 0);
    CHECK(api_write("t", 21.5) && api_read("t", NULL) == 21.5);

    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"on\",\"value\":true}") == 0);
    r = api("{\"cmd\":\"read\",\"var_id\":\"on\"}");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "value")));
    CHECK(cJSON_GetObjectItem(r, "type")->valueint == TYPE_BOOL); // numeric code, as in v0.5
    CHECK(strcmp(cJSON_GetObjectItem(r, "dir")->valuestring, "input") == 0);
    cJSON_Delete(r);

    CHECK(api_error("{\"cmd\":\"read\",\"var_id\":\"nope\"}") == ERR_NOT_FOUND);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"state\",\"value\":1}") == ERR_READ_ONLY);
    CHECK(api_error("not json") == ERR_INVALID_JSON);
    CHECK(api_error("[1,2]") == ERR_INVALID_JSON);
    r = api("{\"cmd\":\"list_errors\"}");
    const cJSON *errs = cJSON_GetObjectItem(r, "errors");
    CHECK(cJSON_IsString(cJSON_GetObjectItem(errs, "1")) && cJSON_IsString(cJSON_GetObjectItem(errs, "6")));
    cJSON_Delete(r);

    CHECK(api_error("{}") == ERR_INVALID_CMD);
    CHECK(api_error("{\"cmd\":\"read\",\"name\":\"speed\"}") == ERR_INVALID_CMD); // the key is var_id
    CHECK(api_error("{\"cmd\":\"delete\",\"var_id\":\"speed\"}") == ERR_INVALID_CMD);
    CHECK(api_error("{\"cmd\":\"read\"}") == ERR_INVALID_CMD);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"speed\"}") == ERR_MISSING_VALUE);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"speed\",\"value\":\"1\"}") == ERR_INVALID_VALUE);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"speed\",\"value\":70000}") == ERR_INVALID_VALUE);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"speed\",\"value\":-1}") == ERR_INVALID_VALUE);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"speed\",\"value\":1.5}") == ERR_INVALID_VALUE);
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"on\",\"value\":2}") == ERR_INVALID_VALUE);
    CHECK(api_read("speed", NULL) == 1234); // rejected writes change nothing

    // Same API over a real nng socket; messages are not NUL-terminated.
    atomic_store(&api_stop, false);
    CHECK(server_open("tcp://127.0.0.1:15555") == 0);
    thrd_t thread;
    thrd_create(&thread, api_thread, NULL);

    nng_socket req;
    CHECK(nng_req0_open(&req) == 0);
    nng_socket_set_ms(req, NNG_OPT_RECVTIMEO, 2000);
    CHECK(nng_dial(req, "tcp://127.0.0.1:15555", NULL, 0) == 0);
    char msg[] = "{\"cmd\":\"read\",\"var_id\":\"speed\"}";
    CHECK(nng_send(req, msg, sizeof(msg) - 1, 0) == 0);
    char *buf = NULL;
    size_t size = 0;
    CHECK(nng_recv(req, &buf, &size, NNG_FLAG_ALLOC) == 0);
    cJSON *reply = buf ? cJSON_ParseWithLength(buf, size) : NULL;
    const cJSON *value = cJSON_GetObjectItem(reply, "value");
    CHECK(cJSON_IsNumber(value) && value->valuedouble == 1234);
    cJSON_Delete(reply);
    if (buf)
        nng_free(buf, size);

    atomic_store(&api_stop, true);
#if defined(__SANITIZE_THREAD__)
    // nng's receive timeout never fires under ThreadSanitizer: wake the server instead.
    if (nng_send(req, msg, sizeof(msg) - 1, 0) == 0 && nng_recv(req, &buf, &size, NNG_FLAG_ALLOC) == 0)
        nng_free(buf, size);
#endif
    nng_close(req);
    thrd_join(thread, NULL);
    server_close();
    unload();
}

// --- Modbus end-to-end -------------------------------------------------------

#define MB_PORT 15502

static modbus_mapping_t *mb_map;
static mtx_t mb_mutex;
static atomic_bool mb_ready = false, mb_drop = false;
static atomic_int mb_accepts = 0;

// Minimal multi-client Modbus TCP server (select loop, as in libmodbus' examples).
static int mb_server(void *arg)
{
    (void)arg;
    modbus_t *ctx = modbus_new_tcp("127.0.0.1", MB_PORT);
    int listener = modbus_tcp_listen(ctx, 8);
    atomic_store(&mb_ready, listener >= 0);
    if (listener < 0)
        return 1;

    fd_set all;
    FD_ZERO(&all);
    FD_SET(listener, &all);
    int max_fd = listener;

    for (;;)
    {
        fd_set ready = all;
        struct timeval tv = {0, 20000};
        if (select(max_fd + 1, &ready, NULL, NULL, &tv) < 0)
            return 1;

        if (atomic_exchange(&mb_drop, false))
        {
            // Simulate a dropped link: close every client connection.
            for (int fd = 0; fd <= max_fd; fd++)
                if (fd != listener && FD_ISSET(fd, &all))
                {
                    close(fd);
                    FD_CLR(fd, &all);
                }
            continue;
        }

        for (int fd = 0; fd <= max_fd; fd++)
        {
            if (!FD_ISSET(fd, &ready))
                continue;
            if (fd == listener)
            {
                int client = accept(listener, NULL, NULL);
                if (client >= 0)
                {
                    FD_SET(client, &all);
                    max_fd = client > max_fd ? client : max_fd;
                    atomic_fetch_add(&mb_accepts, 1);
                }
                continue;
            }
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
            modbus_set_socket(ctx, fd);
            int rc = modbus_receive(ctx, query);
            if (rc > 0)
            {
                mtx_lock(&mb_mutex);
                modbus_reply(ctx, query, rc, mb_map);
                mtx_unlock(&mb_mutex);
            }
            else if (rc < 0)
            {
                close(fd);
                FD_CLR(fd, &all);
            }
        }
    }
}

static uint16_t mb_reg(int addr)
{
    mtx_lock(&mb_mutex);
    uint16_t v = mb_map->tab_registers[addr];
    mtx_unlock(&mb_mutex);
    return v;
}

static void mb_set(int addr, uint16_t v)
{
    mtx_lock(&mb_mutex);
    mb_map->tab_registers[addr] = v;
    mtx_unlock(&mb_mutex);
}

static void test_modbus(void)
{
    mtx_init(&mb_mutex, mtx_plain);
    mb_map = modbus_mapping_new(0, 0, 100, 0);
    thrd_t server;
    thrd_create(&server, mb_server, NULL);
    for (int i = 0; i < 100 && !atomic_load(&mb_ready); i++)
        sleep_ms(10);
    CHECK(atomic_load(&mb_ready));

    // Two connections to the same device: one per word order.
    CHECK(load("{\"plc\":{\"protocol\":\"modbus\",\"host\":\"localhost\",\"port\":15502,\"period_ms\":20,"
               "\"inputs\":[{\"var_id\":\"speed\",\"address\":0,\"type\":\"uint16\"},"
               "{\"var_id\":\"temp\",\"address\":2,\"type\":\"float32\"},"
               "{\"var_id\":\"delta\",\"address\":4,\"type\":\"int32\"},"
               "{\"var_id\":\"enable\",\"address\":6,\"type\":\"bool\"},"
               "{\"var_id\":\"trim\",\"address\":7,\"type\":\"int8\"}],"
               "\"outputs\":[{\"var_id\":\"setpoint\",\"address\":10,\"type\":\"uint16\"},"
               "{\"var_id\":\"pressure\",\"address\":12,\"type\":\"float32\"},"
               "{\"var_id\":\"level\",\"address\":14,\"type\":\"int16\"},"
               "{\"var_id\":\"missing\",\"address\":500,\"type\":\"uint16\"}]},"
               "\"plc_le\":{\"protocol\":\"modbus\",\"host\":\"127.0.0.1\",\"port\":15502,\"period_ms\":20,"
               "\"word_order\":\"little\","
               "\"inputs\":[{\"var_id\":\"le_in\",\"address\":20,\"type\":\"float32\"}],"
               "\"outputs\":[{\"var_id\":\"le_out\",\"address\":22,\"type\":\"uint32\"}]}}"));
    quiet(true); // the unmapped register is reported on purpose
    CHECK(conns_start() == 0);

    CHECK(api_write("speed", 1234));
    CHECK(api_write("temp", 21.5));    // 0x41AC0000
    CHECK(api_write("delta", -5));     // 0xFFFFFFFB
    CHECK(api_write("enable", 1));
    CHECK(api_write("trim", -3));      // sign-extended to 0xFFFD
    CHECK(api_write("le_in", 1.5));    // 0x3FC00000, low word first

    EVENTUALLY(mb_reg(0) == 1234);
    EVENTUALLY(mb_reg(2) == 0x41AC && mb_reg(3) == 0x0000);
    EVENTUALLY(mb_reg(4) == 0xFFFF && mb_reg(5) == 0xFFFB);
    EVENTUALLY(mb_reg(6) == 1);
    EVENTUALLY(mb_reg(7) == 0xFFFD);
    EVENTUALLY(mb_reg(20) == 0x0000 && mb_reg(21) == 0x3FC0);

    mb_set(10, 777);
    mb_set(12, 0x4050); // 3.25
    mb_set(13, 0x0000);
    mb_set(14, 0xFFFE); // -2
    mb_set(22, 0x0002); // little word order: 0x00010002
    mb_set(23, 0x0001);
    EVENTUALLY(api_read("setpoint", NULL) == 777);
    EVENTUALLY(api_read("pressure", NULL) == 3.25);
    EVENTUALLY(api_read("level", NULL) == -2);
    EVENTUALLY(api_read("le_out", NULL) == 65538);
    CHECK(api_timestamp("missing") == 0); // illegal address: reported, link kept

    // Drop the link: ammio reconnects and data flows again.
    int accepts = atomic_load(&mb_accepts);
    atomic_store(&mb_drop, true);
    EVENTUALLY(atomic_load(&mb_accepts) > accepts);
    mb_set(10, 778);
    EVENTUALLY(api_read("setpoint", NULL) == 778);

    quiet(false);
    unload();
}

// --- OPC UA end-to-end -------------------------------------------------------

#define UA_PORT 14840

static UA_Server *ua_server;
static mtx_t ua_mutex;
static atomic_bool ua_running = false;

static int ua_loop(void *arg)
{
    (void)arg;
    while (atomic_load(&ua_running))
    {
        mtx_lock(&ua_mutex);
        UA_Server_run_iterate(ua_server, false);
        mtx_unlock(&ua_mutex);
        sleep_ms(2);
    }
    return 0;
}

static void ua_add(UA_NodeId id, const char *name, const UA_DataType *type, void *initial)
{
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, initial, type);
    attr.dataType = type->typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)name);
    UA_Server_addVariableNode(ua_server, id, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                              UA_QUALIFIEDNAME(1, (char *)name),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);
}

static double ua_get(UA_NodeId id)
{
    UA_Variant v;
    mtx_lock(&ua_mutex);
    UA_Server_readValue(ua_server, id, &v);
    mtx_unlock(&ua_mutex);
    double d = NAN;
    if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_DOUBLE]))
        d = *(UA_Double *)v.data;
    else if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_BOOLEAN]))
        d = *(UA_Boolean *)v.data;
    UA_Variant_clear(&v);
    return d;
}

static void ua_set(UA_NodeId id, void *value, const UA_DataType *type)
{
    UA_Variant v;
    UA_Variant_setScalar(&v, value, type);
    mtx_lock(&ua_mutex);
    UA_Server_writeValue(ua_server, id, v);
    mtx_unlock(&ua_mutex);
}

static void test_opcua(void)
{
    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));
    UA_ServerConfig_setMinimal(&config, UA_PORT, NULL);
    ua_server = UA_Server_newWithConfig(&config);
    CHECK(ua_server != NULL);
    if (!ua_server)
        return;

    UA_Double speed = 0;
    UA_Boolean on = false;
    UA_Float temp = 0;
    UA_Int32 count = 0, other = 7;
    ua_add(UA_NODEID_STRING(1, "in.speed"), "in.speed", &UA_TYPES[UA_TYPES_DOUBLE], &speed);
    ua_add(UA_NODEID_STRING(1, "in.on"), "in.on", &UA_TYPES[UA_TYPES_BOOLEAN], &on);
    ua_add(UA_NODEID_STRING(1, "out.temp"), "out.temp", &UA_TYPES[UA_TYPES_FLOAT], &temp);
    ua_add(UA_NODEID_NUMERIC(1, 5001), "out.count", &UA_TYPES[UA_TYPES_INT32], &count);
    ua_add(UA_NODEID_NUMERIC(1, 5002), "out.other", &UA_TYPES[UA_TYPES_INT32], &other);

    mtx_init(&ua_mutex, mtx_plain);
    CHECK(UA_Server_run_startup(ua_server) == UA_STATUSCODE_GOOD);
    atomic_store(&ua_running, true);
    thrd_t loop;
    thrd_create(&loop, ua_loop, NULL);

    CHECK(load("{\"scada\":{\"protocol\":\"opcua\",\"endpoint\":\"opc.tcp://127.0.0.1:14840\",\"period_ms\":20,"
               "\"inputs\":[{\"var_id\":\"speed\",\"node_id\":\"ns=1;s=in.speed\",\"type\":\"float64\"},"
               "{\"var_id\":\"on\",\"node_id\":\"ns=1;s=in.on\",\"type\":\"bool\"}],"
               "\"outputs\":[{\"var_id\":\"temp\",\"node_id\":\"ns=1;s=out.temp\",\"type\":\"float32\"},"
               "{\"var_id\":\"count\",\"node_id\":\"ns=1;i=5001\",\"type\":\"int32\"},"
               "{\"var_id\":\"wrong\",\"node_id\":\"ns=1;i=5002\",\"type\":\"uint16\"},"
               "{\"var_id\":\"absent\",\"node_id\":\"ns=1;s=nope\",\"type\":\"uint16\"}]}}"));
    quiet(true); // the mismatched and missing nodes are reported on purpose
    CHECK(conns_start() == 0);

    CHECK(api_write("speed", 12.75));
    CHECK(api_write("on", 1));
    EVENTUALLY(ua_get(UA_NODEID_STRING(1, "in.speed")) == 12.75);
    EVENTUALLY(ua_get(UA_NODEID_STRING(1, "in.on")) == 1);

    temp = 36.5f;
    count = -42;
    ua_set(UA_NODEID_STRING(1, "out.temp"), &temp, &UA_TYPES[UA_TYPES_FLOAT]);
    ua_set(UA_NODEID_NUMERIC(1, 5001), &count, &UA_TYPES[UA_TYPES_INT32]);
    EVENTUALLY(api_read("temp", NULL) == 36.5);
    EVENTUALLY(api_read("count", NULL) == -42);
    CHECK(api_timestamp("wrong") == 0);  // Int32 on the server: never reinterpreted
    CHECK(api_timestamp("absent") == 0); // BadNodeIdUnknown: reported, session kept

    quiet(false);
    unload();

    atomic_store(&ua_running, false);
    thrd_join(loop, NULL);
    UA_Server_run_shutdown(ua_server);
    UA_Server_delete(ua_server);
}

// --- TRDP end-to-end ---------------------------------------------------------
// TRDP does not work over loopback: use the first non-loopback IPv4 address.

static bool local_ipv4(char *out, size_t len)
{
    struct ifaddrs *list = NULL, *ifa;
    bool found = false;
    if (getifaddrs(&list) != 0)
        return false;
    for (ifa = list; ifa && !found; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || (ifa->ifa_flags & IFF_LOOPBACK) ||
            !(ifa->ifa_flags & IFF_UP))
            continue;
        found = inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, out, (socklen_t)len);
    }
    freeifaddrs(list);
    return found;
}

// Test configs are written with single quotes; $IP is the local address.
static char *config_from(const char *tmpl, const char *ip)
{
    static char out[8192];
    size_t n = 0;
    for (const char *p = tmpl; *p && n + 20 < sizeof(out); p++)
    {
        if (strncmp(p, "$IP", 3) == 0)
        {
            n += (size_t)snprintf(out + n, sizeof(out) - n, "%s", ip);
            p += 2;
        }
        else
            out[n++] = *p == '\'' ? '"' : *p;
    }
    out[n] = '\0';
    return out;
}

static void test_trdp(void)
{
    char ip[INET_ADDRSTRLEN];
    if (!local_ipv4(ip, sizeof(ip)))
    {
        printf("skipping TRDP test: no non-loopback IPv4 address\n");
        return;
    }

    // One connection sends and receives the same telegrams. Most are read with
    // a different layout than they are written with, which checks the wire:
    //   Pd 4242: written as a bitset, read as the raw byte
    //   Pd 4243: written as the raw byte, read as a bitset
    //   Mn 5000: the padding is read back as a value (must be zero)
    //   Mr 5001 -> Mp 5002: ammio asks itself and answers with an array of structs
    const char *tmpl =
        "{'loop': {'protocol': 'trdp', 'local_ip': '$IP', 'etb_topo_cnt_id': 'loop.etb',"
        " 'inputs': ["
        "  {'var_id': 'TX', 'enable_id': 'TX.enabled', 'comid': 4242, 'dest_ip': '$IP', 'period_ms': 20, 'size': 8,"
        "   'variables': [{'var_id': 'tx_speed', 'offset': 0, 'type': 'uint16'},"
        "                 {'var_id': 'tx_flags', 'offset': 2, 'type': 'bitset8', 'bits': ['door', '', 'fault']},"
        "                 {'var_id': 'tx_delta', 'offset': 4, 'type': 'int32'}]},"
        "  {'comid': 4243, 'dest_ip': '$IP', 'period_ms': 20, 'size': 2,"
        "   'variables': [{'var_id': 'tx_raw', 'offset': 1, 'type': 'uint8'}]},"
        "  {'type': 'Mn', 'var_id': 'note', 'enable_id': 'note.send', 'comid': 5000, 'dest_ip': '$IP',"
        "   'variables': [{'var_id': 'code', 'type': 'uint16'}, {'type': 'padding', 'bits': 16},"
        "                 {'var_id': 'level', 'type': 'float32'}]},"
        "  {'type': 'Mr', 'var_id': 'ask', 'enable_id': 'ask.send', 'comid': 5001, 'dest_ip': '$IP',"
        "   'timeout_ms': 1000, 'variables': [{'var_id': 'q', 'type': 'int32'}]},"
        "  {'type': 'Mp', 'var_id': 'answer_tx', 'reply_to': 'ask_rx', 'comid': 5002,"
        "   'structs': {'entry': [{'var_id': 'id', 'type': 'uint8'}, {'type': 'padding', 'bits': 8},"
        "                         {'var_id': 'v', 'type': 'int16'}]},"
        "   'variables': [{'var_id': 'status', 'type': 'uint8'},"
        "                 {'var_id': 'list', 'type': 'entry', 'iterations_id': 'n',"
        "                  'iterations_type': 'uint16', 'max_iterations': 3}]}],"
        " 'outputs': ["
        "  {'comid': 4242, 'period_ms': 20, 'size': 8,"
        "   'variables': [{'var_id': 'rx_speed', 'offset': 0, 'type': 'uint16'},"
        "                 {'var_id': 'rx_raw', 'offset': 2, 'type': 'uint8'},"
        "                 {'var_id': 'rx_delta', 'offset': 4, 'type': 'int32'}]},"
        "  {'comid': 4243, 'period_ms': 20, 'size': 2,"
        "   'variables': [{'var_id': 'rx_flags', 'offset': 1, 'type': 'bitset8',"
        "                  'bits': ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h']}]},"
        "  {'type': 'Mn', 'var_id': 'note_rx', 'comid': 5000,"
        "   'variables': [{'var_id': 'code', 'type': 'uint16'}, {'var_id': 'pad', 'type': 'uint16'},"
        "                 {'var_id': 'level', 'type': 'float32'}]},"
        "  {'type': 'Mr', 'var_id': 'ask_rx', 'comid': 5001, 'variables': [{'var_id': 'q', 'type': 'int32'}]},"
        "  {'type': 'Mp', 'var_id': 'answer', 'reply_to': 'ask', 'comid': 5002,"
        "   'structs': {'entry': [{'var_id': 'id', 'type': 'uint8'}, {'type': 'padding', 'bits': 8},"
        "                         {'var_id': 'v', 'type': 'int16'}]},"
        "   'variables': [{'var_id': 'status', 'type': 'uint8'},"
        "                 {'var_id': 'list', 'type': 'entry', 'iterations_id': 'n',"
        "                  'iterations_type': 'uint16', 'max_iterations': 3}]}]}}";
    CHECK(load(config_from(tmpl, ip)));
    CHECK(conns_start() == 0);

    // Process data
    CHECK(api_write("tx_speed", 513));
    CHECK(api_write("tx_flags.door", 1));  // bit 0 = 0x80
    CHECK(api_write("tx_flags.fault", 1)); // bit 2 = 0x20
    CHECK(api_write("tx_flags.bit1", 0));  // unnamed bits are "<var_id>.bit<N>"
    CHECK(api_write("tx_delta", -7));
    CHECK(api_write("tx_raw", 0x81));      // bits 0 and 7
    EVENTUALLY(api_read("rx_speed", NULL) == 513);
    EVENTUALLY(api_read("rx_raw", NULL) == 0xA0);
    EVENTUALLY(api_read("rx_delta", NULL) == -7);
    EVENTUALLY(api_read("rx_flags.a", NULL) == 1);
    EVENTUALLY(api_read("rx_flags.h", NULL) == 1);
    CHECK(api_read("rx_flags.b", NULL) == 0);
    EVENTUALLY(api_read("rx_flags", NULL) == 0x81);  // whole bitset of an output
    EVENTUALLY(api_read("tx_flags", NULL) == 0xA0);  // whole bitset of an input: assembled bits
    CHECK(api_error("{\"cmd\":\"write\",\"var_id\":\"tx_flags\",\"value\":1}") == ERR_READ_ONLY);
    CHECK(api_write("loop.etb", 5));                 // topology counter

    // enable_id: 0 stops publishing, 1 starts again
    quiet(true); // the subscriber's timeout is reported on purpose
    CHECK(api_write("TX.enabled", 0));
    sleep_ms(100);
    CHECK(api_write("tx_speed", 999));
    sleep_ms(200);
    CHECK(api_read("rx_speed", NULL) == 513);
    CHECK(api_write("TX.enabled", 1));
    EVENTUALLY(api_read("rx_speed", NULL) == 999);
    quiet(false);

    // Mn: sent once when its enable flag is raised, which ammio then resets
    CHECK(api_write("note.code", 1234));
    CHECK(api_write("note.level", 2.5));
    CHECK(api_write("note.send", 1));
    EVENTUALLY(api_read("note_rx.code", NULL) == 1234);
    EVENTUALLY(api_read("note_rx.level", NULL) == 2.5);
    CHECK(api_read("note_rx.pad", NULL) == 0);
    CHECK(api_read("note.send", NULL) == 0);

    // Mr -> Mp, in both roles: ammio asks (ask/answer) and answers (ask_rx/answer_tx)
    CHECK(api_write("answer_tx.status", 7));
    CHECK(api_write("answer_tx.n", 2));
    CHECK(api_write("answer_tx.list[0].id", 1));
    CHECK(api_write("answer_tx.list[0].v", -100));
    CHECK(api_write("answer_tx.list[1].id", 2));
    CHECK(api_write("answer_tx.list[1].v", 200));
    CHECK(api_write("answer_tx.list[2].id", 9)); // beyond n: not sent
    CHECK(api_write("ask.q", -42));
    CHECK(api_write("ask.send", 1));
    EVENTUALLY(api_read("ask_rx.q", NULL) == -42);
    EVENTUALLY(api_read("answer.status", NULL) == 7);
    EVENTUALLY(api_read("answer.n", NULL) == 2);
    CHECK(api_read("answer.list[0].id", NULL) == 1);
    CHECK(api_read("answer.list[0].v", NULL) == -100);
    CHECK(api_read("answer.list[1].id", NULL) == 2);
    CHECK(api_read("answer.list[1].v", NULL) == 200);
    CHECK(api_timestamp("answer.list[2].id") == 0);

    unload();
}

int main(void)
{
    if (getenv("AMMIO_TEST_DEBUG"))
        base_level = LOG_DEBUG;
    log_set_level(base_level);

    test_values();
    test_config();
    test_api();
    test_modbus();
    test_opcua();
    test_trdp();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
