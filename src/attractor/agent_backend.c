#include "attractor/agent_backend.h"
#include "util/mem.h"
#include "util/str.h"
#include <limits.h>
#include <stdlib.h>

typedef struct {
    LlmClient    *client;
    ExecutionEnv *env;
    char         *model;
    char         *provider;
} AgentBackendState;

typedef struct {
    char *error; /* last error or limit message reported by the session */
} RunCapture;

static ReasoningEffort parse_reasoning_effort(const char *s, const char *provider) {
    if (!s || !*s) return str_eq(provider, "openai") ? REASONING_HIGH : REASONING_NONE;
    if (str_eq(s, "low"))    return REASONING_LOW;
    if (str_eq(s, "medium")) return REASONING_MEDIUM;
    if (str_eq(s, "none"))   return REASONING_NONE;
    return REASONING_HIGH;
}

static ProviderProfile *profile_for(const char *provider, const char *model) {
    if (str_eq(provider, "openai")) return openai_profile_new(model);
    if (str_eq(provider, "gemini")) return gemini_profile_new(model);
    ProviderProfile *p = anthropic_profile_new(model);
    if (p && provider && !str_eq(provider, "anthropic")) {
        /* Custom adapter name: keep Anthropic-style tools, route by name. */
        char *id = str_dup(provider);
        if (!id) { provider_profile_free(p); return NULL; }
        free(p->id); p->id = id;
    }
    return p;
}

static void capture_event(const AgentEvent *event, void *userdata) {
    RunCapture *capture = userdata;
    if ((event->kind == AGENT_EVT_ERROR || event->kind == AGENT_EVT_TURN_LIMIT) && event->data) {
        char *copy = str_dup(event->data);
        if (copy) { free(capture->error); capture->error = copy; }
    }
}

/* Prompt plus the Appendix C status-file instructions for this stage. */
static char *stage_prompt(const char *prompt, const char *stage_dir) {
    if (!stage_dir) return str_dup(prompt);
    char resolved[PATH_MAX];
    const char *dir = realpath(stage_dir, resolved) ? resolved : stage_dir;
    StrBuf b; strbuf_init(&b);
    strbuf_append_cstr(&b, prompt);
    strbuf_appendf(&b,
        "\n\n---\nWhen you finish, report this stage's outcome by writing JSON to %s/status.json:\n"
        "{\"outcome\": \"success\" | \"fail\" | \"retry\" | \"partial_success\", "
        "\"preferred_label\": \"<edge label to follow, optional>\", \"notes\": \"<short summary>\", "
        "\"failure_reason\": \"<why, when not success>\", \"context_updates\": {\"key\": \"value\"}}\n"
        "Only \"outcome\" is required. If you write no file, the stage counts as success.\n", dir);
    return strbuf_detach(&b);
}

static char *agent_backend_run(CodergenBackend *self, const DotNode *node,
                               const char *prompt, const PipelineContext *ctx,
                               const char *stage_dir, Outcome *outcome) {
    (void)ctx;
    AgentBackendState *st = self->impl;
    const char *model = node && node->llm_model ? node->llm_model : st->model;
    const char *provider = node && node->llm_provider ? node->llm_provider
                         : st->provider ? st->provider : st->client->default_provider;

    ProviderProfile *profile = profile_for(provider, model);
    SessionConfig config = agent_default_config();
    config.reasoning_effort = parse_reasoning_effort(node ? node->reasoning_effort : NULL, provider);
    AgentSession *session = profile ? agent_session_new(profile, st->env, st->client, config) : NULL;
    if (!session) {
        provider_profile_free(profile);
        outcome->failure_reason = str_dup("Cannot create agent session");
        return NULL;
    }

    RunCapture capture = {0};
    agent_session_on_event(session, capture_event, &capture);
    char *input = stage_prompt(prompt, stage_dir);
    if (input) agent_session_submit(session, input);
    else capture.error = str_dup("Prompt allocation failed");
    free(input);

    char *text = NULL;
    if (!session->failed && session->history_count &&
        session->history[session->history_count - 1]->role == ROLE_ASSISTANT)
        text = message_text(session->history[session->history_count - 1]);
    if (!text)
        outcome->failure_reason = str_dup(capture.error ? capture.error : "Agent session failed");

    free(capture.error);
    agent_session_free(session);
    provider_profile_free(profile);
    return text;
}

CodergenBackend *agent_codergen_backend_new(LlmClient *client, ExecutionEnv *env,
                                            const char *model, const char *provider) {
    if (!client || !env) return NULL;
    CodergenBackend *b = mem_calloc(1, sizeof(*b));
    AgentBackendState *st = mem_calloc(1, sizeof(*st));
    if (!b || !st) { free(b); free(st); return NULL; }
    st->client = client;
    st->env = env;
    st->model = model ? str_dup(model) : NULL;
    st->provider = provider ? str_dup(provider) : NULL;
    b->impl = st;
    b->run = agent_backend_run;
    if ((model && !st->model) || (provider && !st->provider)) { agent_codergen_backend_free(b); return NULL; }
    return b;
}

void agent_codergen_backend_free(CodergenBackend *b) {
    if (!b) return;
    AgentBackendState *st = b->impl;
    if (st) { free(st->model); free(st->provider); free(st); }
    free(b);
}
