/*============================================================================
 * engine.c  --  Pipeline execution engine for Attractor
 *
 * Implements every function declared in attractor/engine.h.
 * Pure C11.  No platform threads (parallel handler is sequential).
 *==========================================================================*/

#include "attractor/engine.h"
#include "attractor/dot_parser.h"
#include "attractor/validator.h"
#include "util/json.h"
#include "util/str.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

/* ── Forward declarations for internal helpers ────────────────────────── */

static const char *status_to_string(StageStatus s);
static StageStatus string_to_status(const char *s);
static void        emit_event(PipelineRunner *r, PipelineEventKind kind,
                              const char *node_id, const char *data, int attempt);
static const DotEdge *select_edge(const DotGraph *graph, const char *node_id,
                                  const Outcome *outcome,
                                  const PipelineContext *ctx);
static const DotNode *check_goal_gates(const DotGraph *graph,
                                       const char **ids,
                                       const Outcome *outcomes,
                                       size_t count);
static Outcome execute_with_retry(PipelineRunner *runner, Handler *handler,
                                  const DotNode *node,
                                  PipelineContext *ctx,
                                  const DotGraph *graph,
                                  const char *logs_root,
                                  void *handler_data, int max_retries);

/* Built-in handler functions */
static Outcome start_handler(const DotNode *node, PipelineContext *ctx,
                             const DotGraph *graph, const char *logs_root,
                             void *data);
static Outcome exit_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data);
static Outcome conditional_handler(const DotNode *node, PipelineContext *ctx,
                                   const DotGraph *graph, const char *logs_root,
                                   void *data);
static Outcome codergen_handler(const DotNode *node, PipelineContext *ctx,
                                const DotGraph *graph, const char *logs_root,
                                void *data);
static Outcome wait_human_handler(const DotNode *node, PipelineContext *ctx,
                                  const DotGraph *graph, const char *logs_root,
                                  void *data);
static Outcome tool_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data);
static Outcome parallel_handler(const DotNode *node, PipelineContext *ctx,
                                const DotGraph *graph, const char *logs_root,
                                void *data);
static Outcome fan_in_handler(const DotNode *node, PipelineContext *ctx,
                              const DotGraph *graph, const char *logs_root,
                              void *data);
static Outcome manager_loop_handler(const DotNode *node, PipelineContext *ctx,
                                    const DotGraph *graph, const char *logs_root,
                                    void *data);

/* Internal: noop handler for the default fallback */
static Outcome noop_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data);

/* Internal: shape-to-type mapping */
static const char *shape_to_type(const char *shape);

/* Internal: write spec-compliant status.json per Appendix C */
static void write_outcome_status(const char *logs_root, const char *node_id,
                                 const Outcome *o);

/* ── 1. Outcome ──────────────────────────────────────────────────────── */

void outcome_free(Outcome *o)
{
    if (!o) return;
    free(o->preferred_label);
    o->preferred_label = NULL;
    free(o->notes);
    o->notes = NULL;
    free(o->failure_reason);
    o->failure_reason = NULL;

    for (size_t i = 0; i < o->suggested_next_count; i++)
        free(o->suggested_next_ids[i]);
    free(o->suggested_next_ids);
    o->suggested_next_ids = NULL;
    o->suggested_next_count = 0;

    for (size_t i = 0; i < o->update_count; i++) {
        free(o->update_keys[i]);
        free(o->update_values[i]);
    }
    free(o->update_keys);
    free(o->update_values);
    o->update_keys = NULL;
    o->update_values = NULL;
    o->update_count = 0;
}

/* ── 2. PipelineContext ──────────────────────────────────────────────── */

void ctx_init(PipelineContext *c)
{
    memset(c, 0, sizeof(*c));
}

void ctx_free(PipelineContext *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) {
        free(c->keys[i]);
        free(c->values[i]);
    }
    free(c->keys);
    free(c->values);
    for (size_t i = 0; i < c->log_count; i++)
        free(c->logs[i]);
    free(c->logs);
    memset(c, 0, sizeof(*c));
}

void ctx_set(PipelineContext *c, const char *key, const char *value)
{
    /* Look for existing key */
    for (size_t i = 0; i < c->count; i++) {
        if (str_eq(c->keys[i], key)) {
            free(c->values[i]);
            c->values[i] = str_dup(value);
            return;
        }
    }
    /* Append new k/v */
    if (c->count >= c->cap) {
        size_t new_cap = c->cap ? c->cap * 2 : 8;
        c->keys   = realloc(c->keys,   new_cap * sizeof(char *));
        c->values = realloc(c->values,  new_cap * sizeof(char *));
        c->cap    = new_cap;
    }
    c->keys[c->count]   = str_dup(key);
    c->values[c->count]  = str_dup(value);
    c->count++;
}

const char *ctx_get(const PipelineContext *c, const char *key, const char *def)
{
    for (size_t i = 0; i < c->count; i++) {
        if (str_eq(c->keys[i], key))
            return c->values[i];
    }
    return def;
}

void ctx_apply_updates(PipelineContext *c, const Outcome *o)
{
    for (size_t i = 0; i < o->update_count; i++)
        ctx_set(c, o->update_keys[i], o->update_values[i]);
}

PipelineContext *ctx_clone(const PipelineContext *c)
{
    PipelineContext *dup = calloc(1, sizeof(PipelineContext));
    if (!dup) return NULL;
    ctx_init(dup);
    for (size_t i = 0; i < c->count; i++)
        ctx_set(dup, c->keys[i], c->values[i]);
    for (size_t i = 0; i < c->log_count; i++)
        ctx_append_log(dup, c->logs[i]);
    return dup;
}

void ctx_append_log(PipelineContext *c, const char *entry)
{
    if (c->log_count >= c->log_cap) {
        size_t new_cap = c->log_cap ? c->log_cap * 2 : 8;
        c->logs    = realloc(c->logs, new_cap * sizeof(char *));
        c->log_cap = new_cap;
    }
    c->logs[c->log_count++] = str_dup(entry);
}

/* ── Context fidelity preamble ────────────────────────────────────────── */

char *ctx_to_preamble(const PipelineContext *ctx, const char *fidelity)
{
    if (!ctx) return str_dup("");

    /* Default to full */
    if (!fidelity || !*fidelity || str_eq(fidelity, "full")) {
        /* Full: dump all key=value pairs */
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_cstr(&sb, "## Context\n\n");
        for (size_t i = 0; i < ctx->count; i++) {
            strbuf_appendf(&sb, "- **%s**: %s\n",
                           ctx->keys[i], ctx->values[i]);
        }
        if (ctx->log_count > 0) {
            strbuf_append_cstr(&sb, "\n## Log\n\n");
            for (size_t i = 0; i < ctx->log_count; i++)
                strbuf_appendf(&sb, "- %s\n", ctx->logs[i]);
        }
        return strbuf_detach(&sb);
    }

    if (str_eq(fidelity, "truncate")) {
        /* Truncate: last 200 chars per value */
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_cstr(&sb, "## Context (truncated)\n\n");
        for (size_t i = 0; i < ctx->count; i++) {
            const char *val = ctx->values[i];
            size_t vlen = val ? strlen(val) : 0;
            if (vlen > 200) {
                strbuf_appendf(&sb, "- **%s**: ...%s\n",
                               ctx->keys[i], val + vlen - 200);
            } else {
                strbuf_appendf(&sb, "- **%s**: %s\n",
                               ctx->keys[i], val ? val : "");
            }
        }
        return strbuf_detach(&sb);
    }

    if (str_eq(fidelity, "compact")) {
        /* Compact: structured summary, skip internal keys and long values */
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_cstr(&sb, "## Context (compact)\n\n");
        for (size_t i = 0; i < ctx->count; i++) {
            /* Skip internal tracking keys */
            if (str_starts_with(ctx->keys[i], "internal.")) continue;
            const char *val = ctx->values[i];
            size_t vlen = val ? strlen(val) : 0;
            if (vlen > 80) {
                strbuf_appendf(&sb, "%s=%.77s...\n", ctx->keys[i], val);
            } else {
                strbuf_appendf(&sb, "%s=%s\n", ctx->keys[i], val ? val : "");
            }
        }
        return strbuf_detach(&sb);
    }

    /* summary:low, summary:medium, summary:high —
     * These ideally use LLM to generate a summary. Without an LLM call
     * available at this layer, fall back to compact mode. */
    if (str_starts_with(fidelity, "summary")) {
        /* Use compact as a fallback for summary modes */
        return ctx_to_preamble(ctx, "compact");
    }

    /* Unknown fidelity: default to full */
    return ctx_to_preamble(ctx, "full");
}

/* ── 3. Checkpoint ───────────────────────────────────────────────────── */

void checkpoint_save(const Checkpoint *cp, const char *path)
{
    JsonValue *root = json_new_object();
    json_object_set(root, "current_node",
                    json_new_string(str_safe(cp->current_node)));

    JsonValue *arr = json_new_array();
    for (size_t i = 0; i < cp->completed_count; i++)
        json_array_push(arr, json_new_string(cp->completed_nodes[i]));
    json_object_set(root, "completed_nodes", arr);

    JsonValue *ctx_obj = json_new_object();
    for (size_t i = 0; i < cp->context.count; i++)
        json_object_set(ctx_obj, cp->context.keys[i],
                        json_new_string(cp->context.values[i]));
    json_object_set(root, "context", ctx_obj);

    /* Store node outcome statuses keyed by "outcomes.<node_id>" in context.
     * These are already in the context from the engine loop via
     * ctx_set(ctx, "outcome", ...) — the last one is there at least.
     * For per-node outcomes, the engine stores the status in the context's
     * internal.retry_count keys and the outcome is in the status.json files. */

    char *json_str = json_serialize(root);
    json_free(root);

    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json_str, f);
        fclose(f);
    }
    free(json_str);
}

