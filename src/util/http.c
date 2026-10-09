#include "util/http.h"
#include "util/str.h"
#include "util/mem.h"
#include "util/io.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <limits.h>
static pthread_mutex_t global_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned users;
bool http_global_init(void) {
    pthread_mutex_lock(&global_lock);
    bool ok=users>0 || curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
    if(ok) users++;pthread_mutex_unlock(&global_lock);return ok;
}
void http_global_cleanup(void) {
    pthread_mutex_lock(&global_lock);
    if(users && --users==0) curl_global_cleanup();pthread_mutex_unlock(&global_lock);
}
static size_t receive(char *data,size_t size,size_t count,void *userdata) {
    size_t total;if(!size_mul(size,count,&total)) return 0;
    StrBuf *buffer=userdata;strbuf_append(buffer,data,total);return buffer->failed?0:total;
}
static int progress(void *userdata,curl_off_t down_total,curl_off_t down,curl_off_t up_total,curl_off_t up) {
    (void)down_total;(void)down;(void)up_total;(void)up;
    const bool *cancel=userdata;return cancel && *cancel;
}
void http_response_free(HttpResponse *response) {if(response) {free(response->body);free(response->headers_raw);free(response);}}
HttpResponse *http_request(const HttpRequest *req) {
    if(!req || !req->url || req->timeout_ms<0 || (req->cancel && *req->cancel) || !http_global_init()) return NULL;
    CURL *curl=curl_easy_init();HttpResponse *response=NULL;struct curl_slist *headers=NULL;
    StrBuf body,raw_headers;
    strbuf_init_limit(&body,req->max_body_bytes?req->max_body_bytes:ATTRACTOR_INPUT_LIMIT);
    strbuf_init_limit(&raw_headers,req->max_header_bytes?req->max_header_bytes:64u*1024u);
    if(!curl) goto cleanup;
#define OPTION(key,value) do {if(curl_easy_setopt(curl,key,value)!=CURLE_OK) goto cleanup;} while(0)
    OPTION(CURLOPT_URL,req->url);OPTION(CURLOPT_NOSIGNAL,1L);
    /* The legacy bitmask is supported by the libcurl shipped with macOS 13.
     * PROTOCOLS_STR requires 7.85; do not require a newer SDK symbol at runtime. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OPTION(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#pragma clang diagnostic pop
    OPTION(CURLOPT_WRITEFUNCTION,receive);OPTION(CURLOPT_WRITEDATA,&body);
    OPTION(CURLOPT_HEADERFUNCTION,receive);OPTION(CURLOPT_HEADERDATA,&raw_headers);
    OPTION(CURLOPT_TIMEOUT_MS,req->timeout_ms?req->timeout_ms:120000L);
    OPTION(CURLOPT_CONNECTTIMEOUT_MS,30000L);
    if(req->cancel) {OPTION(CURLOPT_NOPROGRESS,0L);OPTION(CURLOPT_XFERINFOFUNCTION,progress);OPTION(CURLOPT_XFERINFODATA,req->cancel);}
    if(req->headers) for(const char **header=req->headers;*header;header++) {
        if(strchr(*header,'\r') || strchr(*header,'\n') || strlen(*header)>8192) goto cleanup;
        struct curl_slist *next=curl_slist_append(headers,*header);if(!next) goto cleanup;headers=next;
    }
    if(headers) OPTION(CURLOPT_HTTPHEADER,headers);
    if(req->method && str_eq(req->method,"POST")) {
        OPTION(CURLOPT_POST,1L);
        if(req->body) {
            size_t length=req->body_len?req->body_len:strlen(req->body);if(length>ATTRACTOR_INPUT_LIMIT) goto cleanup;
            OPTION(CURLOPT_POSTFIELDS,req->body);OPTION(CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)length);
        }
    } else if(req->method && !str_eq(req->method,"GET")) goto cleanup;
    if(curl_easy_perform(curl)!=CURLE_OK || body.failed || raw_headers.failed) goto cleanup;
    long status=0;if(curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK || status<100 || status>599) goto cleanup;
    response=mem_calloc(1,sizeof(*response));if(!response) goto cleanup;
    response->status_code=(int)status;response->body_len=body.len;
    response->body=strbuf_detach(&body);response->headers_raw=strbuf_detach(&raw_headers);
    if(!response->body || !response->headers_raw) {http_response_free(response);response=NULL;}
#undef OPTION
cleanup:
    strbuf_free(&body);strbuf_free(&raw_headers);curl_slist_free_all(headers);if(curl) curl_easy_cleanup(curl);http_global_cleanup();return response;
}
int http_stream_sse(const HttpRequest *req,SseCallback callback,void *userdata) {
    (void)req;(void)callback;(void)userdata;return -1; /* unsupported until provider-event coverage exists */
}
const char *http_header_get(const char *headers,const char *name,char *buffer,size_t capacity) {
    if(!headers || !name || !*name || !buffer || !capacity) return NULL;
    size_t length=strlen(name);
    for(const char *p=headers;*p;) {
        const char *end=p;while(*end && *end!='\r' && *end!='\n') end++;
        if((size_t)(end-p)>length && p[length]==':' && !strncasecmp(p,name,length)) {
            const char *value=p+length+1;while(value<end && (*value==' ' || *value=='\t')) value++;
            size_t n=(size_t)(end-value);if(n>=capacity) n=capacity-1;memcpy(buffer,value,n);buffer[n]=0;return buffer;
        }
        p=end;while(*p=='\r' || *p=='\n') p++;
    }return NULL;
}
