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
/* Shared transfer options (everything except where response bytes go). */
static bool configure(CURL *curl,const HttpRequest *req,struct curl_slist **headers) {
#define OPTION(key,value) do {if(curl_easy_setopt(curl,key,value)!=CURLE_OK) return false;} while(0)
    OPTION(CURLOPT_URL,req->url);OPTION(CURLOPT_NOSIGNAL,1L);
    /* The legacy bitmask is supported by the libcurl shipped with macOS 13.
     * PROTOCOLS_STR requires 7.85; do not require a newer SDK symbol at runtime. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OPTION(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#pragma clang diagnostic pop
    OPTION(CURLOPT_TIMEOUT_MS,req->timeout_ms?req->timeout_ms:120000L);
    OPTION(CURLOPT_CONNECTTIMEOUT_MS,30000L);
    if(req->cancel) {OPTION(CURLOPT_NOPROGRESS,0L);OPTION(CURLOPT_XFERINFOFUNCTION,progress);OPTION(CURLOPT_XFERINFODATA,req->cancel);}
    if(req->headers) for(const char **header=req->headers;*header;header++) {
        if(strchr(*header,'\r') || strchr(*header,'\n') || strlen(*header)>8192) return false;
        struct curl_slist *next=curl_slist_append(*headers,*header);if(!next) return false;*headers=next;
    }
    if(*headers) OPTION(CURLOPT_HTTPHEADER,*headers);
    if(req->method && str_eq(req->method,"POST")) {
        OPTION(CURLOPT_POST,1L);
        if(req->body) {
            size_t length=req->body_len?req->body_len:strlen(req->body);if(length>ATTRACTOR_INPUT_LIMIT) return false;
            OPTION(CURLOPT_POSTFIELDS,req->body);OPTION(CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)length);
        }
    } else if(req->method && !str_eq(req->method,"GET")) return false;
    return true;
#undef OPTION
}
HttpResponse *http_request(const HttpRequest *req) {
    if(!req || !req->url || req->timeout_ms<0 || (req->cancel && *req->cancel) || !http_global_init()) return NULL;
    CURL *curl=curl_easy_init();HttpResponse *response=NULL;struct curl_slist *headers=NULL;
    StrBuf body,raw_headers;
    strbuf_init_limit(&body,req->max_body_bytes?req->max_body_bytes:ATTRACTOR_INPUT_LIMIT);
    strbuf_init_limit(&raw_headers,req->max_header_bytes?req->max_header_bytes:64u*1024u);
    if(!curl || !configure(curl,req,&headers)) goto cleanup;
    if(curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive)!=CURLE_OK || curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body)!=CURLE_OK ||
       curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,receive)!=CURLE_OK || curl_easy_setopt(curl,CURLOPT_HEADERDATA,&raw_headers)!=CURLE_OK) goto cleanup;
    if(curl_easy_perform(curl)!=CURLE_OK || body.failed || raw_headers.failed) goto cleanup;
    long status=0;if(curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK || status<100 || status>599) goto cleanup;
    response=mem_calloc(1,sizeof(*response));if(!response) goto cleanup;
    response->status_code=(int)status;response->body_len=body.len;
    response->body=strbuf_detach(&body);response->headers_raw=strbuf_detach(&raw_headers);
    if(!response->body || !response->headers_raw) {http_response_free(response);response=NULL;}
cleanup:
    strbuf_free(&body);strbuf_free(&raw_headers);curl_slist_free_all(headers);if(curl) curl_easy_cleanup(curl);http_global_cleanup();return response;
}

/* Incremental SSE parser state. Bytes are scanned once; only the trailing
 * partial line is kept between chunks. */
typedef struct {
    CURL *curl;SseCallback callback;void *userdata;
    long status;bool stopped;
    StrBuf pending,event,data,error_body;
    size_t total,limit;
} SseState;
static void sse_line(SseState *s,const char *line,size_t n) {
    if(n && line[n-1]=='\r') n--;
    if(!n) { /* blank line dispatches the event */
        if(s->data.len || s->event.len) {
            if(s->data.failed || s->event.failed || !s->callback(s->event.len?s->event.data:"message",s->data.len?s->data.data:"",s->userdata)) s->stopped=true;
        }
        strbuf_reset(&s->event);strbuf_reset(&s->data);return;
    }
    if(line[0]==':') return;
    const char *colon=memchr(line,':',n);size_t key=colon?(size_t)(colon-line):n;
    const char *value=colon?colon+1:line+n;size_t vn=colon?n-key-1:0;
    if(vn && *value==' ') {value++;vn--;}
    if(key==5 && !memcmp(line,"event",5)) {strbuf_reset(&s->event);strbuf_append(&s->event,value,vn);}
    else if(key==4 && !memcmp(line,"data",4)) {if(s->data.len) strbuf_append(&s->data,"\n",1);strbuf_append(&s->data,value,vn);}
}
static size_t sse_receive(char *bytes,size_t size,size_t count,void *userdata) {
    SseState *s=userdata;size_t total;if(!size_mul(size,count,&total) || s->stopped) return 0;
    if(!s->status && (curl_easy_getinfo(s->curl,CURLINFO_RESPONSE_CODE,&s->status)!=CURLE_OK || !s->status)) return 0;
    s->total+=total;if(s->total>s->limit) return 0;
    if(s->status<200 || s->status>=300) {strbuf_append(&s->error_body,bytes,total);return s->error_body.failed?0:total;}
    strbuf_append(&s->pending,bytes,total);if(s->pending.failed) return 0;
    size_t start=0;
    for(char *nl;!s->stopped && (nl=memchr(s->pending.data+start,'\n',s->pending.len-start));start=(size_t)(nl-s->pending.data)+1)
        sse_line(s,s->pending.data+start,(size_t)(nl-(s->pending.data+start)));
    if(start) {memmove(s->pending.data,s->pending.data+start,s->pending.len-start);s->pending.len-=start;s->pending.data[s->pending.len]=0;}
    return s->stopped?0:total;
}
int http_stream_sse(const HttpRequest *req,SseCallback callback,void *userdata,HttpResponse **error_response) {
    if(error_response) *error_response=NULL;
    if(!req || !req->url || !callback || req->timeout_ms<0 || (req->cancel && *req->cancel) || !http_global_init()) return -1;
    CURL *curl=curl_easy_init();struct curl_slist *headers=NULL;int result=-1;
    SseState s={.curl=curl,.callback=callback,.userdata=userdata,.limit=req->max_body_bytes?req->max_body_bytes:ATTRACTOR_INPUT_LIMIT};
    StrBuf raw_headers;strbuf_init_limit(&raw_headers,req->max_header_bytes?req->max_header_bytes:64u*1024u);
    strbuf_init_limit(&s.pending,ATTRACTOR_INPUT_LIMIT);strbuf_init_limit(&s.event,256);strbuf_init_limit(&s.data,ATTRACTOR_INPUT_LIMIT);strbuf_init_limit(&s.error_body,ATTRACTOR_INPUT_LIMIT);
    if(!curl || !configure(curl,req,&headers)) goto cleanup;
    if(curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,sse_receive)!=CURLE_OK || curl_easy_setopt(curl,CURLOPT_WRITEDATA,&s)!=CURLE_OK ||
       curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,receive)!=CURLE_OK || curl_easy_setopt(curl,CURLOPT_HEADERDATA,&raw_headers)!=CURLE_OK) goto cleanup;
    CURLcode code=curl_easy_perform(curl);
    if(s.stopped && !s.data.failed && !s.event.failed) {result=(int)s.status;goto cleanup;} /* caller stopped */
    if(code!=CURLE_OK || raw_headers.failed) goto cleanup;
    if(!s.status && curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&s.status)!=CURLE_OK) goto cleanup;
    if(s.status>=200 && s.status<300) {
        if(s.pending.len) sse_line(&s,s.pending.data,s.pending.len); /* final line without newline */
        sse_line(&s,"",0);                                           /* flush a trailing event */
        result=(int)s.status;
    } else if(s.status>=100 && s.status<=599) {
        result=(int)s.status;
        if(error_response && (*error_response=mem_calloc(1,sizeof(HttpResponse)))) {
            (*error_response)->status_code=(int)s.status;(*error_response)->body_len=s.error_body.len;
            (*error_response)->body=strbuf_detach(&s.error_body);(*error_response)->headers_raw=strbuf_detach(&raw_headers);
            if(!(*error_response)->body || !(*error_response)->headers_raw) {http_response_free(*error_response);*error_response=NULL;}
        }
    }
cleanup:
    strbuf_free(&s.pending);strbuf_free(&s.event);strbuf_free(&s.data);strbuf_free(&s.error_body);strbuf_free(&raw_headers);
    curl_slist_free_all(headers);if(curl) curl_easy_cleanup(curl);http_global_cleanup();return result;
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
