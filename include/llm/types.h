#ifndef LLM_TYPES_H
#define LLM_TYPES_H

#include <stddef.h>
#include <stdbool.h>

/*============================================================================
 * Enums
 *==========================================================================*/

typedef enum {
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL,
    ROLE_DEVELOPER
} Role;

typedef enum {
    CONTENT_TEXT,
    CONTENT_IMAGE,
    CONTENT_AUDIO,
    CONTENT_DOCUMENT,
    CONTENT_TOOL_CALL,
    CONTENT_TOOL_RESULT,
    CONTENT_THINKING,
    CONTENT_REDACTED_THINKING
} ContentKind;

typedef enum {
    FINISH_STOP,
    FINISH_LENGTH,
    FINISH_TOOL_CALLS,
    FINISH_CONTENT_FILTER,
    FINISH_ERROR,
    FINISH_OTHER
} FinishReasonKind;

typedef enum {
    TOOL_CHOICE_AUTO,
    TOOL_CHOICE_NONE,
    TOOL_CHOICE_REQUIRED,
    TOOL_CHOICE_NAMED
} ToolChoiceMode;

typedef enum {
    STREAM_EVT_STREAM_START,
    STREAM_EVT_TEXT_START,
    STREAM_EVT_TEXT_DELTA,
    STREAM_EVT_TEXT_END,
    STREAM_EVT_REASONING_START,
    STREAM_EVT_REASONING_DELTA,
    STREAM_EVT_REASONING_END,
    STREAM_EVT_TOOL_CALL_START,
    STREAM_EVT_TOOL_CALL_DELTA,
    STREAM_EVT_TOOL_CALL_END,
    STREAM_EVT_FINISH,
    STREAM_EVT_ERROR,
    STREAM_EVT_PROVIDER_EVENT
} StreamEventType;

typedef enum {
    REASONING_NONE,
    REASONING_LOW,
    REASONING_MEDIUM,
    REASONING_HIGH
} ReasoningEffort;

/*============================================================================
 * Data structures
 *==========================================================================*/

typedef struct {
    char   *id;
    char   *name;
    char   *arguments_json;     /* raw JSON string of arguments */
} ToolCallData;

typedef struct {
    char   *tool_call_id;
    char   *content;
    bool    is_error;
} ToolResultData;

typedef struct {
    char   *text;
    char   *signature;
    bool    redacted;
} ThinkingData;

typedef struct {
    char   *url;
    char   *data_base64;
    char   *media_type;
    char   *detail;
} ImageData;

typedef struct {
    ContentKind     kind;
    char           *text;           /* for TEXT */
    ImageData      *image;          /* for IMAGE */
    ToolCallData   *tool_call;      /* for TOOL_CALL */
    ToolResultData *tool_result;    /* for TOOL_RESULT */
    ThinkingData   *thinking;       /* for THINKING/REDACTED_THINKING */
} ContentPart;

typedef struct {
    Role          role;
    ContentPart **parts;
    size_t        part_count;
    char         *name;
    char         *tool_call_id;
} Message;

typedef struct {
    FinishReasonKind reason;
    char            *raw;
} FinishReason;

typedef struct {
    int  input_tokens;
    int  output_tokens;
    int  total_tokens;
    int  reasoning_tokens;      /* -1 if not available */
    int  cache_read_tokens;     /* -1 if not available */
    int  cache_write_tokens;    /* -1 if not available */
} Usage;

typedef struct {
    char *name;
    char *description;
    char *parameters_json;      /* JSON Schema string */
} ToolDefinition;

typedef struct {
    ToolChoiceMode mode;
    char          *tool_name;   /* for NAMED mode */
} ToolChoice;

typedef struct {
    char          *type;        /* "text", "json", "json_schema" */
    char          *json_schema; /* JSON string */
    bool           strict;
} ResponseFormat;

typedef struct {
    char          *id;
    char          *name;
    char          *arguments_json;
    char          *raw_arguments;
} ToolCall;

typedef struct {
    char          *tool_call_id;
    char          *content;
    bool           is_error;
} ToolResult;

/*============================================================================
 * Request / Response
 *==========================================================================*/

typedef struct {
    char            *model;
    Message        **messages;
    size_t           message_count;
    char            *provider;          /* NULL for default */
    ToolDefinition **tools;
    size_t           tool_count;
    ToolChoice      *tool_choice;
    ResponseFormat  *response_format;
    double           temperature;       /* <0 = unset */
    double           top_p;             /* <0 = unset */
    int              max_tokens;        /* 0 = unset */
    char           **stop_sequences;
    size_t           stop_count;
    ReasoningEffort  reasoning_effort;
    char            *provider_options_json;  /* raw JSON escape hatch */
} LlmRequest;

typedef struct {
    char         *id;
    char         *model;
    char         *provider;
    Message      *message;          /* the assistant response message */
    FinishReason  finish_reason;
    Usage         usage;
    char         *raw_json;         /* raw provider response */

    /* convenience extracted fields */
    char         *text;             /* concatenated text from parts */
    char         *reasoning;        /* concatenated reasoning text */
    ToolCall    **tool_calls;
    size_t        tool_call_count;
} LlmResponse;

typedef struct {
    StreamEventType  type;
    char            *delta;
    char            *reasoning_delta;
    ToolCall        *tool_call;
    FinishReason    *finish_reason;
    Usage           *usage;
    LlmResponse    *response;       /* full response on FINISH */
    char            *error_message;
    char            *raw_json;
} StreamEvent;

/*============================================================================
 * Error types
 *==========================================================================*/

typedef enum {
    LLM_OK = 0,
    LLM_ERR_PROVIDER,
    LLM_ERR_AUTH,
    LLM_ERR_ACCESS_DENIED,
    LLM_ERR_NOT_FOUND,
    LLM_ERR_INVALID_REQUEST,
    LLM_ERR_RATE_LIMIT,
    LLM_ERR_SERVER,
    LLM_ERR_CONTENT_FILTER,
    LLM_ERR_CONTEXT_LENGTH,
    LLM_ERR_QUOTA,
    LLM_ERR_TIMEOUT,
    LLM_ERR_ABORT,
    LLM_ERR_NETWORK,
    LLM_ERR_STREAM,
    LLM_ERR_INVALID_TOOL_CALL,
    LLM_ERR_NO_OBJECT,
    LLM_ERR_CONFIG
} LlmErrorCode;

typedef struct {
    LlmErrorCode  code;
    char         *message;
    char         *provider;
    int           status_code;
    bool          retryable;
    double        retry_after;      /* seconds, <0 if unset */
    char         *raw_json;
} LlmError;

/*============================================================================
 * Memory management
 *==========================================================================*/

void        llm_request_free(LlmRequest *r);
void        llm_response_free(LlmResponse *r);
void        llm_error_free(LlmError *e);
void        message_free(Message *m);
void        content_part_free(ContentPart *p);
void        tool_call_free(ToolCall *tc);
void        tool_result_free(ToolResult *tr);
void        tool_definition_free(ToolDefinition *td);
void        stream_event_free(StreamEvent *ev);
Usage       usage_add(Usage a, Usage b);

/* Message constructors */
Message    *message_system(const char *text);
Message    *message_user(const char *text);
Message    *message_assistant(const char *text);
Message    *message_tool_result(const char *call_id, const char *content, bool is_error);

/* Extract text from a message */
char       *message_text(const Message *m);

#endif
