#ifndef UTIL_HTTP_H
#define UTIL_HTTP_H

#include <stddef.h>
#include <stdbool.h>
#include "util/json.h"

/* HTTP response */
typedef struct {
    int     status_code;
    char   *body;
    size_t  body_len;
    char   *headers_raw;
} HttpResponse;

void http_response_free(HttpResponse *r);

/* HTTP request configuration */
typedef struct {
    const char  *url;
    const char  *method;        /* "GET" or "POST" */
    const char  *body;
    size_t       body_len;
    const char **headers;       /* null-terminated array of "Key: Value" strings */
    long         timeout_ms;
} HttpRequest;

/* Execute HTTP request. Caller must free response. */
HttpResponse *http_request(const HttpRequest *req);

/* SSE callback: receives event type and data for each SSE event */
typedef void (*SseCallback)(const char *event_type, const char *data, void *userdata);

/* Stream an SSE response, calling cb for each event. Returns HTTP status code. */
int http_stream_sse(const HttpRequest *req, SseCallback cb, void *userdata);

/* Get a header value from raw headers (case-insensitive). Returns NULL if not found. */
const char *http_header_get(const char *headers_raw, const char *name, char *buf, size_t buf_sz);

/* Global init/cleanup (wraps curl_global_init/cleanup) */
void http_global_init(void);
void http_global_cleanup(void);

#endif
