#include "llm/client.h"
#include "util/str.h"
#include "util/http.h"
#include "util/mem.h"
#include "util/io.h"
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Model Catalog
 *==========================================================================*/

static const ModelInfo MODEL_CATALOG[] = {
    { "claude-opus-4-6",        "anthropic", "Claude Opus 4.6",          200000, true, false, false },
    { "claude-sonnet-4-5",      "anthropic", "Claude Sonnet 4.5",        200000, true, false, false },
    { "claude-haiku-4-5",       "anthropic", "Claude Haiku 4.5",         200000, true, false, false },
    { "gpt-5.2",                "openai",    "GPT-5.2",                 1047576, true, false, true },
    { "gpt-5.2-mini",           "openai",    "GPT-5.2 Mini",            1047576, true, false, true },
    { "gpt-5.2-codex",          "openai",    "GPT-5.2 Codex",           1047576, true, false, true },
    { "gemini-3-pro-preview",   "gemini",    "Gemini 3 Pro (Preview)",  1048576, true, false, false },
    { "gemini-3-flash-preview", "gemini",    "Gemini 3 Flash (Preview)",1048576, true, false, false },
};

static const size_t MODEL_CATALOG_SIZE = sizeof(MODEL_CATALOG) / sizeof(MODEL_CATALOG[0]);

const ModelInfo *llm_get_model_info(const char *model_id) {
    if (!model_id) return NULL;
    for (size_t i = 0; i < MODEL_CATALOG_SIZE; i++) {
        if (strcmp(MODEL_CATALOG[i].id, model_id) == 0)
            return &MODEL_CATALOG[i];
    }
    return NULL;
}

const ModelInfo *llm_list_models(size_t *count) {
    *count = MODEL_CATALOG_SIZE;
    return MODEL_CATALOG;
}

/*============================================================================
 * Client
 *==========================================================================*/

LlmClient *llm_client_new(void) {
    LlmClient *c = mem_calloc(1, sizeof(LlmClient));
    if (c) c->retry_policy = (RetryPolicy){.base_delay=1.0,.max_delay=60.0,.backoff_multiplier=2.0,.jitter=true};
    return c;
}

void llm_client_add_provider(LlmClient *c,ProviderAdapter *adapter) {
    if(!c) return;
    if(!adapter) {c->failed=true;return;}
    ProviderAdapter **items=mem_reallocarray(c->providers,c->provider_count+1,sizeof(*items));
    if(!items) {c->failed=true;if(adapter->close) adapter->close(adapter);free(adapter->name);free(adapter->impl);free(adapter);return;}
    c->providers=items;items[c->provider_count++]=adapter;
    if(!c->default_provider) {c->default_provider=str_dup(adapter->name);if(!c->default_provider) c->failed=true;}
}
void llm_client_set_default(LlmClient *c,const char *name) {
    if(!c) return;
    char *copy=str_dup(name);if(name && !copy) {c->failed=true;return;}free(c->default_provider);c->default_provider=copy;
}
void llm_client_add_middleware(LlmClient *c,MiddlewareFn mw) {
    if(!c) return;
    if(c->middleware_count>=128 || !mw) {c->failed=true;return;}
    MiddlewareFn *items=mem_reallocarray(c->middleware,c->middleware_count+1,sizeof(*items));if(!items) {c->failed=true;return;}
    c->middleware=items;items[c->middleware_count++]=mw;
}

void llm_client_free(LlmClient *c) {
    if (!c) return;
    for (size_t i = 0; i < c->provider_count; i++) {
        ProviderAdapter *a = c->providers[i];
        if (a->close) a->close(a);
        free(a->name);
        /* Free adapter-specific state */
        free(a->impl);
        free(a);
    }
    free(c->providers);
    free(c->default_provider);
    free(c->middleware);
    free(c);
}

static ProviderAdapter *find_provider(LlmClient *c, const char *name) {
    const char *target = name ? name : c->default_provider;
    if (!target) return NULL;
    for (size_t i = 0; i < c->provider_count; i++) {
        if (strcmp(c->providers[i]->name, target) == 0)
            return c->providers[i];
    }
    return NULL;
}

