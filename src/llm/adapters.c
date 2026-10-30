#include <ctype.h>
#include "llm/client.h"
#include "stream_sink.h"
#include "util/str.h"
#include "util/json.h"
#include "util/http.h"
#include "util/mem.h"
#include <limits.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <uuid/uuid.h>

/*============================================================================
 * Shared adapter helpers
 *==========================================================================*/

static char *build_messages_json_anthropic(Message **msgs, size_t count, char **system_out, bool cache);
static char *build_messages_json_openai(Message **msgs, size_t count, char **instructions_out);
static char *build_messages_json_gemini(Message **msgs, size_t count, char **system_instruction_out);
static char *build_tools_json_anthropic(ToolDefinition **tools, size_t count, bool cache);
static JsonValue *ephemeral_cache(void);
static char *build_tools_json_openai(ToolDefinition **tools, size_t count);
static char *build_tools_json_gemini(ToolDefinition **tools, size_t count);

/* Parse Anthropic response JSON into LlmResponse */
static LlmResponse *parse_anthropic_response(const char *json_str);
/* Parse OpenAI Responses API response */
static LlmResponse *parse_openai_response(const char *json_str);
/* Parse Gemini response */
static LlmResponse *parse_gemini_response(const char *json_str);

LlmErrorCode llm_error_from_status(int status) {
    switch (status) {
        case 400: case 422: return LLM_ERR_INVALID_REQUEST;
        case 401: return LLM_ERR_AUTH;
        case 403: return LLM_ERR_ACCESS_DENIED;
        case 404: return LLM_ERR_NOT_FOUND;
        case 408: return LLM_ERR_TIMEOUT;
        case 413: return LLM_ERR_CONTEXT_LENGTH;
        case 429: return LLM_ERR_RATE_LIMIT;
        default:
            if (status >= 500 && status < 600) return LLM_ERR_SERVER;
            return LLM_ERR_PROVIDER;
    }
}

static void set_error(LlmError *err, LlmErrorCode code, const char *msg,
                      const char *provider, int status, bool retryable) {
    if (!err) return;
    llm_error_free(err);
    err->code = code;
    err->message = str_dup(msg);
    err->provider = str_dup(provider);
    err->status_code = status;
    err->retryable = retryable;
    err->retry_after = -1.0;
    err->raw_json = NULL;
}

/* Spec 6.5: statuses that do not identify the failure (400, 422 and
 * unmapped codes) are refined by the provider's message text. */
LlmErrorCode llm_error_classify(int status, const char *message) {
    LlmErrorCode code = llm_error_from_status(status);
    if ((code != LLM_ERR_INVALID_REQUEST && code != LLM_ERR_PROVIDER) || !message) return code;
    if (str_icontains(message, "not found") || str_icontains(message, "does not exist")) return LLM_ERR_NOT_FOUND;
    if (str_icontains(message, "unauthorized") || str_icontains(message, "invalid key")) return LLM_ERR_AUTH;
    if (str_icontains(message, "context length") || str_icontains(message, "too many tokens")) return LLM_ERR_CONTEXT_LENGTH;
    if (str_icontains(message, "content filter") || str_icontains(message, "safety")) return LLM_ERR_CONTENT_FILTER;
    return code;
}

/* Non-2xx response: classified error with the provider's message, unless it
 * echoes the credential, and any Retry-After hint. */
static void status_error(LlmError *err, const HttpResponse *resp, const char *provider, const char *secret) {
    JsonValue *body = resp->body ? json_parse(resp->body, NULL) : NULL;
    const char *detail = json_get_string(json_get(body, "error"), "message");
    if (detail && (strnlen(detail, 1025) > 1024 || (secret && strstr(detail, secret)))) detail = NULL;
    bool retryable = resp->status_code == 429 || resp->status_code >= 500;
    StrBuf message; strbuf_init(&message);
    strbuf_append_cstr(&message, "Provider rejected request");
    if (detail) strbuf_appendf(&message, ": %s", detail);
    char *text = strbuf_detach(&message);
    set_error(err, llm_error_classify(resp->status_code, detail), text ? text : "Provider rejected request", provider, resp->status_code, retryable);
    free(text);
    json_free(body);
    if (err) {
        char hint[64]; const char *value = http_header_get(resp->headers_raw, "Retry-After", hint, sizeof(hint));
        if (value) { char *end; double seconds = strtod(value, &end); if (!*end && isfinite(seconds) && seconds >= 0) err->retry_after = seconds; }
    }
}

