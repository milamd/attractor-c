#include "util/mem.h"
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
static _Thread_local long remaining = -1;
static _Thread_local unsigned long failures;
unsigned long mem_failure_count(void) {return failures;}
bool size_add(size_t a,size_t b,size_t *out) {
    if (b > SIZE_MAX-a) return false;
    *out=a+b; return true;
}
bool size_mul(size_t a,size_t b,size_t *out) {
    if (a && b > SIZE_MAX/a) return false;
    *out=a*b; return true;
}
void mem_fail_after(long count) { remaining=count; }
static bool allowed(void) {
    if (remaining==0) {failures++;errno=ENOMEM;return false;}
    if (remaining>0) remaining--;
    return true;
}
void *mem_alloc(size_t size) {if(!allowed()) return NULL;void *p=malloc(size?size:1);if(!p) failures++;return p;}
void *mem_calloc(size_t count,size_t size) {
    size_t bytes; if(!size_mul(count,size,&bytes)) {failures++;errno=ENOMEM;return NULL;}
    if(!allowed()) return NULL;void *p=calloc(1,bytes?bytes:1);if(!p) failures++;return p;
}
void *mem_reallocarray(void *old,size_t count,size_t size) {
    size_t bytes; if(!size_mul(count,size,&bytes)) {failures++;errno=ENOMEM;return NULL;}
    if(!allowed()) return NULL;void *p=realloc(old,bytes?bytes:1);if(!p) failures++;return p;
}