static void client_error(LlmError *err,LlmErrorCode code,const char *message) {
    if(err) {llm_error_free(err);err->code=code;err->message=str_dup(message);err->retry_after=-1;}
}
typedef struct {LlmClient *client;ProviderAdapter *adapter;size_t index;LlmError *err;} MiddlewareContext;
static LlmResponse *middleware_next(const LlmRequest *req,void *data) {
    MiddlewareContext *ctx=data;
    if(ctx->index==ctx->client->middleware_count) return ctx->adapter->complete(ctx->adapter,req,ctx->err);
    MiddlewareFn middleware=ctx->client->middleware[ctx->index];MiddlewareContext child=*ctx;child.index++;
    return middleware(req,middleware_next,&child);
}
static bool bounded_text(const char *text) {return text && strnlen(text,ATTRACTOR_INPUT_LIMIT+1)<=ATTRACTOR_INPUT_LIMIT;}
static bool message_valid(const Message *m) {
    if(!m || m->role<ROLE_SYSTEM || m->role>ROLE_DEVELOPER || m->part_count>4096 || (m->part_count && !m->parts)) return false;
    size_t bytes=0;
    for(size_t i=0;i<m->part_count;i++) {
        ContentPart *p=m->parts[i];if(!p || p->kind<CONTENT_TEXT || p->kind>CONTENT_REDACTED_THINKING) return false;
        const char *text=NULL;
        switch(p->kind) {
            case CONTENT_TEXT:text=p->text;break;
            case CONTENT_TOOL_CALL:
                if(!p->tool_call || !bounded_text(p->tool_call->id) || !bounded_text(p->tool_call->name)) return false;
                text=p->tool_call->arguments_json;break;
            case CONTENT_TOOL_RESULT:if(!p->tool_result || !bounded_text(p->tool_result->tool_call_id)) return false;text=p->tool_result->content;break;
            case CONTENT_THINKING:case CONTENT_REDACTED_THINKING:if(!p->thinking) return false;text=p->thinking->text;break;
            case CONTENT_IMAGE:if(!p->image || (!p->image->url && !p->image->data_base64)) return false;text=p->image->url?p->image->url:p->image->data_base64;break;
            default:return false; /* Audio/document have no supported representation. */
        }
        if(!bounded_text(text) || !size_add(bytes,strlen(text),&bytes) || bytes>ATTRACTOR_INPUT_LIMIT) return false;
        if(p->provider_metadata_json && !bounded_text(p->provider_metadata_json)) return false;
        if(p->thinking && p->thinking->signature && !bounded_text(p->thinking->signature)) return false;
    }return true;
}
static bool messages_bounded(Message *const *messages,size_t count) {
    size_t bytes=0;
    for(size_t i=0;i<count;i++) {
        const Message *m=messages[i];if(!message_valid(m)) return false;
        for(size_t j=0;j<m->part_count;j++) {
            ContentPart *p=m->parts[j];const char *strings[]={p->text,p->provider_metadata_json,
                p->tool_call?p->tool_call->id:NULL,p->tool_call?p->tool_call->name:NULL,p->tool_call?p->tool_call->arguments_json:NULL,
                p->tool_result?p->tool_result->content:NULL,p->tool_result?p->tool_result->tool_call_id:NULL,
                p->thinking?p->thinking->text:NULL,p->thinking?p->thinking->signature:NULL,
                p->image?p->image->url:NULL,p->image?p->image->data_base64:NULL};
            for(size_t k=0;k<sizeof(strings)/sizeof(*strings);k++) if(strings[k] && (!bounded_text(strings[k]) || !size_add(bytes,strlen(strings[k]),&bytes) || bytes>ATTRACTOR_INPUT_LIMIT)) return false;
        }
    }return true;
}
static bool response_valid(const LlmResponse *r) {
    if(!r || !message_valid(r->message) || r->message->role!=ROLE_ASSISTANT || r->message->part_count>4096 || r->tool_call_count>1024) return false;
    if(r->message->part_count && !r->message->parts) return false;
    for(size_t i=0;i<r->message->part_count;i++) if(!r->message->parts[i]) return false;
    if(r->tool_call_count && !r->tool_calls) return false;
    for(size_t i=0;i<r->tool_call_count;i++) {
        ToolCall *t=r->tool_calls[i];if(!t || !t->id || !*t->id || !t->name || !*t->name || !t->arguments_json) return false;
        JsonValue *args=json_parse(t->arguments_json,NULL);bool valid=args && args->type==JSON_OBJECT;json_free(args);if(!valid) return false;
    }
    return r->finish_reason.reason>=FINISH_STOP && r->finish_reason.reason<=FINISH_OTHER;
}
/* Validates a request and returns its adapter, or NULL with err set. */
static ProviderAdapter *checked_adapter(LlmClient *c,const LlmRequest *req,LlmError *err) {
    if(!c || c->failed || !req || !bounded_text(req->model) || !*req->model || !isfinite(req->temperature) || !isfinite(req->top_p) || req->max_tokens<0 || req->stop_count>1024 || req->message_count>4096 || req->tool_count>1024 || (req->message_count && !req->messages) || (req->tool_count && !req->tools)) {
        client_error(err,LLM_ERR_CONFIG,"Invalid request or client allocation failure");return NULL;
    }
    if((c->cancel && *c->cancel) || (req->cancel && *req->cancel)) {client_error(err,LLM_ERR_ABORT,"Cancelled");return NULL;}
    if(req->reasoning_effort<REASONING_NONE || req->reasoning_effort>REASONING_HIGH ||
       (req->tool_choice && (req->tool_choice->mode<TOOL_CHOICE_AUTO || req->tool_choice->mode>TOOL_CHOICE_NAMED ||
        (req->tool_choice->mode==TOOL_CHOICE_NAMED && (!bounded_text(req->tool_choice->tool_name) || !*req->tool_choice->tool_name))))) {
        client_error(err,LLM_ERR_CONFIG,"Invalid reasoning effort or tool choice");return NULL;
    }
    if(!messages_bounded(req->messages,req->message_count)) {client_error(err,LLM_ERR_CONFIG,"Invalid message or input budget exhausted");return NULL;}
    for(size_t i=0;i<req->tool_count;i++) {
        ToolDefinition *t=req->tools[i];if(!t || !bounded_text(t->name) || !*t->name || !bounded_text(t->parameters_json) || (t->description && !bounded_text(t->description))) {client_error(err,LLM_ERR_CONFIG,"Invalid tool definition");return NULL;}
        JsonValue *schema=json_parse(t->parameters_json,NULL);bool valid=schema && schema->type==JSON_OBJECT;json_free(schema);
        if(!valid) {client_error(err,LLM_ERR_CONFIG,"Invalid tool schema");return NULL;}
    }
    if(req->stop_count) {client_error(err,LLM_ERR_CONFIG,"Stop sequences are unsupported");return NULL;}
    if(req->response_format) {
        const ResponseFormat *rf=req->response_format;bool valid=str_eq(rf->type,"text") || str_eq(rf->type,"json");
        if(str_eq(rf->type,"json_schema") && bounded_text(rf->json_schema)) {JsonValue *schema=json_parse(rf->json_schema,NULL);valid=schema && schema->type==JSON_OBJECT;json_free(schema);}
        if(!valid) {client_error(err,LLM_ERR_CONFIG,"Invalid response format or schema");return NULL;}
    }
    if(req->provider_options_json) {
        JsonValue *options=bounded_text(req->provider_options_json)?json_parse(req->provider_options_json,NULL):NULL;bool valid=options && options->type==JSON_OBJECT;json_free(options);
        if(!valid) {client_error(err,LLM_ERR_CONFIG,"provider_options must be a JSON object");return NULL;}
    }
    ProviderAdapter *adapter=find_provider(c,req->provider);
    if(!adapter) client_error(err,LLM_ERR_CONFIG,"No provider found");
    return adapter;
}
/* Checks a provider response and fills derived text; frees and returns NULL on failure. */
static LlmResponse *finished_response(LlmResponse *r,LlmError *err) {
    if(r && !response_valid(r)) {llm_response_free(r);client_error(err,LLM_ERR_PROVIDER,"Malformed provider response");return NULL;}
    if(r && !r->text) {
        r->text=message_text(r->message);
        if(!r->text) {llm_response_free(r);client_error(err,LLM_ERR_PROVIDER,"Response allocation failure");return NULL;}
    }
    if(!r && err && err->code==LLM_OK) client_error(err,LLM_ERR_PROVIDER,"Provider returned no response");
    return r;
}
LlmResponse *llm_client_complete(LlmClient *c,const LlmRequest *req,LlmError *err) {
    ProviderAdapter *adapter=checked_adapter(c,req,err);if(!adapter) return NULL;
    if(!adapter->complete) {client_error(err,LLM_ERR_CONFIG,"Provider cannot complete");return NULL;}
    LlmRequest effective=*req;if(!effective.cancel) effective.cancel=c->cancel;
    MiddlewareContext ctx={c,adapter,0,err};
    return finished_response(middleware_next(&effective,&ctx),err);
}
LlmResponse *llm_client_stream(LlmClient *c,const LlmRequest *req,StreamCallback cb,void *userdata,LlmError *err) {
    if(!cb) {client_error(err,LLM_ERR_CONFIG,"Stream callback required");return NULL;}
    ProviderAdapter *adapter=checked_adapter(c,req,err);if(!adapter) return NULL;
    if(!adapter->stream) {client_error(err,LLM_ERR_STREAM,"Provider cannot stream");return NULL;}
    LlmRequest effective=*req;if(!effective.cancel) effective.cancel=c->cancel;
    return finished_response(adapter->stream(adapter,&effective,cb,userdata,err),err);
}

