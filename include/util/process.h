#ifndef UTIL_PROCESS_H
#define UTIL_PROCESS_H
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char *stdout_buf;
    char *stderr_buf;
    int exit_code; /* -1 on allocation/spawn/wait error, otherwise exit or 128+signal */
    bool timed_out;
    bool cancelled;
    bool output_limited;
    int duration_ms;
} ExecResult;
typedef struct {
    const char *const *argv; /* executable must be an absolute controlled path */
    const char *working_dir;
    int timeout_ms;
    size_t output_limit;
    const bool *cancel; /* synchronous owner-thread cancellation only */
    const char *const *extra_env; /* explicit KEY=value additions */
} ProcessOptions;
/* Owned result. Shell syntax only via explicit /bin/sh -c argv. Environment is
 * an allowlist (PATH, HOME, TMPDIR, locale, user, terminal); API credentials are
 * excluded. Process groups cannot contain deliberately detached descendants. */
ExecResult *process_run(const ProcessOptions *options);
void exec_result_free(ExecResult *result);
#endif
