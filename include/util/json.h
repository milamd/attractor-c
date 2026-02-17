#ifndef UTIL_JSON_H
#define UTIL_JSON_H

#include <stddef.h>
#include <stdbool.h>

/* Minimal JSON types */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    union {
        bool        boolean;
        double      number;
        char       *string;
        struct {
            JsonValue **items;
            size_t      count;
        } array;
        struct {
            char      **keys;
            JsonValue **values;
            size_t      count;
        } object;
    };
};

/* Parse / free */
JsonValue  *json_parse(const char *src, const char **err);
void        json_free(JsonValue *v);

/* Accessors */
const char *json_get_string(const JsonValue *obj, const char *key);
double      json_get_number(const JsonValue *obj, const char *key, double def);
int         json_get_int(const JsonValue *obj, const char *key, int def);
bool        json_get_bool(const JsonValue *obj, const char *key, bool def);
JsonValue  *json_get(const JsonValue *obj, const char *key);
JsonValue  *json_array_get(const JsonValue *arr, size_t idx);

/* Builder */
JsonValue  *json_new_object(void);
JsonValue  *json_new_array(void);
JsonValue  *json_new_string(const char *s);
JsonValue  *json_new_number(double n);
JsonValue  *json_new_bool(bool b);
JsonValue  *json_new_null(void);
void        json_object_set(JsonValue *obj, const char *key, JsonValue *val);
void        json_array_push(JsonValue *arr, JsonValue *val);

/* Serialization */
char       *json_serialize(const JsonValue *v);

#endif
