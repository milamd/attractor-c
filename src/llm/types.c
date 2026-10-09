#include "llm/types.h"
#include "util/str.h"
#include "util/mem.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*--- Free helpers ---*/

void content_part_free(ContentPart *p) {
    if (!p) return;
    free(p->provider_metadata_json);
    free(p->text);
    if (p->image) {
        free(p->image->url);
        free(p->image->data_base64);
        free(p->image->media_type);
        free(p->image->detail);
        free(p->image);
    }
    if (p->tool_call) {
        free(p->tool_call->id);
        free(p->tool_call->name);
        free(p->tool_call->arguments_json);
        free(p->tool_call);
    }
    if (p->tool_result) {
        free(p->tool_result->tool_call_id);
        free(p->tool_result->content);
        free(p->tool_result);
    }
    if (p->thinking) {
        free(p->thinking->text);
        free(p->thinking->signature);
        free(p->thinking);
    }
    free(p);
}

void message_free(Message *m) {
    if (!m) return;
    for (size_t i = 0; i < m->part_count; i++)
        content_part_free(m->parts[i]);
    free(m->parts);
    free(m->name);
    free(m->tool_call_id);
    free(m);
}

void tool_call_free(ToolCall *tc) {
    if (!tc) return;
    free(tc->id);
    free(tc->name);
    free(tc->arguments_json);
    free(tc->raw_arguments);
    free(tc);
}

void tool_result_free(ToolResult *tr) {
    if (!tr) return;
    free(tr->tool_call_id);
    free(tr->content);
    free(tr);
}

void tool_definition_free(ToolDefinition *td) {
    if (!td) return;
    free(td->name);
    free(td->description);
    free(td->parameters_json);
}

void llm_request_free(LlmRequest *r) {
    if (!r) return;
    free(r->model);
    for (size_t i = 0; i < r->message_count; i++)
        message_free(r->messages[i]);
    free(r->messages);
    free(r->provider);
    for (size_t i = 0; i < r->tool_count; i++) {
        tool_definition_free(r->tools[i]);
        free(r->tools[i]);
    }
    free(r->tools);
    if (r->tool_choice) { free(r->tool_choice->tool_name); free(r->tool_choice); }
    if (r->response_format) { free(r->response_format->type); free(r->response_format->json_schema); free(r->response_format); }
    for (size_t i = 0; i < r->stop_count; i++) free(r->stop_sequences[i]);
    free(r->stop_sequences);
    free(r->provider_options_json);
    free(r);
}

void llm_response_free(LlmResponse *r) {
    if (!r) return;
    free(r->id);
    free(r->model);
    free(r->provider);
    message_free(r->message);
    free(r->finish_reason.raw);
    free(r->raw_json);
    free(r->text);
    free(r->reasoning);
    for (size_t i = 0; i < r->tool_call_count; i++)
        tool_call_free(r->tool_calls[i]);
    free(r->tool_calls);
    free(r);
}

void llm_error_free(LlmError *e) {
    if (!e) return;
    free(e->message);
    free(e->provider);
    free(e->raw_json);
    memset(e,0,sizeof(*e));
}

void stream_event_free(StreamEvent *ev) {
    if (!ev) return;
    free(ev->delta);
    free(ev->reasoning_delta);
    if (ev->tool_call) tool_call_free(ev->tool_call);
    if (ev->finish_reason) { free(ev->finish_reason->raw); free(ev->finish_reason); }
    if (ev->usage) free(ev->usage);
    if (ev->response) llm_response_free(ev->response);
    free(ev->error_message);
    free(ev->raw_json);
}

static int token_sum(int a,int b) {
    if(a<0) a=0;if(b<0) b=0;return b>INT_MAX-a?INT_MAX:a+b;
}
Usage usage_add(Usage a, Usage b) {
    Usage r;
    r.input_tokens = token_sum(a.input_tokens,b.input_tokens);
    r.output_tokens = token_sum(a.output_tokens,b.output_tokens);
    r.total_tokens = token_sum(a.total_tokens,b.total_tokens);
    r.reasoning_tokens = (a.reasoning_tokens >= 0 || b.reasoning_tokens >= 0)
        ? token_sum(a.reasoning_tokens,b.reasoning_tokens)
        : -1;
    r.cache_read_tokens = (a.cache_read_tokens >= 0 || b.cache_read_tokens >= 0)
        ? token_sum(a.cache_read_tokens,b.cache_read_tokens)
        : -1;
    r.cache_write_tokens = (a.cache_write_tokens >= 0 || b.cache_write_tokens >= 0)
        ? token_sum(a.cache_write_tokens,b.cache_write_tokens)
        : -1;
    return r;
}