LlmClient *llm_client_from_env(void) {
    LlmClient *c = llm_client_new();
    if(!c) return NULL;

    const char *anthropic_key = getenv("ANTHROPIC_API_KEY");
    if (anthropic_key) {
        ProviderAdapter *a = anthropic_adapter_new(anthropic_key, getenv("ANTHROPIC_BASE_URL"));
        llm_client_add_provider(c, a);
    }

    const char *openai_key = getenv("OPENAI_API_KEY");
    if (openai_key) {
        ProviderAdapter *a = openai_adapter_new(openai_key, getenv("OPENAI_BASE_URL"));
        llm_client_add_provider(c, a);
    }

    const char *gemini_key = getenv("GEMINI_API_KEY");
    if (!gemini_key) gemini_key = getenv("GOOGLE_API_KEY");
    if (gemini_key) {
        ProviderAdapter *a = gemini_adapter_new(gemini_key, getenv("GEMINI_BASE_URL"));
        llm_client_add_provider(c, a);
    }

    if(c->failed) {llm_client_free(c);return NULL;}
    return c;
}

/*============================================================================
 * High-Level generate()
 *==========================================================================*/

static void steps_free(StepResult *steps,size_t count) {
    for(size_t i=0;i<count;i++) {
        llm_response_free(steps[i].response);
        for(size_t j=0;j<steps[i].tool_result_count;j++) tool_result_free(steps[i].tool_results[j]);
        free(steps[i].tool_results);
    }
    free(steps);
}
void generate_result_free(GenerateResult *r) {
    if(!r) return;free(r->text);free(r->reasoning);steps_free(r->steps,r->step_count);json_free(r->output);free(r);
}