bool checkpoint_load(Checkpoint *cp, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return false; }

    char *buf = malloc((size_t)len + 1);
    size_t rd = fread(buf, 1, (size_t)len, f);
    buf[rd] = '\0';
    fclose(f);

    const char *err = NULL;
    JsonValue *root = json_parse(buf, &err);
    free(buf);
    if (!root) return false;

    memset(cp, 0, sizeof(*cp));
    ctx_init(&cp->context);

    const char *cn = json_get_string(root, "current_node");
    cp->current_node = cn ? str_dup(cn) : NULL;

    JsonValue *arr = json_get(root, "completed_nodes");
    if (arr && arr->type == JSON_ARRAY) {
        cp->completed_count = arr->array.count;
        cp->completed_nodes = calloc(cp->completed_count, sizeof(char *));
        for (size_t i = 0; i < cp->completed_count; i++) {
            JsonValue *item = json_array_get(arr, i);
            cp->completed_nodes[i] =
                (item && item->type == JSON_STRING) ? str_dup(item->string) : str_dup("");
        }
    }

    JsonValue *ctx_obj = json_get(root, "context");
    if (ctx_obj && ctx_obj->type == JSON_OBJECT) {
        for (size_t i = 0; i < ctx_obj->object.count; i++)
            if (ctx_obj->object.values[i]->type == JSON_STRING)
                ctx_set(&cp->context, ctx_obj->object.keys[i],
                        ctx_obj->object.values[i]->string);
    }

    json_free(root);
    return true;
}

void checkpoint_free(Checkpoint *cp)
{
    if (!cp) return;
    free(cp->current_node);
    cp->current_node = NULL;
    for (size_t i = 0; i < cp->completed_count; i++)
        free(cp->completed_nodes[i]);
    free(cp->completed_nodes);
    cp->completed_nodes = NULL;
    cp->completed_count = 0;
    ctx_free(&cp->context);
}

/* ── 4. Condition Evaluator ──────────────────────────────────────────── */

static const char *status_to_string(StageStatus s)
{
    switch (s) {
    case STAGE_SUCCESS:         return "success";
    case STAGE_FAIL:            return "fail";
    case STAGE_RETRY:           return "retry";
    case STAGE_PARTIAL_SUCCESS: return "partial_success";
    case STAGE_SKIPPED:         return "skipped";
    }
    return "unknown";
}

static StageStatus string_to_status(const char *s)
{
    if (str_eq(s, "success"))         return STAGE_SUCCESS;
    if (str_eq(s, "fail"))            return STAGE_FAIL;
    if (str_eq(s, "retry"))           return STAGE_RETRY;
    if (str_eq(s, "partial_success")) return STAGE_PARTIAL_SUCCESS;
    if (str_eq(s, "skipped"))         return STAGE_SKIPPED;
    return STAGE_FAIL;
}

static const char *resolve_key(const char *key, const Outcome *outcome,
                               const PipelineContext *ctx)
{
    static char status_buf[32];

    if (str_eq(key, "outcome")) {
        snprintf(status_buf, sizeof(status_buf), "%s",
                 status_to_string(outcome->status));
        return status_buf;
    }
    if (str_eq(key, "preferred_label"))
        return str_safe(outcome->preferred_label);

    if (str_starts_with(key, "context.")) {
        const char *sub = key + 8; /* skip "context." */
        return ctx_get(ctx, sub, "");
    }
    return ctx_get(ctx, key, "");
}

static bool eval_single_clause(const char *clause, const Outcome *outcome,
                               const PipelineContext *ctx)
{
    /* Work on a mutable trimmed copy */
    char *buf = str_dup(clause);
    char *trimmed = str_trim(buf);
    if (!trimmed || *trimmed == '\0') { free(buf); return true; }

    bool negate = false;
    char *sep = strstr(trimmed, "!=");
    if (sep) {
        negate = true;
    } else {
        sep = strchr(trimmed, '=');
    }

    if (!sep) {
        /* No operator: true if the key resolves to a truthy, non-empty string */
        const char *val = resolve_key(trimmed, outcome, ctx);
        bool result = (val && *val != '\0' &&
                       !str_eq(val, "false") && !str_eq(val, "0"));
        free(buf);
        return result;
    }

    /* Split on the operator */
    size_t key_len;
    char *rhs;
    if (negate) {
        key_len = (size_t)(sep - trimmed);
        rhs = sep + 2;
    } else {
        key_len = (size_t)(sep - trimmed);
        rhs = sep + 1;
    }

    char *lhs_key = str_ndup(trimmed, key_len);
    char *lhs_trimmed = str_trim(lhs_key);
    char *rhs_copy = str_dup(rhs);
    char *rhs_trimmed = str_trim(rhs_copy);

    const char *resolved = resolve_key(lhs_trimmed, outcome, ctx);
    bool match = str_eq(resolved, rhs_trimmed);

    free(lhs_key);
    free(rhs_copy);
    free(buf);

    return negate ? !match : match;
}

/*
 * Recursive descent condition evaluator.
 * Grammar:
 *   Expr     ::= OrExpr
 *   OrExpr   ::= AndExpr ( '||' AndExpr )*
 *   AndExpr  ::= Primary ( '&&' Primary )*
 *   Primary  ::= '(' Expr ')' | Clause
 */
typedef struct {
    const char       *src;
    size_t            pos;
    const Outcome    *outcome;
    const PipelineContext *ctx;
} CondParser;

static void cond_skip_ws(CondParser *cp) {
    while (cp->src[cp->pos] && strchr(" \t\r\n", cp->src[cp->pos]))
        cp->pos++;
}

static bool cond_parse_expr(CondParser *cp);

static bool cond_parse_clause(CondParser *cp) {
    cond_skip_ws(cp);
    /* Read until we hit &&, ||, ), or end */
    size_t start = cp->pos;
    while (cp->src[cp->pos] &&
           cp->src[cp->pos] != ')' &&
           !(cp->src[cp->pos] == '&' && cp->src[cp->pos+1] == '&') &&
           !(cp->src[cp->pos] == '|' && cp->src[cp->pos+1] == '|'))
        cp->pos++;
    char *clause = str_ndup(cp->src + start, cp->pos - start);
    bool result = eval_single_clause(clause, cp->outcome, cp->ctx);
    free(clause);
    return result;
}

static bool cond_parse_primary(CondParser *cp) {
    cond_skip_ws(cp);
    if (cp->src[cp->pos] == '(') {
        cp->pos++; /* skip '(' */
        bool result = cond_parse_expr(cp);
        cond_skip_ws(cp);
        if (cp->src[cp->pos] == ')') cp->pos++; /* skip ')' */
        return result;
    }
    return cond_parse_clause(cp);
}

static bool cond_parse_and(CondParser *cp) {
    bool result = cond_parse_primary(cp);
    while (1) {
        cond_skip_ws(cp);
        if (cp->src[cp->pos] == '&' && cp->src[cp->pos+1] == '&') {
            cp->pos += 2;
            bool rhs = cond_parse_primary(cp);
            result = result && rhs;
        } else {
            break;
        }
    }
    return result;
}

static bool cond_parse_expr(CondParser *cp) {
    bool result = cond_parse_and(cp);
    while (1) {
        cond_skip_ws(cp);
        if (cp->src[cp->pos] == '|' && cp->src[cp->pos+1] == '|') {
            cp->pos += 2;
            bool rhs = cond_parse_and(cp);
            result = result || rhs;
        } else {
            break;
        }
    }
    return result;
}

bool evaluate_condition(const char *condition, const Outcome *outcome,
                        const PipelineContext *ctx)
{
    if (!condition || *condition == '\0') return true;
    CondParser cp = { .src = condition, .pos = 0,
                      .outcome = outcome, .ctx = ctx };
    return cond_parse_expr(&cp);
}

/* ── 5. Handler Registry ─────────────────────────────────────────────── */

void handler_registry_init(HandlerRegistry *reg)
{
    memset(reg, 0, sizeof(*reg));
}

void handler_registry_free(HandlerRegistry *reg)
{
    for (size_t i = 0; i < reg->count; i++) {
        free(reg->handlers[i]->type_name);
        free(reg->handlers[i]);
    }
    free(reg->handlers);
    if (reg->default_handler) {
        free(reg->default_handler->type_name);
        free(reg->default_handler);
    }
    memset(reg, 0, sizeof(*reg));
}

void handler_registry_register(HandlerRegistry *reg, const char *type,
                               HandlerFn fn, void *data)
{
    /* Replace if exists */
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->handlers[i]->type_name, type)) {
            reg->handlers[i]->execute = fn;
            reg->handlers[i]->data    = data;
            return;
        }
    }
    /* Append */
    if (reg->count >= reg->cap) {
        size_t new_cap = reg->cap ? reg->cap * 2 : 8;
        reg->handlers = realloc(reg->handlers, new_cap * sizeof(Handler *));
        reg->cap = new_cap;
    }
    Handler *h = calloc(1, sizeof(Handler));
    h->type_name = str_dup(type);
    h->execute   = fn;
    h->data      = data;
    reg->handlers[reg->count++] = h;
}

