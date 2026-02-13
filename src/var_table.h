#ifndef VAR_TABLE_H
#define VAR_TABLE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    TYPE_UINT8,
    TYPE_INT8,
    TYPE_UINT16,
    TYPE_INT16,
    TYPE_UINT32,
    TYPE_INT32,
    TYPE_FLOAT32,
    TYPE_FLOAT64
} type_t;

typedef struct
{
    char name[256];
    type_t type;
    union
    {
        uint8_t u8;
        int8_t i8;
        uint16_t u16;
        int16_t i16;
        uint32_t u32;
        int32_t i32;
        float f32;
        double f64;
    } value;
    uint64_t timestamp;
} var_t;

int var_table_init(void);
void var_table_cleanup(void);
int var_table_add(const char *name, type_t type);
type_t var_table_type_from_string(const char *type_str);

int var_table_get(const char *name, var_t *out);
int var_table_set(const char *name, const var_t *in);

#endif
