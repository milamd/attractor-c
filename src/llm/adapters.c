#include "llm/client.h"
#include "util/str.h"
#include "util/json.h"
#include "util/http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Shared adapter helpers
 *==========================================================================*/

static char *build_messages_json_anthropic(Message **msgs, size_t count, char **system_out);
static char *build_messages_json_openai(Message **msgs, size_t count, char **instructions_out);
static char *build_messages_json_gemini(Message **msgs, size_t count, char **system_instruction_out);
static char *build_tools_json_anthropic(ToolDefinition **tools, size_t count);
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
    err->code = code;
    err->message = str_dup(msg);
    err->provider = str_dup(provider);
    err->status_code = status;
    err->retryable = retryable;
    err->retry_after = -1.0;
    err->raw_json = NULL;
}

/*============================================================================
 * Anthropic Adapter
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} AnthropicState;

static LlmResponse *anthropic_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    AnthropicState *st = self->impl;

    /* Build request body */
    char *system_prompt = NULL;
    char *messages_json = build_messages_json_anthropic(req->messages, req->message_count, &system_prompt);
    char *tools_json = req->tool_count > 0
        ? build_tools_json_anthropic(req->tools, req->tool_count)
        : NULL;

    StrBuf body;
    strbuf_init(&body);
    strbuf_appendf(&body, "{\"model\":\"%s\",\"max_tokens\":%d",
                   req->model, req->max_tokens > 0 ? req->max_tokens : 4096);

    if (system_prompt) {
        /* Escape system prompt for JSON */
        JsonValue *sp = json_new_string(system_prompt);
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
        if (req->tool_choice) {
            switch (req->tool_choice->mode) {
                case TOOL_CHOICE_AUTO:     strbuf_append_cstr(&body, ",\"tool_choice\":{\"type\":\"auto\"}"); break;
                case TOOL_CHOICE_REQUIRED: strbuf_append_cstr(&body, ",\"tool_choice\":{\"type\":\"any\"}"); break;
                case TOOL_CHOICE_NAMED:
                    strbuf_appendf(&body, ",\"tool_choice\":{\"type\":\"tool\",\"name\":\"%s\"}",
                                   req->tool_choice->tool_name);
                    break;
                case TOOL_CHOICE_NONE:
                    /* Anthropic: omit tools entirely for "none" */
                    break;
            }
        }
        free(tools_json);
    }

    if (req->temperature >= 0) strbuf_appendf(&body, ",\"temperature\":%g", req->temperature);
    if (req->top_p >= 0) strbuf_appendf(&body, ",\"top_p\":%g", req->top_p);

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
        NULL
    };

    HttpRequest hreq = {
        .url = url.data,
        .method = "POST",
        .body = body.data,
        .body_len = body.len,
        .headers = hdrs,
        .timeout_ms = 120000
    };

    HttpResponse *resp = http_request(&hreq);
    strbuf_free(&url);
    char *body_str = strbuf_detach(&body);
    free(body_str);

    if (!resp) {
        set_error(err, LLM_ERR_NETWORK, "HTTP request failed", "anthropic", 0, true);
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        bool retryable = (resp->status_code == 429 || resp->status_code >= 500);
        set_error(err, llm_error_from_status(resp->status_code),
                  resp->body ? resp->body : "request failed",
                  "anthropic", resp->status_code, retryable);
        http_response_free(resp);
        return NULL;
    }

    LlmResponse *result = parse_anthropic_response(resp->body);
    if (result) {
        result->provider = str_dup("anthropic");
        result->raw_json = str_dup(resp->body);
    } else {
        set_error(err, LLM_ERR_PROVIDER, "Failed to parse response", "anthropic", 200, false);
    }

    http_response_free(resp);
    return result;
}

static int anthropic_stream(ProviderAdapter *self, const LlmRequest *req,
                            StreamCallback cb, void *userdata, LlmError *err) {
    (void)self; (void)req; (void)cb; (void)userdata;
    set_error(err, LLM_ERR_CONFIG, "Streaming not yet implemented for Anthropic", "anthropic", 0, false);
    return -1;
}

ProviderAdapter *anthropic_adapter_new(const char *api_key, const char *base_url) {
    ProviderAdapter *a = calloc(1, sizeof(ProviderAdapter));
    a->name = str_dup("anthropic");
    AnthropicState *st = calloc(1, sizeof(AnthropicState));
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = anthropic_complete;
    a->stream = anthropic_stream;
    a->close = NULL;
    return a;
}