static const char *shape_to_type(const char *shape)
{
    if (!shape) return NULL;
    if (str_eq(shape, "Mdiamond"))       return "start";
    if (str_eq(shape, "Msquare"))        return "exit";
    if (str_eq(shape, "box"))            return "codergen";
    if (str_eq(shape, "hexagon"))        return "wait.human";
    if (str_eq(shape, "diamond"))        return "conditional";
    if (str_eq(shape, "component"))      return "parallel";
    if (str_eq(shape, "tripleoctagon"))  return "parallel.fan_in";
    if (str_eq(shape, "parallelogram"))  return "tool";
    if (str_eq(shape, "house"))          return "stack.manager_loop";
    return NULL;
}

Handler *handler_registry_resolve(const HandlerRegistry *reg, const DotNode *node)
{
    /* 1. Check explicit node->type attribute */
    if (node->type && *node->type) {
        for (size_t i = 0; i < reg->count; i++)
            if (str_eq(reg->handlers[i]->type_name, node->type))
                return reg->handlers[i];
    }
    /* 2. Map shape to type */
    const char *mapped = shape_to_type(node->shape);
    if (mapped) {
        for (size_t i = 0; i < reg->count; i++)
            if (str_eq(reg->handlers[i]->type_name, mapped))
                return reg->handlers[i];
    }
    /* 3. Default handler */
    return reg->default_handler;
}

/* ── 6. Built-in Handlers ────────────────────────────────────────────── */

static Outcome start_handler(const DotNode *node, PipelineContext *ctx,
                             const DotGraph *graph, const char *logs_root,
                             void *data)
{
    (void)node; (void)ctx; (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));
    o.status = STAGE_SUCCESS;
    return o;
}

static Outcome exit_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data)
{
    (void)node; (void)ctx; (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));
    o.status = STAGE_SUCCESS;
    return o;
}

static Outcome conditional_handler(const DotNode *node, PipelineContext *ctx,
                                   const DotGraph *graph, const char *logs_root,
                                   void *data)
{
    (void)ctx; (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));
    o.status = STAGE_SUCCESS;

    StrBuf sb;
    strbuf_init(&sb);
    strbuf_appendf(&sb, "Conditional node evaluated: %s", str_safe(node->id));
    o.notes = strbuf_detach(&sb);
    return o;
}

static void mkdirs(const char *path)
{
    char *tmp = str_dup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
}

static char *expand_goal(const char *input, const char *goal)
{
    if (!input) return str_dup("");
    if (!goal)  return str_dup(input);

    StrBuf sb;
    strbuf_init(&sb);
    const char *p = input;
    while (*p) {
        const char *found = strstr(p, "$goal");
        if (!found) {
            strbuf_append_cstr(&sb, p);
            break;
        }
        strbuf_append(&sb, p, (size_t)(found - p));
        strbuf_append_cstr(&sb, goal);
        p = found + 5; /* strlen("$goal") */
    }
    return strbuf_detach(&sb);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content ? content : "", f);
        fclose(f);
    }
}

/* Write spec-compliant status.json per Appendix C:
 * { "outcome": "...", "preferred_next_label": "...",
 *   "suggested_next_ids": [...], "context_updates": {...}, "notes": "..." } */
static void write_outcome_status(const char *logs_root, const char *node_id,
                                 const Outcome *o)
{
    if (!logs_root || !node_id || !o) return;

    StrBuf dir_buf;
    strbuf_init(&dir_buf);
    strbuf_appendf(&dir_buf, "%s/%s", logs_root, node_id);
    char *dir_path = strbuf_detach(&dir_buf);
    mkdirs(dir_path);

    JsonValue *root = json_new_object();
    json_object_set(root, "outcome",
                    json_new_string(status_to_string(o->status)));

    if (o->preferred_label)
        json_object_set(root, "preferred_next_label",
                        json_new_string(o->preferred_label));

    if (o->suggested_next_count > 0) {
        JsonValue *arr = json_new_array();
        for (size_t i = 0; i < o->suggested_next_count; i++)
            json_array_push(arr, json_new_string(o->suggested_next_ids[i]));
        json_object_set(root, "suggested_next_ids", arr);
    }

    if (o->update_count > 0) {
        JsonValue *updates = json_new_object();
        for (size_t i = 0; i < o->update_count; i++)
            json_object_set(updates, o->update_keys[i],
                            json_new_string(o->update_values[i]));
        json_object_set(root, "context_updates", updates);
    }

    if (o->notes)
        json_object_set(root, "notes", json_new_string(o->notes));

    if (o->failure_reason)
        json_object_set(root, "failure_reason",
                        json_new_string(o->failure_reason));

    char *json_str = json_serialize(root);
    json_free(root);

    StrBuf path_buf;
    strbuf_init(&path_buf);
    strbuf_appendf(&path_buf, "%s/status.json", dir_path);
    char *path = strbuf_detach(&path_buf);
    write_file(path, json_str);

    free(path);
    free(json_str);
    free(dir_path);
}

static Outcome codergen_handler(const DotNode *node, PipelineContext *ctx,
                                const DotGraph *graph, const char *logs_root,
                                void *data)
{
    PipelineRunner *runner = (PipelineRunner *)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    /* Resolve fidelity: node > graph default > "full" */
    const char *fidelity = node->fidelity;
    if (!fidelity || !*fidelity) fidelity = graph->default_fidelity;
    if (!fidelity || !*fidelity) fidelity = "full";

    /* Build prompt with context preamble */
    const char *raw_prompt = (node->prompt && *node->prompt) ? node->prompt : node->label;
    char *base_prompt = expand_goal(raw_prompt, graph->goal);

    /* Prepend context preamble based on fidelity mode */
    char *preamble = ctx_to_preamble(ctx, fidelity);
    StrBuf full_prompt;
    strbuf_init(&full_prompt);
    if (preamble && *preamble) {
        strbuf_append_cstr(&full_prompt, preamble);
        strbuf_append_cstr(&full_prompt, "\n---\n\n");
    }
    strbuf_append_cstr(&full_prompt, base_prompt);
    char *prompt = strbuf_detach(&full_prompt);
    free(preamble);
    free(base_prompt);

    /* Ensure log directory */
    StrBuf dir;
    strbuf_init(&dir);
    strbuf_appendf(&dir, "%s/%s", str_safe(logs_root), str_safe(node->id));
    char *dir_path = strbuf_detach(&dir);
    mkdirs(dir_path);

    /* Write prompt */
    StrBuf path_buf;
    strbuf_init(&path_buf);
    strbuf_appendf(&path_buf, "%s/prompt.md", dir_path);
    char *prompt_path = strbuf_detach(&path_buf);
    write_file(prompt_path, prompt);
    free(prompt_path);

    /* Execute via backend or simulate */
    char *response = NULL;
    if (runner && runner->backend) {
        response = runner->backend->run(runner->backend, node, prompt, ctx);
        if (!response) {
            o.status = STAGE_FAIL;
            o.failure_reason = str_dup("Backend returned NULL response");
            free(prompt);
            free(dir_path);
            return o;
        }
    } else {
        /* Simulated response */
        StrBuf sim;
        strbuf_init(&sim);
        strbuf_appendf(&sim, "[simulated] Response for node '%s'", str_safe(node->id));
        response = strbuf_detach(&sim);
    }

    /* Write response */
    strbuf_init(&path_buf);
    strbuf_appendf(&path_buf, "%s/response.md", dir_path);
    char *resp_path = strbuf_detach(&path_buf);
    write_file(resp_path, response);
    free(resp_path);

    /* Build outcome */
    o.status = STAGE_SUCCESS;

    /* Context updates: last_stage, last_response, and per-node full response.
     * last_response is capped at 16 KB for checkpoint size sanity.
     * response.<node_id> carries the full text (capped at 64 KB) so
     * downstream nodes can access any predecessor's complete output. */
    o.update_count  = 3;
    o.update_keys   = calloc(3, sizeof(char *));
    o.update_values = calloc(3, sizeof(char *));
    o.update_keys[0]   = str_dup("last_stage");
    o.update_values[0] = str_dup(str_safe(node->id));
    o.update_keys[1]   = str_dup("last_response");

    size_t resp_len = strlen(response);
    if (resp_len > 16384)
        o.update_values[1] = str_ndup(response, 16384);
    else
        o.update_values[1] = str_dup(response);

    /* Per-node keyed response for multi-predecessor context flow */
    {
        StrBuf rk;
        strbuf_init(&rk);
        strbuf_appendf(&rk, "response.%s", str_safe(node->id));
        o.update_keys[2] = strbuf_detach(&rk);
    }
    if (resp_len > 65536)
        o.update_values[2] = str_ndup(response, 65536);
    else
        o.update_values[2] = str_dup(response);

    /* Write spec-compliant status.json */
    write_outcome_status(logs_root, node->id, &o);

    free(response);
    free(prompt);
    free(dir_path);
    return o;
}

/* Parse accelerator key from a label such as "[K] Label", "K) Label",
   "K - Label", or just take the first character. Returns the key char. */
