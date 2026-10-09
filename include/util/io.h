#ifndef UTIL_IO_H
#define UTIL_IO_H
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#ifndef ATTRACTOR_INPUT_LIMIT
#define ATTRACTOR_INPUT_LIMIT (16u * 1024u * 1024u)
#endif
#ifndef ATTRACTOR_OUTPUT_LIMIT
#define ATTRACTOR_OUTPUT_LIMIT (4u * 1024u * 1024u)
#endif
/* Owned bytes, actual length, including a trailing NUL. No seeking required.
 * Text boundaries reject embedded NUL; io_read_stream itself is binary-safe. */
char *io_read_stream(FILE *stream, size_t limit, size_t *length);
char *io_read_file(const char *path, size_t limit, size_t *length);
char *io_read_text(const char *path, size_t limit);
typedef enum {IO_ATOMIC, IO_SYNC, IO_FULL_SYNC} IoDurability;
typedef enum {IO_FAULT_NONE, IO_FAULT_WRITE, IO_FAULT_FLUSH, IO_FAULT_SYNC, IO_FAULT_REPLACE} IoFault;
void io_test_fault(IoFault fault);
bool io_atomic_write(const char *path, const char *bytes, IoDurability durability);
bool io_mkdirs(const char *path);
/* Descriptor-relative traversal; rejects symlinks and, when contained, escapes.
 * Writes create intermediate directories with 0700 and files with supplied mode. */
int io_open_workspace(const char *root, const char *path, int flags, unsigned mode, bool contained);
char *io_artifact_name(const char *node_id);
bool io_artifact_write(const char *root, const char *node_id, const char *name, const char *content);
#endif
