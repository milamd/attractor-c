#include "llm/client.h"
#include "util/str.h"
#include "util/http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Model Catalog
 *==========================================================================*/

static const ModelInfo MODEL_CATALOG[] = {
    { "claude-opus-4-6",        "anthropic", "Claude Opus 4.6",          200000, true, true, true },
    { "claude-sonnet-4-5",      "anthropic", "Claude Sonnet 4.5",        200000, true, true, true },
    { "claude-haiku-4-5",       "anthropic", "Claude Haiku 4.5",         200000, true, true, false },
    { "gpt-5.2",                "openai",    "GPT-5.2",                 1047576, true, true, true },
    { "gpt-5.2-mini",           "openai",    "GPT-5.2 Mini",            1047576, true, true, true },
    { "gpt-5.2-codex",          "openai",    "GPT-5.2 Codex",           1047576, true, true, true },
    { "gemini-3-pro-preview",   "gemini",    "Gemini 3 Pro (Preview)",  1048576, true, true, true },
    { "gemini-3-flash-preview", "gemini",    "Gemini 3 Flash (Preview)",1048576, true, true, true },
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
    LlmClient *c = calloc(1, sizeof(LlmClient));
    return c;
}

void llm_client_add_provider(LlmClient *c, ProviderAdapter *adapter) {
    c->providers = realloc(c->providers, (c->provider_count + 1) * sizeof(ProviderAdapter *));
    c->providers[c->provider_count++] = adapter;
    if (!c->default_provider)
        c->default_provider = str_dup(adapter->name);
}

void llm_client_set_default(LlmClient *c, const char *name) {
    free(c->default_provider);
    c->default_provider = str_dup(name);
}