static char parse_accelerator(const char *label)
{
    if (!label || !*label) return '\0';
    /* [K] ... */
    if (label[0] == '[' && label[1] && label[2] == ']')
        return label[1];
    /* K) ... */
    if (label[0] && label[1] == ')')
        return label[0];
    /* K - ... */
    if (label[0] && label[1] == ' ' && label[2] == '-' && label[3] == ' ')
        return label[0];
    /* First char */
    return label[0];
}

/* Strip accelerator prefix patterns from a label for matching purposes. */
static char *strip_accelerator(const char *label)
{
    if (!label) return str_dup("");
    /* [K] Label */
    if (label[0] == '[' && label[1] && label[2] == ']') {
        const char *p = label + 3;
        while (*p == ' ') p++;
        return str_dup(p);
    }
    /* K) Label */
    if (label[0] && label[1] == ')') {
        const char *p = label + 2;
        while (*p == ' ') p++;
        return str_dup(p);
    }
    /* K - Label */
    if (label[0] && label[1] == ' ' && label[2] == '-' && label[3] == ' ') {
        return str_dup(label + 4);
    }
    return str_dup(label);
}

static Outcome wait_human_handler(const DotNode *node, PipelineContext *ctx,
                                  const DotGraph *graph, const char *logs_root,
                                  void *data)
{
    (void)ctx; (void)logs_root;
    PipelineRunner *runner = (PipelineRunner *)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    /* Get outgoing edges */
    const DotEdge *edges[64];
    size_t edge_count = dot_outgoing_edges(graph, node->id, edges, 64);

    /* Build question */
    Question q;
    memset(&q, 0, sizeof(q));
    q.text  = str_dup(node->label ? node->label : node->id);
    q.stage = str_dup(str_safe(node->id));
    q.type  = QUESTION_MULTIPLE_CHOICE;

    q.option_count = edge_count;
    q.options = calloc(edge_count, sizeof(QuestionOption));
    for (size_t i = 0; i < edge_count; i++) {
        const char *lbl = edges[i]->label ? edges[i]->label : edges[i]->to;
        char key_buf[2] = { parse_accelerator(lbl), '\0' };
        q.options[i].key   = str_dup(key_buf);
        q.options[i].label = str_dup(lbl);
    }

    /* Ask interviewer */
    Answer ans;
    memset(&ans, 0, sizeof(ans));
    ans.option_index = -1;

    if (runner && runner->interviewer) {
        emit_event(runner, PIPE_EVT_INTERVIEW_STARTED, node->id, NULL, 0);
        ans = runner->interviewer->ask(runner->interviewer, &q);
        emit_event(runner, PIPE_EVT_INTERVIEW_COMPLETED, node->id, NULL, 0);
    } else {
        /* Default: pick first option */
        if (edge_count > 0) {
            ans.option_index = 0;
            ans.value = str_dup(edges[0]->to);
        }
    }

    /* Find the selected edge and its target */
    const char *selected_target = NULL;
    const char *selected_label  = NULL;
    if (ans.option_index >= 0 && (size_t)ans.option_index < edge_count) {
        selected_target = edges[ans.option_index]->to;
        selected_label  = edges[ans.option_index]->label;
    } else if (ans.value) {
        /* Try matching the answer value to an edge target or label */
        for (size_t i = 0; i < edge_count; i++) {
            if (str_eq(ans.value, edges[i]->to) ||
                (edges[i]->label && str_eq(ans.value, edges[i]->label))) {
                selected_target = edges[i]->to;
                selected_label  = edges[i]->label;
                break;
            }
        }
    }

    o.status = STAGE_SUCCESS;
    if (selected_target) {
        o.suggested_next_count = 1;
        o.suggested_next_ids = calloc(1, sizeof(char *));
        o.suggested_next_ids[0] = str_dup(selected_target);
        if (selected_label)
            o.preferred_label = str_dup(selected_label);
    }

    /* Cleanup */
    answer_free(&ans);
    for (size_t i = 0; i < q.option_count; i++) {
        free(q.options[i].key);
        free(q.options[i].label);
    }
    free(q.options);
    free(q.text);
    free(q.stage);

    return o;
}

/* Parse a timeout string like "30s", "5m", "300" (seconds) */
static int parse_timeout_seconds(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    long val = strtol(s, &end, 10);
    if (val <= 0) return 0;
    if (end && (*end == 'm' || *end == 'M'))
        return (int)(val * 60);
    /* default: seconds (handles "s" suffix or bare number) */
    return (int)val;
}

#include <signal.h>
static volatile sig_atomic_t tool_timed_out = 0;
static void tool_alarm_handler(int sig) { (void)sig; tool_timed_out = 1; }

