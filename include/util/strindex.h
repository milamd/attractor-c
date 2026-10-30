#ifndef UTIL_STRINDEX_H
#define UTIL_STRINDEX_H
#include <stddef.h>
#include <stdbool.h>
/* Open-addressed string -> size_t map. Keys are borrowed and must outlive the
 * index without changing. Put replaces an existing key's value. */
typedef struct {
    const char *key;
    size_t      hash;
    size_t      value;
} StrIndexSlot;

typedef struct StrIndex {
    StrIndexSlot *slots;
    size_t        cap;   /* zero or a power of two */
    size_t        count;
} StrIndex;
void strindex_init(StrIndex *ix);
void strindex_free(StrIndex *ix);
bool strindex_put(StrIndex *ix, const char *key, size_t value);
bool strindex_get(const StrIndex *ix, const char *key, size_t *value);
#endif
