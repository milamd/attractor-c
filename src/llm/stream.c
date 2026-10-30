/* Provider SSE decoding (unified spec 4.3). Every event is handled once and
 * text is appended to growable buffers, so the work is linear in the stream. */
#include "stream_sink.h"
#include "util/io.h"
#include "util/json.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef enum { BLOCK_TEXT, BLOCK_TOOL, BLOCK_THINKING, BLOCK_REDACTED } BlockKind;

typedef struct {
    BlockKind kind;
    char     *id, *name;
    StrBuf    buf;       /* text, tool input JSON, thinking, or redacted data */
    StrBuf    signature;
} Block;

struct StreamSink {
    SinkKind       kind;
    StreamCallback cb;
    void          *userdata;
    bool           started, stopped, done, text_open, reasoning_open;
    char          *error;
    char          *final_json;

    /* Anthropic: message envelope plus content blocks by index. */
    JsonValue     *message;
    Block         *blocks;
    size_t         block_count, block_cap;

    /* OpenAI: the open function call. */
    char          *call_id, *call_name;
    StrBuf         call_args;

    /* Gemini: parts so far, the open text/thought runs, latest metadata. */
    JsonValue     *parts;
    StrBuf         text, thought;
    char          *text_signature;
    char          *finish_reason;
    JsonValue     *usage;
};

StreamSink *stream_sink_new(SinkKind kind, StreamCallback cb, void *userdata) {
    StreamSink *s = mem_calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->kind = kind; s->cb = cb; s->userdata = userdata;
    strbuf_init_limit(&s->call_args, ATTRACTOR_INPUT_LIMIT);
    strbuf_init_limit(&s->text, ATTRACTOR_INPUT_LIMIT);
    strbuf_init_limit(&s->thought, ATTRACTOR_INPUT_LIMIT);
    s->parts = json_new_array();
    if (!s->parts) { free(s); return NULL; }
    return s;
}

void stream_sink_free(StreamSink *s) {
    if (!s) return;
    for (size_t i = 0; i < s->block_count; i++) {
        free(s->blocks[i].id); free(s->blocks[i].name);
        strbuf_free(&s->blocks[i].buf); strbuf_free(&s->blocks[i].signature);
    }
    free(s->blocks);
    json_free(s->message); json_free(s->parts); json_free(s->usage);
    free(s->error); free(s->final_json); free(s->call_id); free(s->call_name);
    free(s->text_signature); free(s->finish_reason);
    strbuf_free(&s->call_args); strbuf_free(&s->text); strbuf_free(&s->thought);
    free(s);
}

bool stream_sink_stopped(const StreamSink *s) { return s->stopped; }
const char *stream_sink_error(const StreamSink *s) { return s->error; }

static void fail(StreamSink *s, const char *message) {
    if (!s->error) s->error = str_dup(message ? message : "Stream error");
}

static bool deliver(StreamSink *s, StreamEvent *ev) {
    if (s->stopped) return false;
    if (!s->started) {
        s->started = true;
        StreamEvent start = {.type = STREAM_EVT_STREAM_START};
        if (!s->cb(&start, s->userdata)) { s->stopped = true; return false; }
    }
    if (!s->cb(ev, s->userdata)) s->stopped = true;
    return !s->stopped;
}
static bool emit(StreamSink *s, StreamEventType type, const char *delta, const char *reasoning, ToolCall *call, const char *raw) {
    StreamEvent ev = {.type = type, .delta = (char *)delta, .reasoning_delta = (char *)reasoning, .tool_call = call, .raw_json = (char *)raw};
    return deliver(s, &ev);
}

void stream_sink_finish(StreamSink *s, LlmResponse *r) {
    StreamEvent ev = {.type = STREAM_EVT_FINISH, .response = r, .usage = &r->usage, .finish_reason = &r->finish_reason};
    deliver(s, &ev);
}
void stream_sink_fail(StreamSink *s, const char *message) {
    if (s->stopped) return;
    StreamEvent ev = {.type = STREAM_EVT_ERROR, .error_message = (char *)message};
    deliver(s, &ev);
}

/* ── Anthropic ───────────────────────────────────────────────────────── */

static Block *block_at(StreamSink *s, const JsonValue *event) {
    double index = json_get_number(event, "index", -1);
    if (!(index >= 0 && index < 4096) || (double)(size_t)index != index || (size_t)index >= s->block_count) return NULL;
    return &s->blocks[(size_t)index];
}

