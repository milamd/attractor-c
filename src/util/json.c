#include "util/json.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*============================================================================
 * Parser internals
 *==========================================================================*/

typedef struct {
    const char *src;
    size_t      pos;
    const char *err;
} JsonParser;

static void skip_ws(JsonParser *p) {
    while (p->src[p->pos] && strchr(" \t\r\n", p->src[p->pos])) p->pos++;
}

static char peek(JsonParser *p) { skip_ws(p); return p->src[p->pos]; }

static JsonValue *parse_value(JsonParser *p);

static char *parse_string_raw(JsonParser *p) {
    if (p->src[p->pos] != '"') { p->err = "expected '\"'"; return NULL; }
    p->pos++;
    StrBuf sb;
    strbuf_init(&sb);
    while (p->src[p->pos] && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') {
            p->pos++;
            switch (p->src[p->pos]) {
                case '"':  strbuf_append(&sb, "\"", 1); break;
                case '\\': strbuf_append(&sb, "\\", 1); break;
                case '/':  strbuf_append(&sb, "/", 1); break;
                case 'b':  strbuf_append(&sb, "\b", 1); break;
                case 'f':  strbuf_append(&sb, "\f", 1); break;
                case 'n':  strbuf_append(&sb, "\n", 1); break;
                case 'r':  strbuf_append(&sb, "\r", 1); break;
                case 't':  strbuf_append(&sb, "\t", 1); break;
                case 'u': {
                    /* Basic \uXXXX handling - just pass through as UTF-8 placeholder */
                    char hex[5] = {0};
                    for (int i = 0; i < 4 && p->src[p->pos+1+i]; i++)
                        hex[i] = p->src[p->pos+1+i];
                    unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                    if (cp < 0x80) {
                        char c = (char)cp;
                        strbuf_append(&sb, &c, 1);
                    } else if (cp < 0x800) {
                        char buf[2] = { (char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F)) };
                        strbuf_append(&sb, buf, 2);
                    } else {
                        char buf[3] = { (char)(0xE0 | (cp >> 12)),
                                        (char)(0x80 | ((cp >> 6) & 0x3F)),
                                        (char)(0x80 | (cp & 0x3F)) };
                        strbuf_append(&sb, buf, 3);
                    }
                    p->pos += 4;
                    break;
                }
                default:
                    strbuf_append(&sb, &p->src[p->pos], 1);
                    break;
            }
        } else {
            strbuf_append(&sb, &p->src[p->pos], 1);
        }
        p->pos++;
    }
    if (p->src[p->pos] == '"') p->pos++;
    return strbuf_detach(&sb);
}

static JsonValue *alloc_val(JsonType t) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    v->type = t;
    return v;
}

static JsonValue *parse_string(JsonParser *p) {
    char *s = parse_string_raw(p);
    if (!s) return NULL;
    JsonValue *v = alloc_val(JSON_STRING);
    v->string = s;
    return v;
}

