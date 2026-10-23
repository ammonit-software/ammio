#include "util/codec.h"

#include <string.h>

// Canonical big-endian byte representation of a value; returns the number of bytes
static size_t to_be_bytes(ammio_type_t type, ammio_value_t value, uint8_t out[8])
{
    uint64_t bits = 0;
    size_t size = ammio_type_size_bytes(type);

    switch (type)
    {
        case AMMIO_TYPE_BOOL:    bits = value.b ? 1u : 0u; break;
        case AMMIO_TYPE_UINT8:   bits = value.u8; break;
        case AMMIO_TYPE_INT8:    bits = (uint8_t)value.i8; break;
        case AMMIO_TYPE_UINT16:  bits = value.u16; break;
        case AMMIO_TYPE_INT16:   bits = (uint16_t)value.i16; break;
        case AMMIO_TYPE_UINT32:  bits = value.u32; break;
        case AMMIO_TYPE_INT32:   bits = (uint32_t)value.i32; break;
        case AMMIO_TYPE_UINT64:  bits = value.u64; break;
        case AMMIO_TYPE_INT64:   bits = (uint64_t)value.i64; break;
        case AMMIO_TYPE_FLOAT32:
        {
            uint32_t u;
            memcpy(&u, &value.f32, sizeof(u));
            bits = u;
            break;
        }
        case AMMIO_TYPE_FLOAT64:
            memcpy(&bits, &value.f64, sizeof(bits));
            break;
        default:
            return 0;
    }

    for (size_t i = 0; i < size; i++)
        out[i] = (uint8_t)(bits >> (8 * (size - 1 - i)));
    return size;
}

static ammio_value_t from_be_bytes(ammio_type_t type, const uint8_t *in)
{
    ammio_value_t value;
    memset(&value, 0, sizeof(value));

    size_t size = ammio_type_size_bytes(type);
    uint64_t bits = 0;
    for (size_t i = 0; i < size; i++)
        bits = (bits << 8) | in[i];

    switch (type)
    {
        case AMMIO_TYPE_BOOL:    value.b = bits != 0; break;
        case AMMIO_TYPE_UINT8:   value.u8 = (uint8_t)bits; break;
        case AMMIO_TYPE_INT8:    value.i8 = (int8_t)(uint8_t)bits; break;
        case AMMIO_TYPE_UINT16:  value.u16 = (uint16_t)bits; break;
        case AMMIO_TYPE_INT16:   value.i16 = (int16_t)(uint16_t)bits; break;
        case AMMIO_TYPE_UINT32:  value.u32 = (uint32_t)bits; break;
        case AMMIO_TYPE_INT32:   value.i32 = (int32_t)(uint32_t)bits; break;
        case AMMIO_TYPE_UINT64:  value.u64 = bits; break;
        case AMMIO_TYPE_INT64:   value.i64 = (int64_t)bits; break;
        case AMMIO_TYPE_FLOAT32:
        {
            uint32_t u = (uint32_t)bits;
            memcpy(&value.f32, &u, sizeof(u));
            break;
        }
        case AMMIO_TYPE_FLOAT64:
            memcpy(&value.f64, &bits, sizeof(bits));
            break;
        default:
            break;
    }
    return value;
}

void codec_put_be(uint8_t *dst, ammio_type_t type, ammio_value_t value)
{
    uint8_t bytes[8];
    size_t n = to_be_bytes(type, value, bytes);
    memcpy(dst, bytes, n);
}

ammio_value_t codec_get_be(const uint8_t *src, ammio_type_t type)
{
    return from_be_bytes(type, src);
}

bool codec_byte_order_from_string(const char *str, codec_byte_order_t *out)
{
    static const char *const names[] = {"ABCD", "CDAB", "BADC", "DCBA"};
    for (int i = 0; i < 4; i++)
    {
        if (strcmp(str, names[i]) == 0)
        {
            *out = (codec_byte_order_t)i;
            return true;
        }
    }
    return false;
}