static Outcome tool_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data)
{
    (void)ctx; (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    const char *cmd = dot_node_attr(node, "tool_command", NULL);
    if (!cmd || !*cmd) {
        o.status = STAGE_FAIL;
        o.failure_reason = str_dup("No tool_command attribute on node");
        return o;
    }

    /* Parse timeout from node attribute */
    int timeout_sec = parse_timeout_seconds(node->timeout);

    /* Set up alarm for timeout */
    struct sigaction sa_old, sa_new;
    memset(&sa_new, 0, sizeof(sa_new));
    tool_timed_out = 0;
    if (timeout_sec > 0) {
        sa_new.sa_handler = tool_alarm_handler;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = 0;
        sigaction(SIGALRM, &sa_new, &sa_old);
        alarm((unsigned int)timeout_sec);
    }

    /* Execute via popen */
    FILE *proc = popen(cmd, "r");
    if (!proc) {
        if (timeout_sec > 0) {
            alarm(0);
            sigaction(SIGALRM, &sa_old, NULL);
        }
        o.status = STAGE_FAIL;
        o.failure_reason = str_dup("Failed to execute tool_command");
        return o;
    }

    StrBuf output;
    strbuf_init(&output);
    char buf[4096];
    while (fgets(buf, sizeof(buf), proc)) {
        if (tool_timed_out) break;
        strbuf_append_cstr(&output, buf);
    }

    int exit_code = pclose(proc);

    /* Restore alarm state */
    if (timeout_sec > 0) {
        alarm(0);
        sigaction(SIGALRM, &sa_old, NULL);
    }

    if (tool_timed_out) {
        strbuf_free(&output);
        o.status = STAGE_FAIL;
        StrBuf reason;
        strbuf_init(&reason);
        strbuf_appendf(&reason, "tool_command timed out after %ds", timeout_sec);
        o.failure_reason = strbuf_detach(&reason);
        return o;
    }

    char *out_str = strbuf_detach(&output);

    if (exit_code != 0) {
        o.status = STAGE_FAIL;
        StrBuf reason;
        strbuf_init(&reason);
        strbuf_appendf(&reason, "tool_command exited with code %d", exit_code);
        o.failure_reason = strbuf_detach(&reason);
        free(out_str);
        return o;
    }

    o.status = STAGE_SUCCESS;
    o.update_count  = 1;
    o.update_keys   = calloc(1, sizeof(char *));
    o.update_values = calloc(1, sizeof(char *));
    o.update_keys[0]   = str_dup("tool.output");
    o.update_values[0] = out_str;

    return o;
}

static Outcome parallel_handler(const DotNode *node, PipelineContext *ctx,
                                const DotGraph *graph, const char *logs_root,
                                void *data)
{
    PipelineRunner *runner = (PipelineRunner *)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    const DotEdge *edges[64];
    size_t edge_count = dot_outgoing_edges(graph, node->id, edges, 64);

    /* Read join/error policies from node attributes */
    const char *join_policy  = dot_node_attr(node, "join_policy",  "wait_all");
    const char *error_policy = dot_node_attr(node, "error_policy", "fail_fast");

    /* Execute each branch target sequentially (concurrent would need pthreads) */
    size_t success_count = 0;
    size_t fail_count    = 0;
    size_t total         = edge_count;

    /* Build a JSON array of results for context */
    JsonValue *results_arr = json_new_array();

    for (size_t i = 0; i < edge_count; i++) {
        DotNode *target = dot_find_node(graph, edges[i]->to);
        if (!target) { fail_count++; continue; }

        /* Resolve and execute handler for branch target */
        Handler *h = handler_registry_resolve(&runner->handler_reg, target);
        Outcome sub;
        memset(&sub, 0, sizeof(sub));
        if (h) {
            sub = h->execute(target, ctx, graph, logs_root, h->data);
        } else {
            sub.status = STAGE_FAIL;
            sub.failure_reason = str_dup("No handler for parallel branch");
        }

        /* Record result */
        JsonValue *entry = json_new_object();
        json_object_set(entry, "node_id", json_new_string(str_safe(target->id)));
        json_object_set(entry, "outcome", json_new_string(status_to_string(sub.status)));
        if (sub.notes)
            json_object_set(entry, "notes", json_new_string(sub.notes));
        json_array_push(results_arr, entry);

        /* Write status.json for branch node */
        write_outcome_status(logs_root, target->id, &sub);

        if (sub.status == STAGE_SUCCESS || sub.status == STAGE_PARTIAL_SUCCESS)
            success_count++;
        else
            fail_count++;

        /* Check error_policy */
        if (str_eq(error_policy, "fail_fast") && sub.status == STAGE_FAIL) {
            outcome_free(&sub);
            break;
        }

        outcome_free(&sub);
    }

    /* Store results in context */
    char *results_json = json_serialize(results_arr);
    json_free(results_arr);
    ctx_set(ctx, "parallel.results", results_json);
    free(results_json);

    /* Apply join_policy */
    if (str_eq(join_policy, "wait_all")) {
        o.status = (fail_count == 0) ? STAGE_SUCCESS :
                   (success_count > 0) ? STAGE_PARTIAL_SUCCESS : STAGE_FAIL;
    } else if (str_eq(join_policy, "first_success")) {
        o.status = (success_count > 0) ? STAGE_SUCCESS : STAGE_FAIL;
    } else if (str_eq(join_policy, "quorum")) {
        o.status = (success_count > total / 2) ? STAGE_SUCCESS : STAGE_FAIL;
    } else {
        /* k_of_n or unknown: default to wait_all */
        o.status = (fail_count == 0) ? STAGE_SUCCESS :
                   (success_count > 0) ? STAGE_PARTIAL_SUCCESS : STAGE_FAIL;
    }

    StrBuf note;
    strbuf_init(&note);
    strbuf_appendf(&note, "Parallel: %zu/%zu succeeded (join=%s, error=%s)",
                   success_count, total, join_policy, error_policy);
    o.notes = strbuf_detach(&note);

    return o;
}

static Outcome fan_in_handler(const DotNode *node, PipelineContext *ctx,
                              const DotGraph *graph, const char *logs_root,
                              void *data)
{
    (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    /* Read parallel.results from context */
    const char *results_json = ctx_get(ctx, "parallel.results", "");
    if (!results_json || !*results_json) {
        o.status = STAGE_SUCCESS;
        o.notes  = str_dup("Fan-in: no parallel results to evaluate");
        return o;
    }

    /* Parse results to find the best candidate */
    const char *parse_err = NULL;
    JsonValue *results = json_parse(results_json, &parse_err);
    if (!results || results->type != JSON_ARRAY) {
        json_free(results);
        o.status = STAGE_SUCCESS;
        o.notes  = str_dup("Fan-in: could not parse parallel results");
        return o;
    }

    /* Heuristic: pick the first successful result as "best" */
    const char *best_id = NULL;
    const char *best_outcome = NULL;
    for (size_t i = 0; i < results->array.count; i++) {
        JsonValue *entry = json_array_get(results, i);
        const char *out = json_get_string(entry, "outcome");
        const char *nid = json_get_string(entry, "node_id");
        if (out && (str_eq(out, "success") || str_eq(out, "partial_success"))) {
            best_id      = nid;
            best_outcome = out;
            break;
        }
    }
    if (!best_id && results->array.count > 0) {
        /* No success; pick the first */
        JsonValue *entry = json_array_get(results, 0);
        best_id      = json_get_string(entry, "node_id");
        best_outcome = json_get_string(entry, "outcome");
    }

    /* Set context keys */
    if (best_id) {
        ctx_set(ctx, "parallel.fan_in.best_id", best_id);
        if (best_outcome)
            ctx_set(ctx, "parallel.fan_in.best_outcome", best_outcome);
    }

    /* If node has a prompt, we'd call LLM to rank candidates.
     * For now, use heuristic only. */
    (void)node;

    json_free(results);
    o.status = STAGE_SUCCESS;
    o.notes  = str_dup(best_id ? best_id : "fan-in complete");
    if (best_id) {
        o.suggested_next_count = 1;
        o.suggested_next_ids = calloc(1, sizeof(char *));
        o.suggested_next_ids[0] = str_dup(best_id);
    }
    return o;
}

static Outcome manager_loop_handler(const DotNode *node, PipelineContext *ctx,
                                    const DotGraph *graph, const char *logs_root,
                                    void *data)
{
    (void)graph; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));

    /* Read manager loop configuration from node attributes */
    const char *child_dotfile = dot_node_attr(node, "stack.child_dotfile", NULL);
    int max_cycles = 1;
    {
        const char *mc_str = dot_node_attr(node, "manager.max_cycles", "1");
        max_cycles = atoi(mc_str);
        if (max_cycles <= 0) max_cycles = 1;
    }

    if (!child_dotfile || !*child_dotfile) {
        /* No child dotfile: act as a simple pass-through */
        o.status = STAGE_SUCCESS;
        o.notes  = str_dup("Manager loop: no child_dotfile configured");
        return o;
    }

    /* Read the child DOT file */
    FILE *f = fopen(child_dotfile, "r");
    if (!f) {
        o.status = STAGE_FAIL;
        StrBuf reason;
        strbuf_init(&reason);
        strbuf_appendf(&reason, "Cannot open child dotfile: %s", child_dotfile);
        o.failure_reason = strbuf_detach(&reason);
        return o;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *child_src = malloc((size_t)fsize + 1);
    fread(child_src, 1, (size_t)fsize, f);
    child_src[fsize] = '\0';
    fclose(f);

    /* Run the child pipeline for max_cycles */
    int cycle;
    for (cycle = 0; cycle < max_cycles; cycle++) {
        char *child_err = NULL;
        DotGraph *child_graph = dot_parse(child_src, &child_err);
        if (!child_graph) {
            o.status = STAGE_FAIL;
            o.failure_reason = child_err ? child_err : str_dup("Child parse error");
            free(child_src);
            return o;
        }

        /* Apply transforms */
        transform_expand_variables(child_graph);
        transform_apply_stylesheet(child_graph);

        /* Create child logs directory */
        StrBuf child_logs;
        strbuf_init(&child_logs);
        strbuf_appendf(&child_logs, "%s/%s/cycle_%d",
                        logs_root, str_safe(node->id), cycle);
        char *cl = strbuf_detach(&child_logs);
        mkdirs(cl);

        /* Create and run child runner */
        PipelineRunner *child_runner = pipeline_runner_new(child_graph, cl);
        pipeline_register_builtin_handlers(child_runner);

        Outcome child_result = pipeline_run(child_runner);

        /* Store child status in context */
        ctx_set(ctx, "stack.child.status", status_to_string(child_result.status));

        pipeline_runner_free(child_runner);
        dot_graph_free(child_graph);
        free(cl);

        if (child_result.status == STAGE_SUCCESS) {
            outcome_free(&child_result);
            break;
        }
        outcome_free(&child_result);
    }

    free(child_src);
    o.status = STAGE_SUCCESS;
    StrBuf note;
    strbuf_init(&note);
    strbuf_appendf(&note, "Manager loop completed after %d cycle(s)", cycle + 1);
    o.notes = strbuf_detach(&note);
    return o;
}

static Outcome noop_handler(const DotNode *node, PipelineContext *ctx,
                            const DotGraph *graph, const char *logs_root,
                            void *data)
{
    (void)node; (void)ctx; (void)graph; (void)logs_root; (void)data;
    Outcome o;
    memset(&o, 0, sizeof(o));
    o.status = STAGE_SUCCESS;
    o.notes  = str_dup("Handled by default (noop) handler");
    return o;
}

/* ── 7. Register Built-in Handlers ───────────────────────────────────── */

void pipeline_register_builtin_handlers(PipelineRunner *r)
{
    handler_registry_register(&r->handler_reg, "start",
                              start_handler, r);
    handler_registry_register(&r->handler_reg, "exit",
                              exit_handler, r);
    handler_registry_register(&r->handler_reg, "conditional",
                              conditional_handler, r);
    handler_registry_register(&r->handler_reg, "codergen",
                              codergen_handler, r);
    handler_registry_register(&r->handler_reg, "wait.human",
                              wait_human_handler, r);
    handler_registry_register(&r->handler_reg, "tool",
                              tool_handler, r);
    handler_registry_register(&r->handler_reg, "parallel",
                              parallel_handler, r);
    handler_registry_register(&r->handler_reg, "parallel.fan_in",
                              fan_in_handler, r);
    handler_registry_register(&r->handler_reg, "stack.manager_loop",
                              manager_loop_handler, r);

    /* Default handler as a fallback */
    Handler *def = calloc(1, sizeof(Handler));
    def->type_name = str_dup("_default");
    def->execute   = noop_handler;
    def->data      = r;
    r->handler_reg.default_handler = def;
}

/* ── 8. Edge Selection Algorithm ─────────────────────────────────────── */

/* Normalize a label for matching: lowercase, trim, strip accelerator prefix. */
static char *normalize_label(const char *label)
{
    if (!label) return str_dup("");
    char *stripped = strip_accelerator(label);
    char *trimmed  = str_trim(stripped);
    char *lowered  = str_lower(str_dup(trimmed));
    free(stripped);
    return lowered;
}

static int compare_edges_weight_lexical(const DotEdge *a, const DotEdge *b)
{
    /* Higher weight is better */
    if (a->weight != b->weight) return (b->weight - a->weight);
    /* Lexical tiebreak on target */
    return strcmp(str_safe(a->to), str_safe(b->to));
}

static const DotEdge *best_edge(const DotEdge **edges, size_t count)
{
    if (count == 0) return NULL;
    const DotEdge *best = edges[0];
    for (size_t i = 1; i < count; i++) {
        if (compare_edges_weight_lexical(best, edges[i]) > 0)
            best = edges[i];
    }
    return best;
}

static const DotEdge *select_edge(const DotGraph *graph, const char *node_id,
                                  const Outcome *outcome,
                                  const PipelineContext *ctx)
{
    const DotEdge *all_edges[128];
    size_t count = dot_outgoing_edges(graph, node_id, all_edges, 128);
    if (count == 0) return NULL;

    /* Step 1: Conditional edges whose condition evaluates true */
    const DotEdge *cond_true[128];
    size_t cond_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (all_edges[i]->condition && *all_edges[i]->condition) {
            if (evaluate_condition(all_edges[i]->condition, outcome, ctx))
                cond_true[cond_count++] = all_edges[i];
        }
    }
    if (cond_count > 0)
        return best_edge(cond_true, cond_count);

    /* Step 2: Preferred label matching */
    if (outcome->preferred_label && *outcome->preferred_label) {
        char *norm_pref = normalize_label(outcome->preferred_label);
        for (size_t i = 0; i < count; i++) {
            if (all_edges[i]->label) {
                char *norm_edge = normalize_label(all_edges[i]->label);
                bool match = str_eq(norm_pref, norm_edge);
                free(norm_edge);
                if (match) { free(norm_pref); return all_edges[i]; }
            }
        }
        free(norm_pref);
    }

    /* Step 3: Suggested next IDs */
    for (size_t s = 0; s < outcome->suggested_next_count; s++) {
        for (size_t i = 0; i < count; i++) {
            if (str_eq(all_edges[i]->to, outcome->suggested_next_ids[s]))
                return all_edges[i];
        }
    }

    /* Step 4+5: All remaining edges by weight, then lexical tiebreak */
    return best_edge(all_edges, count);
}

