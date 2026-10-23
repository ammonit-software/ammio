#include <stdlib.h>

#include "core/types.h"
#include "test.h"

static ammio_value_status_t from_json_text(ammio_type_t type, const char *text, ammio_value_t *out)
{
    cJSON *json = cJSON_Parse(text);
    ammio_value_status_t rc = ammio_value_from_json(type, json, out);
    cJSON_Delete(json);
    return rc;
}

TEST(test_types_names)
{
    ammio_type_t t;
    CHECK(ammio_type_from_string("float32", &t) && t == AMMIO_TYPE_FLOAT32);
    CHECK(ammio_type_from_string("bool", &t) && t == AMMIO_TYPE_BOOL);
    CHECK(!ammio_type_from_string("uint12", &t));
    CHECK(!ammio_type_from_string("UINT8", &t));
    CHECK_EQ_STR(ammio_type_name(AMMIO_TYPE_INT64), "int64");
    CHECK_EQ_INT(ammio_type_size_bytes(AMMIO_TYPE_FLOAT64), 8);
}

TEST(test_types_range_checks)
{
    ammio_value_t v;
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "255", &v) == AMMIO_VALUE_OK && v.u8 == 255);
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "256", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "-1", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_INT8, "-128", &v) == AMMIO_VALUE_OK && v.i8 == -128);
    CHECK(from_json_text(AMMIO_TYPE_INT8, "128", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_UINT16, "1.5", &v) == AMMIO_VALUE_WRONG_TYPE);
    CHECK(from_json_text(AMMIO_TYPE_UINT16, "2.0", &v) == AMMIO_VALUE_OK && v.u16 == 2);
    CHECK(from_json_text(AMMIO_TYPE_UINT32, "4294967295", &v) == AMMIO_VALUE_OK && v.u32 == 4294967295u);
    CHECK(from_json_text(AMMIO_TYPE_INT32, "-2147483649", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_UINT64, "9007199254740992", &v) == AMMIO_VALUE_OK);
    CHECK(from_json_text(AMMIO_TYPE_UINT64, "1e19", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_FLOAT32, "1e39", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_FLOAT32, "-0.25", &v) == AMMIO_VALUE_OK && v.f32 == -0.25f);
    CHECK(from_json_text(AMMIO_TYPE_FLOAT64, "1e300", &v) == AMMIO_VALUE_OK);
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "\"1\"", &v) == AMMIO_VALUE_WRONG_TYPE);
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "null", &v) == AMMIO_VALUE_WRONG_TYPE);
}

TEST(test_types_bools)
{
    ammio_value_t v;
    CHECK(from_json_text(AMMIO_TYPE_BOOL, "true", &v) == AMMIO_VALUE_OK && v.b);
    CHECK(from_json_text(AMMIO_TYPE_BOOL, "0", &v) == AMMIO_VALUE_OK && !v.b);
    CHECK(from_json_text(AMMIO_TYPE_BOOL, "2", &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(from_json_text(AMMIO_TYPE_UINT8, "true", &v) == AMMIO_VALUE_OK && v.u8 == 1);
}

TEST(test_types_device_conversions)
{
    ammio_value_t v;
    CHECK(ammio_value_from_int64(AMMIO_TYPE_UINT16, 100000, &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(ammio_value_from_uint64(AMMIO_TYPE_UINT64, UINT64_MAX, &v) == AMMIO_VALUE_OK && v.u64 == UINT64_MAX);
    CHECK(ammio_value_from_uint64(AMMIO_TYPE_INT64, UINT64_MAX, &v) == AMMIO_VALUE_OUT_OF_RANGE);
    CHECK(ammio_value_from_double(AMMIO_TYPE_INT16, 2.5, &v) == AMMIO_VALUE_WRONG_TYPE);
    CHECK(ammio_value_from_double(AMMIO_TYPE_FLOAT32, 2.5, &v) == AMMIO_VALUE_OK && v.f32 == 2.5f);
}

TEST(test_types_json_output)
{
    ammio_value_t v = {0};
    v.u64 = 18446744073709551615ULL;
    cJSON *j = ammio_value_to_json(AMMIO_TYPE_UINT64, v);
    char *s = cJSON_PrintUnformatted(j);
    CHECK_EQ_STR(s, "18446744073709551615");  // exact, not rounded through a double
    free(s);
    cJSON_Delete(j);

    v.b = true;
    j = ammio_value_to_json(AMMIO_TYPE_BOOL, v);
    CHECK(cJSON_IsTrue(j));
    cJSON_Delete(j);

    v.f64 = NAN;
    j = ammio_value_to_json(AMMIO_TYPE_FLOAT64, v);
    CHECK(cJSON_IsNull(j));  // JSON has no NaN
    cJSON_Delete(j);

    ammio_value_t a = {0}, b = {0};
    a.f32 = NAN;
    b.f32 = NAN;
    CHECK(ammio_value_equal(AMMIO_TYPE_FLOAT32, a, b));
}