static bool anthropic_event(StreamSink *s, const JsonValue *ev) {
    const char *type = json_get_string(ev, "type");
    if (str_eq(type, "message_start")) {
        json_free(s->message);
        char *copy = json_serialize(json_get(ev, "message"));
        s->message = copy ? json_parse(copy, NULL) : NULL;
        free(copy);
        if (!s->message || s->message->type != JSON_OBJECT) { fail(s, "Malformed message_start"); return false; }
        return true;
    }
    if (str_eq(type, "content_block_start")) {
        const JsonValue *cb = json_get(ev, "content_block");
        const char *kind = json_get_string(cb, "type");
        double index = json_get_number(ev, "index", -1);
    if (s->block_count >= 4096 || !(index >= 0 && index < 4096) || (size_t)index != s->block_count) { fail(s, "Out-of-order content block"); return false; }
        if (s->block_count == s->block_cap) {
            size_t cap = s->block_cap ? s->block_cap * 2 : 8;
            Block *blocks = mem_reallocarray(s->blocks, cap, sizeof(*blocks));
            if (!blocks) { fail(s, "Out of memory"); return false; }
            s->blocks = blocks; s->block_cap = cap;
        }
        Block *b = &s->blocks[s->block_count++];
        *b = (Block){0};
        strbuf_init_limit(&b->buf, ATTRACTOR_INPUT_LIMIT); strbuf_init_limit(&b->signature, 65536);
        if (str_eq(kind, "text")) { b->kind = BLOCK_TEXT; return emit(s, STREAM_EVT_TEXT_START, NULL, NULL, NULL, NULL); }
        if (str_eq(kind, "tool_use")) {
            b->kind = BLOCK_TOOL;
            b->id = str_dup(json_get_string(cb, "id")); b->name = str_dup(json_get_string(cb, "name"));
            if (!b->id || !b->name) { fail(s, "Malformed tool_use block"); return false; }
            ToolCall call = {.id = b->id, .name = b->name, .arguments_json = ""};
            return emit(s, STREAM_EVT_TOOL_CALL_START, NULL, NULL, &call, NULL);
        }
        if (str_eq(kind, "thinking")) { b->kind = BLOCK_THINKING; return emit(s, STREAM_EVT_REASONING_START, NULL, NULL, NULL, NULL); }
        if (str_eq(kind, "redacted_thinking")) { b->kind = BLOCK_REDACTED; strbuf_append_cstr(&b->buf, str_safe(json_get_string(cb, "data"))); return true; }
        fail(s, "Unknown content block type"); return false;
    }
    if (str_eq(type, "content_block_delta")) {
        Block *b = block_at(s, ev);
        const JsonValue *d = json_get(ev, "delta");
        const char *kind = json_get_string(d, "type");
        if (!b) { fail(s, "Delta for unknown block"); return false; }
        if (str_eq(kind, "text_delta") && b->kind == BLOCK_TEXT) {
            const char *t = str_safe(json_get_string(d, "text")); strbuf_append_cstr(&b->buf, t);
            return emit(s, STREAM_EVT_TEXT_DELTA, t, NULL, NULL, NULL);
        }
        if (str_eq(kind, "input_json_delta") && b->kind == BLOCK_TOOL) {
            const char *t = str_safe(json_get_string(d, "partial_json")); strbuf_append_cstr(&b->buf, t);
            ToolCall call = {.id = b->id, .name = b->name, .arguments_json = (char *)t};
            return emit(s, STREAM_EVT_TOOL_CALL_DELTA, t, NULL, &call, NULL);
        }
        if (str_eq(kind, "thinking_delta") && b->kind == BLOCK_THINKING) {
            const char *t = str_safe(json_get_string(d, "thinking")); strbuf_append_cstr(&b->buf, t);
            return emit(s, STREAM_EVT_REASONING_DELTA, NULL, t, NULL, NULL);
        }
        if (str_eq(kind, "signature_delta") && b->kind == BLOCK_THINKING) { strbuf_append_cstr(&b->signature, str_safe(json_get_string(d, "signature"))); return true; }
        fail(s, "Unexpected content delta"); return false;
    }
    if (str_eq(type, "content_block_stop")) {
        Block *b = block_at(s, ev);
        if (!b || b->buf.failed || b->signature.failed) { fail(s, "Malformed content block"); return false; }
        if (b->kind == BLOCK_TEXT) return emit(s, STREAM_EVT_TEXT_END, NULL, NULL, NULL, NULL);
        if (b->kind == BLOCK_THINKING) return emit(s, STREAM_EVT_REASONING_END, NULL, NULL, NULL, NULL);
        if (b->kind == BLOCK_TOOL) {
            ToolCall call = {.id = b->id, .name = b->name, .arguments_json = b->buf.len ? b->buf.data : "{}"};
            return emit(s, STREAM_EVT_TOOL_CALL_END, NULL, NULL, &call, NULL);
        }
        return true;
    }
    if (str_eq(type, "message_delta")) {
        if (!s->message) { fail(s, "message_delta before message_start"); return false; }
        const char *stop = json_get_string(json_get(ev, "delta"), "stop_reason");
        if (stop) json_object_set(s->message, "stop_reason", json_new_string(stop));
        const JsonValue *usage = json_get(ev, "usage"), *total = json_get(s->message, "usage");
        if (usage && usage->type == JSON_OBJECT && total && total->type == JSON_OBJECT)
            for (size_t i = 0; i < usage->object.count; i++)
                if (usage->object.values[i]->type == JSON_NUMBER)
                    json_object_set((JsonValue *)total, usage->object.keys[i], json_new_number(usage->object.values[i]->number));
        return true;
    }
    if (str_eq(type, "message_stop")) {
        if (!s->message) { fail(s, "message_stop before message_start"); return false; }
        JsonValue *content = json_new_array();
        for (size_t i = 0; i < s->block_count; i++) {
            Block *b = &s->blocks[i];
            JsonValue *o = json_new_object();
            const char *text = b->buf.len ? b->buf.data : "";
            switch (b->kind) {
            case BLOCK_TEXT: json_object_set(o, "type", json_new_string("text")); json_object_set(o, "text", json_new_string(text)); break;
            case BLOCK_TOOL: {
                JsonValue *input = json_parse(b->buf.len ? text : "{}", NULL);
                if (!input) { json_free(o); json_free(content); fail(s, "Tool input is not valid JSON"); return false; }
                json_object_set(o, "type", json_new_string("tool_use")); json_object_set(o, "id", json_new_string(b->id));
                json_object_set(o, "name", json_new_string(b->name)); json_object_set(o, "input", input); break;
            }
            case BLOCK_THINKING:
                json_object_set(o, "type", json_new_string("thinking")); json_object_set(o, "thinking", json_new_string(text));
                if (b->signature.len) json_object_set(o, "signature", json_new_string(b->signature.data));
                break;
            case BLOCK_REDACTED: json_object_set(o, "type", json_new_string("redacted_thinking")); json_object_set(o, "data", json_new_string(text)); break;
            }
            json_array_push(content, o);
        }
        json_object_set(s->message, "content", content);
        s->final_json = json_serialize(s->message);
        s->done = s->final_json != NULL;
        if (!s->done) fail(s, "Out of memory");
        return s->done;
    }
    if (str_eq(type, "error")) { fail(s, json_get_string(json_get(ev, "error"), "message")); return false; }
    return true; /* ping and future event types */
}

