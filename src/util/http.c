#include "util/http.h"
#include "util/str.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*--- Write callback for response body ---*/
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    StrBuf *sb = userdata;
    size_t total = size * nmemb;
    strbuf_append(sb, ptr, total);
    return total;
}

/*--- Header callback ---*/
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    StrBuf *sb = userdata;
    size_t total = size * nmemb;
    strbuf_append(sb, ptr, total);
    return total;
}

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->body);
    free(r->headers_raw);
    free(r);
}

void http_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void) {
    curl_global_cleanup();
}

HttpResponse *http_request(const HttpRequest *req) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    StrBuf body_buf, hdr_buf;
    strbuf_init(&body_buf);
    strbuf_init(&hdr_buf);

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_buf);

    if (req->timeout_ms > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req->timeout_ms);

    struct curl_slist *headers = NULL;
    if (req->headers) {
        for (const char **h = req->headers; *h; h++)
            headers = curl_slist_append(headers, *h);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(req->body_len ? req->body_len : strlen(req->body)));
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        strbuf_free(&body_buf);
        strbuf_free(&hdr_buf);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(resp);
        return NULL;
    }

    long status;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp->status_code = (int)status;
    resp->body = strbuf_detach(&body_buf);
    resp->body_len = body_buf.len;  /* Note: len was reset by detach, need to save before */
    resp->headers_raw = strbuf_detach(&hdr_buf);

    /* Fix: recalculate body_len since detach resets */
    resp->body_len = resp->body ? strlen(resp->body) : 0;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

/*--- SSE streaming ---*/

typedef struct {
    SseCallback  cb;
    void        *userdata;
    StrBuf       line_buf;
    StrBuf       event_type;
    StrBuf       event_data;
} SseState;

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    SseState *st = userdata;
    size_t total = size * nmemb;

    for (size_t i = 0; i < total; i++) {
        char c = ptr[i];
        if (c == '\n') {
            /* Process line */
            char *line = st->line_buf.data;
            size_t len = st->line_buf.len;

            if (len == 0) {
                /* Empty line = event boundary */
                if (st->event_data.len > 0) {
                    /* Remove trailing newline from data */
                    if (st->event_data.len > 0 && st->event_data.data[st->event_data.len - 1] == '\n') {
                        st->event_data.data[--st->event_data.len] = '\0';
                    }
                    const char *etype = (st->event_type.len > 0) ? st->event_type.data : "message";
                    st->cb(etype, st->event_data.data, st->userdata);
                    strbuf_reset(&st->event_type);
                    strbuf_reset(&st->event_data);
                }
            } else if (line && line[0] == ':') {
                /* Comment, ignore */
            } else if (line && str_starts_with(line, "event:")) {
                const char *val = str_trim(line + 6);
                strbuf_reset(&st->event_type);
                strbuf_append_cstr(&st->event_type, val);
            } else if (line && str_starts_with(line, "data:")) {
                const char *val = line + 5;
                if (*val == ' ') val++;
                strbuf_append_cstr(&st->event_data, val);
                strbuf_append_cstr(&st->event_data, "\n");
            } else if (line && str_starts_with(line, "retry:")) {
                /* ignore retry hints */
            }

            strbuf_reset(&st->line_buf);
        } else if (c != '\r') {
            strbuf_append(&st->line_buf, &c, 1);
        }
    }
    return total;
}

int http_stream_sse(const HttpRequest *req, SseCallback cb, void *userdata) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    SseState state;
    state.cb = cb;
    state.userdata = userdata;
    strbuf_init(&state.line_buf);
    strbuf_init(&state.event_type);
    strbuf_init(&state.event_data);

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

    if (req->timeout_ms > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req->timeout_ms);

    struct curl_slist *headers = NULL;
    if (req->headers) {
        for (const char **h = req->headers; *h; h++)
            headers = curl_slist_append(headers, *h);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req->body)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    strbuf_free(&state.line_buf);
    strbuf_free(&state.event_type);
    strbuf_free(&state.event_data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (int)status;
}

const char *http_header_get(const char *headers_raw, const char *name, char *buf, size_t buf_sz) {
    if (!headers_raw || !name) return NULL;
    size_t nlen = strlen(name);
    const char *p = headers_raw;
    while (*p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *val = p + nlen + 1;
            while (*val == ' ') val++;
            const char *end = val;
            while (*end && *end != '\r' && *end != '\n') end++;
            size_t vlen = (size_t)(end - val);
            if (vlen >= buf_sz) vlen = buf_sz - 1;
            memcpy(buf, val, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}