static char *quoted(const char *text) {
    JsonValue *value=json_new_string(text);char *encoded=json_serialize(value);json_free(value);return encoded;
}
static void append_number(StrBuf *buffer,const char *key,double number) {
    JsonValue *value=json_new_number(number);char *encoded=json_serialize(value);json_free(value);
    if(encoded) strbuf_appendf(buffer,"%s%s",key,encoded);else buffer->failed=true;free(encoded);
}
static char *url_component(const char *text) {
    StrBuf b;strbuf_init(&b);
    for(const unsigned char *p=(const unsigned char *)str_safe(text);*p;p++) {
        if((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || strchr("-_.~",*p)) strbuf_append(&b,(const char *)p,1);
        else strbuf_appendf(&b,"%%%02X",(unsigned)*p);
    }return strbuf_detach(&b);
}

/* Credentials travel in request headers, so require TLS except for loopback
 * hosts used by local proxies and test fixtures. */
static bool base_url_allowed(const char *url) {
    if(!url) return true;
    if(!strncasecmp(url,"https://",8)) return url[8]!=0;
    if(strncasecmp(url,"http://",7)) return false;
    const char *host=url+7,*loopback[]={"127.0.0.1","localhost","[::1]"};
    for(size_t i=0;i<sizeof(loopback)/sizeof(*loopback);i++) {
        size_t n=strlen(loopback[i]);
        if(!strncasecmp(host,loopback[i],n) && (!host[n] || host[n]==':' || host[n]=='/')) return true;
    }
    return false;
}
static void free_secret(char *secret) {if(secret) {mem_wipe(secret,strlen(secret));free(secret);}}

/*============================================================================
 * Anthropic Adapter
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} AnthropicState;

/* Shared transport: blocking request, or SSE when streaming. */
static HttpResponse *send_request(const HttpRequest *hreq, StreamSink *sink) {
    return sink ? stream_sink_transport(sink, hreq) : http_request(hreq);
}
/* Maps a missing response to cancel, caller stop, stream failure, or network error. */
static void transport_error(LlmError *err, const LlmRequest *req, StreamSink *sink, const char *provider) {
    if (req->cancel && *req->cancel) set_error(err, LLM_ERR_ABORT, "Cancelled", provider, 0, false);
    else if (sink && stream_sink_stopped(sink)) set_error(err, LLM_ERR_ABORT, "Stream stopped by caller", provider, 0, false);
    else if (sink && stream_sink_error(sink)) set_error(err, LLM_ERR_STREAM, stream_sink_error(sink), provider, 0, false);
    else set_error(err, LLM_ERR_NETWORK, "HTTP request failed", provider, 0, true);
}
typedef LlmResponse *(*RequestFn)(ProviderAdapter *, const LlmRequest *, LlmError *, StreamSink *);
static LlmResponse *run_stream(ProviderAdapter *self, const LlmRequest *req, StreamCallback cb, void *userdata,
                               LlmError *err, SinkKind kind, RequestFn request) {
    StreamSink *sink = stream_sink_new(kind, cb, userdata);
    if (!sink) { set_error(err, LLM_ERR_CONFIG, "Allocation failed", self->name, 0, false); return NULL; }
    LlmResponse *r = request(self, req, err, sink);
    if (r) stream_sink_finish(sink, r);
    else stream_sink_fail(sink, err && err->message ? err->message : "Stream failed");
    stream_sink_free(sink);
    return r;
}

#define ANTHROPIC_EXTRACTOR "structured_output"

/* Replaces the forced extraction tool call with its arguments as plain text. */
static bool extracted_to_text(LlmResponse *r) {
    for (size_t i = 0; i < r->tool_call_count; i++) {
        if (!str_eq(r->tool_calls[i]->name, ANTHROPIC_EXTRACTOR)) continue;
        char *text = str_dup(r->tool_calls[i]->arguments_json);
        Message *message = text ? message_assistant(text) : NULL;
        if (!message) { free(text); return false; }
        for (size_t k = 0; k < r->tool_call_count; k++) tool_call_free(r->tool_calls[k]);
        free(r->tool_calls); r->tool_calls = NULL; r->tool_call_count = 0;
        message_free(r->message); r->message = message;
        free(r->text); r->text = text;
        r->finish_reason.reason = FINISH_STOP;
        return true;
    }
    return false;
}

static LlmResponse *anthropic_request(ProviderAdapter *self, const LlmRequest *req, LlmError *err, StreamSink *sink) {
    unsigned long request_failures=mem_failure_count();
    AnthropicState *st = self->impl;
    if(req->reasoning_effort!=REASONING_NONE) {set_error(err,LLM_ERR_CONFIG,"Reasoning effort control is unsupported for Anthropic","anthropic",0,false);return NULL;}
    for(size_t i=0;i<req->message_count;i++) for(size_t k=0;k<req->messages[i]->part_count;k++) {
        ContentPart *part=req->messages[i]->parts[k];
        if(!part || !(part->kind==CONTENT_TEXT || part->kind==CONTENT_TOOL_CALL || part->kind==CONTENT_TOOL_RESULT || part->kind==CONTENT_THINKING || part->kind==CONTENT_REDACTED_THINKING)) {set_error(err,LLM_ERR_CONFIG,"Unsupported content kind for adapter","anthropic",0,false);return NULL;}
    }


    /* provider_options.anthropic: auto_cache (default true) and beta_headers.
     * Caching heuristic: breakpoints on the system prompt, the last tool
     * definition, and the newest message block (3 of Anthropic's 4). */
    JsonValue *options = req->provider_options_json ? json_parse(req->provider_options_json, NULL) : NULL;
    JsonValue *anthropic_options = json_get(options, "anthropic");
    bool cache = json_get_bool(anthropic_options, "auto_cache", true);
    StrBuf beta; strbuf_init(&beta);
    JsonValue *betas = json_get(anthropic_options, "beta_headers");
    for (size_t i = 0; betas && betas->type == JSON_ARRAY && i < betas->array.count; i++) {
        const JsonValue *b = betas->array.items[i];
        bool safe = b->type == JSON_STRING && *b->string && strlen(b->string) <= 128;
        for (const char *c = safe ? b->string : ""; *c && safe; c++) safe = isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.';
        if (!safe) { json_free(options); strbuf_free(&beta); set_error(err,LLM_ERR_CONFIG,"Invalid anthropic beta header","anthropic",0,false); return NULL; }
        strbuf_appendf(&beta, "%s%s", beta.len ? "," : "anthropic-beta: ", b->string);
    }
    json_free(options);
    if (cache) strbuf_appendf(&beta, "%sprompt-caching-2024-07-31", beta.len ? "," : "anthropic-beta: ");

    /* Build request body */
    char *system_prompt = NULL;
    char *messages_json = build_messages_json_anthropic(req->messages, req->message_count, &system_prompt, cache);
    /* Structured output has no native mode: json_schema forces a call to an
     * extraction tool whose input schema is the requested schema. */
    const ResponseFormat *rf = req->response_format;
    bool extract = rf && str_eq(rf->type, "json_schema");
    ToolDefinition extractor = {.name = ANTHROPIC_EXTRACTOR, .description = "Return the final answer as structured output.", .parameters_json = extract ? rf->json_schema : NULL};
    ToolDefinition **tool_list = req->tools; size_t tool_total = req->tool_count;
    ToolDefinition **with_extractor = NULL;
    if (extract) {
        with_extractor = mem_calloc(req->tool_count + 1, sizeof(*with_extractor));
        if (!with_extractor) { free(messages_json); free(system_prompt); strbuf_free(&beta); set_error(err,LLM_ERR_CONFIG,"Allocation failed","anthropic",0,false); return NULL; }
        for (size_t i = 0; i < req->tool_count; i++) with_extractor[i] = req->tools[i];
        with_extractor[req->tool_count] = &extractor;
        tool_list = with_extractor; tool_total = req->tool_count + 1;
    }
    char *tools_json = tool_total > 0 && (extract || !(req->tool_choice && req->tool_choice->mode==TOOL_CHOICE_NONE))
        ? build_tools_json_anthropic(tool_list, tool_total, cache)
        : NULL;
    free(with_extractor);
    if (rf && str_eq(rf->type, "json")) {
        StrBuf sys; strbuf_init(&sys);
        if (system_prompt) { strbuf_append_cstr(&sys, system_prompt); strbuf_append_cstr(&sys, "\n\n"); }
        strbuf_append_cstr(&sys, "Respond with only a single valid JSON object and no other text.");
        free(system_prompt); system_prompt = strbuf_detach(&sys);
    }

    StrBuf body;
    strbuf_init(&body);
    char *model_json=quoted(req->model);
    if(!model_json) body.failed=true;
    else strbuf_appendf(&body,"{\"model\":%s,\"max_tokens\":%d",model_json,req->max_tokens>0?req->max_tokens:4096);
    free(model_json);

    if (system_prompt) {
        /* Escape system prompt for JSON */
        JsonValue *sp = json_new_string(system_prompt);
        if (cache) {
            JsonValue *block = json_new_object();
            json_object_set(block, "type", json_new_string("text"));
            json_object_set(block, "text", sp);
            json_object_set(block, "cache_control", ephemeral_cache());
            sp = json_new_array();
            json_array_push(sp, block);
        }
        char *sp_json = json_serialize(sp);
        strbuf_appendf(&body, ",\"system\":%s", sp_json);
        free(sp_json);
        json_free(sp);
        free(system_prompt);
    }

    strbuf_appendf(&body, ",\"messages\":%s", messages_json);
    free(messages_json);

    if (tools_json) {
        strbuf_appendf(&body, ",\"tools\":%s", tools_json);
        if (extract) strbuf_append_cstr(&body, ",\"tool_choice\":{\"type\":\"tool\",\"name\":\"" ANTHROPIC_EXTRACTOR "\"}");
        else if (req->tool_choice) {
            switch (req->tool_choice->mode) {
                case TOOL_CHOICE_AUTO:     strbuf_append_cstr(&body, ",\"tool_choice\":{\"type\":\"auto\"}"); break;
                case TOOL_CHOICE_REQUIRED: strbuf_append_cstr(&body, ",\"tool_choice\":{\"type\":\"any\"}"); break;
                case TOOL_CHOICE_NAMED:
                    {char *name=quoted(req->tool_choice->tool_name);
                    if(!name) body.failed=true;else strbuf_appendf(&body,",\"tool_choice\":{\"type\":\"tool\",\"name\":%s}",name);free(name);}
                    break;
                case TOOL_CHOICE_NONE:
                    /* Anthropic: omit tools entirely for "none" */
                    break;
            }
        }
        free(tools_json);
    }

    if (req->temperature >= 0) append_number(&body,",\"temperature\":",req->temperature);
    if (req->top_p >= 0) append_number(&body,",\"top_p\":",req->top_p);
    if (sink) strbuf_append_cstr(&body, ",\"stream\":true");

    strbuf_append_cstr(&body, "}");

    /* Build URL */
    StrBuf url;
    strbuf_init(&url);
    strbuf_appendf(&url, "%s/v1/messages", st->base_url ? st->base_url : "https://api.anthropic.com");

    /* Headers */
    char auth_hdr[512], ver_hdr[] = "anthropic-version: 2023-06-01";
    snprintf(auth_hdr, sizeof(auth_hdr), "x-api-key: %s", st->api_key);
    const char *hdrs[] = {
        auth_hdr,
        ver_hdr,
        "content-type: application/json",
        beta.len ? beta.data : NULL,
        NULL
    };
    if (beta.failed) body.failed = true;

    HttpRequest hreq = {
        .url = url.data,
        .method = "POST",
        .body = body.data,
        .body_len = body.len,
        .headers = hdrs,
        .cancel=req->cancel, .timeout_ms = 120000
    };

    JsonValue *validated=body.data && !body.failed?json_parse(body.data,NULL):NULL;
    HttpResponse *resp=validated && !url.failed && mem_failure_count()==request_failures?send_request(&hreq,sink):NULL;
    mem_wipe(auth_hdr,sizeof(auth_hdr));
    json_free(validated);
    strbuf_free(&url);
    strbuf_free(&beta);
    char *body_str = strbuf_detach(&body);
    free(body_str);

    if (!resp) {
        transport_error(err, req, sink, "anthropic");
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        status_error(err, resp, "anthropic", st->api_key);
        http_response_free(resp);
        return NULL;
    }

    LlmResponse *result = parse_anthropic_response(resp->body);
    if (result && extract && !extracted_to_text(result)) {
        llm_response_free(result); result = NULL;
        set_error(err, LLM_ERR_NO_OBJECT, "Model did not return structured output", "anthropic", 200, false);
        http_response_free(resp);
        return NULL;
    }
    if (result) {
        result->provider = str_dup("anthropic");
        result->raw_json = str_dup(resp->body);
    } else {
        set_error(err, LLM_ERR_PROVIDER, "Failed to parse response", "anthropic", 200, false);
    }

    if(result && (!result->provider || !result->raw_json)) {llm_response_free(result);result=NULL;set_error(err,LLM_ERR_PROVIDER,"Response allocation failed",self->name,0,false);}
    http_response_free(resp);
    return result;
}

static LlmResponse *anthropic_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    return anthropic_request(self, req, err, NULL);
}

