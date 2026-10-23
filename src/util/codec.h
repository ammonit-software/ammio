#ifndef AMMIO_CODEC_H
#define AMMIO_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

// ---------------------------------------------------------------------------------------------
// Big-endian (network order) scalars, as used by TRDP datasets. bool is one byte (BOOL8).
// ---------------------------------------------------------------------------------------------

void codec_put_be(uint8_t *dst, ammio_type_t type, ammio_value_t value);
ammio_value_t codec_get_be(const uint8_t *src, ammio_type_t type);

// ---------------------------------------------------------------------------------------------
// Modbus registers. The byte order names the order of the value's bytes from most (A) to least
// significant across the registers, e.g. CDAB = "word swapped". 8-bit types and bool occupy one
// register (value in the low byte, int8 sign-extended).
// ---------------------------------------------------------------------------------------------

typedef enum
{
    CODEC_ORDER_ABCD,  // big-endian, high word first (Modbus default)
    CODEC_ORDER_CDAB,  // low word first, bytes big-endian within each word
    CODEC_ORDER_BADC,  // high word first, bytes swapped within each word
    CODEC_ORDER_DCBA   // fully little-endian
} codec_byte_order_t;

bool codec_byte_order_from_string(const char *str, codec_byte_order_t *out);
const char *codec_byte_order_name(codec_byte_order_t order);

// Number of 16-bit registers a value of this type occupies (1, 2 or 4)
unsigned codec_reg_count(ammio_type_t type);

void codec_to_regs(ammio_type_t type, ammio_value_t value, codec_byte_order_t order, uint16_t *regs);
ammio_value_t codec_from_regs(ammio_type_t type, const uint16_t *regs, codec_byte_order_t order);

// ---------------------------------------------------------------------------------------------
// TRDP bitsets: bit 0 is the most significant bit of the first byte, bit 8 the MSB of the second.
// ---------------------------------------------------------------------------------------------

bool codec_bitset_get(const uint8_t *base, unsigned bit_index);
void codec_bitset_set(uint8_t *base, unsigned bit_index, bool value);

#endif
