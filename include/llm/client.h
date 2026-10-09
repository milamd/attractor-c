#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include "llm/types.h"

/* Blocking APIs borrow requests and all request fields for the call. Responses
 * are owned by the caller. Initialize LlmError to zero and release with
 * llm_error_free; callbacks/context pointers are borrowed for their call only. */

/*============================================================================
 * Provider Adapter Interface
 *==========================================================================*/

typedef struct ProviderAdapter ProviderAdapter;

/* Stream callback: called for each stream event. Return false to stop. */
typedef bool (*StreamCallback)(const StreamEvent *event, void *userdata);

/* Middleware wraps completion in registration order (first is outermost).
 * The opaque ctx must be passed unchanged to next; do not retain it. */
typedef LlmResponse *(*MiddlewareFn)(
    const LlmRequest *req,
    LlmResponse *(*next)(const LlmRequest *, void *),
    void *ctx
);

struct ProviderAdapter {
    char *name;                 /* "openai", "anthropic", "gemini" */
    void *impl;                 /* adapter-specific state */

    /* Required methods */
    LlmResponse *(*complete)(ProviderAdapter *self, const LlmRequest *req, LlmError *err);
    int          (*stream)(ProviderAdapter *self, const LlmRequest *req,
                           StreamCallback cb, void *userdata, LlmError *err);

    /* Optional methods */
    /* close frees nested impl members; the client then frees impl/name/adapter. */
    void (*close)(ProviderAdapter *self);
    void (*initialize)(ProviderAdapter *self);
};

/*============================================================================
 * Client
 *==========================================================================*/

typedef struct {
    ProviderAdapter **providers;
    size_t            provider_count;
    char             *default_provider;
    MiddlewareFn     *middleware;
    size_t            middleware_count;
    const bool       *cancel; /* borrowed, synchronous owner-thread cancellation */
    bool              failed;
} LlmClient;

/* Owned client; NULL on invalid configured credentials or allocation failure. */
LlmClient  *llm_client_from_env(void);

/* Create client manually */
LlmClient  *llm_client_new(void);
/* Transfers adapter ownership, including registration allocation failure. */
void        llm_client_add_provider(LlmClient *c, ProviderAdapter *adapter);
void        llm_client_set_default(LlmClient *c, const char *provider_name);
void        llm_client_add_middleware(LlmClient *c, MiddlewareFn mw);
void        llm_client_free(LlmClient *c);

/* Low-level: blocking complete */
LlmResponse *llm_client_complete(LlmClient *c, const LlmRequest *req, LlmError *err);

/* Low-level: streaming */
int          llm_client_stream(LlmClient *c, const LlmRequest *req,
                               StreamCallback cb, void *userdata, LlmError *err);

/*============================================================================
 * High-Level API
 *==========================================================================*/

/* Tool with execute handler for high-level generate() */
typedef struct {
    ToolDefinition def;
    /* Returns owned text; userdata is borrowed; failures set *is_error. */
    char *(*execute)(const char *arguments_json, void *userdata, bool *is_error);
    void *userdata;
} ActiveTool;

typedef struct {
    char         *text;
    char         *reasoning;
    ToolCall    **tool_calls; /* borrowed from owned response */
    size_t        tool_call_count;
    FinishReason  finish_reason;
    Usage         usage;
    Usage         total_usage;
    LlmResponse  *response; /* owned; finish_reason.raw borrows from this response */
} GenerateResult;

void generate_result_free(GenerateResult *r);

/* High-level blocking generate with automatic tool loop */
GenerateResult *llm_generate(
    LlmClient      *client,
    const char      *model,
    const char      *prompt,        /* simple string prompt (or NULL if messages) */
    Message        **messages,      /* full conversation (or NULL if prompt) */
    size_t           message_count,
    const char      *system_prompt,
    ActiveTool      *tools,
    size_t           tool_count,
    int              max_tool_rounds,
    ReasoningEffort  reasoning_effort,
    const char      *provider,
    int              max_retries,
    LlmError        *err
);

/*============================================================================
 * Provider Adapter Constructors
 *==========================================================================*/

ProviderAdapter *anthropic_adapter_new(const char *api_key, const char *base_url);
ProviderAdapter *openai_adapter_new(const char *api_key, const char *base_url);
ProviderAdapter *gemini_adapter_new(const char *api_key, const char *base_url);

/*============================================================================
 * Model Catalog
 *==========================================================================*/

typedef struct {
    const char *id;
    const char *provider;
    const char *display_name;
    int         context_window;
    bool        supports_tools;
    bool        supports_vision;
    bool        supports_reasoning;
} ModelInfo;

const ModelInfo *llm_get_model_info(const char *model_id);
const ModelInfo *llm_list_models(size_t *count);

/*============================================================================
 * Error mapping utility
 *==========================================================================*/

LlmErrorCode llm_error_from_status(int http_status);

#endif