static JsonValue *parse_number(JsonParser *p) {
    const char *start = p->src + p->pos;
    if (p->src[p->pos] == '-') p->pos++;
    while (isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->src[p->pos] == '.') {
        p->pos++;
        while (isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->src[p->pos] == 'e' || p->src[p->pos] == 'E') {
        p->pos++;
        if (p->src[p->pos] == '+' || p->src[p->pos] == '-') p->pos++;
        while (isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    JsonValue *v = alloc_val(JSON_NUMBER);
    v->number = strtod(start, NULL);
    return v;
}

static JsonValue *parse_array(JsonParser *p) {
    p->pos++; /* skip '[' */
    JsonValue *v = alloc_val(JSON_ARRAY);
    v->array.items = NULL;
    v->array.count = 0;
    size_t cap = 0;

    if (peek(p) == ']') { p->pos++; return v; }

    while (1) {
        JsonValue *item = parse_value(p);
        if (!item) { json_free(v); return NULL; }
        if (v->array.count >= cap) {
            cap = cap ? cap * 2 : 8;
            v->array.items = realloc(v->array.items, cap * sizeof(JsonValue *));
        }
        v->array.items[v->array.count++] = item;
        if (peek(p) == ',') { p->pos++; continue; }
        if (peek(p) == ']') { p->pos++; break; }
        p->err = "expected ',' or ']'";
        json_free(v);
        return NULL;
    }
    return v;
}

static JsonValue *parse_object(JsonParser *p) {
    p->pos++; /* skip '{' */
    JsonValue *v = alloc_val(JSON_OBJECT);
    v->object.keys = NULL;
    v->object.values = NULL;
    v->object.count = 0;
    size_t cap = 0;

    if (peek(p) == '}') { p->pos++; return v; }

    while (1) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) { p->err = "expected string key"; json_free(v); return NULL; }
        skip_ws(p);
        if (p->src[p->pos] != ':') { free(key); p->err = "expected ':'"; json_free(v); return NULL; }
        p->pos++;
        JsonValue *val = parse_value(p);
        if (!val) { free(key); json_free(v); return NULL; }
        if (v->object.count >= cap) {
            cap = cap ? cap * 2 : 8;
            v->object.keys = realloc(v->object.keys, cap * sizeof(char *));
            v->object.values = realloc(v->object.values, cap * sizeof(JsonValue *));
        }
        v->object.keys[v->object.count] = key;
        v->object.values[v->object.count] = val;
        v->object.count++;
        if (peek(p) == ',') { p->pos++; continue; }
        if (peek(p) == '}') { p->pos++; break; }
        p->err = "expected ',' or '}'";
        json_free(v);
        return NULL;
    }
    return v;
}

static JsonValue *parse_value(JsonParser *p) {
    skip_ws(p);
    char c = p->src[p->pos];
    if (c == '"') return parse_string(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(p);
    if (strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JsonValue *v = alloc_val(JSON_BOOL);
        v->boolean = true;
        return v;
    }
    if (strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JsonValue *v = alloc_val(JSON_BOOL);
        v->boolean = false;
        return v;
    }
    if (strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return alloc_val(JSON_NULL);
    }
    p->err = "unexpected character";
    return NULL;
}

/*============================================================================
 * Public API
 *==========================================================================*/

JsonValue *json_parse(const char *src, const char **err) {
    if (!src) { if (err) *err = "null input"; return NULL; }
    JsonParser p = { .src = src, .pos = 0, .err = NULL };
    JsonValue *v = parse_value(&p);
    if (!v && err) *err = p.err ? p.err : "parse error";
    return v;
}

void json_free(JsonValue *v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STRING: free(v->string); break;
        case JSON_ARRAY:
            for (size_t i = 0; i < v->array.count; i++)
                json_free(v->array.items[i]);
            free(v->array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < v->object.count; i++) {
                free(v->object.keys[i]);
                json_free(v->object.values[i]);
            }
            free(v->object.keys);
            free(v->object.values);
            break;
        default: break;
    }
    free(v);
}

const char *json_get_string(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.keys[i], key) == 0) {
            JsonValue *v = obj->object.values[i];
            return (v && v->type == JSON_STRING) ? v->string : NULL;
        }
    }
    return NULL;
}

double json_get_number(const JsonValue *obj, const char *key, double def) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? v->number : def;
}

int json_get_int(const JsonValue *obj, const char *key, int def) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? (int)v->number : def;
}

bool json_get_bool(const JsonValue *obj, const char *key, bool def) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_BOOL) ? v->boolean : def;
}

JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.keys[i], key) == 0)
            return obj->object.values[i];
    }
    return NULL;
}

JsonValue *json_array_get(const JsonValue *arr, size_t idx) {
    if (!arr || arr->type != JSON_ARRAY || idx >= arr->array.count) return NULL;
    return arr->array.items[idx];
}

/*--- Builder ---*/

JsonValue *json_new_object(void) { return alloc_val(JSON_OBJECT); }
JsonValue *json_new_array(void)  { return alloc_val(JSON_ARRAY); }