void llm_client_add_middleware(LlmClient *c, MiddlewareFn mw) {
    c->middleware = realloc(c->middleware, (c->middleware_count + 1) * sizeof(MiddlewareFn));
    c->middleware[c->middleware_count++] = mw;
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

LlmResponse *llm_client_complete(LlmClient *c, const LlmRequest *req, LlmError *err) {
    ProviderAdapter *adapter = find_provider(c, req->provider);
    if (!adapter) {
        if (err) {
            err->code = LLM_ERR_CONFIG;
            err->message = str_dup("No provider found");
            err->retryable = false;
        }
        return NULL;
    }
    return adapter->complete(adapter, req, err);
}

int llm_client_stream(LlmClient *c, const LlmRequest *req,
                      StreamCallback cb, void *userdata, LlmError *err) {
    ProviderAdapter *adapter = find_provider(c, req->provider);
    if (!adapter) {
        if (err) {
            err->code = LLM_ERR_CONFIG;
            err->message = str_dup("No provider found");
        }
        return -1;
    }
    return adapter->stream(adapter, req, cb, userdata, err);
}

LlmClient *llm_client_from_env(void) {
    LlmClient *c = llm_client_new();

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

    return c;
}

/*============================================================================
 * High-Level generate()
 *==========================================================================*/

void generate_result_free(GenerateResult *r) {
    if (!r) return;
    free(r->text);
    free(r->reasoning);
    for (size_t i = 0; i < r->tool_call_count; i++)
        tool_call_free(r->tool_calls[i]);
    free(r->tool_calls);
    /* Don't free r->response — it's owned by the caller chain */
    free(r);
}

GenerateResult *llm_generate(
    LlmClient      *client,
    const char      *model,
    const char      *prompt,
    Message        **messages,
    size_t           message_count,
    const char      *system_prompt,
    ActiveTool      *tools,
    size_t           tool_count,
    int              max_tool_rounds,
    ReasoningEffort  reasoning_effort,
    const char      *provider,
    int              max_retries,
    LlmError        *err
) {
    /* Build initial message list */
    size_t msg_cap = message_count + 2;
    size_t msg_count = 0;
    Message **msgs = calloc(msg_cap, sizeof(Message *));

    if (system_prompt)
        msgs[msg_count++] = message_system(system_prompt);

    if (prompt) {
        msgs[msg_count++] = message_user(prompt);
    } else {
        for (size_t i = 0; i < message_count; i++)
            msgs[msg_count++] = messages[i]; /* shallow copy, don't free these */
    }

    /* Build tool definitions */
    ToolDefinition **tool_defs = NULL;
    if (tool_count > 0) {
        tool_defs = calloc(tool_count, sizeof(ToolDefinition *));
        for (size_t i = 0; i < tool_count; i++) {
            tool_defs[i] = calloc(1, sizeof(ToolDefinition));
            tool_defs[i]->name = str_dup(tools[i].def.name);
            tool_defs[i]->description = str_dup(tools[i].def.description);
            tool_defs[i]->parameters_json = str_dup(tools[i].def.parameters_json);
        }
    }

    Usage total_usage = { 0, 0, 0, -1, -1, -1 };
    LlmResponse *last_response = NULL;

    for (int round = 0; round <= max_tool_rounds; round++) {
        LlmRequest req = {
            .model = str_dup(model),
            .messages = msgs,
            .message_count = msg_count,
            .provider = str_dup(provider),
            .tools = tool_defs,
            .tool_count = tool_count,
            .temperature = -1,
            .top_p = -1,
            .max_tokens = 0,
            .reasoning_effort = reasoning_effort,
        };

        /* Retry loop */
        LlmResponse *resp = NULL;
        for (int attempt = 0; attempt <= max_retries; attempt++) {
            LlmError local_err = {0};
            resp = llm_client_complete(client, &req, &local_err);
            if (resp) break;
            if (!local_err.retryable || attempt == max_retries) {
                if (err) *err = local_err;
                else llm_error_free(&local_err);
                free(req.model);
                free(req.provider);
                /* cleanup */
                for (size_t i = 0; i < tool_count; i++) {
                    tool_definition_free(tool_defs[i]);
                    free(tool_defs[i]);
                }
                free(tool_defs);
                /* free messages we allocated */
                if (prompt) {
                    for (size_t i = 0; i < msg_count; i++)
                        message_free(msgs[i]);
                }
                free(msgs);
                return NULL;
            }
            llm_error_free(&local_err);
        }

        free(req.model);
        free(req.provider);

        total_usage = usage_add(total_usage, resp->usage);

        if (last_response) llm_response_free(last_response);
        last_response = resp;

        /* If no tool calls, done */
        if (resp->tool_call_count == 0 || resp->finish_reason.reason != FINISH_TOOL_CALLS)
            break;

        if (round >= max_tool_rounds)
            break;

        /* Execute tools and continue */
        /* Add assistant message to history */
        if (msg_count + resp->tool_call_count + 1 >= msg_cap) {
            msg_cap = (msg_count + resp->tool_call_count + 2) * 2;
            msgs = realloc(msgs, msg_cap * sizeof(Message *));
        }

        /* Clone assistant message into history */
        Message *asst = calloc(1, sizeof(Message));
        asst->role = ROLE_ASSISTANT;
        asst->parts = calloc(resp->message->part_count, sizeof(ContentPart *));
        asst->part_count = resp->message->part_count;
        for (size_t i = 0; i < resp->message->part_count; i++) {
            ContentPart *src = resp->message->parts[i];
            ContentPart *dst = calloc(1, sizeof(ContentPart));
            dst->kind = src->kind;
            if (src->text) dst->text = str_dup(src->text);
            if (src->tool_call) {
                dst->tool_call = calloc(1, sizeof(ToolCallData));
                dst->tool_call->id = str_dup(src->tool_call->id);
                dst->tool_call->name = str_dup(src->tool_call->name);
                dst->tool_call->arguments_json = str_dup(src->tool_call->arguments_json);
            }
            asst->parts[i] = dst;
        }
        msgs[msg_count++] = asst;

        /* Execute each tool call */
        for (size_t i = 0; i < resp->tool_call_count; i++) {
            ToolCall *tc = resp->tool_calls[i];
            char *result_str = NULL;
            bool is_error = false;

            /* Find tool */
            ActiveTool *active = NULL;
            for (size_t t = 0; t < tool_count; t++) {
                if (str_eq(tools[t].def.name, tc->name)) {
                    active = &tools[t];
                    break;
                }
            }

            if (active && active->execute) {
                result_str = active->execute(tc->arguments_json);
                if (!result_str) {
                    result_str = str_dup("Tool execution failed");
                    is_error = true;
                }
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "Unknown tool: %s", tc->name);
                result_str = str_dup(buf);
                is_error = true;
            }

            if (msg_count >= msg_cap) {
                msg_cap *= 2;
                msgs = realloc(msgs, msg_cap * sizeof(Message *));
            }
            msgs[msg_count++] = message_tool_result(tc->id, result_str, is_error);
            free(result_str);
        }
    }

    /* Build result */
    GenerateResult *result = calloc(1, sizeof(GenerateResult));
    if (last_response) {
        result->text = str_dup(last_response->text);
        result->reasoning = str_dup(last_response->reasoning);
        result->finish_reason = last_response->finish_reason;
        result->usage = last_response->usage;
        result->tool_call_count = last_response->tool_call_count;
        if (last_response->tool_call_count > 0) {
            result->tool_calls = calloc(last_response->tool_call_count, sizeof(ToolCall *));
            for (size_t i = 0; i < last_response->tool_call_count; i++) {
                result->tool_calls[i] = calloc(1, sizeof(ToolCall));
                result->tool_calls[i]->id = str_dup(last_response->tool_calls[i]->id);
                result->tool_calls[i]->name = str_dup(last_response->tool_calls[i]->name);
                result->tool_calls[i]->arguments_json = str_dup(last_response->tool_calls[i]->arguments_json);
            }
        }
        result->response = last_response;
    }
    result->total_usage = total_usage;

    /* Cleanup */
    for (size_t i = 0; i < tool_count; i++) {
        tool_definition_free(tool_defs[i]);
        free(tool_defs[i]);
    }
    free(tool_defs);

    /* Free messages we created (system + user if from prompt) */
    if (prompt) {
        for (size_t i = 0; i < msg_count; i++) {
            /* Don't double-free the last_response message if it's in the list */
            message_free(msgs[i]);
        }
    }
    free(msgs);

    return result;
}
