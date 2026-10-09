#ifndef UTIL_STR_H
#define UTIL_STR_H

#include <stddef.h>
#include <stdbool.h>

/* Dynamic string buffer */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    size_t limit;
    bool failed;  /* sticky; detach returns NULL and frees storage on failure */
} StrBuf;

void    strbuf_init(StrBuf *sb);
void    strbuf_init_limit(StrBuf *sb, size_t limit);
void    strbuf_free(StrBuf *sb);
void    strbuf_append(StrBuf *sb, const char *s, size_t n);
void    strbuf_append_cstr(StrBuf *sb, const char *s);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void    strbuf_appendf(StrBuf *sb, const char *fmt, ...);
char   *strbuf_detach(StrBuf *sb);   /* caller owns the returned string */
void    strbuf_reset(StrBuf *sb);

/* String utilities */
char   *str_dup(const char *s);
char   *str_ndup(const char *s, size_t n);
bool    str_eq(const char *a, const char *b);
bool    str_starts_with(const char *s, const char *prefix);
bool    str_ends_with(const char *s, const char *suffix);
bool    str_contains(const char *haystack, const char *needle);
bool    str_icontains(const char *haystack, const char *needle);
char   *str_trim(char *s);
char   *str_lower(char *s);
size_t  str_split(const char *s, char delim, char ***out);
void    str_split_free(char **parts, size_t count);

/* String-safe null returns "" */
static inline const char *str_safe(const char *s) { return s ? s : ""; }

#endif
