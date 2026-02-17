#include "util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/*--- StrBuf ---*/

void strbuf_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void strbuf_grow(StrBuf *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    size_t newcap = sb->cap ? sb->cap * 2 : 256;
    while (newcap < sb->len + need + 1) newcap *= 2;
    sb->data = realloc(sb->data, newcap);
    sb->cap = newcap;
}

void strbuf_append(StrBuf *sb, const char *s, size_t n) {
    if (!s || n == 0) return;
    strbuf_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void strbuf_append_cstr(StrBuf *sb, const char *s) {
    if (!s) return;
    strbuf_append(sb, s, strlen(s));
}

void strbuf_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    strbuf_grow(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    sb->len += (size_t)n;
    va_end(ap2);
}

char *strbuf_detach(StrBuf *sb) {
    char *r = sb->data;
    if (!r) { r = calloc(1, 1); }
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return r;
}

void strbuf_reset(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

/*--- String utilities ---*/

char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *str_ndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

bool str_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s), pl = strlen(suffix);
    if (pl > sl) return false;
    return strcmp(s + sl - pl, suffix) == 0;
}

bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

bool str_icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t hl = strlen(haystack), nl = strlen(needle);
    if (nl > hl) return false;
    for (size_t i = 0; i <= hl - nl; i++) {
        bool match = true;
        for (size_t j = 0; j < nl; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

char *str_trim(char *s) {
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

char *str_lower(char *s) {
    if (!s) return NULL;
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

size_t str_split(const char *s, char delim, char ***out) {
    if (!s) { *out = NULL; return 0; }
    size_t count = 1;
    for (const char *p = s; *p; p++) {
        if (*p == delim) count++;
    }
    *out = malloc(count * sizeof(char *));
    size_t idx = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == delim || *p == '\0') {
            (*out)[idx++] = str_ndup(start, (size_t)(p - start));
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    return count;
}

void str_split_free(char **parts, size_t count) {
    if (!parts) return;
    for (size_t i = 0; i < count; i++) free(parts[i]);
    free(parts);
}