/* ── 9. Goal Gate Checking ───────────────────────────────────────────── */

static const DotNode *check_goal_gates(const DotGraph *graph,
                                       const char **ids,
                                       const Outcome *outcomes,
                                       size_t count)
{
    for (size_t i = 0; i < count; i++) {
        DotNode *node = dot_find_node(graph, ids[i]);
        if (!node) continue;
        if (!node->goal_gate) continue;
        StageStatus st = outcomes[i].status;
        if (st != STAGE_SUCCESS && st != STAGE_PARTIAL_SUCCESS)
            return node;
    }
    return NULL;
}

/* ── 10. Retry Logic ─────────────────────────────────────────────────── */

static Outcome execute_with_retry(PipelineRunner *runner, Handler *handler,
                                  const DotNode *node,
                                  PipelineContext *ctx,
                                  const DotGraph *graph,
                                  const char *logs_root,
                                  void *handler_data, int max_retries)
{
    Outcome o;
    memset(&o, 0, sizeof(o));

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        o = handler->execute(node, ctx, graph, logs_root, handler_data);

        if (o.status == STAGE_SUCCESS || o.status == STAGE_PARTIAL_SUCCESS)
            return o;

        if (o.status == STAGE_FAIL)
            return o;

        if (o.status == STAGE_RETRY && attempt < max_retries) {
            /* Exponential backoff: 100ms * 2^attempt, capped at 5s */
            double delay = 0.1 * pow(2.0, (double)attempt);
            if (delay > 5.0) delay = 5.0;

            emit_event(runner, PIPE_EVT_STAGE_RETRYING, node->id,
                       NULL, attempt + 1);

            struct timespec ts;
            ts.tv_sec  = (time_t)delay;
            ts.tv_nsec = (long)((delay - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);

            /* Free this attempt's outcome before retrying */
            outcome_free(&o);
            memset(&o, 0, sizeof(o));
            continue;
        }

        /* RETRY but no attempts left */
        if (o.status == STAGE_RETRY && node->allow_partial) {
            outcome_free(&o);
            memset(&o, 0, sizeof(o));
            o.status = STAGE_PARTIAL_SUCCESS;
            o.notes = str_dup("Retries exhausted, partial accepted");
            return o;
        }
        break;
    }
    return o;
}

/* ── 11. Pipeline Runner lifecycle ───────────────────────────────────── */

PipelineRunner *pipeline_runner_new(DotGraph *graph, const char *logs_root)
{
    PipelineRunner *r = calloc(1, sizeof(PipelineRunner));
    r->graph    = graph;
    r->logs_root = str_dup(logs_root ? logs_root : "./logs");
    handler_registry_init(&r->handler_reg);
    return r;
}

void pipeline_runner_free(PipelineRunner *r)
{
    if (!r) return;
    handler_registry_free(&r->handler_reg);
    free(r->logs_root);
    if (r->interviewer)
        interviewer_free(r->interviewer);
    /* Note: graph and backend are not owned by runner */
    free(r);
}

void pipeline_runner_set_backend(PipelineRunner *r, CodergenBackend *b)
{
    r->backend = b;
}

void pipeline_runner_set_interviewer(PipelineRunner *r, Interviewer *iv)
{
    r->interviewer = iv;
}

void pipeline_runner_on_event(PipelineRunner *r, PipelineEventCallback cb,
                              void *ud)
{
    r->event_cb       = cb;
    r->event_userdata = ud;
}

/* ── 17. Event emission helper ───────────────────────────────────────── */

static void emit_event(PipelineRunner *r, PipelineEventKind kind,
                       const char *node_id, const char *data, int attempt)
{
    if (!r || !r->event_cb) return;
    PipelineEvent ev;
    ev.kind    = kind;
    ev.data    = data    ? str_dup(data)    : NULL;
    ev.node_id = node_id ? str_dup(node_id) : NULL;
    ev.attempt = attempt;
    r->event_cb(&ev, r->event_userdata);
    free(ev.data);
    free(ev.node_id);
}

/* ── 12. pipeline_run  (THE CORE LOOP) ───────────────────────────────── */

/* Internal core that runs from a given starting state. */
static Outcome pipeline_run_core(PipelineRunner *r,
                                 const char *start_node_id,
                                 PipelineContext *ctx,
                                 char **completed_ids,
                                 Outcome *completed_outcomes,
                                 size_t completed_count)
{
    Outcome final_outcome;
    memset(&final_outcome, 0, sizeof(final_outcome));

    /* Dynamic arrays for completion tracking */
    size_t comp_cap = completed_count > 0 ? completed_count * 2 : 16;
    char    **comp_ids  = calloc(comp_cap, sizeof(char *));
    Outcome  *comp_outs = calloc(comp_cap, sizeof(Outcome));
    size_t    comp_cnt  = 0;

    /* Copy pre-existing completions */
    for (size_t i = 0; i < completed_count; i++) {
        comp_ids[i]  = str_dup(completed_ids[i]);
        memset(&comp_outs[i], 0, sizeof(Outcome));
        comp_outs[i].status = completed_outcomes[i].status;
        comp_cnt++;
    }

    char *current_id = str_dup(start_node_id);
    int iteration_limit = 10000; /* safety valve */

    while (iteration_limit-- > 0) {
        DotNode *node = dot_find_node(r->graph, current_id);
        if (!node) {
            final_outcome.status = STAGE_FAIL;
            final_outcome.failure_reason = str_dup("Node not found in graph");
            break;
        }

        /* (a) Check if terminal node (Msquare = exit) */
        if (node->shape && str_eq(node->shape, "Msquare")) {
            /* Check goal gates */
            const DotNode *gate = check_goal_gates(
                r->graph,
                (const char **)comp_ids, comp_outs, comp_cnt);
            if (gate) {
                /* Try retry_target */
                const char *retry = gate->retry_target;
                if (!retry || !*retry)
                    retry = r->graph->retry_target;
                if (!retry || !*retry)
                    retry = gate->fallback_retry_target;
                if (!retry || !*retry)
                    retry = r->graph->fallback_retry_target;

                if (retry && *retry) {
                    free(current_id);
                    current_id = str_dup(retry);
                    continue;
                }
                /* No retry target, fail */
                final_outcome.status = STAGE_FAIL;
                StrBuf fb;
                strbuf_init(&fb);
                strbuf_appendf(&fb, "Goal gate unsatisfied at node '%s'",
                               str_safe(gate->id));
                final_outcome.failure_reason = strbuf_detach(&fb);
                break;
            }
            /* Execute exit handler then succeed */
            Handler *h = handler_registry_resolve(&r->handler_reg, node);
            if (h) {
                Outcome exit_o = h->execute(node, ctx, r->graph,
                                            r->logs_root, h->data);
                outcome_free(&exit_o);
            }
            final_outcome.status = STAGE_SUCCESS;
            final_outcome.notes  = str_dup("Pipeline completed successfully");
            emit_event(r, PIPE_EVT_PIPELINE_COMPLETED, current_id, NULL, 0);
            break;
        }

        /* (b) Resolve handler */
        Handler *handler = handler_registry_resolve(&r->handler_reg, node);
        if (!handler) {
            final_outcome.status = STAGE_FAIL;
            StrBuf fb;
            strbuf_init(&fb);
            strbuf_appendf(&fb, "No handler found for node '%s'",
                           str_safe(node->id));
            final_outcome.failure_reason = strbuf_detach(&fb);
            break;
        }

        /* (c) Execute with retry */
        emit_event(r, PIPE_EVT_STAGE_STARTED, node->id, NULL, 0);

        int max_retries = node->max_retries;
        if (max_retries < 0)   /* not set on node: use graph default */
            max_retries = r->graph->default_max_retry;
        if (max_retries < 0)
            max_retries = 0;

        Outcome step_outcome = execute_with_retry(
            r, handler, node, ctx, r->graph, r->logs_root,
            handler->data, max_retries);

        /* (d) Record completion */
        if (comp_cnt >= comp_cap) {
            comp_cap *= 2;
            comp_ids  = realloc(comp_ids,  comp_cap * sizeof(char *));
            comp_outs = realloc(comp_outs, comp_cap * sizeof(Outcome));
        }
        comp_ids[comp_cnt]  = str_dup(node->id);
        /* Store a lightweight copy of status */
        memset(&comp_outs[comp_cnt], 0, sizeof(Outcome));
        comp_outs[comp_cnt].status = step_outcome.status;
        comp_cnt++;

        /* Apply context updates */
        ctx_apply_updates(ctx, &step_outcome);
        ctx_set(ctx, "outcome", status_to_string(step_outcome.status));
        ctx_set(ctx, "current_node", node->id);
        if (step_outcome.preferred_label)
            ctx_set(ctx, "preferred_label", step_outcome.preferred_label);

        /* Log */
        {
            StrBuf log_entry;
            strbuf_init(&log_entry);
            strbuf_appendf(&log_entry, "[%s] status=%s",
                           str_safe(node->id),
                           status_to_string(step_outcome.status));
            char *le = strbuf_detach(&log_entry);
            ctx_append_log(ctx, le);
            free(le);
        }

        /* Write status.json for every non-terminal node */
        write_outcome_status(r->logs_root, node->id, &step_outcome);

        /* Emit stage event */
        if (step_outcome.status == STAGE_SUCCESS ||
            step_outcome.status == STAGE_PARTIAL_SUCCESS)
            emit_event(r, PIPE_EVT_STAGE_COMPLETED, node->id, NULL, 0);
        else
            emit_event(r, PIPE_EVT_STAGE_FAILED, node->id,
                       step_outcome.failure_reason, 0);

        /* (e) Save checkpoint */
        {
            Checkpoint cp;
            memset(&cp, 0, sizeof(cp));
            cp.current_node    = current_id;
            cp.completed_nodes = comp_ids;
            cp.completed_count = comp_cnt;
            cp.context         = *ctx;

            StrBuf cp_path;
            strbuf_init(&cp_path);
            strbuf_appendf(&cp_path, "%s/checkpoint.json", str_safe(r->logs_root));
            char *cp_path_str = strbuf_detach(&cp_path);

            mkdirs(r->logs_root);
            checkpoint_save(&cp, cp_path_str);
            emit_event(r, PIPE_EVT_CHECKPOINT_SAVED, node->id, cp_path_str, 0);
            free(cp_path_str);

            /* Reset cp without freeing owned data */
            cp.current_node    = NULL;
            cp.completed_nodes = NULL;
            cp.completed_count = 0;
            memset(&cp.context, 0, sizeof(cp.context));
        }

        /* (f) Select next edge */
        const DotEdge *edge = select_edge(r->graph, node->id,
                                          &step_outcome, ctx);
        if (!edge) {
            if (step_outcome.status == STAGE_FAIL) {
                final_outcome.status         = STAGE_FAIL;
                final_outcome.failure_reason  = step_outcome.failure_reason
                    ? str_dup(step_outcome.failure_reason)
                    : str_dup("Stage failed with no outgoing edge");
                emit_event(r, PIPE_EVT_PIPELINE_FAILED, node->id, NULL, 0);
                outcome_free(&step_outcome);
                break;
            }
            /* No edge and not fail: treat as completion */
            final_outcome.status = step_outcome.status;
            final_outcome.notes  = step_outcome.notes
                ? str_dup(step_outcome.notes) : NULL;
            outcome_free(&step_outcome);
            break;
        }

        /* (g) loop_restart on edge */
        if (edge->loop_restart) {
            outcome_free(&step_outcome);
            /* Re-run from this same node (simplified: continue loop) */
            continue;
        }

        /* (h) Advance to next node */
        free(current_id);
        current_id = str_dup(edge->to);

        outcome_free(&step_outcome);
    }

    /* Cleanup */
    free(current_id);
    for (size_t i = 0; i < comp_cnt; i++)
        free(comp_ids[i]);
    free(comp_ids);
    free(comp_outs);

    return final_outcome;
}

