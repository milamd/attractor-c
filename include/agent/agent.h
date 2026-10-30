#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include "llm/client.h"
#include <stdbool.h>
#include <stddef.h>

/*============================================================================
 * Session Events
 *==========================================================================*/

typedef enum {
    AGENT_EVT_SESSION_START,        /* before the first input */
    AGENT_EVT_SESSION_END,          /* on close: abort, unrecoverable error or free */
    AGENT_EVT_USER_INPUT,
    AGENT_EVT_PROCESSING_END,       /* an input finished processing */
    AGENT_EVT_ASSISTANT_TEXT_START,
    AGENT_EVT_ASSISTANT_TEXT_DELTA,
    AGENT_EVT_ASSISTANT_TEXT_END,
    AGENT_EVT_TOOL_CALL_START,
    AGENT_EVT_TOOL_CALL_OUTPUT_DELTA,
    AGENT_EVT_TOOL_CALL_END,
    AGENT_EVT_STEERING_INJECTED,
    AGENT_EVT_TURN_LIMIT,
    AGENT_EVT_LOOP_DETECTION,
    AGENT_EVT_WARNING,
    AGENT_EVT_ERROR
} AgentEventKind;

typedef struct {
    AgentEventKind  kind;
    char           *session_id;
    char           *data;           /* event-specific payload (text, error, etc.) */
    char           *tool_name;
    char           *call_id;
} AgentEvent;

void agent_event_free(AgentEvent *ev);

/* Events and their fields are borrowed until the callback returns. */
typedef void (*AgentEventCallback)(const AgentEvent *event, void *userdata);

/*============================================================================
 * Execution Environment
 *==========================================================================*/

typedef struct ExecutionEnv ExecutionEnv;

#include "util/process.h"

struct ExecutionEnv {
    void *impl;
    const bool *cancel; /* borrowed, owner-thread cancellation */
    const char *const *extra_env; /* borrowed explicit subprocess additions */

    /* Raw owned text separately from line-numbered presentation; required for edits. */
    char *(*read_raw)(ExecutionEnv *self, const char *path);
    char *(*read_file)(ExecutionEnv *self, const char *path, int offset, int limit);
    bool  (*write_file)(ExecutionEnv *self, const char *path, const char *content);
    bool  (*delete_file)(ExecutionEnv *self, const char *path); /* may be NULL */
    /* Sorted listing, directories suffixed '/', recursing to depth (1 = entries
     * of path only). Owned text or NULL. May be NULL. */
    char *(*list_directory)(ExecutionEnv *self, const char *path, int depth);
    bool  (*file_exists)(ExecutionEnv *self, const char *path);

    ExecResult *(*exec_command)(ExecutionEnv *self, const char *cmd,
                                int timeout_ms, const char *working_dir);

    char *(*grep)(ExecutionEnv *self, const char *pattern, const char *path,
                  const char *glob_filter, bool case_insensitive, int max_results);

    char *(*glob)(ExecutionEnv *self, const char *pattern, const char *path);

    char *(*working_directory)(ExecutionEnv *self);
    char *(*platform)(ExecutionEnv *self);

    void  (*cleanup)(ExecutionEnv *self);
};

ExecutionEnv *local_exec_env_new(const char *working_dir);
ExecutionEnv *local_exec_env_new_policy(const char *working_dir, bool contained);
void          exec_env_free(ExecutionEnv *env);

/*============================================================================
 * Tool Registry
 *==========================================================================*/

typedef struct {
    ToolDefinition  def;
    /* Owned text; failure is explicit, independent of output text. */
    char *(*execute)(const char *args_json, ExecutionEnv *env, bool *is_error);
} RegisteredTool;

typedef struct {
    RegisteredTool  *tools;
    size_t           count;
    size_t           cap;
    bool             failed;
} ToolRegistry;

void            tool_registry_init(ToolRegistry *reg);
void            tool_registry_free(ToolRegistry *reg);
/* Registry consumes the tool and definition strings on success/failure. */
void            tool_registry_register(ToolRegistry *reg, RegisteredTool *tool);
void            tool_registry_unregister(ToolRegistry *reg, const char *name);
RegisteredTool *tool_registry_get(const ToolRegistry *reg, const char *name);

typedef struct {
    RegisteredTool *tool; /* borrowed */
    ExecutionEnv *env; /* borrowed */
    int default_timeout_ms;
    int max_timeout_ms;
} ToolExecutionContext;
/* Validates core tool arguments and returns an owned JSON result envelope
 * whose output is truncated with the default limits for that tool. */
char *agent_active_tool(const char *arguments_json, void *userdata, bool *is_error);

