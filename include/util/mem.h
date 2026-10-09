#ifndef UTIL_MEM_H
#define UTIL_MEM_H
#include <stddef.h>
#include <stdbool.h>
bool size_add(size_t a, size_t b, size_t *out);
bool size_mul(size_t a, size_t b, size_t *out);
void *mem_alloc(size_t size);
void *mem_calloc(size_t count, size_t size);
/* Old allocation remains owned by caller on failure. */
void *mem_reallocarray(void *old, size_t count, size_t size);
/* Test-only, per-thread: -1 disables; 0 fails next and subsequent allocations. */
void mem_fail_after(long successful_allocations);
unsigned long mem_failure_count(void);
#endif