Outcome pipeline_run(PipelineRunner *r)
{
    Outcome fail;
    memset(&fail, 0, sizeof(fail));

    if (!r || !r->graph) {
        fail.status = STAGE_FAIL;
        fail.failure_reason = str_dup("Runner or graph is NULL");
        return fail;
    }

    /* Init context */
    PipelineContext ctx;
    ctx_init(&ctx);

    /* Mirror graph-level attributes into context */
    if (r->graph->goal)
        ctx_set(&ctx, "graph.goal", r->graph->goal);
    if (r->graph->label)
        ctx_set(&ctx, "graph.label", r->graph->label);
    if (r->graph->default_fidelity)
        ctx_set(&ctx, "graph.default_fidelity", r->graph->default_fidelity);

    /* Find start node (shape = Mdiamond) */
    DotNode *start = NULL;
    for (size_t i = 0; i < r->graph->node_count; i++) {
        if (r->graph->nodes[i].shape &&
            str_eq(r->graph->nodes[i].shape, "Mdiamond")) {
            start = &r->graph->nodes[i];
            break;
        }
    }
    if (!start) {
        ctx_free(&ctx);
        fail.status = STAGE_FAIL;
        fail.failure_reason = str_dup("No start node (Mdiamond) found in graph");
        return fail;
    }

    emit_event(r, PIPE_EVT_PIPELINE_STARTED, start->id, NULL, 0);

    Outcome result = pipeline_run_core(r, start->id, &ctx, NULL, NULL, 0);
    ctx_free(&ctx);
    return result;
}

/* ── 13. pipeline_resume ─────────────────────────────────────────────── */

Outcome pipeline_resume(PipelineRunner *r, const char *checkpoint_path)
{
    Outcome fail;
    memset(&fail, 0, sizeof(fail));

    if (!r || !r->graph) {
        fail.status = STAGE_FAIL;
        fail.failure_reason = str_dup("Runner or graph is NULL");
        return fail;
    }

    Checkpoint cp;
    if (!checkpoint_load(&cp, checkpoint_path)) {
        fail.status = STAGE_FAIL;
        fail.failure_reason = str_dup("Failed to load checkpoint");
        return fail;
    }

    /* Restore context */
    PipelineContext ctx;
    ctx_init(&ctx);
    for (size_t i = 0; i < cp.context.count; i++)
        ctx_set(&ctx, cp.context.keys[i], cp.context.values[i]);
    for (size_t i = 0; i < cp.context.log_count; i++)
        ctx_append_log(&ctx, cp.context.logs[i]);

    /* Build lightweight outcome array from completed nodes.
     * Try to read each node's status.json for actual outcome. */
    Outcome *comp_outcomes = NULL;
    if (cp.completed_count > 0) {
        comp_outcomes = calloc(cp.completed_count, sizeof(Outcome));
        for (size_t i = 0; i < cp.completed_count; i++) {
            comp_outcomes[i].status = STAGE_SUCCESS; /* default */

            /* Try reading status.json from the logs directory */
            StrBuf sp;
            strbuf_init(&sp);
            strbuf_appendf(&sp, "%s/%s/status.json",
                           str_safe(r->logs_root),
                           cp.completed_nodes[i]);
            char *spath = strbuf_detach(&sp);

            FILE *sf = fopen(spath, "r");
            if (sf) {
                fseek(sf, 0, SEEK_END);
                long slen = ftell(sf);
                fseek(sf, 0, SEEK_SET);
                if (slen > 0) {
                    char *sbuf = malloc((size_t)slen + 1);
                    size_t srd = fread(sbuf, 1, (size_t)slen, sf);
                    sbuf[srd] = '\0';
                    const char *serr = NULL;
                    JsonValue *sroot = json_parse(sbuf, &serr);
                    if (sroot) {
                        const char *outcome_str = json_get_string(sroot, "outcome");
                        if (outcome_str)
                            comp_outcomes[i].status = string_to_status(outcome_str);
                        json_free(sroot);
                    }
                    free(sbuf);
                }
                fclose(sf);
            }
            free(spath);
        }
    }

    /* Select the NEXT node after the checkpoint's current node */
    const DotEdge *next_edge = NULL;
    if (cp.current_node) {
        /* Create a dummy "success" outcome for edge selection */
        Outcome dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.status = STAGE_SUCCESS;
        next_edge = select_edge(r->graph, cp.current_node, &dummy, &ctx);
    }

    char *resume_node;
    if (next_edge) {
        resume_node = str_dup(next_edge->to);
    } else {
        /* Fall back to current node itself */
        resume_node = str_dup(str_safe(cp.current_node));
    }

    emit_event(r, PIPE_EVT_PIPELINE_STARTED, resume_node, "resumed", 0);

    Outcome result = pipeline_run_core(r, resume_node,
                                       &ctx,
                                       cp.completed_nodes,
                                       comp_outcomes,
                                       cp.completed_count);

    /* Cleanup */
    free(resume_node);
    free(comp_outcomes);
    ctx_free(&ctx);
    checkpoint_free(&cp);
    return result;
}

/* ── 14. Transforms ──────────────────────────────────────────────────── */

void transform_expand_variables(DotGraph *g)
{
    if (!g || !g->goal) return;
    for (size_t i = 0; i < g->node_count; i++) {
        DotNode *n = &g->nodes[i];
        if (n->prompt && strstr(n->prompt, "$goal")) {
            char *expanded = expand_goal(n->prompt, g->goal);
            free(n->prompt);
            n->prompt = expanded;
        }
    }
}

void transform_apply_stylesheet(DotGraph *g)
{
    if (!g || !g->model_stylesheet) return;
    char *err = NULL;
    Stylesheet *ss = stylesheet_parse(g->model_stylesheet, &err);
    if (!ss) {
        free(err);
        return;
    }
    stylesheet_apply(ss, g);
    stylesheet_free(ss);
}