/* ── OpenAI Responses API ────────────────────────────────────────────── */

static bool openai_event(StreamSink *s, const JsonValue *ev, const char *raw) {
    const char *type = json_get_string(ev, "type");
    const JsonValue *item = json_get(ev, "item");
    if (str_eq(type, "response.output_text.delta")) {
        if (!s->text_open) { s->text_open = true; if (!emit(s, STREAM_EVT_TEXT_START, NULL, NULL, NULL, NULL)) return false; }
        return emit(s, STREAM_EVT_TEXT_DELTA, str_safe(json_get_string(ev, "delta")), NULL, NULL, NULL);
    }
    if (str_eq(type, "response.output_text.done")) {
        if (!s->text_open) return true;
        s->text_open = false;
        return emit(s, STREAM_EVT_TEXT_END, NULL, NULL, NULL, NULL);
    }
    if (str_eq(type, "response.reasoning_summary_text.delta") || str_eq(type, "response.reasoning_text.delta")) {
        if (!s->reasoning_open) { s->reasoning_open = true; if (!emit(s, STREAM_EVT_REASONING_START, NULL, NULL, NULL, NULL)) return false; }
        return emit(s, STREAM_EVT_REASONING_DELTA, NULL, str_safe(json_get_string(ev, "delta")), NULL, NULL);
    }
    if ((str_eq(type, "response.reasoning_summary_text.done") || str_eq(type, "response.reasoning_text.done")) && s->reasoning_open) {
        s->reasoning_open = false;
        return emit(s, STREAM_EVT_REASONING_END, NULL, NULL, NULL, NULL);
    }
    if (str_eq(type, "response.output_item.added") && str_eq(json_get_string(item, "type"), "function_call")) {
        free(s->call_id); free(s->call_name); strbuf_reset(&s->call_args);
        s->call_id = str_dup(json_get_string(item, "call_id")); s->call_name = str_dup(json_get_string(item, "name"));
        if (!s->call_id || !s->call_name) { fail(s, "Malformed function_call item"); return false; }
        ToolCall call = {.id = s->call_id, .name = s->call_name, .arguments_json = ""};
        return emit(s, STREAM_EVT_TOOL_CALL_START, NULL, NULL, &call, NULL);
    }
    if (str_eq(type, "response.function_call_arguments.delta")) {
        const char *d = str_safe(json_get_string(ev, "delta"));
        strbuf_append_cstr(&s->call_args, d);
        ToolCall call = {.id = s->call_id, .name = s->call_name, .arguments_json = (char *)d};
        return emit(s, STREAM_EVT_TOOL_CALL_DELTA, d, NULL, &call, NULL);
    }
    if (str_eq(type, "response.output_item.done") && str_eq(json_get_string(item, "type"), "function_call")) {
        const char *args = json_get_string(item, "arguments");
        ToolCall call = {.id = (char *)json_get_string(item, "call_id"), .name = (char *)json_get_string(item, "name"),
                         .arguments_json = (char *)(args ? args : s->call_args.len ? s->call_args.data : "{}")};
        return emit(s, STREAM_EVT_TOOL_CALL_END, NULL, NULL, &call, NULL);
    }
    if (str_eq(type, "response.completed")) {
        s->final_json = json_serialize(json_get(ev, "response"));
        s->done = s->final_json != NULL;
        if (!s->done) fail(s, "Malformed response.completed");
        return s->done;
    }
    if (str_eq(type, "response.failed") || str_eq(type, "response.incomplete") || str_eq(type, "error")) {
        const char *message = json_get_string(json_get(json_get(ev, "response"), "error"), "message");
        fail(s, message ? message : json_get_string(ev, "message"));
        return false;
    }
    return emit(s, STREAM_EVT_PROVIDER_EVENT, NULL, NULL, NULL, raw);
}

