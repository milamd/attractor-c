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
    size_t       max_body_bytes; /* 0: 16 MiB */
    size_t       max_header_bytes; /* 0: 64 KiB */
    const bool  *cancel; /* borrowed, owner-thread */
} HttpRequest;

/* Execute HTTP request. Caller must free response. */
HttpResponse *http_request(const HttpRequest *req);

/* SSE callback: receives the event type ("message" when unnamed) and data for
 * each event. Return false to stop the stream. */
typedef bool (*SseCallback)(const char *event_type, const char *data, void *userdata);

/* Streams a request as Server-Sent Events, parsing incrementally. For a 2xx
 * response each event is delivered as it arrives; otherwise the body is
 * returned in *error_response (owned) for error mapping. Returns the HTTP
 * status, or -1 on transport failure or cancellation. */
int http_stream_sse(const HttpRequest *req, SseCallback cb, void *userdata, HttpResponse **error_response);

/* Get a header value from raw headers (case-insensitive). Returns NULL if not found. */
const char *http_header_get(const char *headers_raw, const char *name, char *buf, size_t buf_sz);

/* Global init/cleanup (wraps curl_global_init/cleanup) */
bool http_global_init(void);
void http_global_cleanup(void);

#endif