/* ── 15. Stylesheet ──────────────────────────────────────────────────── */

/* Parse a stylesheet source.
 * Format:
 *   selector { prop: value; prop: value; }
 * Selectors: "*", ".classname", "#nodeid"
 */
Stylesheet *stylesheet_parse(const char *src, char **err)
{
    if (!src || !*src) {
        if (err) *err = str_dup("Empty stylesheet source");
        return NULL;
    }

    /* Temporary dynamic array of rules */
    size_t cap = 8;
    size_t cnt = 0;
    StyleRule *rules = calloc(cap, sizeof(StyleRule));

    const char *p = src;
    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Read selector */
        const char *sel_start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '{') p++;
        if (p == sel_start) { p++; continue; } /* skip junk */
        char *selector = str_ndup(sel_start, (size_t)(p - sel_start));

        /* Skip to '{' */
        while (*p && *p != '{') p++;
        if (*p == '{') p++;

        /* Determine specificity */
        int specificity = 0;
        if (selector[0] == '#')      specificity = 2;
        else if (selector[0] == '.') specificity = 1;
        else                         specificity = 0;

        /* Parse properties until '}' */
        char *llm_model = NULL;
        char *llm_provider = NULL;
        char *reasoning_effort = NULL;

        while (*p && *p != '}') {
            /* Skip whitespace */
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '}') break;

            /* Read property name */
            const char *prop_start = p;
            while (*p && *p != ':' && *p != '}') p++;
            if (*p != ':') { if (*p == '}') break; p++; continue; }
            char *prop = str_ndup(prop_start, (size_t)(p - prop_start));
            char *prop_trimmed = str_trim(prop);
            p++; /* skip ':' */

            /* Read value */
            while (*p && isspace((unsigned char)*p)) p++;
            const char *val_start = p;
            while (*p && *p != ';' && *p != '}') p++;
            char *val = str_ndup(val_start, (size_t)(p - val_start));
            char *val_trimmed = str_trim(val);

            if (*p == ';') p++;

            /* Assign to the right field */
            if (str_eq(prop_trimmed, "llm_model") || str_eq(prop_trimmed, "model")) {
                free(llm_model);
                llm_model = str_dup(val_trimmed);
            } else if (str_eq(prop_trimmed, "llm_provider") || str_eq(prop_trimmed, "provider")) {
                free(llm_provider);
                llm_provider = str_dup(val_trimmed);
            } else if (str_eq(prop_trimmed, "reasoning_effort") || str_eq(prop_trimmed, "reasoning")) {
                free(reasoning_effort);
                reasoning_effort = str_dup(val_trimmed);
            }

            free(prop);
            free(val);
        }
        if (*p == '}') p++;

        /* Only add rule if at least one property was set */
        if (llm_model || llm_provider || reasoning_effort) {
            if (cnt >= cap) {
                cap *= 2;
                rules = realloc(rules, cap * sizeof(StyleRule));
            }
            rules[cnt].selector         = selector;
            rules[cnt].llm_model        = llm_model;
            rules[cnt].llm_provider     = llm_provider;
            rules[cnt].reasoning_effort = reasoning_effort;
            rules[cnt].specificity      = specificity;
            cnt++;
        } else {
            free(selector);
            free(llm_model);
            free(llm_provider);
            free(reasoning_effort);
        }
    }

    Stylesheet *ss = calloc(1, sizeof(Stylesheet));
    ss->rules = rules;
    ss->count = cnt;
    if (err) *err = NULL;
    return ss;
}

void stylesheet_free(Stylesheet *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) {
        free(s->rules[i].selector);
        free(s->rules[i].llm_model);
        free(s->rules[i].llm_provider);
        free(s->rules[i].reasoning_effort);
    }
    free(s->rules);
    free(s);
}

/* Check whether a rule's selector matches a given node. */
static bool selector_matches(const char *selector, const DotNode *node)
{
    if (!selector) return false;
    if (str_eq(selector, "*")) return true;
    if (selector[0] == '#') {
        /* Match node id */
        return str_eq(selector + 1, str_safe(node->id));
    }
    if (selector[0] == '.') {
        /* Match node class_attr */
        if (!node->class_attr) return false;
        return str_eq(selector + 1, node->class_attr);
    }
    return false;
}

/* Compare rules by specificity for sorting (ascending) */
static int rule_cmp(const void *a, const void *b)
{
    const StyleRule *ra = (const StyleRule *)a;
    const StyleRule *rb = (const StyleRule *)b;
    return ra->specificity - rb->specificity;
}

void stylesheet_apply(const Stylesheet *s, DotGraph *g)
{
    if (!s || !g) return;

    /* Sort rules by specificity (lower first so higher specificity overrides) */
    StyleRule *sorted = calloc(s->count, sizeof(StyleRule));
    memcpy(sorted, s->rules, s->count * sizeof(StyleRule));
    qsort(sorted, s->count, sizeof(StyleRule), rule_cmp);

    for (size_t n = 0; n < g->node_count; n++) {
        DotNode *node = &g->nodes[n];
        /* Track what was set by explicit node attributes vs stylesheet.
         * We only apply stylesheet rules if the node doesn't have an
         * explicit attribute, but higher-specificity rules override
         * lower-specificity ones. */
        bool has_explicit_model    = (node->llm_model && *node->llm_model);
        bool has_explicit_provider = (node->llm_provider && *node->llm_provider);
        bool has_explicit_effort   = (node->reasoning_effort && *node->reasoning_effort);

        for (size_t r = 0; r < s->count; r++) {
            if (!selector_matches(sorted[r].selector, node))
                continue;
            /* Apply: higher specificity rules come later and override */
            if (sorted[r].llm_model && !has_explicit_model) {
                free(node->llm_model);
                node->llm_model = str_dup(sorted[r].llm_model);
            }
            if (sorted[r].llm_provider && !has_explicit_provider) {
                free(node->llm_provider);
                node->llm_provider = str_dup(sorted[r].llm_provider);
            }
            if (sorted[r].reasoning_effort && !has_explicit_effort) {
                free(node->reasoning_effort);
                node->reasoning_effort = str_dup(sorted[r].reasoning_effort);
            }
        }
    }

    free(sorted);
}

/* ── 16. Interviewer Implementations ─────────────────────────────────── */

/* --- Auto-approve interviewer --- */

static Answer auto_approve_ask(Interviewer *self, const Question *q)
{
    (void)self;
    Answer a;
    memset(&a, 0, sizeof(a));
    a.option_index = -1;

    switch (q->type) {
    case QUESTION_YES_NO:
    case QUESTION_CONFIRMATION:
        a.value = str_dup("yes");
        a.text  = str_dup("yes");
        a.option_index = 0;
        break;
    case QUESTION_MULTIPLE_CHOICE:
        if (q->option_count > 0) {
            a.option_index = 0;
            a.value = str_dup(q->options[0].key ? q->options[0].key : "");
            a.text  = str_dup(q->options[0].label ? q->options[0].label : "");
        }
        break;
    case QUESTION_FREEFORM:
        a.value = str_dup("");
        a.text  = str_dup("");
        break;
    }
    return a;
}

Interviewer *auto_approve_interviewer_new(void)
{
    Interviewer *iv = calloc(1, sizeof(Interviewer));
    iv->ask = auto_approve_ask;
    return iv;
}

/* --- Console interviewer --- */

static Answer console_ask(Interviewer *self, const Question *q)
{
    (void)self;
    Answer a;
    memset(&a, 0, sizeof(a));
    a.option_index = -1;

    printf("\n=== %s ===\n", str_safe(q->text));

    switch (q->type) {
    case QUESTION_YES_NO:
        printf("(yes/no): ");
        break;
    case QUESTION_CONFIRMATION:
        printf("(confirm? yes/no): ");
        break;
    case QUESTION_MULTIPLE_CHOICE:
        for (size_t i = 0; i < q->option_count; i++)
            printf("  [%s] %s\n", str_safe(q->options[i].key),
                   str_safe(q->options[i].label));
        printf("Choice: ");
        break;
    case QUESTION_FREEFORM:
        printf("Answer: ");
        break;
    }

    fflush(stdout);
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        a.value = str_dup("");
        a.text  = str_dup("");
        return a;
    }
    /* Trim newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    a.text  = str_dup(buf);
    a.value = str_dup(buf);

    /* For multiple choice, try to resolve option_index */
    if (q->type == QUESTION_MULTIPLE_CHOICE) {
        for (size_t i = 0; i < q->option_count; i++) {
            if ((q->options[i].key && str_eq(buf, q->options[i].key)) ||
                (q->options[i].label && str_eq(buf, q->options[i].label))) {
                a.option_index = (int)i;
                break;
            }
        }
    }

    return a;
}

Interviewer *console_interviewer_new(void)
{
    Interviewer *iv = calloc(1, sizeof(Interviewer));
    iv->ask = console_ask;
    return iv;
}

void interviewer_free(Interviewer *iv)
{
    if (!iv) return;
    /* If impl was dynamically allocated, the creator would need a custom free.
       For our built-in interviewers, impl is NULL, so just free the struct. */
    free(iv);
}

void answer_free(Answer *a)
{
    if (!a) return;
    free(a->value);
    a->value = NULL;
    free(a->text);
    a->text = NULL;
    a->option_index = -1;
}