/* ── Structured output ───────────────────────────────────────────────── */

static bool json_scalar_equal(const JsonValue *a,const JsonValue *b) {
    if(a->type!=b->type) return false;
    switch(a->type) {
        case JSON_STRING: return str_eq(a->string,b->string);
        case JSON_NUMBER: return a->number==b->number;
        case JSON_BOOL: return a->boolean==b->boolean;
        case JSON_NULL: return true;
        default: {char *x=json_serialize(a),*y=json_serialize(b);bool same=x && y && str_eq(x,y);free(x);free(y);return same;}
    }
}
static bool type_matches(const char *type,const JsonValue *v) {
    if(str_eq(type,"object")) return v->type==JSON_OBJECT;
    if(str_eq(type,"array")) return v->type==JSON_ARRAY;
    if(str_eq(type,"string")) return v->type==JSON_STRING;
    if(str_eq(type,"number")) return v->type==JSON_NUMBER;
    if(str_eq(type,"integer")) return v->type==JSON_NUMBER && isfinite(v->number) && trunc(v->number)==v->number;
    if(str_eq(type,"boolean")) return v->type==JSON_BOOL;
    if(str_eq(type,"null")) return v->type==JSON_NULL;
    return true; /* unknown keywords are not enforced */
}
/* Validates the JSON Schema subset providers enforce for structured output.
 * Each value node is visited once; property lookups use the object index. */
