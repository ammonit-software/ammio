#include "test.h"
#include "util/codec.h"

TEST(test_codec_big_endian)
{
    uint8_t buf[8] = {0};
    ammio_value_t v = {0};

    v.u16 = 0x1234;
    codec_put_be(buf, AMMIO_TYPE_UINT16, v);
    CHECK_BYTES(buf, 0x12, 0x34);

    v.i32 = -2;
    codec_put_be(buf, AMMIO_TYPE_INT32, v);
    CHECK_BYTES(buf, 0xFF, 0xFF, 0xFF, 0xFE);
    CHECK_EQ_INT(codec_get_be(buf, AMMIO_TYPE_INT32).i32, -2);

    v.f32 = 1.0f;
    codec_put_be(buf, AMMIO_TYPE_FLOAT32, v);
    CHECK_BYTES(buf, 0x3F, 0x80, 0x00, 0x00);
    CHECK(codec_get_be(buf, AMMIO_TYPE_FLOAT32).f32 == 1.0f);

    v.f64 = -2.5;
    codec_put_be(buf, AMMIO_TYPE_FLOAT64, v);
    CHECK_BYTES(buf, 0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(codec_get_be(buf, AMMIO_TYPE_FLOAT64).f64 == -2.5);

    v.u64 = 0x0102030405060708ULL;
    codec_put_be(buf, AMMIO_TYPE_UINT64, v);
    CHECK_BYTES(buf, 1, 2, 3, 4, 5, 6, 7, 8);
    CHECK(codec_get_be(buf, AMMIO_TYPE_UINT64).u64 == 0x0102030405060708ULL);

    v.i8 = -1;
    codec_put_be(buf, AMMIO_TYPE_INT8, v);
    CHECK_BYTES(buf, 0xFF);

    v.b = true;
    codec_put_be(buf, AMMIO_TYPE_BOOL, v);
    CHECK_BYTES(buf, 0x01);
    buf[0] = 0x80;  // any non-zero BOOL8 is true
    CHECK(codec_get_be(buf, AMMIO_TYPE_BOOL).b);
}

TEST(test_codec_modbus_byte_orders)
{
    uint16_t regs[4];
    ammio_value_t v = {0};
    v.u32 = 0xAABBCCDD;

    codec_to_regs(AMMIO_TYPE_UINT32, v, CODEC_ORDER_ABCD, regs);
    CHECK_EQ_INT(regs[0], 0xAABB);
    CHECK_EQ_INT(regs[1], 0xCCDD);
    codec_to_regs(AMMIO_TYPE_UINT32, v, CODEC_ORDER_CDAB, regs);
    CHECK_EQ_INT(regs[0], 0xCCDD);
    CHECK_EQ_INT(regs[1], 0xAABB);
    codec_to_regs(AMMIO_TYPE_UINT32, v, CODEC_ORDER_BADC, regs);
    CHECK_EQ_INT(regs[0], 0xBBAA);
    CHECK_EQ_INT(regs[1], 0xDDCC);
    codec_to_regs(AMMIO_TYPE_UINT32, v, CODEC_ORDER_DCBA, regs);
    CHECK_EQ_INT(regs[0], 0xDDCC);
    CHECK_EQ_INT(regs[1], 0xBBAA);

    // Round trip of every type in every order
    for (int order = 0; order < 4; order++)
    {
        ammio_value_t in = {0};
        in.f64 = 123.456;
        codec_to_regs(AMMIO_TYPE_FLOAT64, in, (codec_byte_order_t)order, regs);
        CHECK(codec_from_regs(AMMIO_TYPE_FLOAT64, regs, (codec_byte_order_t)order).f64 == 123.456);

        in.i16 = -300;
        codec_to_regs(AMMIO_TYPE_INT16, in, (codec_byte_order_t)order, regs);
        CHECK_EQ_INT(codec_from_regs(AMMIO_TYPE_INT16, regs, (codec_byte_order_t)order).i16, -300);

        in.i8 = -3;
        codec_to_regs(AMMIO_TYPE_INT8, in, (codec_byte_order_t)order, regs);
        CHECK_EQ_INT(codec_from_regs(AMMIO_TYPE_INT8, regs, (codec_byte_order_t)order).i8, -3);
    }

    // 64-bit word swap reverses the word order
    v.u64 = 0x1122334455667788ULL;
    codec_to_regs(AMMIO_TYPE_UINT64, v, CODEC_ORDER_CDAB, regs);
    CHECK_EQ_INT(regs[0], 0x7788);
    CHECK_EQ_INT(regs[3], 0x1122);

    // int8 is sign-extended into the register, 8-bit values use the low byte
    v.i8 = -1;
    codec_to_regs(AMMIO_TYPE_INT8, v, CODEC_ORDER_ABCD, regs);
    CHECK_EQ_INT(regs[0], 0xFFFF);
    regs[0] = 0x1234;
    CHECK_EQ_INT(codec_from_regs(AMMIO_TYPE_UINT8, regs, CODEC_ORDER_ABCD).u8, 0x34);

    CHECK_EQ_INT(codec_reg_count(AMMIO_TYPE_BOOL), 1);
    CHECK_EQ_INT(codec_reg_count(AMMIO_TYPE_UINT16), 1);
    CHECK_EQ_INT(codec_reg_count(AMMIO_TYPE_FLOAT32), 2);
    CHECK_EQ_INT(codec_reg_count(AMMIO_TYPE_INT64), 4);

    codec_byte_order_t o;
    CHECK(codec_byte_order_from_string("CDAB", &o) && o == CODEC_ORDER_CDAB);
    CHECK(!codec_byte_order_from_string("cdab", &o));
}

TEST(test_codec_bitset_numbering)
{
    uint8_t buf[2] = {0};
    codec_bitset_set(buf, 0, true);   // MSB of the first byte
    CHECK_BYTES(buf, 0x80, 0x00);
    codec_bitset_set(buf, 7, true);
    codec_bitset_set(buf, 8, true);   // MSB of the second byte
    CHECK_BYTES(buf, 0x81, 0x80);
    CHECK(codec_bitset_get(buf, 8));
    CHECK(!codec_bitset_get(buf, 9));
    codec_bitset_set(buf, 0, false);
    CHECK_BYTES(buf, 0x01, 0x80);
}