static LlmResponse *anthropic_stream(ProviderAdapter *self, const LlmRequest *req,
                                StreamCallback cb, void *userdata, LlmError *err) {
    return run_stream(self, req, cb, userdata, err, SINK_ANTHROPIC, anthropic_request);
}

static void anthropic_close(ProviderAdapter *self) {
    AnthropicState *st=self->impl;
    if(st) {free_secret(st->api_key);free(st->base_url);}
}

ProviderAdapter *anthropic_adapter_new(const char *api_key, const char *base_url) {
    if(!api_key || !*api_key || strlen(api_key)>400 || strchr(api_key,'\r') || strchr(api_key,'\n') || !base_url_allowed(base_url)) return NULL;
    ProviderAdapter *a = mem_calloc(1, sizeof(ProviderAdapter));
    if(!a) return NULL;
    a->name = str_dup("anthropic");
    AnthropicState *st = mem_calloc(1, sizeof(AnthropicState));
    if(!st) {free(a->name);free(a);return NULL;}
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = anthropic_complete;
    a->stream = anthropic_stream;
    a->close = anthropic_close;
    if(!a->name || !st->api_key || (base_url && !st->base_url)) {anthropic_close(a);free(st);free(a->name);free(a);return NULL;}
    return a;
}

/*============================================================================
 * OpenAI Adapter (Responses API)
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} OpenAIState;

static LlmResponse *openai_request(ProviderAdapter *self, const LlmRequest *req, LlmError *err, StreamSink *sink) {
    unsigned long request_failures=mem_failure_count();
    OpenAIState *st = self->impl;
    for(size_t i=0;i<req->message_count;i++) for(size_t k=0;k<req->messages[i]->part_count;k++) {
        ContentPart *part=req->messages[i]->parts[k];
        if(!part || !(part->kind==CONTENT_TEXT || part->kind==CONTENT_TOOL_CALL || part->kind==CONTENT_TOOL_RESULT || part->kind==CONTENT_THINKING)) {set_error(err,LLM_ERR_CONFIG,"Unsupported content kind for adapter","openai",0,false);return NULL;}
    }


    char *instructions = NULL;
    char *input_json = build_messages_json_openai(req->messages, req->message_count, &instructions);
    char *tools_json = req->tool_count > 0
        ? build_tools_json_openai(req->tools, req->tool_count)
        : NULL;

    StrBuf body;
    strbuf_init(&body);
    char *model_json=quoted(req->model);
    if(!model_json) body.failed=true;else strbuf_appendf(&body,"{\"model\":%s",model_json);
    free(model_json);

    if (instructions) {
        JsonValue *ins = json_new_string(instructions);
        char *ins_json = json_serialize(ins);
        strbuf_appendf(&body, ",\"instructions\":%s", ins_json);
        free(ins_json);
        json_free(ins);
        free(instructions);
    }

    strbuf_appendf(&body, ",\"input\":%s", input_json);
    free(input_json);

    if (tools_json) {
        strbuf_appendf(&body, ",\"tools\":%s", tools_json);
        free(tools_json);
    }

    if(req->tool_choice) {
        const char *choice=req->tool_choice->mode==TOOL_CHOICE_NONE?"none":req->tool_choice->mode==TOOL_CHOICE_REQUIRED?"required":"auto";
        if(req->tool_choice->mode==TOOL_CHOICE_NAMED) {char *name=quoted(req->tool_choice->tool_name);if(!name) body.failed=true;else strbuf_appendf(&body,",\"tool_choice\":{\"type\":\"function\",\"name\":%s}",name);free(name);}
        else strbuf_appendf(&body,",\"tool_choice\":\"%s\"",choice);
    }
    if(req->top_p>=0) append_number(&body,",\"top_p\":",req->top_p);
    if (req->max_tokens > 0) strbuf_appendf(&body, ",\"max_output_tokens\":%d", req->max_tokens);
    if (req->temperature >= 0) append_number(&body,",\"temperature\":",req->temperature);

    if (req->reasoning_effort != REASONING_NONE) {
        const char *effort = NULL;
        switch (req->reasoning_effort) {
            case REASONING_LOW:    effort = "low"; break;
            case REASONING_MEDIUM: effort = "medium"; break;
            case REASONING_HIGH:   effort = "high"; break;
            default: break;
        }
        if (effort)
            strbuf_appendf(&body, ",\"reasoning\":{\"effort\":\"%s\"}", effort);
    }

    /* Structured output: Responses API text.format (schema validated by the client). */
    const ResponseFormat *rf = req->response_format;
    if (rf && str_eq(rf->type, "json_schema"))
        strbuf_appendf(&body, ",\"text\":{\"format\":{\"type\":\"json_schema\",\"name\":\"output\",\"schema\":%s,\"strict\":%s}}",
                       rf->json_schema, rf->strict ? "true" : "false");
    else if (rf && str_eq(rf->type, "json"))
        strbuf_append_cstr(&body, ",\"text\":{\"format\":{\"type\":\"json_object\"}}");
    if (sink) strbuf_append_cstr(&body, ",\"stream\":true");

    strbuf_append_cstr(&body, "}");

    StrBuf url;
    strbuf_init(&url);
    strbuf_appendf(&url, "%s/v1/responses", st->base_url ? st->base_url : "https://api.openai.com");

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", st->api_key);
    const char *hdrs[] = { auth_hdr, "Content-Type: application/json", NULL };

    HttpRequest hreq = {
        .url = url.data, .method = "POST",
        .body = body.data, .body_len = body.len,
        .headers = hdrs, .cancel=req->cancel, .timeout_ms = 120000
    };

    JsonValue *validated=body.data && !body.failed?json_parse(body.data,NULL):NULL;
    HttpResponse *resp=validated && !url.failed && mem_failure_count()==request_failures?send_request(&hreq,sink):NULL;
    mem_wipe(auth_hdr,sizeof(auth_hdr));
    json_free(validated);
    strbuf_free(&url);
    char *body_cpy = strbuf_detach(&body);
    free(body_cpy);

    if (!resp) {
        transport_error(err, req, sink, "openai");
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        status_error(err, resp, "openai", st->api_key);
        http_response_free(resp);
        return NULL;
    }

    LlmResponse *result = parse_openai_response(resp->body);
    if (result) {
        result->provider = str_dup("openai");
        result->raw_json = str_dup(resp->body);
    } else {
        set_error(err, LLM_ERR_PROVIDER, "Failed to parse response", "openai", 200, false);
    }

    if(result && (!result->provider || !result->raw_json)) {llm_response_free(result);result=NULL;set_error(err,LLM_ERR_PROVIDER,"Response allocation failed",self->name,0,false);}
    http_response_free(resp);
    return result;
}