JsonValue *json_new_string(const char *s) {
    JsonValue *v = alloc_val(JSON_STRING);
    v->string = str_dup(s ? s : "");
    return v;
}

JsonValue *json_new_number(double n) {
    JsonValue *v = alloc_val(JSON_NUMBER);
    v->number = n;
    return v;
}

JsonValue *json_new_bool(bool b) {
    JsonValue *v = alloc_val(JSON_BOOL);
    v->boolean = b;
    return v;
}

JsonValue *json_new_null(void) { return alloc_val(JSON_NULL); }

void json_object_set(JsonValue *obj, const char *key, JsonValue *val) {
    if (!obj || obj->type != JSON_OBJECT || !key) return;
    /* Check if key already exists */
    for (size_t i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.keys[i], key) == 0) {
            json_free(obj->object.values[i]);
            obj->object.values[i] = val;
            return;
        }
    }
    size_t n = obj->object.count;
    obj->object.keys = realloc(obj->object.keys, (n + 1) * sizeof(char *));
    obj->object.values = realloc(obj->object.values, (n + 1) * sizeof(JsonValue *));
    obj->object.keys[n] = str_dup(key);
    obj->object.values[n] = val;
    obj->object.count++;
}

void json_array_push(JsonValue *arr, JsonValue *val) {
    if (!arr || arr->type != JSON_ARRAY) return;
    size_t n = arr->array.count;
    arr->array.items = realloc(arr->array.items, (n + 1) * sizeof(JsonValue *));
    arr->array.items[n] = val;
    arr->array.count++;
}

/*--- Serialization ---*/

static void json_serialize_to(const JsonValue *v, StrBuf *sb) {
    if (!v) { strbuf_append_cstr(sb, "null"); return; }
    switch (v->type) {
        case JSON_NULL:
            strbuf_append_cstr(sb, "null");
            break;
        case JSON_BOOL:
            strbuf_append_cstr(sb, v->boolean ? "true" : "false");
            break;
        case JSON_NUMBER:
            strbuf_appendf(sb, "%g", v->number);
            break;
        case JSON_STRING: {
            strbuf_append_cstr(sb, "\"");
            for (const char *p = v->string; p && *p; p++) {
                switch (*p) {
                    case '"':  strbuf_append_cstr(sb, "\\\""); break;
                    case '\\': strbuf_append_cstr(sb, "\\\\"); break;
                    case '\b': strbuf_append_cstr(sb, "\\b"); break;
                    case '\f': strbuf_append_cstr(sb, "\\f"); break;
                    case '\n': strbuf_append_cstr(sb, "\\n"); break;
                    case '\r': strbuf_append_cstr(sb, "\\r"); break;
                    case '\t': strbuf_append_cstr(sb, "\\t"); break;
                    default:
                        if ((unsigned char)*p < 0x20) {
                            strbuf_appendf(sb, "\\u%04x", (unsigned char)*p);
                        } else {
                            strbuf_append(sb, p, 1);
                        }
                }
            }
            strbuf_append_cstr(sb, "\"");
            break;
        }
        case JSON_ARRAY:
            strbuf_append_cstr(sb, "[");
            for (size_t i = 0; i < v->array.count; i++) {
                if (i > 0) strbuf_append_cstr(sb, ",");
                json_serialize_to(v->array.items[i], sb);
            }
            strbuf_append_cstr(sb, "]");
            break;
        case JSON_OBJECT:
            strbuf_append_cstr(sb, "{");
            for (size_t i = 0; i < v->object.count; i++) {
                if (i > 0) strbuf_append_cstr(sb, ",");
                strbuf_append_cstr(sb, "\"");
                strbuf_append_cstr(sb, v->object.keys[i]);
                strbuf_append_cstr(sb, "\":");
                json_serialize_to(v->object.values[i], sb);
            }
            strbuf_append_cstr(sb, "}");
            break;
    }
}

char *json_serialize(const JsonValue *v) {
    StrBuf sb;
    strbuf_init(&sb);
    json_serialize_to(v, &sb);
    return strbuf_detach(&sb);
}