/*============================================================================
 * OpenAI Adapter (Responses API)
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} OpenAIState;

static LlmResponse *openai_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    OpenAIState *st = self->impl;

    char *instructions = NULL;
    char *input_json = build_messages_json_openai(req->messages, req->message_count, &instructions);
    char *tools_json = req->tool_count > 0
        ? build_tools_json_openai(req->tools, req->tool_count)
        : NULL;

    StrBuf body;
    strbuf_init(&body);
    strbuf_appendf(&body, "{\"model\":\"%s\"", req->model);

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

    if (req->max_tokens > 0) strbuf_appendf(&body, ",\"max_output_tokens\":%d", req->max_tokens);
    if (req->temperature >= 0) strbuf_appendf(&body, ",\"temperature\":%g", req->temperature);

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
        .headers = hdrs, .timeout_ms = 120000
    };

    HttpResponse *resp = http_request(&hreq);
    strbuf_free(&url);
    char *body_cpy = strbuf_detach(&body);
    free(body_cpy);

    if (!resp) {
        set_error(err, LLM_ERR_NETWORK, "HTTP request failed", "openai", 0, true);
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        bool retryable = (resp->status_code == 429 || resp->status_code >= 500);
        set_error(err, llm_error_from_status(resp->status_code),
                  resp->body ? resp->body : "request failed",
                  "openai", resp->status_code, retryable);
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

    http_response_free(resp);
    return result;
}

static int openai_stream(ProviderAdapter *self, const LlmRequest *req,
                         StreamCallback cb, void *userdata, LlmError *err) {
    (void)self; (void)req; (void)cb; (void)userdata;
    set_error(err, LLM_ERR_CONFIG, "Streaming not yet implemented for OpenAI", "openai", 0, false);
    return -1;
}

ProviderAdapter *openai_adapter_new(const char *api_key, const char *base_url) {
    ProviderAdapter *a = calloc(1, sizeof(ProviderAdapter));
    a->name = str_dup("openai");
    OpenAIState *st = calloc(1, sizeof(OpenAIState));
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = openai_complete;
    a->stream = openai_stream;
    return a;
}

/*============================================================================
 * Gemini Adapter
 *==========================================================================*/

typedef struct {
    char *api_key;
    char *base_url;
} GeminiState;

static LlmResponse *gemini_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    GeminiState *st = self->impl;

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
    if (req->temperature >= 0) { if (has_gc) strbuf_append_cstr(&gc, ","); strbuf_appendf(&gc, "\"temperature\":%g", req->temperature); has_gc = true; }
    strbuf_append_cstr(&gc, "}");
    if (has_gc) strbuf_append_cstr(&body, gc.data);
    strbuf_free(&gc);

    strbuf_append_cstr(&body, "}");

    StrBuf url;
    strbuf_init(&url);
    strbuf_appendf(&url, "%s/v1beta/models/%s:generateContent?key=%s",
                   st->base_url ? st->base_url : "https://generativelanguage.googleapis.com",
                   req->model, st->api_key);

    const char *hdrs[] = { "Content-Type: application/json", NULL };
    HttpRequest hreq = {
        .url = url.data, .method = "POST",
        .body = body.data, .body_len = body.len,
        .headers = hdrs, .timeout_ms = 120000
    };

    HttpResponse *resp = http_request(&hreq);
    strbuf_free(&url);
    char *body_cpy = strbuf_detach(&body);
    free(body_cpy);

    if (!resp) {
        set_error(err, LLM_ERR_NETWORK, "HTTP request failed", "gemini", 0, true);
        return NULL;
    }

    if (resp->status_code < 200 || resp->status_code >= 300) {
        bool retryable = (resp->status_code == 429 || resp->status_code >= 500);
        set_error(err, llm_error_from_status(resp->status_code),
                  resp->body ? resp->body : "request failed",
                  "gemini", resp->status_code, retryable);
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

    http_response_free(resp);
    return result;
}

static int gemini_stream(ProviderAdapter *self, const LlmRequest *req,
                         StreamCallback cb, void *userdata, LlmError *err) {
    (void)self; (void)req; (void)cb; (void)userdata;
    set_error(err, LLM_ERR_CONFIG, "Streaming not yet implemented for Gemini", "gemini", 0, false);
    return -1;
}

