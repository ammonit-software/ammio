#include <stdio.h>

#include "test.h"
#include "util/log.h"

int test_failures = 0;
const char *test_current = "";

#define TESTS(X)                                  \
    X(test_codec_big_endian)                      \
    X(test_codec_modbus_byte_orders)              \
    X(test_codec_bitset_numbering)                \
    X(test_types_names)                           \
    X(test_types_range_checks)                    \
    X(test_types_bools)                           \
    X(test_types_device_conversions)              \
    X(test_types_json_output)                     \
    X(test_var_table_registration)                \
    X(test_var_table_writes_and_quality)          \
    X(test_var_table_batch_and_hook)              \
    X(test_config_two_connections_of_one_protocol)\
    X(test_config_rejects_mistakes)               \
    X(test_config_trdp_layout_checks)             \
    X(test_config_api_requests)                   \
    X(test_trdp_xml_import)                       \
    X(test_trdp_xml_connection)

#define DECLARE(name) void name(void);
TESTS(DECLARE)

int main(void)
{
    // Expected configuration errors would otherwise clutter the output
    log_set_level(AMMIO_LOG_OFF);

    int count = 0;
#define RUN(name)                                  \
    do {                                           \
        int before = test_failures;                \
        test_current = #name;                      \
        name();                                    \
        printf("%s %s\n", test_failures == before ? "ok  " : "FAIL", #name); \
        count++;                                   \
    } while (0);
    TESTS(RUN)

    printf("\n%d test(s), %d failed check(s)\n", count, test_failures);
    return test_failures == 0 ? 0 : 1;
}