static LlmResponse *openai_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    return openai_request(self, req, err, NULL);
}

static LlmResponse *openai_stream(ProviderAdapter *self, const LlmRequest *req,
                                StreamCallback cb, void *userdata, LlmError *err) {
    return run_stream(self, req, cb, userdata, err, SINK_OPENAI, openai_request);
}

static void openai_close(ProviderAdapter *self) {
    OpenAIState *st=self->impl;
    if(st) {free_secret(st->api_key);free(st->base_url);}
}

ProviderAdapter *openai_adapter_new(const char *api_key, const char *base_url) {
    if(!api_key || !*api_key || strlen(api_key)>400 || strchr(api_key,'\r') || strchr(api_key,'\n') || !base_url_allowed(base_url)) return NULL;
    ProviderAdapter *a = mem_calloc(1, sizeof(ProviderAdapter));
    if(!a) return NULL;
    a->name = str_dup("openai");
    OpenAIState *st = mem_calloc(1, sizeof(OpenAIState));
    if(!st) {free(a->name);free(a);return NULL;}
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = openai_complete;
    a->stream = openai_stream;
    a->close = openai_close;
    if(!a->name || !st->api_key || (base_url && !st->base_url)) {openai_close(a);free(st);free(a->name);free(a);return NULL;}
    return a;
}

