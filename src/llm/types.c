#include "llm/types.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/*--- Free helpers ---*/

void content_part_free(ContentPart *p) {
    if (!p) return;
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

Usage usage_add(Usage a, Usage b) {
    Usage r;
    r.input_tokens = a.input_tokens + b.input_tokens;
    r.output_tokens = a.output_tokens + b.output_tokens;
    r.total_tokens = a.total_tokens + b.total_tokens;
    r.reasoning_tokens = (a.reasoning_tokens >= 0 || b.reasoning_tokens >= 0)
        ? (a.reasoning_tokens > 0 ? a.reasoning_tokens : 0) + (b.reasoning_tokens > 0 ? b.reasoning_tokens : 0)
        : -1;
    r.cache_read_tokens = (a.cache_read_tokens >= 0 || b.cache_read_tokens >= 0)
        ? (a.cache_read_tokens > 0 ? a.cache_read_tokens : 0) + (b.cache_read_tokens > 0 ? b.cache_read_tokens : 0)
        : -1;
    r.cache_write_tokens = (a.cache_write_tokens >= 0 || b.cache_write_tokens >= 0)
        ? (a.cache_write_tokens > 0 ? a.cache_write_tokens : 0) + (b.cache_write_tokens > 0 ? b.cache_write_tokens : 0)
        : -1;
    return r;
}

/*--- Message constructors ---*/

static Message *msg_new(Role role, const char *text) {
    Message *m = calloc(1, sizeof(Message));
    m->role = role;
    if (text) {
        m->parts = malloc(sizeof(ContentPart *));
        ContentPart *p = calloc(1, sizeof(ContentPart));
        p->kind = CONTENT_TEXT;
        p->text = str_dup(text);
        m->parts[0] = p;
        m->part_count = 1;
    }
    return m;
}

Message *message_system(const char *text)    { return msg_new(ROLE_SYSTEM, text); }
Message *message_user(const char *text)      { return msg_new(ROLE_USER, text); }
Message *message_assistant(const char *text)  { return msg_new(ROLE_ASSISTANT, text); }

Message *message_tool_result(const char *call_id, const char *content, bool is_error) {
    Message *m = calloc(1, sizeof(Message));
    m->role = ROLE_TOOL;
    m->tool_call_id = str_dup(call_id);
    ContentPart *p = calloc(1, sizeof(ContentPart));
    p->kind = CONTENT_TOOL_RESULT;
    p->tool_result = calloc(1, sizeof(ToolResultData));
    p->tool_result->tool_call_id = str_dup(call_id);
    p->tool_result->content = str_dup(content);
    p->tool_result->is_error = is_error;
    m->parts = malloc(sizeof(ContentPart *));
    m->parts[0] = p;
    m->part_count = 1;
    return m;
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