ProviderAdapter *gemini_adapter_new(const char *api_key, const char *base_url) {
    ProviderAdapter *a = calloc(1, sizeof(ProviderAdapter));
    a->name = str_dup("gemini");
    GeminiState *st = calloc(1, sizeof(GeminiState));
    st->api_key = str_dup(api_key);
    st->base_url = str_dup(base_url);
    a->impl = st;
    a->complete = gemini_complete;
    a->stream = gemini_stream;
    return a;
}

/*============================================================================
 * Message/Tool building helpers
 *==========================================================================*/

/* Build Anthropic messages JSON, extracting system messages */
static char *build_messages_json_anthropic(Message **msgs, size_t count, char **system_out) {
    StrBuf sys;
    strbuf_init(&sys);
    JsonValue *arr = json_new_array();

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

        JsonValue *msg = json_new_object();
        json_object_set(msg, "role", json_new_string(role_str));

        JsonValue *content = json_new_array();
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
                    json_object_set(block, "input", args ? args : json_new_object());
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
        json_object_set(msg, "content", content);
        json_array_push(arr, msg);
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
            } else {
                json_free(item);
                continue;
            }
            json_array_push(arr, item);
        }
    }

    *instructions_out = instr.len > 0 ? strbuf_detach(&instr) : NULL;
    if (instr.len == 0) strbuf_free(&instr);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build Gemini contents JSON */
static char *build_messages_json_gemini(Message **msgs, size_t count, char **system_instruction_out) {
    StrBuf sys;
    strbuf_init(&sys);
    JsonValue *arr = json_new_array();

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

        JsonValue *content = json_new_object();
        json_object_set(content, "role", json_new_string(role));
        JsonValue *parts = json_new_array();

        for (size_t j = 0; j < m->part_count; j++) {
            ContentPart *p = m->parts[j];
            if (p->kind == CONTENT_TEXT && p->text) {
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
                    json_object_set(fc, "args", args ? args : json_new_object());
                }
                json_object_set(part, "functionCall", fc);
                json_array_push(parts, part);
            } else if (p->kind == CONTENT_TOOL_RESULT && p->tool_result) {
                JsonValue *part = json_new_object();
                JsonValue *fr = json_new_object();
                json_object_set(fr, "name", json_new_string(p->tool_result->tool_call_id));
                JsonValue *resp_obj = json_new_object();
                json_object_set(resp_obj, "result", json_new_string(p->tool_result->content ? p->tool_result->content : ""));
                json_object_set(fr, "response", resp_obj);
                json_object_set(part, "functionResponse", fr);
                json_array_push(parts, part);
            }
        }
        json_object_set(content, "parts", parts);
        json_array_push(arr, content);
    }

    *system_instruction_out = sys.len > 0 ? strbuf_detach(&sys) : NULL;
    if (sys.len == 0) strbuf_free(&sys);
    char *result = json_serialize(arr);
    json_free(arr);
    return result;
}