/*============================================================================
 * Gemini Adapter
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} GeminiState;

static LlmResponse *gemini_request(ProviderAdapter *self, const LlmRequest *req, LlmError *err, StreamSink *sink) {
    unsigned long request_failures=mem_failure_count();
    GeminiState *st = self->impl;
    if(req->reasoning_effort!=REASONING_NONE || (req->tool_choice && req->tool_choice->mode!=TOOL_CHOICE_AUTO)) {set_error(err,LLM_ERR_CONFIG,"Reasoning effort and named tool-choice control are unsupported for Gemini","gemini",0,false);return NULL;}
    for(size_t i=0;i<req->message_count;i++) for(size_t k=0;k<req->messages[i]->part_count;k++) {
        ContentPart *part=req->messages[i]->parts[k];
        if(!part || !(part->kind==CONTENT_TEXT || part->kind==CONTENT_TOOL_CALL || part->kind==CONTENT_TOOL_RESULT || (part->kind==CONTENT_THINKING && part->provider_metadata_json))) {set_error(err,LLM_ERR_CONFIG,"Unsupported content kind for adapter","gemini",0,false);return NULL;}
    }


    char *system_instruction = NULL;
    char *contents_json = build_messages_json_gemini(req->messages, req->message_count, &system_instruction);
    char *tools_json = req->tool_count > 0
        ? build_tools_json_gemini(req->tools, req->tool_count)
        : NULL;

    StrBuf body;
    strbuf_init(&body);
    strbuf_append_cstr(&body, "{\"contents\":");
    strbuf_append_cstr(&body, contents_json);
    free(contents_json);

    if (system_instruction) {
        JsonValue *si = json_new_string(system_instruction);
        char *si_json = json_serialize(si);
        strbuf_appendf(&body, ",\"systemInstruction\":{\"parts\":[{\"text\":%s}]}", si_json);
        free(si_json);
        json_free(si);
        free(system_instruction);
    }

    if (tools_json) {
        strbuf_appendf(&body, ",\"tools\":%s", tools_json);
        free(tools_json);
    }

    StrBuf gc;
    strbuf_init(&gc);
    strbuf_append_cstr(&gc, ",\"generationConfig\":{");
    bool has_gc = false;
    if (req->max_tokens > 0) { strbuf_appendf(&gc, "\"maxOutputTokens\":%d", req->max_tokens); has_gc = true; }
    if (req->temperature >= 0) { if (has_gc) strbuf_append_cstr(&gc, ","); append_number(&gc,"\"temperature\":",req->temperature); has_gc = true; }
    if(req->top_p>=0) {if(has_gc) strbuf_append_cstr(&gc,",");append_number(&gc,"\"topP\":",req->top_p);has_gc=true;}
    const ResponseFormat *rf = req->response_format;
    if (rf && (str_eq(rf->type, "json") || str_eq(rf->type, "json_schema"))) {
        strbuf_appendf(&gc, "%s\"responseMimeType\":\"application/json\"", has_gc ? "," : "");
        if (str_eq(rf->type, "json_schema")) strbuf_appendf(&gc, ",\"responseSchema\":%s", rf->json_schema);
        has_gc = true;
    }
    strbuf_append_cstr(&gc, "}");
    if (has_gc) strbuf_append_cstr(&body, gc.data);
    strbuf_free(&gc);

    strbuf_append_cstr(&body, "}");

    StrBuf url;
    strbuf_init(&url);
    char *model_path=url_component(req->model);
    if(!model_path) url.failed=true;
    else strbuf_appendf(&url,"%s/v1beta/models/%s:%s",st->base_url?st->base_url:"https://generativelanguage.googleapis.com",model_path,sink?"streamGenerateContent?alt=sse":"generateContent");
    free(model_path);

    char auth_hdr[512];snprintf(auth_hdr,sizeof(auth_hdr),"x-goog-api-key: %s",st->api_key);
    const char *hdrs[] = { "Content-Type: application/json", auth_hdr, NULL };
    HttpRequest hreq = {
        .url = url.data, .method = "POST",
        .body = body.data, .body_len = body.len,
        .headers = hdrs, .cancel=req->cancel, .timeout_ms = 120000
    };

    JsonValue *validated=body.data && !body.failed?json_parse(body.data,NULL):NULL;
    HttpResponse *resp=validated && !url.failed && mem_failure_count()==request_failures?send_request(&hreq,sink):NULL;
    mem_wipe(auth_hdr,sizeof(auth_hdr));
    json_free(validated);
    strbuf_free(&url);
    char *body_cpy = strbuf_detach(&body);
    free(body_cpy);

    if (!resp) {
        transport_error(err, req, sink, "gemini");
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        status_error(err, resp, "gemini", st->api_key);
        http_response_free(resp);
        return NULL;
    }

    LlmResponse *result = parse_gemini_response(resp->body);
    if (result) {
        result->provider = str_dup("gemini");
        result->raw_json = str_dup(resp->body);
    } else {
        set_error(err, LLM_ERR_PROVIDER, "Failed to parse response", "gemini", 200, false);
    }

    if(result && (!result->provider || !result->raw_json)) {llm_response_free(result);result=NULL;set_error(err,LLM_ERR_PROVIDER,"Response allocation failed",self->name,0,false);}
    http_response_free(resp);
    return result;
}

static LlmResponse *gemini_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    return gemini_request(self, req, err, NULL);
}

static LlmResponse *gemini_stream(ProviderAdapter *self, const LlmRequest *req,
                                StreamCallback cb, void *userdata, LlmError *err) {
    return run_stream(self, req, cb, userdata, err, SINK_GEMINI, gemini_request);
}

static void gemini_close(ProviderAdapter *self) {
    GeminiState *st=self->impl;
    if(st) {free_secret(st->api_key);free(st->base_url);}
}

ProviderAdapter *gemini_adapter_new(const char *api_key, const char *base_url) {
    if(!api_key || !*api_key || strlen(api_key)>400 || strchr(api_key,'\r') || strchr(api_key,'\n') || !base_url_allowed(base_url)) return NULL;
    ProviderAdapter *a = mem_calloc(1, sizeof(ProviderAdapter));
    if(!a) return NULL;
    a->name = str_dup("gemini");
    GeminiState *st = mem_calloc(1, sizeof(GeminiState));
    if(!st) {free(a->name);free(a);return NULL;}
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = gemini_complete;
    a->stream = gemini_stream;
    a->close = gemini_close;
    if(!a->name || !st->api_key || (base_url && !st->base_url)) {gemini_close(a);free(st);free(a->name);free(a);return NULL;}
    return a;
}

/*============================================================================
 * Message/Tool building helpers
 *==========================================================================*/