static bool schema_accepts(const JsonValue *schema,const JsonValue *v,int depth) {
    if(depth>64) return false;
    if(!schema || schema->type!=JSON_OBJECT) return true;
    const JsonValue *type=json_get(schema,"type"),*choices=json_get(schema,"enum");
    if(type && type->type==JSON_STRING && !type_matches(type->string,v)) return false;
    if(type && type->type==JSON_ARRAY) {
        bool any=false;for(size_t i=0;i<type->array.count && !any;i++) any=type->array.items[i]->type==JSON_STRING && type_matches(type->array.items[i]->string,v);
        if(!any) return false;
    }
    if(choices && choices->type==JSON_ARRAY) {
        bool any=false;for(size_t i=0;i<choices->array.count && !any;i++) any=json_scalar_equal(choices->array.items[i],v);
        if(!any) return false;
    }
    if(v->type==JSON_OBJECT) {
        const JsonValue *props=json_get(schema,"properties"),*required=json_get(schema,"required"),*extra=json_get(schema,"additionalProperties");
        for(size_t i=0;required && required->type==JSON_ARRAY && i<required->array.count;i++) {
            const JsonValue *key=required->array.items[i];if(key->type==JSON_STRING && !json_get(v,key->string)) return false;
        }
        for(size_t i=0;i<v->object.count;i++) {
            const JsonValue *sub=json_get(props,v->object.keys[i]);
            if(!sub && extra && extra->type==JSON_BOOL && !extra->boolean) return false;
            if(!sub && extra && extra->type==JSON_OBJECT) sub=extra;
            if(sub && !schema_accepts(sub,v->object.values[i],depth+1)) return false;
        }
    }
    if(v->type==JSON_ARRAY) {
        const JsonValue *items=json_get(schema,"items");
        for(size_t i=0;items && i<v->array.count;i++) if(!schema_accepts(items,v->array.items[i],depth+1)) return false;
    }
    return true;
}
/* Parses model text as JSON, tolerating surrounding whitespace and a ``` fence. */
static JsonValue *parse_model_json(const char *text) {
    if(!text) return NULL;
    while(isspace((unsigned char)*text)) text++;
    size_t n=strlen(text);while(n && isspace((unsigned char)text[n-1])) n--;
    if(n>=6 && !strncmp(text,"```",3) && !strncmp(text+n-3,"```",3)) {
        const char *body=memchr(text,'\n',n);if(!body) return NULL;body++;
        n=(size_t)(text+n-3-body);text=body;
    }
    char *copy=str_ndup(text,n);JsonValue *v=copy?json_parse(copy,NULL):NULL;free(copy);return v;
}
GenerateResult *llm_generate_object(LlmClient *client,const char *model,const char *prompt,const char *system_prompt,const char *schema_json,const char *provider,int max_retries,LlmError *err) {
    JsonValue *schema=schema_json && bounded_text(schema_json)?json_parse(schema_json,NULL):NULL;
    if(!client || !bounded_text(model) || !bounded_text(prompt) || (system_prompt && !bounded_text(system_prompt)) || !schema || schema->type!=JSON_OBJECT) {
        json_free(schema);client_error(err,LLM_ERR_CONFIG,"Invalid generate_object arguments or schema");return NULL;
    }
    Message *messages[2]={0};size_t count=0;
    if(system_prompt) messages[count++]=message_system(system_prompt);
    messages[count++]=message_user(prompt);
    GenerateResult *result=NULL;LlmResponse *response=NULL;
    if(!messages[0] || !messages[count-1]) {client_error(err,LLM_ERR_CONFIG,"Allocation failed");goto done;}
    ResponseFormat format={.type="json_schema",.json_schema=(char *)schema_json,.strict=true};
    LlmRequest req={.model=(char *)model,.provider=(char *)provider,.messages=messages,.message_count=count,.temperature=-1,.top_p=-1,.response_format=&format};
    response=llm_client_complete_retry(client,&req,max_retries,err);
    if(!response) goto done;
    JsonValue *output=parse_model_json(response->text);
    if(!output || !schema_accepts(schema,output,0)) {json_free(output);client_error(err,LLM_ERR_NO_OBJECT,output?"Structured output does not match the schema":"Structured output is not valid JSON");goto done;}
    result=mem_calloc(1,sizeof(*result));
    if(result) {result->text=str_dup(response->text);result->reasoning=response->reasoning?str_dup(response->reasoning):NULL;}
    if(!result || !result->text) {json_free(output);generate_result_free(result);result=NULL;client_error(err,LLM_ERR_CONFIG,"Allocation failed");goto done;}
    result->steps=mem_calloc(1,sizeof(*result->steps));
    if(!result->steps) {json_free(output);generate_result_free(result);result=NULL;client_error(err,LLM_ERR_CONFIG,"Allocation failed");goto done;}
    result->output=output;result->steps[0].response=result->response=response;result->step_count=1;response=NULL;
    result->finish_reason=result->response->finish_reason;result->usage=result->total_usage=result->response->usage;
done:
    for(size_t i=0;i<count;i++) message_free(messages[i]);
    llm_response_free(response);json_free(schema);return result;
}
static bool append_message(Message ***messages,size_t *count,size_t *capacity,Message *message) {
    if(!message || *count>=4096) {message_free(message);return false;}
    if(*count>=*capacity) {
        size_t cap=*capacity?*capacity*2:16;Message **items=mem_reallocarray(*messages,cap,sizeof(*items));
        if(!items) {message_free(message);return false;}*messages=items;*capacity=cap;
    }
    (*messages)[(*count)++]=message;return true;
}
/* Backoff for attempt n, or a negative value when Retry-After exceeds max_delay. */
static double retry_backoff(const RetryPolicy *p,int attempt,double hint) {
    if(isfinite(hint) && hint>=0) return hint<=p->max_delay?hint:-1;
    double delay=p->base_delay*pow(p->backoff_multiplier,attempt);
    if(!isfinite(delay) || delay>p->max_delay) delay=p->max_delay;
    if(p->jitter) delay*=0.5+(double)arc4random_uniform(1000001)/1000000.0;
    return delay;
}
static bool retry_sleep(LlmClient *c,const bool *cancel,double seconds) {
    for(double left=seconds;left>0;left-=0.01) {
        if((c->cancel && *c->cancel) || (cancel && *cancel)) return false;
        double step=left<0.01?left:0.01;struct timespec pause={0,(long)(step*1e9)};nanosleep(&pause,NULL);
    }
    return !((c->cancel && *c->cancel) || (cancel && *cancel));
}
LlmResponse *llm_client_complete_retry(LlmClient *c,const LlmRequest *req,int max_retries,LlmError *err) {
    if(!c || max_retries<0 || max_retries>100) {client_error(err,LLM_ERR_CONFIG,"Invalid retry budget");return NULL;}
    for(int attempt=0;;attempt++) {
        LlmError local={0};LlmResponse *r=llm_client_complete(c,req,&local);
        if(r) {llm_error_free(&local);return r;}
        double delay=local.retryable && attempt<max_retries?retry_backoff(&c->retry_policy,attempt,local.retry_after):-1;
        if(delay<0) {if(err) {llm_error_free(err);*err=local;}else llm_error_free(&local);return NULL;}
        if(c->retry_policy.on_retry) c->retry_policy.on_retry(&local,attempt,delay,c->retry_policy.on_retry_userdata);
        llm_error_free(&local);
        if(!retry_sleep(c,req?req->cancel:NULL,delay)) {client_error(err,LLM_ERR_ABORT,"Cancelled");return NULL;}
    }
}
GenerateResult *llm_generate(LlmClient *client,const char *model,const char *prompt,Message **messages,size_t message_count,const char *system_prompt,ActiveTool *tools,size_t tool_count,int max_tool_rounds,ReasoningEffort reasoning_effort,const char *provider,int max_retries,LlmError *err) {
    /* Either prompt or messages, never both (spec 4.3). */
    if(!client || !bounded_text(model) || (prompt && (messages || message_count)) || (prompt && !bounded_text(prompt)) || (system_prompt && !bounded_text(system_prompt)) || (!prompt && ((message_count && !messages) || message_count>4096 || !messages_bounded(messages,message_count))) || max_retries<0 || max_tool_rounds<0 || max_retries>100 || max_tool_rounds>1000 || message_count>4096 || tool_count>1024 || (tool_count && !tools) || (!prompt && message_count && !messages)) {
        client_error(err,LLM_ERR_CONFIG,"Invalid generation arguments/limits");return NULL;
    }
    Message **msgs=NULL;size_t count=0,capacity=0,step_count=0;ToolDefinition **defs=NULL;LlmResponse *response=NULL;GenerateResult *result=NULL;
    StepResult *steps=mem_calloc((size_t)max_tool_rounds+1,sizeof(*steps));if(!steps) goto allocation;
    if(system_prompt && !append_message(&msgs,&count,&capacity,message_system(system_prompt))) goto allocation;
    if(prompt) {if(!append_message(&msgs,&count,&capacity,message_user(prompt))) goto allocation;}
    else for(size_t i=0;i<message_count;i++) if(!append_message(&msgs,&count,&capacity,message_clone(messages[i]))) goto allocation;
    defs=mem_calloc(tool_count,sizeof(*defs));if(!defs) goto allocation;
    for(size_t i=0;i<tool_count;i++) defs[i]=&tools[i].def;
    Usage total={0,0,0,-1,-1,-1};
    for(int round=0;round<=max_tool_rounds;round++) {
        if(client->cancel && *client->cancel) {client_error(err,LLM_ERR_ABORT,"Cancelled");goto cleanup;}
        LlmRequest req={.model=(char *)model,.messages=msgs,.message_count=count,.provider=(char *)provider,.tools=defs,.tool_count=tool_count,.temperature=-1,.top_p=-1,.reasoning_effort=reasoning_effort};
        LlmResponse *next=llm_client_complete_retry(client,&req,max_retries,err);
        if(!next) goto cleanup;
        StepResult *step=&steps[step_count++];step->response=response=next;total=usage_add(total,response->usage);
        if(!response->tool_call_count) break;
        if(round==max_tool_rounds) break; /* budget exhausted: tool calls go back to the caller */
        bool passive=false; /* a defined tool without execute: the caller runs the loop */
        for(size_t i=0;i<response->tool_call_count && !passive;i++)
            for(size_t j=0;j<tool_count;j++) if(str_eq(tools[j].def.name,response->tool_calls[i]->name)) {passive=!tools[j].execute;break;}
        if(passive) break;
        if(!append_message(&msgs,&count,&capacity,message_clone(response->message))) goto allocation;
        step->tool_results=mem_calloc(response->tool_call_count,sizeof(*step->tool_results));if(!step->tool_results) goto allocation;
        for(size_t i=0;i<response->tool_call_count;i++) {
            ToolCall *call=response->tool_calls[i];ActiveTool *active=NULL;
            for(size_t j=0;j<tool_count;j++) if(str_eq(tools[j].def.name,call->name)) {active=&tools[j];break;}
            bool is_error=false;char *output=active && active->execute?active->execute(call->arguments_json,active->userdata,&is_error):NULL;
            if(output && strnlen(output,ATTRACTOR_OUTPUT_LIMIT+1)>ATTRACTOR_OUTPUT_LIMIT) {free(output);output=str_dup("Tool output exceeded limit");is_error=true;}
            if(!output) {output=str_dup("Unknown tool or execution failure");is_error=true;}
            ToolResult *recorded=mem_calloc(1,sizeof(*recorded));
            if(recorded) {step->tool_results[step->tool_result_count++]=recorded;recorded->tool_call_id=str_dup(call->id);recorded->content=str_dup(output);recorded->is_error=is_error;}
            Message *message=output?message_tool_result(call->id,output,is_error):NULL;free(output);
            if(!recorded || !recorded->tool_call_id || !recorded->content) {message_free(message);goto allocation;}
            if(!append_message(&msgs,&count,&capacity,message)) goto allocation;
        }
    }
    if(!response) {client_error(err,LLM_ERR_PROVIDER,"Missing response");goto cleanup;}
    result=mem_calloc(1,sizeof(*result));if(!result) goto allocation;
    result->text=str_dup(response->text);result->reasoning=str_dup(response->reasoning);
    if((response->text && !result->text) || (response->reasoning && !result->reasoning)) {generate_result_free(result);result=NULL;goto allocation;}
    result->response=response;result->steps=steps;result->step_count=step_count;steps=NULL;step_count=0;result->finish_reason=result->response->finish_reason;
    result->usage=result->response->usage;result->total_usage=total;result->tool_calls=result->response->tool_calls;result->tool_call_count=result->response->tool_call_count;
    goto cleanup;
allocation:client_error(err,LLM_ERR_CONFIG,"Allocation or conversation limit exceeded");
cleanup:
    for(size_t i=0;i<count;i++) message_free(msgs[i]);free(msgs);free(defs);steps_free(steps,step_count);return result;
}