/* Build Anthropic tools JSON */
static char *build_tools_json_anthropic(ToolDefinition **tools, size_t count) {
    JsonValue *arr = json_new_array();
    for (size_t i = 0; i < count; i++) {
        JsonValue *t = json_new_object();
        json_object_set(t, "name", json_new_string(tools[i]->name));
        json_object_set(t, "description", json_new_string(tools[i]->description));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(t, "input_schema", schema ? schema : json_new_object());
        }
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
        json_object_set(t, "description", json_new_string(tools[i]->description));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(t, "parameters", schema ? schema : json_new_object());
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
        json_object_set(fn, "description", json_new_string(tools[i]->description));
        if (tools[i]->parameters_json) {
            const char *e = NULL;
            JsonValue *schema = json_parse(tools[i]->parameters_json, &e);
            json_object_set(fn, "parameters", schema ? schema : json_new_object());
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

static LlmResponse *parse_anthropic_response(const char *json_str) {
    const char *e = NULL;
    JsonValue *root = json_parse(json_str, &e);
    if (!root) return NULL;

    LlmResponse *r = calloc(1, sizeof(LlmResponse));
    r->id = str_dup(json_get_string(root, "id"));
    r->model = str_dup(json_get_string(root, "model"));

    /* Parse content blocks */
    JsonValue *content = json_get(root, "content");
    StrBuf text_buf, reasoning_buf;
    strbuf_init(&text_buf);
    strbuf_init(&reasoning_buf);
    size_t tc_cap = 0;
    r->tool_calls = NULL;
    r->tool_call_count = 0;

    Message *msg = calloc(1, sizeof(Message));
    msg->role = ROLE_ASSISTANT;
    size_t parts_cap = 0;

    if (content && content->type == JSON_ARRAY) {
        for (size_t i = 0; i < content->array.count; i++) {
            JsonValue *block = content->array.items[i];
            const char *btype = json_get_string(block, "type");
            if (!btype) continue;

            ContentPart *cp = calloc(1, sizeof(ContentPart));

            if (strcmp(btype, "text") == 0) {
                const char *t = json_get_string(block, "text");
                cp->kind = CONTENT_TEXT;
                cp->text = str_dup(t ? t : "");
                strbuf_append_cstr(&text_buf, t ? t : "");
            } else if (strcmp(btype, "tool_use") == 0) {
                cp->kind = CONTENT_TOOL_CALL;
                cp->tool_call = calloc(1, sizeof(ToolCallData));
                cp->tool_call->id = str_dup(json_get_string(block, "id"));
                cp->tool_call->name = str_dup(json_get_string(block, "name"));
                JsonValue *input = json_get(block, "input");
                cp->tool_call->arguments_json = input ? json_serialize(input) : str_dup("{}");

                /* Also add to tool_calls array */
                if (r->tool_call_count >= tc_cap) {
                    tc_cap = tc_cap ? tc_cap * 2 : 4;
                    r->tool_calls = realloc(r->tool_calls, tc_cap * sizeof(ToolCall *));
                }
                ToolCall *tc = calloc(1, sizeof(ToolCall));
                tc->id = str_dup(cp->tool_call->id);
                tc->name = str_dup(cp->tool_call->name);
                tc->arguments_json = str_dup(cp->tool_call->arguments_json);
                r->tool_calls[r->tool_call_count++] = tc;
            } else if (strcmp(btype, "thinking") == 0) {
                cp->kind = CONTENT_THINKING;
                cp->thinking = calloc(1, sizeof(ThinkingData));
                const char *t = json_get_string(block, "thinking");
                cp->thinking->text = str_dup(t ? t : "");
                cp->thinking->signature = str_dup(json_get_string(block, "signature"));
                strbuf_append_cstr(&reasoning_buf, t ? t : "");
            } else {
                content_part_free(cp);
                continue;
            }

            if (msg->part_count >= parts_cap) {
                parts_cap = parts_cap ? parts_cap * 2 : 4;
                msg->parts = realloc(msg->parts, parts_cap * sizeof(ContentPart *));
            }
            msg->parts[msg->part_count++] = cp;
        }
    }

    r->message = msg;
    r->text = strbuf_detach(&text_buf);
    r->reasoning = reasoning_buf.len > 0 ? strbuf_detach(&reasoning_buf) : NULL;
    if (reasoning_buf.len == 0) strbuf_free(&reasoning_buf);

    /* Finish reason */
    const char *stop = json_get_string(root, "stop_reason");
    if (stop) {
        r->finish_reason.raw = str_dup(stop);
        if (strcmp(stop, "end_turn") == 0 || strcmp(stop, "stop_sequence") == 0)
            r->finish_reason.reason = FINISH_STOP;
        else if (strcmp(stop, "max_tokens") == 0)
            r->finish_reason.reason = FINISH_LENGTH;
        else if (strcmp(stop, "tool_use") == 0)
            r->finish_reason.reason = FINISH_TOOL_CALLS;
        else
            r->finish_reason.reason = FINISH_OTHER;
    }

    /* Usage */
    JsonValue *usage = json_get(root, "usage");
    if (usage) {
        r->usage.input_tokens = json_get_int(usage, "input_tokens", 0);
        r->usage.output_tokens = json_get_int(usage, "output_tokens", 0);
        r->usage.total_tokens = r->usage.input_tokens + r->usage.output_tokens;
        r->usage.cache_read_tokens = json_get_int(usage, "cache_read_input_tokens", -1);
        r->usage.cache_write_tokens = json_get_int(usage, "cache_creation_input_tokens", -1);
        r->usage.reasoning_tokens = -1;
    }

    json_free(root);
    return r;
}

static LlmResponse *parse_openai_response(const char *json_str) {
    const char *e = NULL;
    JsonValue *root = json_parse(json_str, &e);
    if (!root) return NULL;

    LlmResponse *r = calloc(1, sizeof(LlmResponse));
    r->id = str_dup(json_get_string(root, "id"));
    r->model = str_dup(json_get_string(root, "model"));

    StrBuf text_buf;
    strbuf_init(&text_buf);
    size_t tc_cap = 0;

    Message *msg = calloc(1, sizeof(Message));
    msg->role = ROLE_ASSISTANT;
    size_t parts_cap = 0;

    /* Parse output array */
    JsonValue *output = json_get(root, "output");
    if (output && output->type == JSON_ARRAY) {
        for (size_t i = 0; i < output->array.count; i++) {
            JsonValue *item = output->array.items[i];
            const char *itype = json_get_string(item, "type");
            if (!itype) continue;

            if (strcmp(itype, "message") == 0) {
                JsonValue *content = json_get(item, "content");
                if (content && content->type == JSON_ARRAY) {
                    for (size_t j = 0; j < content->array.count; j++) {
                        JsonValue *part = content->array.items[j];
                        const char *ptype = json_get_string(part, "type");
                        if (ptype && strcmp(ptype, "output_text") == 0) {
                            const char *t = json_get_string(part, "text");
                            ContentPart *cp = calloc(1, sizeof(ContentPart));
                            cp->kind = CONTENT_TEXT;
                            cp->text = str_dup(t ? t : "");
                            strbuf_append_cstr(&text_buf, t ? t : "");
                            if (msg->part_count >= parts_cap) {
                                parts_cap = parts_cap ? parts_cap * 2 : 4;
                                msg->parts = realloc(msg->parts, parts_cap * sizeof(ContentPart *));
                            }
                            msg->parts[msg->part_count++] = cp;
                        }
                    }
                }
            } else if (strcmp(itype, "function_call") == 0) {
                ContentPart *cp = calloc(1, sizeof(ContentPart));
                cp->kind = CONTENT_TOOL_CALL;
                cp->tool_call = calloc(1, sizeof(ToolCallData));
                /* Responses API: use call_id for referencing in function_call_output */
                const char *cid = json_get_string(item, "call_id");
                if (!cid) cid = json_get_string(item, "id");
                cp->tool_call->id = str_dup(cid);
                cp->tool_call->name = str_dup(json_get_string(item, "name"));
                cp->tool_call->arguments_json = str_dup(json_get_string(item, "arguments"));
                if (msg->part_count >= parts_cap) {
                    parts_cap = parts_cap ? parts_cap * 2 : 4;
                    msg->parts = realloc(msg->parts, parts_cap * sizeof(ContentPart *));
                }
                msg->parts[msg->part_count++] = cp;

                if (r->tool_call_count >= tc_cap) {
                    tc_cap = tc_cap ? tc_cap * 2 : 4;
                    r->tool_calls = realloc(r->tool_calls, tc_cap * sizeof(ToolCall *));
                }
                ToolCall *tc = calloc(1, sizeof(ToolCall));
                tc->id = str_dup(cp->tool_call->id);
                tc->name = str_dup(cp->tool_call->name);
                tc->arguments_json = str_dup(cp->tool_call->arguments_json);
                r->tool_calls[r->tool_call_count++] = tc;
            }
        }
    }

    r->message = msg;
    r->text = strbuf_detach(&text_buf);

    /* Finish reason */
    const char *status = json_get_string(root, "status");
    if (status && strcmp(status, "completed") == 0) {
        r->finish_reason.reason = r->tool_call_count > 0 ? FINISH_TOOL_CALLS : FINISH_STOP;
    }

    /* Usage */
    JsonValue *usage = json_get(root, "usage");
    if (usage) {
        r->usage.input_tokens = json_get_int(usage, "input_tokens", 0);
        r->usage.output_tokens = json_get_int(usage, "output_tokens", 0);
        r->usage.total_tokens = json_get_int(usage, "total_tokens", 0);
        r->usage.reasoning_tokens = -1;
        JsonValue *output_detail = json_get(usage, "output_tokens_details");
        if (output_detail)
            r->usage.reasoning_tokens = json_get_int(output_detail, "reasoning_tokens", -1);
    }

    json_free(root);
    return r;
}

static LlmResponse *parse_gemini_response(const char *json_str) {
    const char *e = NULL;
    JsonValue *root = json_parse(json_str, &e);
    if (!root) return NULL;

    LlmResponse *r = calloc(1, sizeof(LlmResponse));
    StrBuf text_buf;
    strbuf_init(&text_buf);
    size_t tc_cap = 0;

    Message *msg = calloc(1, sizeof(Message));
    msg->role = ROLE_ASSISTANT;
    size_t parts_cap = 0;

    JsonValue *candidates = json_get(root, "candidates");
    if (candidates && candidates->type == JSON_ARRAY && candidates->array.count > 0) {
        JsonValue *cand = candidates->array.items[0];
        JsonValue *content = json_get(cand, "content");
        JsonValue *parts = content ? json_get(content, "parts") : NULL;

        if (parts && parts->type == JSON_ARRAY) {
            for (size_t i = 0; i < parts->array.count; i++) {
                JsonValue *part = parts->array.items[i];
                ContentPart *cp = calloc(1, sizeof(ContentPart));

                const char *text = json_get_string(part, "text");
                JsonValue *fc = json_get(part, "functionCall");

                if (text) {
                    cp->kind = CONTENT_TEXT;
                    cp->text = str_dup(text);
                    strbuf_append_cstr(&text_buf, text);
                } else if (fc) {
                    cp->kind = CONTENT_TOOL_CALL;
                    cp->tool_call = calloc(1, sizeof(ToolCallData));
                    cp->tool_call->name = str_dup(json_get_string(fc, "name"));
                    JsonValue *args = json_get(fc, "args");
                    cp->tool_call->arguments_json = args ? json_serialize(args) : str_dup("{}");
                    /* Generate synthetic ID */
                    char syn_id[64];
                    snprintf(syn_id, sizeof(syn_id), "call_%zu", i);
                    cp->tool_call->id = str_dup(syn_id);

                    if (r->tool_call_count >= tc_cap) {
                        tc_cap = tc_cap ? tc_cap * 2 : 4;
                        r->tool_calls = realloc(r->tool_calls, tc_cap * sizeof(ToolCall *));
                    }
                    ToolCall *tc = calloc(1, sizeof(ToolCall));
                    tc->id = str_dup(cp->tool_call->id);
                    tc->name = str_dup(cp->tool_call->name);
                    tc->arguments_json = str_dup(cp->tool_call->arguments_json);
                    r->tool_calls[r->tool_call_count++] = tc;
                } else {
                    content_part_free(cp);
                    continue;
                }

                if (msg->part_count >= parts_cap) {
                    parts_cap = parts_cap ? parts_cap * 2 : 4;
                    msg->parts = realloc(msg->parts, parts_cap * sizeof(ContentPart *));
                }
                msg->parts[msg->part_count++] = cp;
            }
        }

        const char *finish = json_get_string(cand, "finishReason");
        if (finish) {
            r->finish_reason.raw = str_dup(finish);
            if (strcmp(finish, "STOP") == 0)
                r->finish_reason.reason = r->tool_call_count > 0 ? FINISH_TOOL_CALLS : FINISH_STOP;
            else if (strcmp(finish, "MAX_TOKENS") == 0)
                r->finish_reason.reason = FINISH_LENGTH;
            else if (strcmp(finish, "SAFETY") == 0 || strcmp(finish, "RECITATION") == 0)
                r->finish_reason.reason = FINISH_CONTENT_FILTER;
            else
                r->finish_reason.reason = FINISH_OTHER;
        }
    }

    r->message = msg;
    r->text = strbuf_detach(&text_buf);

    /* Usage */
    JsonValue *usage_meta = json_get(root, "usageMetadata");
    if (usage_meta) {
        r->usage.input_tokens = json_get_int(usage_meta, "promptTokenCount", 0);
        r->usage.output_tokens = json_get_int(usage_meta, "candidatesTokenCount", 0);
        r->usage.total_tokens = r->usage.input_tokens + r->usage.output_tokens;
        r->usage.reasoning_tokens = json_get_int(usage_meta, "thoughtsTokenCount", -1);
        r->usage.cache_read_tokens = json_get_int(usage_meta, "cachedContentTokenCount", -1);
        r->usage.cache_write_tokens = -1;
    }

    json_free(root);
    return r;
}
