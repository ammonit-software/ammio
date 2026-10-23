#include <stdlib.h>

#include "core/var_table.h"
#include "test.h"

static int hook_calls;
static char hook_last[64];

static void hook(const ammio_var_snapshot_t *snap, void *ctx)
{
    (void)ctx;
    hook_calls++;
    snprintf(hook_last, sizeof(hook_last), "%s", snap->var_id);
}

TEST(test_var_table_registration)
{
    var_table_init();
    ammio_var_t *a = var_table_add("a", AMMIO_TYPE_UINT16, AMMIO_DIR_INPUT, "c1");
    CHECK(a != NULL);
    CHECK(var_table_add("a", AMMIO_TYPE_UINT8, AMMIO_DIR_OUTPUT, "c2") == NULL);  // duplicate
    CHECK(var_table_find("a") == a);
    CHECK(var_table_find("missing") == NULL);
    CHECK_EQ_STR(var_id(a), "a");
    CHECK_EQ_INT(var_table_count(), 1);

    ammio_var_snapshot_t s;
    var_table_read(a, &s);
    CHECK(!s.has_value);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_NO_DATA);
    CHECK_EQ_STR(s.connection, "c1");
    var_table_free();
}

TEST(test_var_table_writes_and_quality)
{
    var_table_init();
    ammio_var_t *in = var_table_add("in", AMMIO_TYPE_UINT16, AMMIO_DIR_INPUT, "c");
    ammio_var_t *out = var_table_add("out", AMMIO_TYPE_UINT16, AMMIO_DIR_OUTPUT, "c");
    ammio_var_t *other = var_table_add("other", AMMIO_TYPE_UINT16, AMMIO_DIR_OUTPUT, "d");

    ammio_value_t v = {0};
    v.u16 = 7;
    var_table_client_write_many(&in, &v, 1);
    ammio_var_snapshot_t s;
    var_table_read(in, &s);
    CHECK(s.has_value && s.value.u16 == 7);
    CHECK_EQ_INT(s.write_seq, 1);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_GOOD);
    CHECK(s.timestamp_ns > 0);

    // Writing the same value again still bumps write_seq (drivers resend on every client write)
    var_table_client_write_many(&in, &v, 1);
    var_table_read(in, &s);
    CHECK_EQ_INT(s.write_seq, 2);

    // Silent sets keep write_seq
    v.u16 = 9;
    var_table_set_value_silent(in, v);
    var_table_read(in, &s);
    CHECK(s.value.u16 == 9 && s.write_seq == 2);

    var_table_update(out, v, AMMIO_QUALITY_GOOD);
    var_table_mark_connection("c", false);
    var_table_read(in, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_DISCONNECTED);
    var_table_read(out, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_DISCONNECTED);
    CHECK(s.value.u16 == 9);  // value kept
    var_table_read(other, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_NO_DATA);  // other connection untouched

    var_table_mark_connection("c", true);
    var_table_read(in, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_GOOD);
    var_table_read(out, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_DISCONNECTED);  // until fresh data arrives
    var_table_update(out, v, AMMIO_QUALITY_GOOD);
    var_table_read(out, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_GOOD);

    var_table_set_quality_many(&out, 1, AMMIO_QUALITY_STALE);
    var_table_read(out, &s);
    CHECK_EQ_INT(s.quality, AMMIO_QUALITY_STALE);
    var_table_free();
}

TEST(test_var_table_batch_and_hook)
{
    var_table_init();
    ammio_var_t *vars[3] = {
        var_table_add("x", AMMIO_TYPE_INT32, AMMIO_DIR_INPUT, "c"),
        var_table_add("y", AMMIO_TYPE_INT32, AMMIO_DIR_INPUT, "c"),
        var_table_add("z", AMMIO_TYPE_FLOAT32, AMMIO_DIR_OUTPUT, "c"),
    };
    var_table_set_change_hook(hook, NULL);
    hook_calls = 0;

    ammio_value_t vals[2] = {{0}, {0}};
    vals[0].i32 = -1;
    vals[1].i32 = 2;
    var_table_client_write_many(vars, vals, 2);
    CHECK_EQ_INT(hook_calls, 2);

    // Unchanged driver updates do not produce events
    ammio_value_t f = {0};
    f.f32 = 1.5f;
    var_table_update(vars[2], f, AMMIO_QUALITY_GOOD);
    var_table_update(vars[2], f, AMMIO_QUALITY_GOOD);
    CHECK_EQ_INT(hook_calls, 3);
    CHECK_EQ_STR(hook_last, "z");
    var_table_set_quality_many(&vars[2], 1, AMMIO_QUALITY_BAD);
    CHECK_EQ_INT(hook_calls, 4);

    ammio_var_snapshot_t snaps[3];
    var_table_read_many(vars, 3, snaps);
    CHECK(snaps[0].value.i32 == -1 && snaps[1].value.i32 == 2 && snaps[2].value.f32 == 1.5f);

    ammio_var_snapshot_t *all;
    size_t n;
    CHECK(var_table_list(&all, &n) == 0);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_STR(all[0].var_id, "x");  // registration order
    CHECK_EQ_STR(all[2].var_id, "z");
    free(all);
    var_table_free();
}