/* Per-tool truncation override (spec 5.2/5.3). Zero fields keep the tool
 * default; max_lines < 0 disables line truncation. Strings are borrowed. */
typedef struct {
    const char *tool;
    int max_chars;
    int max_lines;
} ToolOutputLimit;
/* Character (head_tail or tail) then line truncation with visible markers.
 * Returns an owned string, or NULL on allocation failure. O(n). */
char *agent_truncate_output(const char *output, const char *tool_name,
                            const ToolOutputLimit *overrides, size_t override_count);

/*============================================================================
 * Provider Profile
 *==========================================================================*/

typedef struct {
    char          *id;              /* "openai", "anthropic", "gemini" */
    char          *model;
    ToolRegistry   tool_registry;
    bool           supports_reasoning;
    bool           supports_streaming;
    bool           supports_parallel_tool_calls;
    int            context_window_size;
    int            default_command_timeout_ms; /* shell default; 0 = SessionConfig */

    char *(*build_system_prompt)(const char *env_info, const char *project_docs);
} ProviderProfile;

ProviderProfile *anthropic_profile_new(const char *model);
ProviderProfile *openai_profile_new(const char *model);
ProviderProfile *gemini_profile_new(const char *model);
void             provider_profile_free(ProviderProfile *p);

/*============================================================================
 * Session
 *==========================================================================*/

typedef enum {
    SESSION_IDLE,
    SESSION_PROCESSING,
    SESSION_AWAITING_INPUT,
    SESSION_CLOSED
} SessionState;

typedef struct {
    int  max_turns;                 /* 0 = unlimited */
    int  max_tool_rounds_per_input; /* per input; 0 = unlimited */
    int  default_command_timeout_ms;
    int  max_command_timeout_ms;
    ReasoningEffort reasoning_effort;
    bool enable_loop_detection;
    int  loop_detection_window;
    int  max_subagent_depth;
    const ToolOutputLimit *tool_output_limits; /* borrowed, may be NULL */
    size_t tool_output_limit_count;
} SessionConfig;

/* Forward declare for subagent support */
typedef struct AgentSession AgentSession;

struct AgentSession {
    char               *id;
    ProviderProfile    *profile;
    ExecutionEnv       *exec_env;
    LlmClient          *llm_client;
    SessionConfig       config;
    SessionState        state;
    AgentEventCallback  event_cb;
    void               *event_userdata;

    /* History stored as messages */
    Message           **history;
    size_t              history_count;
    size_t              history_cap;
    size_t              history_chars;

    /* Steering / follow-up queues */
    char              **steering_queue;
    size_t              steering_count;
    char              **followup_queue;
    size_t              followup_head;
    size_t              followup_count;

    bool                abort_signaled;
    bool                started; /* SESSION_START emitted */
    char               *project_docs; /* discovered at start (spec 6.5), may be NULL */
    size_t              turn_count;
    bool                failed;

    /* Subagents (spec 7): offered while depth < config.max_subagent_depth. */
    int                 depth;
    struct SubAgent   **subagents;
    size_t              subagent_count;
};

AgentSession *agent_session_new(ProviderProfile *profile, ExecutionEnv *env,
                                 LlmClient *client, SessionConfig config);
/* Emits SESSION_END first when the session started and has not closed. */
void          agent_session_free(AgentSession *s);

/* Submit user input and run the agentic loop */
void agent_session_submit(AgentSession *s, const char *input);

/* Sessions and callbacks are synchronous and confined to one owner thread.
 * Concurrent steering/abort is unsupported. Dependencies are borrowed. */
/* Inject steering message between tool rounds */
void agent_session_steer(AgentSession *s, const char *message);

/* Queue a follow-up message */
void agent_session_follow_up(AgentSession *s, const char *message);

/* Signal abort */
void agent_session_abort(AgentSession *s);

/* Set event callback */
void agent_session_on_event(AgentSession *s, AgentEventCallback cb, void *userdata);

/* Default session config */
SessionConfig agent_default_config(void);

/*============================================================================
 * Built-in Tools
 *==========================================================================*/

/* Register the core tool set (read_file, write_file, edit_file, shell, grep, glob) */
void agent_register_core_tools(ToolRegistry *reg);
/* Registers apply_patch (v4a format, coding-agent spec Appendix A). */
void agent_register_apply_patch(ToolRegistry *reg);
/* Applies a v4a patch through env. Every operation is verified before any file
 * changes. Returns an owned summary or "Error: ..." text (setting *is_error). */
char *agent_apply_patch(ExecutionEnv *env, const char *patch, bool *is_error);

#endif
