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

typedef enum
{
    DIR_INPUT,
    DIR_OUTPUT
} dir_t;

typedef struct
{
    char var_id[256];
    type_t type;
    dir_t dir;
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
int var_table_add(const char *var_id, type_t type, dir_t dir);
type_t var_table_type_from_string(const char *type_str);

int var_table_get(const char *var_id, var_t *out);
int var_table_set(const char *var_id, const var_t *in);
int var_table_get_all(var_t **out, size_t *count);

const char *var_table_type_to_string(type_t type);
const char *var_table_dir_to_string(dir_t dir);

#endif