/* ── Gemini ──────────────────────────────────────────────────────────── */

static bool push_part(StreamSink *s, JsonValue *part) {
    if (!part || !json_array_push(s->parts, part)) { json_free(part); fail(s, "Out of memory"); return false; }
    return true;
}
/* Closes the open text run into one part, keeping its thought signature. */
static bool gemini_flush_text(StreamSink *s) {
    if (!s->text_open) return true;
    s->text_open = false;
    JsonValue *part = json_new_object();
    json_object_set(part, "text", json_new_string(s->text.len ? s->text.data : ""));
    if (s->text_signature) json_object_set(part, "thoughtSignature", json_new_string(s->text_signature));
    free(s->text_signature); s->text_signature = NULL;
    strbuf_reset(&s->text);
    return push_part(s, part) && emit(s, STREAM_EVT_TEXT_END, NULL, NULL, NULL, NULL);
}
static bool gemini_flush_thought(StreamSink *s) {
    if (!s->reasoning_open) return true;
    s->reasoning_open = false;
    JsonValue *part = json_new_object();
    json_object_set(part, "text", json_new_string(s->thought.len ? s->thought.data : ""));
    json_object_set(part, "thought", json_new_bool(true));
    strbuf_reset(&s->thought);
    return push_part(s, part) && emit(s, STREAM_EVT_REASONING_END, NULL, NULL, NULL, NULL);
}