const char *codec_byte_order_name(codec_byte_order_t order)
{
    static const char *const names[] = {"ABCD", "CDAB", "BADC", "DCBA"};
    return (unsigned)order < 4 ? names[order] : "?";
}

unsigned codec_reg_count(ammio_type_t type)
{
    size_t size = ammio_type_size_bytes(type);
    return size <= 2 ? 1u : (unsigned)(size / 2);
}

static bool order_swaps_bytes(codec_byte_order_t order)
{
    return order == CODEC_ORDER_BADC || order == CODEC_ORDER_DCBA;
}

static bool order_swaps_words(codec_byte_order_t order)
{
    return order == CODEC_ORDER_CDAB || order == CODEC_ORDER_DCBA;
}

void codec_to_regs(ammio_type_t type, ammio_value_t value, codec_byte_order_t order, uint16_t *regs)
{
    size_t size = ammio_type_size_bytes(type);

    if (size == 1)
    {
        // 8-bit values sit in the low byte of a single register; int8 is sign-extended
        uint16_t r;
        if (type == AMMIO_TYPE_INT8)
            r = (uint16_t)(int16_t)value.i8;
        else if (type == AMMIO_TYPE_BOOL)
            r = value.b ? 1u : 0u;
        else
            r = value.u8;
        regs[0] = order_swaps_bytes(order) ? (uint16_t)((r << 8) | (r >> 8)) : r;
        return;
    }

    uint8_t bytes[8];
    to_be_bytes(type, value, bytes);

    unsigned words = (unsigned)(size / 2);
    for (unsigned w = 0; w < words; w++)
    {
        unsigned src_word = order_swaps_words(order) ? words - 1 - w : w;
        uint8_t hi = bytes[2 * src_word];
        uint8_t lo = bytes[2 * src_word + 1];
        regs[w] = order_swaps_bytes(order) ? (uint16_t)((lo << 8) | hi) : (uint16_t)((hi << 8) | lo);
    }
}

ammio_value_t codec_from_regs(ammio_type_t type, const uint16_t *regs, codec_byte_order_t order)
{
    size_t size = ammio_type_size_bytes(type);

    if (size == 1)
    {
        uint16_t r = order_swaps_bytes(order) ? (uint16_t)((regs[0] << 8) | (regs[0] >> 8)) : regs[0];
        ammio_value_t value;
        memset(&value, 0, sizeof(value));
        if (type == AMMIO_TYPE_BOOL)
            value.b = r != 0;
        else if (type == AMMIO_TYPE_INT8)
            value.i8 = (int8_t)(uint8_t)(r & 0xFF);
        else
            value.u8 = (uint8_t)(r & 0xFF);
        return value;
    }

    uint8_t bytes[8];
    unsigned words = (unsigned)(size / 2);
    for (unsigned w = 0; w < words; w++)
    {
        unsigned dst_word = order_swaps_words(order) ? words - 1 - w : w;
        uint8_t hi = (uint8_t)(regs[w] >> 8);
        uint8_t lo = (uint8_t)(regs[w] & 0xFF);
        if (order_swaps_bytes(order))
        {
            uint8_t t = hi;
            hi = lo;
            lo = t;
        }
        bytes[2 * dst_word] = hi;
        bytes[2 * dst_word + 1] = lo;
    }
    return from_be_bytes(type, bytes);
}

bool codec_bitset_get(const uint8_t *base, unsigned bit_index)
{
    return (base[bit_index / 8] >> (7 - bit_index % 8)) & 1u;
}

void codec_bitset_set(uint8_t *base, unsigned bit_index, bool value)
{
    uint8_t mask = (uint8_t)(1u << (7 - bit_index % 8));
    if (value)
        base[bit_index / 8] |= mask;
    else
        base[bit_index / 8] &= (uint8_t)~mask;
}
