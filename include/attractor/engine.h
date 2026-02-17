#ifndef ATTRACTOR_ENGINE_H
#define ATTRACTOR_ENGINE_H

#include "attractor/dot_parser.h"
#include "attractor/validator.h"
#include "util/json.h"
#include <stdbool.h>
#include <stddef.h>

/*============================================================================
 * Stage Status / Outcome
 *==========================================================================*/

typedef enum {
    STAGE_SUCCESS,
    STAGE_PARTIAL_SUCCESS,
    STAGE_RETRY,
    STAGE_FAIL,
    STAGE_SKIPPED
} StageStatus;

typedef struct {
    StageStatus  status;
    char        *preferred_label;
    char       **suggested_next_ids;
    size_t       suggested_next_count;
    char        *notes;
    char        *failure_reason;

    /* Context updates: parallel arrays */
    char       **update_keys;
    char       **update_values;
    size_t       update_count;
} Outcome;

void outcome_free(Outcome *o);

/*============================================================================
 * Context
 *==========================================================================*/

typedef struct {
    char       **keys;
    char       **values;
    size_t       count;
    size_t       cap;
    char       **logs;
    size_t       log_count;
    size_t       log_cap;
} PipelineContext;

void        ctx_init(PipelineContext *c);
void        ctx_free(PipelineContext *c);
void        ctx_set(PipelineContext *c, const char *key, const char *value);
const char *ctx_get(const PipelineContext *c, const char *key, const char *def);
void        ctx_apply_updates(PipelineContext *c, const Outcome *o);
PipelineContext *ctx_clone(const PipelineContext *c);
void        ctx_append_log(PipelineContext *c, const char *entry);

/*============================================================================
 * Checkpoint
 *==========================================================================*/

typedef struct {
    char           *current_node;
    char          **completed_nodes;
    size_t          completed_count;
    PipelineContext context;
} Checkpoint;

void checkpoint_save(const Checkpoint *cp, const char *path);
bool checkpoint_load(Checkpoint *cp, const char *path);
void checkpoint_free(Checkpoint *cp);

/*============================================================================
 * Condition Expression Evaluator
 *==========================================================================*/

bool evaluate_condition(const char *condition, const Outcome *outcome,
                        const PipelineContext *ctx);

/*============================================================================
 * Handlers
 *==========================================================================*/

typedef Outcome (*HandlerFn)(const DotNode *node, PipelineContext *ctx,
                             const DotGraph *graph, const char *logs_root,
                             void *handler_data);

typedef struct {
    char      *type_name;
    HandlerFn  execute;
    void      *data;
} Handler;

typedef struct {
    Handler **handlers;
    size_t    count;
    size_t    cap;
    Handler  *default_handler;
} HandlerRegistry;

void     handler_registry_init(HandlerRegistry *reg);
void     handler_registry_free(HandlerRegistry *reg);
void     handler_registry_register(HandlerRegistry *reg, const char *type, HandlerFn fn, void *data);
Handler *handler_registry_resolve(const HandlerRegistry *reg, const DotNode *node);

/*============================================================================
 * Codergen Backend Interface
 *==========================================================================*/

typedef struct CodergenBackend CodergenBackend;

struct CodergenBackend {
    void *impl;
    /* Returns a result string or NULL on error. Caller frees. */
    char *(*run)(CodergenBackend *self, const DotNode *node,
                 const char *prompt, const PipelineContext *ctx);
};

/*============================================================================
 * Interviewer Interface
 *==========================================================================*/

typedef enum {
    QUESTION_YES_NO,
    QUESTION_MULTIPLE_CHOICE,
    QUESTION_FREEFORM,
    QUESTION_CONFIRMATION
} QuestionType;

typedef struct {
    char *key;
    char *label;
} QuestionOption;

typedef struct {
    char           *text;
    QuestionType    type;
    QuestionOption *options;
    size_t          option_count;
    char           *stage;
} Question;

typedef struct {
    char           *value;
    char           *text;
    int             option_index;   /* -1 if freeform */
} Answer;

typedef struct Interviewer Interviewer;
struct Interviewer {
    void *impl;
    Answer (*ask)(Interviewer *self, const Question *q);
};

Interviewer *auto_approve_interviewer_new(void);
Interviewer *console_interviewer_new(void);
void         interviewer_free(Interviewer *iv);
void         answer_free(Answer *a);

/*============================================================================
 * Pipeline Events
 *==========================================================================*/

typedef enum {
    PIPE_EVT_PIPELINE_STARTED,
    PIPE_EVT_PIPELINE_COMPLETED,
    PIPE_EVT_PIPELINE_FAILED,
    PIPE_EVT_STAGE_STARTED,
    PIPE_EVT_STAGE_COMPLETED,
    PIPE_EVT_STAGE_FAILED,
    PIPE_EVT_STAGE_RETRYING,
    PIPE_EVT_INTERVIEW_STARTED,
    PIPE_EVT_INTERVIEW_COMPLETED,
    PIPE_EVT_CHECKPOINT_SAVED
} PipelineEventKind;

typedef struct {
    PipelineEventKind kind;
    char             *data;
    char             *node_id;
    int               attempt;
} PipelineEvent;

typedef void (*PipelineEventCallback)(const PipelineEvent *ev, void *userdata);

/*============================================================================
 * Pipeline Runner
 *==========================================================================*/

typedef struct {
    DotGraph         *graph;
    HandlerRegistry   handler_reg;
    CodergenBackend  *backend;          /* may be NULL for simulation */
    Interviewer      *interviewer;
    char             *logs_root;
    PipelineEventCallback event_cb;
    void             *event_userdata;
} PipelineRunner;

/* Create a runner for a parsed & validated graph */
PipelineRunner *pipeline_runner_new(DotGraph *graph, const char *logs_root);
void            pipeline_runner_free(PipelineRunner *r);
void            pipeline_runner_set_backend(PipelineRunner *r, CodergenBackend *b);
void            pipeline_runner_set_interviewer(PipelineRunner *r, Interviewer *iv);
void            pipeline_runner_on_event(PipelineRunner *r, PipelineEventCallback cb, void *ud);

/* Register built-in handlers (start, exit, codergen, wait.human, conditional, parallel, fan-in, tool) */
void pipeline_register_builtin_handlers(PipelineRunner *r);

/* Run the pipeline. Returns the final outcome. */
Outcome pipeline_run(PipelineRunner *r);

/* Resume from a checkpoint */
Outcome pipeline_resume(PipelineRunner *r, const char *checkpoint_path);

/*============================================================================
 * Transforms
 *==========================================================================*/

typedef void (*TransformFn)(DotGraph *g);

/* Built-in transforms */
void transform_expand_variables(DotGraph *g);
void transform_apply_stylesheet(DotGraph *g);

/* Context fidelity: produce a preamble string filtered by fidelity mode.
 * Modes: "full", "truncate", "compact", "summary:low", "summary:medium", "summary:high"
 * Caller must free() the returned string. */
char *ctx_to_preamble(const PipelineContext *ctx, const char *fidelity);

/*============================================================================
 * Model Stylesheet
 *==========================================================================*/

typedef struct {
    char *selector;         /* "*", ".classname", "#nodeid" */
    char *llm_model;
    char *llm_provider;
    char *reasoning_effort;
    int   specificity;      /* 0=universal, 1=class, 2=id */
} StyleRule;

typedef struct {
    StyleRule *rules;
    size_t     count;
} Stylesheet;

Stylesheet *stylesheet_parse(const char *src, char **err);
void        stylesheet_free(Stylesheet *s);
void        stylesheet_apply(const Stylesheet *s, DotGraph *g);

#endif