static bool gemini_event(StreamSink *s, const JsonValue *ev) {
    const JsonValue *candidates = json_get(ev, "candidates");
    const JsonValue *candidate = candidates && candidates->type == JSON_ARRAY && candidates->array.count ? candidates->array.items[0] : NULL;
    const JsonValue *parts = json_get(json_get(candidate, "content"), "parts");
    for (size_t i = 0; parts && parts->type == JSON_ARRAY && i < parts->array.count; i++) {
        const JsonValue *p = parts->array.items[i];
        const char *text = json_get_string(p, "text");
        const JsonValue *fc = json_get(p, "functionCall");
        if (text && json_get_bool(p, "thought", false)) {
            if (!gemini_flush_text(s)) return false;
            if (!s->reasoning_open) { s->reasoning_open = true; if (!emit(s, STREAM_EVT_REASONING_START, NULL, NULL, NULL, NULL)) return false; }
            strbuf_append_cstr(&s->thought, text);
            if (!emit(s, STREAM_EVT_REASONING_DELTA, NULL, text, NULL, NULL)) return false;
        } else if (text) {
            if (!gemini_flush_thought(s)) return false;
            if (!s->text_open) { s->text_open = true; if (!emit(s, STREAM_EVT_TEXT_START, NULL, NULL, NULL, NULL)) return false; }
            strbuf_append_cstr(&s->text, text);
            const char *signature = json_get_string(p, "thoughtSignature");
            if (signature) { free(s->text_signature); s->text_signature = str_dup(signature); }
            if (!emit(s, STREAM_EVT_TEXT_DELTA, text, NULL, NULL, NULL)) return false;
        } else if (fc) {
            if (!gemini_flush_thought(s) || !gemini_flush_text(s)) return false;
            char *copy = json_serialize(p);
            if (!push_part(s, copy ? json_parse(copy, NULL) : NULL)) { free(copy); return false; }
            free(copy);
            char *args = json_serialize(json_get(fc, "args"));
            ToolCall call = {.name = (char *)json_get_string(fc, "name"), .arguments_json = args ? args : "{}"};
            bool ok = emit(s, STREAM_EVT_TOOL_CALL_START, NULL, NULL, &call, NULL) && emit(s, STREAM_EVT_TOOL_CALL_END, NULL, NULL, &call, NULL);
            free(args);
            if (!ok) return false;
        }
    }
    const char *finish = json_get_string(candidate, "finishReason");
    if (finish) { free(s->finish_reason); s->finish_reason = str_dup(finish); }
    const JsonValue *usage = json_get(ev, "usageMetadata");
    if (usage) {
        char *copy = json_serialize(usage);
        json_free(s->usage); s->usage = copy ? json_parse(copy, NULL) : NULL;
        free(copy);
    }
    return true;
}

/* Gemini has no terminal event: the stream ends after the finishReason chunk. */
static bool gemini_finish(StreamSink *s) {
    if (!gemini_flush_thought(s) || !gemini_flush_text(s)) return false;
    if (!s->finish_reason) { fail(s, "Stream ended before a finish reason"); return false; }
    JsonValue *root = json_new_object(), *candidates = json_new_array(), *candidate = json_new_object(), *content = json_new_object();
    json_object_set(content, "role", json_new_string("model"));
    json_object_set(content, "parts", s->parts); s->parts = NULL;
    json_object_set(candidate, "content", content);
    json_object_set(candidate, "finishReason", json_new_string(s->finish_reason));
    json_array_push(candidates, candidate);
    json_object_set(root, "candidates", candidates);
    if (s->usage) { json_object_set(root, "usageMetadata", s->usage); s->usage = NULL; }
    s->final_json = json_serialize(root);
    json_free(root);
    s->done = s->final_json != NULL;
    if (!s->done) fail(s, "Out of memory");
    return s->done;
}

/* ── Transport ───────────────────────────────────────────────────────── */

static bool on_sse(const char *event_type, const char *data, void *userdata) {
    (void)event_type;
    StreamSink *s = userdata;
    if (s->done || s->error || s->stopped) return false;
    if (!*data || str_eq(data, "[DONE]")) return true;
    JsonValue *ev = json_parse(data, NULL);
    if (!ev || ev->type != JSON_OBJECT) { json_free(ev); fail(s, "Malformed stream event"); return false; }
    bool ok = s->kind == SINK_ANTHROPIC ? anthropic_event(s, ev)
            : s->kind == SINK_OPENAI ? openai_event(s, ev, data)
            : gemini_event(s, ev);
    json_free(ev);
    return ok && !s->stopped && !s->done;
}

HttpResponse *stream_sink_transport(StreamSink *s, const HttpRequest *req) {
    HttpResponse *error = NULL;
    int status = http_stream_sse(req, on_sse, s, &error);
    if (error) return error;
    if (status < 0 && !s->done) return NULL;
    if (s->stopped || s->error) return NULL;
    if (!s->done && s->kind == SINK_GEMINI && !gemini_finish(s)) return NULL;
    if (!s->done) { fail(s, "Stream ended before completion"); return NULL; }
    HttpResponse *r = mem_calloc(1, sizeof(*r));
    if (!r) { fail(s, "Out of memory"); return NULL; }
    r->status_code = 200;
    r->body = s->final_json; s->final_json = NULL;
    r->body_len = strlen(r->body);
    r->headers_raw = str_dup("");
    if (!r->headers_raw) { http_response_free(r); fail(s, "Out of memory"); return NULL; }
    return r;
}