/* Build Anthropic messages JSON, extracting system messages */
static char *build_messages_json_anthropic(Message **msgs, size_t count, char **system_out, bool cache) {
    StrBuf sys;
    strbuf_init(&sys);
    JsonValue *arr = json_new_array();
    JsonValue *prev_content = NULL;
    const char *prev_role = NULL;

    for (size_t i = 0; i < count; i++) {
        Message *m = msgs[i];
        if (m->role == ROLE_SYSTEM || m->role == ROLE_DEVELOPER) {
            char *t = message_text(m);
            if (sys.len > 0) strbuf_append_cstr(&sys, "\n\n");
            strbuf_append_cstr(&sys, t);
            free(t);
            continue;
        }

        const char *role_str = "user";
        if (m->role == ROLE_ASSISTANT) role_str = "assistant";
        /* TOOL role -> user role with tool_result blocks */
        if (m->role == ROLE_TOOL) role_str = "user";

        /* Anthropic requires alternation: consecutive same-role messages
         * (e.g. parallel tool results) merge into one. */
        bool merge = prev_content && str_eq(prev_role, role_str);
        JsonValue *msg = merge ? NULL : json_new_object();
        JsonValue *content = merge ? prev_content : json_new_array();
        if (msg) json_object_set(msg, "role", json_new_string(role_str));
        for (size_t j = 0; j < m->part_count; j++) {
            ContentPart *p = m->parts[j];
            JsonValue *block = json_new_object();

            if (p->kind == CONTENT_TEXT && p->text) {
                json_object_set(block, "type", json_new_string("text"));
                json_object_set(block, "text", json_new_string(p->text));
            } else if (p->kind == CONTENT_TOOL_CALL && p->tool_call) {
                json_object_set(block, "type", json_new_string("tool_use"));
                json_object_set(block, "id", json_new_string(p->tool_call->id));
                json_object_set(block, "name", json_new_string(p->tool_call->name));
                /* Parse arguments JSON and embed as object */
                if (p->tool_call->arguments_json) {
                    const char *e = NULL;
                    JsonValue *args = json_parse(p->tool_call->arguments_json, &e);
                    json_object_set(block, "input", args);
                }
            } else if (p->kind == CONTENT_TOOL_RESULT && p->tool_result) {
                json_object_set(block, "type", json_new_string("tool_result"));
                json_object_set(block, "tool_use_id", json_new_string(p->tool_result->tool_call_id));
                json_object_set(block, "content", json_new_string(p->tool_result->content ? p->tool_result->content : ""));
                if (p->tool_result->is_error)
                    json_object_set(block, "is_error", json_new_bool(true));
            } else if ((p->kind == CONTENT_THINKING || p->kind == CONTENT_REDACTED_THINKING) && p->thinking) {
                if (p->thinking->redacted) {
                    json_object_set(block, "type", json_new_string("redacted_thinking"));
                    json_object_set(block, "data", json_new_string(p->thinking->text));
                } else {
                    json_object_set(block, "type", json_new_string("thinking"));
                    json_object_set(block, "thinking", json_new_string(p->thinking->text));
                    if (p->thinking->signature)
                        json_object_set(block, "signature", json_new_string(p->thinking->signature));
                }
            } else {
                json_free(block);
                continue;
            }
            json_array_push(content, block);
        }
        if (msg) {
            json_object_set(msg, "content", content);
            json_array_push(arr, msg);
            prev_content = content;
            prev_role = role_str;
        }
    }

    /* Breakpoint on the newest cacheable block: the next turn reuses the whole
     * conversation prefix up to here (thinking blocks cannot carry one). */
    if (cache && prev_content) {
        for (size_t i = prev_content->array.count; i > 0; i--) {
            JsonValue *block = prev_content->array.items[i - 1];
            const char *type = json_get_string(block, "type");
            if (str_eq(type, "thinking") || str_eq(type, "redacted_thinking")) continue;
            json_object_set(block, "cache_control", ephemeral_cache());
            break;
        }
    }

    *system_out = sys.len > 0 ? strbuf_detach(&sys) : NULL;
    if (sys.len == 0) strbuf_free(&sys);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build OpenAI Responses API input array */
static char *build_messages_json_openai(Message **msgs, size_t count, char **instructions_out) {
    StrBuf instr;
    strbuf_init(&instr);
    JsonValue *arr = json_new_array();

    for (size_t i = 0; i < count; i++) {
        Message *m = msgs[i];
        if (m->role == ROLE_SYSTEM || m->role == ROLE_DEVELOPER) {
            char *t = message_text(m);
            if (instr.len > 0) strbuf_append_cstr(&instr, "\n\n");
            strbuf_append_cstr(&instr, t);
            free(t);
            continue;
        }

        for (size_t j = 0; j < m->part_count; j++) {
            ContentPart *p = m->parts[j];
            JsonValue *item = json_new_object();

            if (p->kind == CONTENT_TEXT && p->text) {
                json_object_set(item, "type", json_new_string("message"));
                json_object_set(item, "role", json_new_string(m->role == ROLE_ASSISTANT ? "assistant" : "user"));
                JsonValue *content = json_new_array();
                JsonValue *text_part = json_new_object();
                json_object_set(text_part, "type", json_new_string(m->role == ROLE_ASSISTANT ? "output_text" : "input_text"));
                json_object_set(text_part, "text", json_new_string(p->text));
                json_array_push(content, text_part);
                json_object_set(item, "content", content);
            } else if (p->kind == CONTENT_TOOL_CALL && p->tool_call) {
                json_object_set(item, "type", json_new_string("function_call"));
                json_object_set(item, "call_id", json_new_string(p->tool_call->id));
                json_object_set(item, "name", json_new_string(p->tool_call->name));
                json_object_set(item, "arguments", json_new_string(p->tool_call->arguments_json ? p->tool_call->arguments_json : "{}"));
            } else if (p->kind == CONTENT_TOOL_RESULT && p->tool_result) {
                json_object_set(item, "type", json_new_string("function_call_output"));
                json_object_set(item, "call_id", json_new_string(p->tool_result->tool_call_id));
                json_object_set(item, "output", json_new_string(p->tool_result->content ? p->tool_result->content : ""));
            } else if(p->kind==CONTENT_THINKING && p->thinking && p->thinking->signature) {
                json_free(item);item=json_parse(p->thinking->signature,NULL);
            } else {json_free(item);arr->failed=true;continue;}
            json_array_push(arr, item);
        }
    }

    *instructions_out = instr.len > 0 ? strbuf_detach(&instr) : NULL;
    if (instr.len == 0) strbuf_free(&instr);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

static const char *tool_name_for_id(Message **messages,size_t count,const char *id) {
    for(size_t i=count;i>0;i--) for(size_t j=0;j<messages[i-1]->part_count;j++) {
        ContentPart *part=messages[i-1]->parts[j];
        if(part->kind==CONTENT_TOOL_CALL && part->tool_call && str_eq(part->tool_call->id,id)) return part->tool_call->name;
    }
    return NULL;
}
/* Build Gemini contents JSON */
static char *build_messages_json_gemini(Message **msgs, size_t count, char **system_instruction_out) {
    StrBuf sys;
    strbuf_init(&sys);
    JsonValue *arr = json_new_array();
    JsonValue *prev_parts = NULL;
    const char *prev_role = NULL;

    for (size_t i = 0; i < count; i++) {
        Message *m = msgs[i];
        if (m->role == ROLE_SYSTEM || m->role == ROLE_DEVELOPER) {
            char *t = message_text(m);
            if (sys.len > 0) strbuf_append_cstr(&sys, "\n\n");
            strbuf_append_cstr(&sys, t);
            free(t);
            continue;
        }

        const char *role = "user";
        if (m->role == ROLE_ASSISTANT) role = "model";

        /* Group consecutive same-role turns (parallel functionResponses). */
        bool merge = prev_parts && str_eq(prev_role, role);
        JsonValue *content = merge ? NULL : json_new_object();
        JsonValue *parts = merge ? prev_parts : json_new_array();
        if (content) json_object_set(content, "role", json_new_string(role));

        for (size_t j = 0; j < m->part_count; j++) {
            ContentPart *p = m->parts[j];
            if(p->provider_metadata_json) {
                JsonValue *original=json_parse(p->provider_metadata_json,NULL);json_array_push(parts,original);
            } else if (p->kind == CONTENT_TEXT && p->text) {
                JsonValue *part = json_new_object();
                json_object_set(part, "text", json_new_string(p->text));
                json_array_push(parts, part);
            } else if (p->kind == CONTENT_TOOL_CALL && p->tool_call) {
                JsonValue *part = json_new_object();
                JsonValue *fc = json_new_object();
                json_object_set(fc, "name", json_new_string(p->tool_call->name));
                if (p->tool_call->arguments_json) {
                    const char *e = NULL;
                    JsonValue *args = json_parse(p->tool_call->arguments_json, &e);
                    json_object_set(fc, "args", args);
                }
                json_object_set(part, "functionCall", fc);
                json_array_push(parts, part);
            } else if (p->kind == CONTENT_TOOL_RESULT && p->tool_result) {
                JsonValue *part = json_new_object();
                JsonValue *fr = json_new_object();
                json_object_set(fr, "name", json_new_string(tool_name_for_id(msgs,i,p->tool_result->tool_call_id)));
                JsonValue *resp_obj = json_new_object();
                json_object_set(resp_obj, p->tool_result->is_error?"error":"result", json_new_string(p->tool_result->content ? p->tool_result->content : ""));
                json_object_set(fr, "response", resp_obj);
                json_object_set(part, "functionResponse", fr);
                json_array_push(parts, part);
            }
        }
        if (content) {
            json_object_set(content, "parts", parts);
            json_array_push(arr, content);
            prev_parts = parts;
            prev_role = role;
        }
    }

    *system_instruction_out = sys.len > 0 ? strbuf_detach(&sys) : NULL;
    if (sys.len == 0) strbuf_free(&sys);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build Anthropic tools JSON */
static JsonValue *ephemeral_cache(void) {
    JsonValue *c = json_new_object();
    json_object_set(c, "type", json_new_string("ephemeral"));
    return c;
}

static char *build_tools_json_anthropic(ToolDefinition **tools, size_t count, bool cache) {
    JsonValue *arr = json_new_array();
    for (size_t i = 0; i < count; i++) {
        JsonValue *t = json_new_object();
        json_object_set(t, "name", json_new_string(tools[i]->name));
        json_object_set(t, "description", json_new_string(str_safe(tools[i]->description)));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(t, "input_schema", schema);
        }
        /* A breakpoint on the last tool caches the whole tool list. */
        if (cache && i + 1 == count) json_object_set(t, "cache_control", ephemeral_cache());
        json_array_push(arr, t);
    }
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build OpenAI tools JSON (Responses API format: flat, not nested under "function") */
static char *build_tools_json_openai(ToolDefinition **tools, size_t count) {
    JsonValue *arr = json_new_array();
    for (size_t i = 0; i < count; i++) {
        JsonValue *t = json_new_object();
        json_object_set(t, "type", json_new_string("function"));
        json_object_set(t, "name", json_new_string(tools[i]->name));
        json_object_set(t, "description", json_new_string(str_safe(tools[i]->description)));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(t, "parameters", schema);
        }
        json_array_push(arr, t);
    }
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build Gemini tools JSON */
static char *build_tools_json_gemini(ToolDefinition **tools, size_t count) {
    JsonValue *arr = json_new_array();
    JsonValue *decls_obj = json_new_object();
    JsonValue *decls = json_new_array();
    for (size_t i = 0; i < count; i++) {
        JsonValue *fn = json_new_object();
        json_object_set(fn, "name", json_new_string(tools[i]->name));
        json_object_set(fn, "description", json_new_string(str_safe(tools[i]->description)));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(fn, "parameters", schema);
        }
        json_array_push(decls, fn);
    }
    json_object_set(decls_obj, "functionDeclarations", decls);
    json_array_push(arr, decls_obj);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/*============================================================================
 * Response Parsers
 *==========================================================================*/

static bool add_part(LlmResponse *r,ContentPart *part) {
    if(!part || r->message->part_count>=4096) {content_part_free(part);return false;}
    size_t n=r->message->part_count;
    ContentPart **parts=mem_reallocarray(r->message->parts,n+1,sizeof(*parts));
    if(!parts) {content_part_free(part);return false;}r->message->parts=parts;parts[n]=part;r->message->part_count++;
    if(part->kind==CONTENT_TOOL_CALL) {
        ToolCallData *data=part->tool_call;if(!data || !data->id || !data->name || !data->arguments_json) return false;
        ToolCall *call=mem_calloc(1,sizeof(*call));if(!call) return false;
        call->id=str_dup(data->id);call->name=str_dup(data->name);call->arguments_json=str_dup(data->arguments_json);
        if(!call->id || !call->name || !call->arguments_json) {tool_call_free(call);return false;}
        ToolCall **calls=mem_reallocarray(r->tool_calls,r->tool_call_count+1,sizeof(*calls));
        if(!calls) {tool_call_free(call);return false;}r->tool_calls=calls;calls[r->tool_call_count++]=call;
    }return true;
}
static ContentPart *text_part(const char *text) {
    if(!text) return NULL;ContentPart *p=mem_calloc(1,sizeof(*p));if(!p) return NULL;
    p->kind=CONTENT_TEXT;p->text=str_dup(text);if(!p->text) {content_part_free(p);return NULL;}return p;
}
static ContentPart *tool_part(const char *id,const char *name,const char *arguments) {
    if(!id || !name || !arguments) return NULL;ContentPart *p=mem_calloc(1,sizeof(*p));if(!p) return NULL;
    p->kind=CONTENT_TOOL_CALL;p->tool_call=mem_calloc(1,sizeof(*p->tool_call));if(!p->tool_call) goto fail;
    p->tool_call->id=str_dup(id);p->tool_call->name=str_dup(name);p->tool_call->arguments_json=str_dup(arguments);
    if(!p->tool_call->id || !p->tool_call->name || !p->tool_call->arguments_json) goto fail;return p;
fail:content_part_free(p);return NULL;
}
static bool usage_value(const JsonValue *j,const char *key,int *out,int fallback) {
    JsonValue *v=json_get(j,key);if(!v) {*out=fallback;return true;}
    int n=json_get_int(j,key,-2);if(n<0) return false;*out=n;return true;
}
static LlmResponse *parse_response(const char *provider,const char *source) {
    unsigned long failures=mem_failure_count();JsonValue *root=json_parse(source,NULL);if(!root || root->type!=JSON_OBJECT) {json_free(root);return NULL;}
    LlmResponse *r=mem_calloc(1,sizeof(*r));if(!r) {json_free(root);return NULL;}
    r->message=message_assistant(NULL);if(!r->message) goto fail;
    r->id=str_dup(json_get_string(root,"id"));r->model=str_dup(json_get_string(root,"model"));
    JsonValue *items=NULL,*usage=NULL;const char *finish=NULL;
    if(str_eq(provider,"anthropic")) {items=json_get(root,"content");finish=json_get_string(root,"stop_reason");usage=json_get(root,"usage");}
    else if(str_eq(provider,"openai")) {items=json_get(root,"output");finish=json_get_string(root,"status");usage=json_get(root,"usage");}
    else {
        JsonValue *candidate=json_array_get(json_get(root,"candidates"),0);
        items=json_get(json_get(candidate,"content"),"parts");finish=json_get_string(candidate,"finishReason");usage=json_get(root,"usageMetadata");
    }
    if(!items || items->type!=JSON_ARRAY || items->array.count>4096 || !finish || !*finish) goto fail;
    StrBuf reasoning;strbuf_init(&reasoning);
    for(size_t i=0;i<items->array.count;i++) {
        JsonValue *item=items->array.items[i];if(item->type!=JSON_OBJECT) {strbuf_free(&reasoning);goto fail;}
        const char *type=json_get_string(item,"type");ContentPart *part=NULL;
        if(str_eq(provider,"anthropic")) {
            if(str_eq(type,"text")) part=text_part(json_get_string(item,"text"));
            else if(str_eq(type,"tool_use")) {
                char *args=json_serialize(json_get(item,"input"));part=tool_part(json_get_string(item,"id"),json_get_string(item,"name"),args);free(args);
            } else if(str_eq(type,"thinking") || str_eq(type,"redacted_thinking")) {
                part=mem_calloc(1,sizeof(*part));if(!part) {strbuf_free(&reasoning);goto fail;}
                part->kind=str_eq(type,"thinking")?CONTENT_THINKING:CONTENT_REDACTED_THINKING;
                part->thinking=mem_calloc(1,sizeof(*part->thinking));if(!part->thinking) {content_part_free(part);strbuf_free(&reasoning);goto fail;}
                part->thinking->redacted=part->kind==CONTENT_REDACTED_THINKING;
                part->thinking->text=str_dup(json_get_string(item,part->thinking->redacted?"data":"thinking"));part->thinking->signature=str_dup(json_get_string(item,"signature"));
                if(!part->thinking->text) {content_part_free(part);strbuf_free(&reasoning);goto fail;}
                if(!part->thinking->redacted) strbuf_append_cstr(&reasoning,part->thinking->text);
            }
        } else if(str_eq(provider,"openai")) {
            if(str_eq(type,"message")) {
                JsonValue *content=json_get(item,"content");if(!content || content->type!=JSON_ARRAY) {strbuf_free(&reasoning);goto fail;}
                for(size_t k=0;k<content->array.count;k++) {
                    JsonValue *v=content->array.items[k];if(!str_eq(json_get_string(v,"type"),"output_text") || !add_part(r,text_part(json_get_string(v,"text")))) {strbuf_free(&reasoning);goto fail;}
                }continue;
            } else if(str_eq(type,"function_call")) part=tool_part(json_get_string(item,"call_id"),json_get_string(item,"name"),json_get_string(item,"arguments"));
            else if(str_eq(type,"reasoning")) {
                /* Preserve opaque provider reasoning item for subsequent requests. */
                part=mem_calloc(1,sizeof(*part));if(!part) {strbuf_free(&reasoning);goto fail;}
                part->kind=CONTENT_THINKING;part->thinking=mem_calloc(1,sizeof(*part->thinking));
                if(!part->thinking) {content_part_free(part);strbuf_free(&reasoning);goto fail;}
                part->thinking->signature=json_serialize(item);part->thinking->text=str_dup("");
            }
        } else {
            const char *text=json_get_string(item,"text");JsonValue *call=json_get(item,"functionCall");
            if(text && json_get_bool(item,"thought",false)) {
                part=mem_calloc(1,sizeof(*part));if(part) {
                    part->kind=CONTENT_THINKING;part->thinking=mem_calloc(1,sizeof(*part->thinking));
                    if(part->thinking) {part->thinking->text=str_dup(text);strbuf_append_cstr(&reasoning,text);}
                    else {content_part_free(part);part=NULL;}
                }
            } else if(text) part=text_part(text);
            else if(call) {
                /* Gemini assigns no IDs; synthesize unique ones (spec 7.3). */
                uuid_t uuid;uuid_generate_random(uuid);char id[48],text_id[37];uuid_unparse_lower(uuid,text_id);snprintf(id,sizeof(id),"call_%s",text_id);
                char *args=json_serialize(json_get(call,"args"));
                part=tool_part(id,json_get_string(call,"name"),args);free(args);
            }
            if(part) part->provider_metadata_json=json_serialize(item);
        }
        if(!add_part(r,part)) {strbuf_free(&reasoning);goto fail;}
    }
    r->reasoning=reasoning.len?strbuf_detach(&reasoning):NULL;strbuf_free(&reasoning);
    r->text=message_text(r->message);if(!r->text) goto fail;
    r->finish_reason.raw=str_dup(finish);r->finish_reason.reason=FINISH_OTHER;
    if(str_eq(finish,"end_turn") || str_eq(finish,"stop_sequence") || str_eq(finish,"completed") || str_eq(finish,"STOP")) r->finish_reason.reason=r->tool_call_count?FINISH_TOOL_CALLS:FINISH_STOP;
    else if(str_eq(finish,"tool_use")) r->finish_reason.reason=FINISH_TOOL_CALLS;
    else if(str_eq(finish,"max_tokens") || str_eq(finish,"MAX_TOKENS") || str_eq(finish,"incomplete")) r->finish_reason.reason=FINISH_LENGTH;
    else if(str_eq(finish,"SAFETY") || str_eq(finish,"RECITATION")) r->finish_reason.reason=FINISH_CONTENT_FILTER;
    else if(str_eq(finish,"failed") || str_eq(finish,"cancelled")) goto fail;
    if(!usage_value(usage,str_eq(provider,"gemini")?"promptTokenCount":"input_tokens",&r->usage.input_tokens,0) ||
       !usage_value(usage,str_eq(provider,"gemini")?"candidatesTokenCount":"output_tokens",&r->usage.output_tokens,0)) goto fail;
    r->usage.total_tokens=usage_add((Usage){.total_tokens=r->usage.input_tokens},(Usage){.total_tokens=r->usage.output_tokens}).total_tokens;
    r->usage.reasoning_tokens=r->usage.cache_read_tokens=r->usage.cache_write_tokens=-1;
    if(str_eq(provider,"anthropic")) {
        if(!usage_value(usage,"cache_read_input_tokens",&r->usage.cache_read_tokens,-1) || !usage_value(usage,"cache_creation_input_tokens",&r->usage.cache_write_tokens,-1)) goto fail;
    } else if(str_eq(provider,"openai")) {
        if(!usage_value(json_get(usage,"input_tokens_details"),"cached_tokens",&r->usage.cache_read_tokens,-1) || !usage_value(json_get(usage,"output_tokens_details"),"reasoning_tokens",&r->usage.reasoning_tokens,-1)) goto fail;
    } else if(!usage_value(usage,"thoughtsTokenCount",&r->usage.reasoning_tokens,-1) || !usage_value(usage,"cachedContentTokenCount",&r->usage.cache_read_tokens,-1)) goto fail;
    if(mem_failure_count()!=failures) goto fail;json_free(root);return r;
fail:json_free(root);llm_response_free(r);return NULL;
}
static LlmResponse *parse_anthropic_response(const char *source) {return parse_response("anthropic",source);}
static LlmResponse *parse_openai_response(const char *source) {return parse_response("openai",source);}
static LlmResponse *parse_gemini_response(const char *source) {return parse_response("gemini",source);}