/*--- Message constructors ---*/

static Message *msg_new(Role role,const char *text) {
    Message *m=mem_calloc(1,sizeof(*m));if(!m) return NULL;m->role=role;
    if(text) {
        m->parts=mem_calloc(1,sizeof(*m->parts));if(!m->parts) goto fail;
        ContentPart *p=mem_calloc(1,sizeof(*p));if(!p) goto fail;
        m->parts[0]=p;m->part_count=1;p->kind=CONTENT_TEXT;p->text=str_dup(text);if(!p->text) goto fail;
    }return m;
fail:message_free(m);return NULL;
}
Message *message_system(const char *text) {return msg_new(ROLE_SYSTEM,text);}
Message *message_user(const char *text) {return msg_new(ROLE_USER,text);}
Message *message_assistant(const char *text) {return msg_new(ROLE_ASSISTANT,text);}
Message *message_tool_result(const char *id,const char *content,bool error) {
    Message *m=msg_new(ROLE_TOOL,NULL);if(!m) return NULL;
    m->tool_call_id=str_dup(id);m->parts=mem_calloc(1,sizeof(*m->parts));if(!m->parts) goto fail;
    ContentPart *p=mem_calloc(1,sizeof(*p));if(!p) goto fail;m->parts[0]=p;m->part_count=1;p->kind=CONTENT_TOOL_RESULT;
    p->tool_result=mem_calloc(1,sizeof(*p->tool_result));if(!p->tool_result) goto fail;
    p->tool_result->tool_call_id=str_dup(id);p->tool_result->content=str_dup(content);p->tool_result->is_error=error;
    if((id && (!m->tool_call_id || !p->tool_result->tool_call_id)) || (content && !p->tool_result->content)) goto fail;return m;
fail:message_free(m);return NULL;
}

char *message_text(const Message *m) {
    if (!m) return str_dup("");
    StrBuf sb;
    strbuf_init(&sb);
    for (size_t i = 0; i < m->part_count; i++) {
        if (m->parts[i]->kind == CONTENT_TEXT && m->parts[i]->text)
            strbuf_append_cstr(&sb, m->parts[i]->text);
    }
    return strbuf_detach(&sb);
}

/* Every populated content field is cloned, including image and reasoning metadata. */
Message *message_clone(const Message *source) {
    if(!source) return NULL;
    Message *m=mem_calloc(1,sizeof(*m));if(!m) return NULL;
    m->role=source->role;
    m->name=str_dup(source->name);m->tool_call_id=str_dup(source->tool_call_id);
    if((source->name && !m->name)||(source->tool_call_id && !m->tool_call_id)) goto fail;
    m->parts=mem_calloc(source->part_count,sizeof(*m->parts));if(!m->parts) goto fail;
    for(size_t i=0;i<source->part_count;i++) {
        const ContentPart *s=source->parts[i];if(!s) goto fail;
        ContentPart *d=mem_calloc(1,sizeof(*d));if(!d) goto fail;
        m->parts[m->part_count++]=d;d->kind=s->kind;
#define COPY_STRING(dst,src) do { (dst)=str_dup(src);if((src) && !(dst)) goto fail; } while(0)
#define NEW_FIELD(field) do {d->field=mem_calloc(1,sizeof(*d->field));if(!d->field) goto fail;} while(0)
        COPY_STRING(d->provider_metadata_json,s->provider_metadata_json);
        COPY_STRING(d->text,s->text);
        if(s->image) {
            NEW_FIELD(image);COPY_STRING(d->image->url,s->image->url);COPY_STRING(d->image->data_base64,s->image->data_base64);
            COPY_STRING(d->image->media_type,s->image->media_type);COPY_STRING(d->image->detail,s->image->detail);
        }
        if(s->tool_call) {
            NEW_FIELD(tool_call);COPY_STRING(d->tool_call->id,s->tool_call->id);COPY_STRING(d->tool_call->name,s->tool_call->name);
            COPY_STRING(d->tool_call->arguments_json,s->tool_call->arguments_json);
        }
        if(s->tool_result) {
            NEW_FIELD(tool_result);COPY_STRING(d->tool_result->tool_call_id,s->tool_result->tool_call_id);
            COPY_STRING(d->tool_result->content,s->tool_result->content);d->tool_result->is_error=s->tool_result->is_error;
        }
        if(s->thinking) {
            NEW_FIELD(thinking);COPY_STRING(d->thinking->text,s->thinking->text);COPY_STRING(d->thinking->signature,s->thinking->signature);
            d->thinking->redacted=s->thinking->redacted;
        }
#undef COPY_STRING
#undef NEW_FIELD
    }
    return m;
fail:message_free(m);return NULL;
}
