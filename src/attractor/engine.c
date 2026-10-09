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
#include "util/io.h"
#include "util/mem.h"
#include "util/process.h"
#include "agent/agent.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <poll.h>
#include <limits.h>

/* ── Forward declarations for internal helpers ────────────────────────── */

static const char *status_to_string(StageStatus s);
static StageStatus string_to_status(const char *s);
static void        emit_event(PipelineRunner *r, PipelineEventKind kind,
                              const char *node_id, const char *data, int attempt);
static const DotEdge *select_edge(const DotGraph *graph, const char *node_id,
                                  const Outcome *outcome,
                                  const PipelineContext *ctx);
static Outcome execute_with_retry(PipelineRunner *runner, Handler *handler,
                                  const DotNode *node,
                                  PipelineContext *ctx,
                                  const DotGraph *graph,
                                  const char *logs_root,
                                  void *handler_data, int max_retries);

static Outcome execute_graph(PipelineRunner *r,Checkpoint *cp,const char *boundary);
static bool initial_checkpoint(PipelineRunner *r,Checkpoint *cp,const char *start,const PipelineContext *context);
static char *graph_identity(const DotGraph *g);
static Outcome fail_outcome(const char *reason);

static int parse_timeout_seconds(const char *s);

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

/* Internal: write spec-compliant status.json per Appendix C */
static bool write_outcome_status(const char *logs_root, const char *node_id,
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
    if (!c || !key || !value) {if(c) c->failed=true;return;}
    size_t key_bytes=strnlen(key,65537),value_bytes=strnlen(value,ATTRACTOR_INPUT_LIMIT+1),replacement=c->count;
    if(key_bytes>65536 || value_bytes>ATTRACTOR_INPUT_LIMIT) {c->failed=true;return;}
    for(size_t i=0;i<c->count;i++) if(str_eq(c->keys[i],key)) {replacement=i;break;}
    size_t total=c->byte_count;
    if(replacement<c->count) total-=strlen(c->values[replacement]);else if(!size_add(total,key_bytes,&total)) {c->failed=true;return;}
    if(!size_add(total,value_bytes,&total) || total>ATTRACTOR_INPUT_LIMIT || (replacement==c->count && c->count>=10000)) {c->failed=true;return;}
    char *copy=str_dup(value);if(!copy) {c->failed=true;return;}
    if(replacement<c->count) {free(c->values[replacement]);c->values[replacement]=copy;c->byte_count=total;return;}
    char *owned_key=str_dup(key);if(!owned_key) {free(copy);c->failed=true;return;}
    if(c->count>=c->cap) {
        size_t cap=c->cap?c->cap*2:8;
        char **keys=mem_reallocarray(c->keys,cap,sizeof(*keys));
        if(!keys) {free(copy);free(owned_key);c->failed=true;return;}c->keys=keys;
        char **values=mem_reallocarray(c->values,cap,sizeof(*values));
        if(!values) {free(copy);free(owned_key);c->failed=true;return;}c->values=values;c->cap=cap;
    }
    c->keys[c->count]=owned_key;c->values[c->count++]=copy;c->byte_count=total;
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
    PipelineContext *dup = mem_calloc(1, sizeof(PipelineContext));
    if (!dup) return NULL;
    ctx_init(dup);
    for (size_t i = 0; i < c->count; i++)
        ctx_set(dup, c->keys[i], c->values[i]);
    for (size_t i = 0; i < c->log_count; i++)
        ctx_append_log(dup, c->logs[i]);
    if(dup->failed) {ctx_free(dup);free(dup);return NULL;}
    return dup;
}

void ctx_append_log(PipelineContext *c,const char *entry) {
    if(c->log_count>=10000) {c->failed=true;return;}
    char *copy=str_dup(entry);if(!copy) {c->failed=true;return;}
    if(c->log_count>=c->log_cap) {
        size_t cap=c->log_cap?c->log_cap*2:8;
        char **logs=mem_reallocarray(c->logs,cap,sizeof(*logs));
        if(!logs) {free(copy);c->failed=true;return;}c->logs=logs;c->log_cap=cap;
    }
    c->logs[c->log_count++]=copy;
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

static JsonValue *json_copy(const JsonValue *v) {
    char *encoded=json_serialize(v);if(!encoded) return NULL;
    JsonValue *copy=json_parse(encoded,NULL);free(encoded);return copy;
}
static JsonValue *outcome_json(const Outcome *o) {
    JsonValue *j=json_new_object();
    json_object_set(j,"status",json_new_string(status_to_string(o->status)));
    json_object_set(j,"preferred_label",json_new_string(str_safe(o->preferred_label)));
    json_object_set(j,"notes",json_new_string(str_safe(o->notes)));
    json_object_set(j,"failure_reason",json_new_string(str_safe(o->failure_reason)));
    JsonValue *suggested=json_new_array();for(size_t i=0;i<o->suggested_next_count;i++) json_array_push(suggested,json_new_string(o->suggested_next_ids[i]));
    json_object_set(j,"suggested",suggested);
    JsonValue *updates=json_new_object();for(size_t i=0;i<o->update_count;i++) json_object_set(updates,o->update_keys[i],json_new_string(o->update_values[i]));
    json_object_set(j,"updates",updates);return j;
}
static bool outcome_from_json(Outcome *o,const JsonValue *j) {
    const char *status=json_get_string(j,"status");
    if(!status || (!str_eq(status,"success") && !str_eq(status,"partial_success") && !str_eq(status,"retry") && !str_eq(status,"fail") && !str_eq(status,"skipped"))) return false;
    unsigned long failures=mem_failure_count();
    if(!json_get_string(j,"preferred_label") || !json_get_string(j,"notes") || !json_get_string(j,"failure_reason")) return false;
    *o=(Outcome){.status=string_to_status(status)};
    o->preferred_label=str_dup(json_get_string(j,"preferred_label"));
    o->notes=str_dup(json_get_string(j,"notes"));o->failure_reason=str_dup(json_get_string(j,"failure_reason"));
    JsonValue *arr=json_get(j,"suggested"),*updates=json_get(j,"updates");
    if(!arr || arr->type!=JSON_ARRAY || !updates || updates->type!=JSON_OBJECT) goto fail;
    o->suggested_next_ids=mem_calloc(arr->array.count,sizeof(char *));if(!o->suggested_next_ids) goto fail;
    for(size_t i=0;i<arr->array.count;i++) {
        JsonValue *v=arr->array.items[i];if(v->type!=JSON_STRING) goto fail;
        o->suggested_next_ids[o->suggested_next_count]=str_dup(v->string);if(!o->suggested_next_ids[o->suggested_next_count]) goto fail;o->suggested_next_count++;
    }
    o->update_keys=mem_calloc(updates->object.count,sizeof(char *));o->update_values=mem_calloc(updates->object.count,sizeof(char *));
    if(!o->update_keys || !o->update_values) goto fail;
    for(size_t i=0;i<updates->object.count;i++) {
        if(updates->object.values[i]->type!=JSON_STRING) goto fail;
        o->update_keys[i]=str_dup(updates->object.keys[i]);o->update_values[i]=str_dup(updates->object.values[i]->string);
        o->update_count++;if(!o->update_keys[i] || !o->update_values[i]) goto fail;
    }
    if(mem_failure_count()!=failures) goto fail;return true;
fail:outcome_free(o);return false;
}
static Outcome outcome_copy(const Outcome *source) {
    JsonValue *j=outcome_json(source);Outcome o={.status=STAGE_FAIL};
    if(!j || !outcome_from_json(&o,j)) o.failure_reason=str_dup("Cannot copy outcome");json_free(j);return o;
}
bool checkpoint_save(const Checkpoint *cp,const char *path) {
    if(!cp || cp->version!=2 || !cp->graph_identity || !cp->run_id || cp->context.failed) return false;
    JsonValue *j=json_new_object();json_object_set(j,"version",json_new_number(2));
    json_object_set(j,"graph_identity",json_new_string(cp->graph_identity));json_object_set(j,"run_id",json_new_string(cp->run_id));
    json_object_set(j,"next_node",json_new_string(str_safe(cp->next_node)));json_object_set(j,"pending_node",json_new_string(str_safe(cp->pending_node)));
    json_object_set(j,"complete",json_new_bool(cp->complete));json_object_set(j,"final_outcome",outcome_json(&cp->final_outcome));
    json_object_set(j,"history",json_copy(cp->history));json_object_set(j,"latest",json_copy(cp->latest));
    JsonValue *context=json_new_object(),*logs=json_new_array();
    for(size_t i=0;i<cp->context.count;i++) json_object_set(context,cp->context.keys[i],json_new_string(cp->context.values[i]));
    for(size_t i=0;i<cp->context.log_count;i++) json_array_push(logs,json_new_string(cp->context.logs[i]));
    json_object_set(j,"context",context);json_object_set(j,"logs",logs);
    char *encoded=json_serialize(j);json_free(j);if(!encoded) return false;
    bool ok=io_atomic_write(path,encoded,IO_SYNC);free(encoded);return ok;
}
bool checkpoint_load(Checkpoint *cp,const char *path) {
    char *source=io_read_text(path,ATTRACTOR_INPUT_LIMIT);if(!source) return false;
    JsonValue *j=json_parse(source,NULL);free(source);if(!j) return false;
    Checkpoint temporary={.version=2};
    JsonValue *history=json_get(j,"history"),*latest=json_get(j,"latest"),*context=json_get(j,"context"),*logs=json_get(j,"logs");
    const char *identity=json_get_string(j,"graph_identity"),*run=json_get_string(j,"run_id"),*next=json_get_string(j,"next_node"),*pending=json_get_string(j,"pending_node");
    JsonValue *complete=json_get(j,"complete");
    if(json_get_int(j,"version",0)!=2 || !identity || !*identity || !run || !*run || !next || !pending || !complete || complete->type!=JSON_BOOL ||
       !history || history->type!=JSON_ARRAY || history->array.count>10000 || !latest || latest->type!=JSON_OBJECT || latest->object.count>10000 ||
       !context || context->type!=JSON_OBJECT || !logs || logs->type!=JSON_ARRAY || logs->array.count>10000) goto fail;
    uuid_t run_uuid;if(uuid_parse(run,run_uuid)!=0) goto fail;
    if(*pending && !str_eq(pending,next)) goto fail;
    temporary.graph_identity=str_dup(identity);temporary.run_id=str_dup(run);temporary.next_node=str_dup(next);temporary.pending_node=str_dup(pending);temporary.complete=complete->boolean;
    if(!temporary.graph_identity || !temporary.run_id || !temporary.next_node || !temporary.pending_node || !outcome_from_json(&temporary.final_outcome,json_get(j,"final_outcome"))) goto fail;
    if(temporary.complete && (*next || *pending)) goto fail;
    if(!temporary.complete && !*next) goto fail;
    for(size_t i=0;i<history->array.count;i++) {
        JsonValue *record=history->array.items[i];Outcome o={0};
        if(json_get_int(record,"attempt",-1)<0 || !json_get_string(record,"node_id") || !outcome_from_json(&o,record)) goto fail;outcome_free(&o);
    }
    for(size_t i=0;i<latest->object.count;i++) {Outcome o={0};if(!outcome_from_json(&o,latest->object.values[i])) goto fail;outcome_free(&o);}
    for(size_t i=0;i<context->object.count;i++) {JsonValue *v=context->object.values[i];if(v->type!=JSON_STRING) goto fail;ctx_set(&temporary.context,context->object.keys[i],v->string);}
    for(size_t i=0;i<logs->array.count;i++) {JsonValue *v=logs->array.items[i];if(v->type!=JSON_STRING) goto fail;ctx_append_log(&temporary.context,v->string);}
    temporary.history=json_copy(history);temporary.latest=json_copy(latest);
    if(temporary.context.failed || !temporary.history || !temporary.latest) goto fail;
    checkpoint_free(cp);*cp=temporary;json_free(j);return true;
fail:checkpoint_free(&temporary);json_free(j);return false;
}
void checkpoint_free(Checkpoint *cp) {
    if(!cp) return;
    free(cp->graph_identity);free(cp->run_id);free(cp->next_node);free(cp->pending_node);
    outcome_free(&cp->final_outcome);json_free(cp->history);json_free(cp->latest);ctx_free(&cp->context);memset(cp,0,sizeof(*cp));
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
        Handler **handlers=mem_reallocarray(reg->handlers,new_cap,sizeof(*handlers));
        if(!handlers) {reg->failed=true;return;}reg->handlers=handlers;reg->cap=new_cap;
    }
    Handler *h = mem_calloc(1, sizeof(Handler));
    if(!h) {reg->failed=true;return;}
    h->type_name = str_dup(type);
    if(!h->type_name || !fn) {reg->failed=true;free(h->type_name);free(h);return;}
    h->execute   = fn;
    h->data      = data;
    reg->handlers[reg->count++] = h;
}

Handler *handler_registry_resolve(const HandlerRegistry *reg,const DotNode *node) {
    const char *role=dot_node_role(node);
    for(size_t i=0;i<reg->count;i++) if(str_eq(reg->handlers[i]->type_name,role)) return reg->handlers[i];
    return NULL;
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

/* Write spec-compliant status.json per Appendix C:
 * { "outcome": "...", "preferred_next_label": "...",
 *   "suggested_next_ids": [...], "context_updates": {...}, "notes": "..." } */
static bool write_outcome_status(const char *logs_root, const char *node_id,
                                 const Outcome *o)
{
    if (!logs_root || !node_id || !o) return false;

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

    bool ok=json_str && io_artifact_write(logs_root,node_id,"status.json",json_str);
    free(json_str);return ok;
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

    if(!prompt || !io_artifact_write(logs_root,node->id,"prompt.md",prompt)) {
        free(prompt);return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup("Cannot write contained prompt artifact")};
    }

    /* Execute via backend or simulate */
    char *response = NULL;
    if (runner && runner->backend) {
        response = runner->backend->run(runner->backend, node, prompt, ctx);
        if (!response) {
            o.status = STAGE_FAIL;
            o.failure_reason = str_dup("Backend returned NULL response");
            free(prompt);
            return o;
        }
    } else {
        /* Simulated response */
        StrBuf sim;
        strbuf_init(&sim);
        strbuf_appendf(&sim, "[simulated] Response for node '%s'", str_safe(node->id));
        response = strbuf_detach(&sim);
    }

    if(!response || strnlen(response,ATTRACTOR_OUTPUT_LIMIT+1)>ATTRACTOR_OUTPUT_LIMIT || !io_artifact_write(logs_root,node->id,"response.md",response)) {
        free(response);free(prompt);return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup("Cannot write response artifact")};
    }

    /* Build outcome */
    o.status = STAGE_SUCCESS;

    /* Context updates: last_stage, last_response, and per-node full response.
     * last_response is capped at 16 KB for checkpoint size sanity.
     * response.<node_id> carries the full text (capped at 64 KB) so
     * downstream nodes can access any predecessor's complete output. */
    o.update_keys   = mem_calloc(3, sizeof(char *));
    o.update_values = mem_calloc(3, sizeof(char *));
    if(!o.update_keys || !o.update_values) {outcome_free(&o);free(response);free(prompt);return fail_outcome("Outcome allocation failed");}
    o.update_count=3;
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


    free(response);
    free(prompt);
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
    const DotEdge **edges=mem_calloc(graph->edge_count,sizeof(*edges));
    if(!edges) return fail_outcome("Cannot collect human options");
    size_t edge_count=0;Outcome selection_outcome={.status=STAGE_SUCCESS};
    for(size_t i=0;i<graph->edge_count;i++) {
        const DotEdge *edge=&graph->edges[i];if(str_eq(edge->from,node->id) && evaluate_condition(edge->condition,&selection_outcome,ctx)) edges[edge_count++]=edge;
    }
    int seconds=parse_timeout_seconds(node->timeout);
    if(seconds<0) {free(edges);return fail_outcome("Invalid human timeout");}

    /* Build question */
    Question q;
    memset(&q, 0, sizeof(q));
    q.text  = str_dup(node->label ? node->label : node->id);
    q.stage = str_dup(str_safe(node->id));
    q.type  = QUESTION_MULTIPLE_CHOICE;
    q.timeout_ms=seconds*1000;

    q.option_count = edge_count;
    q.options = mem_calloc(edge_count, sizeof(QuestionOption));
    if(!q.options) {free(q.text);free(q.stage);free(edges);return fail_outcome("Cannot build question");}
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
    }

    if(ans.kind==ANSWER_TIMEOUT) {
        const char *default_choice=dot_node_attr(node,"human.default_choice",NULL);
        if(default_choice && *default_choice) {answer_free(&ans);ans.kind=ANSWER_SELECTION;ans.value=str_dup(default_choice);}
    }
    /* Find the selected edge and its target */
    const char *selected_target = NULL;
    const char *selected_label  = NULL;
    if (ans.kind == ANSWER_SELECTION && ans.option_index >= 0 && (size_t)ans.option_index < edge_count) {
        selected_target = edges[ans.option_index]->to;
        selected_label  = edges[ans.option_index]->label;
    } else if (ans.kind == ANSWER_SELECTION && ans.value) {
        /* Try matching the answer value to an edge target or label */
        for (size_t i = 0; i < edge_count; i++) {
            if (str_eq(ans.value, edges[i]->to) || str_eq(ans.value,q.options[i].key) ||
                (edges[i]->label && str_eq(ans.value, edges[i]->label))) {
                selected_target = edges[i]->to;
                selected_label  = edges[i]->label;
                break;
            }
        }
    }

    o.status = selected_target ? STAGE_SUCCESS : STAGE_FAIL;
    if (!selected_target) o.failure_reason = str_dup("Human gate has no valid selection");
    if (selected_target) {
        o.suggested_next_ids = mem_calloc(1, sizeof(char *));
        if(o.suggested_next_ids) {o.suggested_next_count=1;o.suggested_next_ids[0] = str_dup(selected_target);}
        else {o.status=STAGE_FAIL;o.failure_reason=str_dup("Selection allocation failed");}
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
    free(q.stage);free(edges);

    return o;
}

/* Parse a timeout string like "30s", "5m", "300" (seconds) */
static int parse_timeout_seconds(const char *s) {
    if(!s || !*s) return 0;
    char *end;errno=0;long value=strtol(s,&end,10);
    if(errno==ERANGE || end==s || value<=0) return -1;
    long multiplier=1;
    if(str_eq(end,"m") || str_eq(end,"M")) multiplier=60;
    else if(*end && !str_eq(end,"s") && !str_eq(end,"S")) return -1;
    if(value>INT_MAX/1000/multiplier) return -1;return (int)(value*multiplier);
}

static Outcome tool_handler(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs_root,void *data) {
    (void)ctx;(void)graph;(void)logs_root;PipelineRunner *runner=data;
    const char *cmd=dot_node_attr(node,"tool_command",NULL);
    if(!cmd || !*cmd) return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup("No tool_command attribute")};
    const char *argv[]={"/bin/sh","-c",cmd,NULL};
    int seconds=parse_timeout_seconds(node->timeout);
    if(seconds<0 || seconds>2147483) return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup("Timeout out of range")};
    ExecResult *result=runner && runner->execution_env?runner->execution_env->exec_command(runner->execution_env,cmd,seconds>0?seconds*1000:10000,NULL):process_run(&(ProcessOptions){.argv=argv,.timeout_ms=seconds>0?seconds*1000:10000,.cancel=runner?runner->cancel:NULL});
    Outcome o={.status=STAGE_FAIL};
    if(!result || result->exit_code!=0 || result->timed_out || result->cancelled || result->output_limited) {
        o.failure_reason=str_dup(result && result->timed_out?"tool_command timed out":"tool_command failed or exceeded output limit");
    } else {
        o.update_keys=mem_calloc(1,sizeof(char *));o.update_values=mem_calloc(1,sizeof(char *));
        if(o.update_keys && o.update_values) {
            o.update_count=1;o.update_keys[0]=str_dup("tool.output");o.update_values[0]=result->stdout_buf;result->stdout_buf=NULL;o.status=STAGE_SUCCESS;
        }
    }
    exec_result_free(result);return o;
}

static bool registered_type(const char *type,void *data) {
    PipelineRunner *runner=data;
    for(size_t i=0;i<runner->handler_reg.count;i++) if(str_eq(type,runner->handler_reg.handlers[i]->type_name)) return true;
    return false;
}
static PipelineRunner *inherited_runner(PipelineRunner *parent,DotGraph *graph,const char *logs) {
    PipelineRunner *child=pipeline_runner_new(graph,logs);if(!child) return NULL;
    child->execution_env=parent->execution_env;child->cancel=parent->cancel;
    child->backend=parent->backend;child->interviewer=parent->interviewer;child->owns_interviewer=false;
    child->event_cb=parent->event_cb;child->event_userdata=parent->event_userdata;
    child->max_iterations=parent->max_iterations;child->max_child_depth=parent->max_child_depth;child->child_depth=parent->child_depth+1;
    pipeline_register_builtin_handlers(child);
    for(size_t i=0;i<parent->handler_reg.count;i++) {
        Handler *h=parent->handler_reg.handlers[i];
        handler_registry_register(&child->handler_reg,h->type_name,h->execute,h->data==parent?child:h->data);
    }return child;
}
static bool merge_branch(PipelineContext *parent,const PipelineContext *base,const PipelineContext *branch,PipelineContext *merged,const char *scope,const char *id) {
    for(size_t i=0;i<branch->count;i++) {
        const char *key=branch->keys[i],*value=branch->values[i];
        if(str_starts_with(key,"internal.") || str_eq(key,"outcome") || str_eq(key,"current_node") || str_eq(key,"preferred_label") || str_eq(key,"last_stage") || str_eq(key,"last_response") || str_eq(key,"parallel.results") || str_starts_with(key,"parallel.fan_in.")) continue;
        if(str_eq(ctx_get(base,key,NULL),value)) continue;
        StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"%s.%s.%s",scope,id,key);char *name=strbuf_detach(&b);
        if(!name) return false;ctx_set(parent,name,value);free(name);
        const char *prior=ctx_get(merged,key,NULL);
        if(prior && !str_eq(prior,value)) return false;
        ctx_set(merged,key,value);
    }return !parent->failed && !merged->failed;
}
static Outcome parallel_handler(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs_root,void *data) {
    PipelineRunner *r=data;if(!r || r->child_depth>=r->max_child_depth) return fail_outcome("Branch depth budget exhausted");
    char *error=NULL;const DotNode *join=dot_parallel_join(graph,node,&error);
    if(!join) {Outcome fail=fail_outcome(error?error:"Invalid parallel region");free(error);return fail;}
    PipelineContext *base=ctx_clone(ctx);if(!base) return fail_outcome("Cannot isolate branch context");
    PipelineContext merged={0};JsonValue *results=json_new_array();size_t successes=0,failures=0,total=0;
    const char *policy=dot_node_attr(node,"error_policy","fail_fast"),*join_policy=dot_node_attr(node,"join_policy","wait_all");
    size_t instance=r->active_checkpoint?r->active_checkpoint->history->array.count:0;
    StrBuf scope_buf;strbuf_init(&scope_buf);strbuf_appendf(&scope_buf,"parallel.%s.%zu",r->active_checkpoint?r->active_checkpoint->run_id:node->id,instance);
    char *scope=strbuf_detach(&scope_buf),*fork_name=io_artifact_name(node->id);bool conflict=false;
    if(!scope || !fork_name || !results) {free(scope);free(fork_name);ctx_free(base);free(base);json_free(results);return fail_outcome("Cannot initialize fork");}
    for(size_t i=0;i<graph->edge_count;i++) if(str_eq(graph->edges[i].from,node->id)) total++;
    for(size_t i=0;i<graph->edge_count;i++) {
        const DotEdge *edge=&graph->edges[i];if(!str_eq(edge->from,node->id)) continue;
        char *branch_name=io_artifact_name(edge->to);StrBuf path;strbuf_init(&path);
        strbuf_appendf(&path,"%s/%s/fork-%s-%zu/%s",logs_root,fork_name,r->active_checkpoint?r->active_checkpoint->run_id:"direct",instance,str_safe(branch_name));free(branch_name);
        char *logs=strbuf_detach(&path);PipelineRunner *child=logs?inherited_runner(r,(DotGraph *)graph,logs):NULL;
        Checkpoint cp={0};Outcome branch=fail_outcome("Cannot initialize branch");
        if(child) {
            StrBuf cp_path;strbuf_init(&cp_path);strbuf_appendf(&cp_path,"%s/checkpoint.json",logs);char *saved=strbuf_detach(&cp_path);
            bool exists=saved && access(saved,F_OK)==0;
            bool loaded=exists && checkpoint_load(&cp,saved);free(saved);
            char *identity=graph_identity(graph);
            const DotNode *pending=cp.pending_node?dot_find_node(graph,cp.pending_node):NULL;
            bool recoverable=!cp.pending_node || !*cp.pending_node || (pending && (str_eq(dot_node_role(pending),"parallel") || str_eq(dot_node_role(pending),"stack.manager_loop")));
            bool compatible=loaded && identity && str_eq(cp.graph_identity,identity);free(identity);
            if(exists && (!compatible || !recoverable)) {outcome_free(&branch);branch=fail_outcome("Ambiguous or corrupt branch checkpoint");}
            else if(loaded || initial_checkpoint(child,&cp,edge->to,base)) {
                outcome_free(&branch);branch=execute_graph(child,&cp,join->id);
                if(!merge_branch(ctx,base,&cp.context,&merged,scope,edge->to)) conflict=true;
                if(r->active_checkpoint) {
                    for(size_t h=0;h<cp.history->array.count;h++) json_array_push(r->active_checkpoint->history,json_copy(cp.history->array.items[h]));
                    for(size_t k=0;k<cp.latest->object.count;k++) json_object_set(r->active_checkpoint->latest,cp.latest->object.keys[k],json_copy(cp.latest->object.values[k]));
                }
            }
        }
        JsonValue *record=outcome_json(&branch);json_object_set(record,"outcome",json_new_string(status_to_string(branch.status)));json_object_set(record,"node_id",json_new_string(edge->to));json_array_push(results,record);
        if(branch.status==STAGE_SUCCESS || branch.status==STAGE_PARTIAL_SUCCESS) successes++;else failures++;
        checkpoint_free(&cp);pipeline_runner_free(child);free(logs);outcome_free(&branch);
        if(conflict || (failures && str_eq(policy,"fail_fast")) || (successes && str_eq(join_policy,"first_success"))) break;
    }
    char *encoded=json_serialize(results);json_free(results);
    if(encoded) {ctx_set(ctx,scope,encoded);ctx_set(ctx,"parallel.results",encoded);}else conflict=true;
    free(encoded);free(scope);free(fork_name);ctx_free(base);free(base);
    if(!conflict) for(size_t i=0;i<merged.count;i++) ctx_set(ctx,merged.keys[i],merged.values[i]);ctx_free(&merged);
    bool accepted=str_eq(join_policy,"first_success")?successes>0:str_eq(join_policy,"quorum")?successes>total/2:failures==0 && successes==total;
    Outcome o={.status=accepted && !conflict?STAGE_SUCCESS:STAGE_FAIL};
    if(o.status==STAGE_FAIL) o.failure_reason=str_dup(conflict?"Conflicting branch context updates":"Parallel join policy unsatisfied");
    else {
        o.suggested_next_ids=mem_calloc(1,sizeof(char *));if(!o.suggested_next_ids) return fail_outcome("Cannot select join");
        o.suggested_next_ids[0]=str_dup(join->id);o.suggested_next_count=1;
    }
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
        o.status = STAGE_FAIL;
        o.failure_reason = str_dup("Fan-in: missing parallel results");
        return o;
    }

    /* Parse results to find the best candidate */
    const char *parse_err = NULL;
    JsonValue *results = json_parse(results_json, &parse_err);
    if (!results || results->type != JSON_ARRAY) {
        json_free(results);
        o.status = STAGE_FAIL;
        o.failure_reason = str_dup("Fan-in: malformed parallel results");
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
    if(!best_id) {json_free(results);return fail_outcome("Fan-in has no successful candidate");}

    /* Set context keys */
    if (best_id) {
        ctx_set(ctx, "parallel.fan_in.best_id", best_id);
        if (best_outcome)
            ctx_set(ctx, "parallel.fan_in.best_outcome", best_outcome);
    }

    /* If node has a prompt, we'd call LLM to rank candidates.
     * For now, use heuristic only. */
    (void)node;

    char *owned_id = str_dup(best_id);
    json_free(results);
    best_id = owned_id;
    o.status = STAGE_SUCCESS;
    o.notes  = str_dup(best_id ? best_id : "fan-in complete");
    free(owned_id);
    return o;
}

static Outcome manager_loop_handler(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs_root,void *data) {
    (void)graph;PipelineRunner *parent=data;
    if(!parent || parent->child_depth>=parent->max_child_depth) return fail_outcome("Child depth budget exhausted");
    const char *file=dot_node_attr(node,"stack.child_dotfile",NULL);
    if(!file || !*file) return fail_outcome("Manager requires stack.child_dotfile");
    const char *limit=dot_node_attr(node,"manager.max_cycles","1");char *end;long cycles=strtol(limit,&end,10);
    if(*end || cycles<1 || cycles>1000) return fail_outcome("Invalid child cycle budget");
    char *source=parent->execution_env && parent->execution_env->read_raw?parent->execution_env->read_raw(parent->execution_env,file):io_read_text(file,ATTRACTOR_INPUT_LIMIT);if(!source) return fail_outcome("Cannot read child graph");
    char *component=io_artifact_name(node->id);Outcome result=fail_outcome("Child cycles exhausted");
    if(!component) {free(source);return result;}
    for(long cycle=0;cycle<cycles;cycle++) {
        char *err=NULL;DotGraph *child_graph=dot_parse(source,&err);
        if(!child_graph) {outcome_free(&result);result=fail_outcome(err?err:"Invalid child DOT");free(err);break;}
        transform_expand_variables(child_graph);transform_apply_stylesheet(child_graph);
        if(!validate_or_raise_with_handlers(child_graph,registered_type,parent,&err)) {outcome_free(&result);result=fail_outcome(err?err:"Invalid child graph");free(err);dot_graph_free(child_graph);break;}
        size_t instance=parent->active_checkpoint?parent->active_checkpoint->history->array.count:0;
        StrBuf path;strbuf_init(&path);strbuf_appendf(&path,"%s/%s/attempt-%s-%zu/cycle-%ld",logs_root,component,parent->active_checkpoint?parent->active_checkpoint->run_id:"direct",instance,cycle);
        char *logs=strbuf_detach(&path);PipelineRunner *child=logs?inherited_runner(parent,child_graph,logs):NULL;
        outcome_free(&result);
        if(child) {
            StrBuf checkpoint;strbuf_init(&checkpoint);strbuf_appendf(&checkpoint,"%s/checkpoint.json",logs);char *saved=strbuf_detach(&checkpoint);
            result=saved && access(saved,F_OK)==0?pipeline_resume(child,saved):pipeline_run(child);free(saved);
        } else result=fail_outcome("Cannot create child runner");
        ctx_set(ctx,"stack.child.status",status_to_string(result.status));
        char count[32];snprintf(count,sizeof(count),"%ld",cycle+1);ctx_set(ctx,"stack.child.cycles",count);
        pipeline_runner_free(child);dot_graph_free(child_graph);free(logs);
        if(result.status==STAGE_SUCCESS) break;
    }
    free(component);free(source);return result;
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
    if (a->weight != b->weight) return a->weight>b->weight?-1:1;
    /* Lexical tiebreak on target */
    return strcmp(str_safe(a->to), str_safe(b->to));
}

static const DotEdge *select_edge(const DotGraph *graph,const char *id,const Outcome *o,const PipelineContext *ctx) {
    const DotEdge *conditional=NULL,*fallback=NULL,*label=NULL,*suggested=NULL;
    char *preferred=o->preferred_label?normalize_label(o->preferred_label):NULL;
    bool failed=o->status==STAGE_FAIL || o->status==STAGE_RETRY;
    for(size_t i=0;i<graph->edge_count;i++) {
        const DotEdge *e=&graph->edges[i];if(!str_eq(e->from,id)) continue;
        bool has_condition=e->condition && *e->condition;
        if(has_condition) {
            if(evaluate_condition(e->condition,o,ctx) && (!conditional || compare_edges_weight_lexical(conditional,e)>0)) conditional=e;
            continue;
        }
        /* Failure may only recover through a matched conditional or an
         * explicitly designated suggested recovery edge / retry target. */
        if(!failed && (!fallback || compare_edges_weight_lexical(fallback,e)>0)) fallback=e;
        if(!failed && preferred && e->label) {
            char *norm=normalize_label(e->label);bool match=str_eq(preferred,norm);free(norm);
            if(match && (!label || compare_edges_weight_lexical(label,e)>0)) label=e;
        }
        for(size_t j=0;j<o->suggested_next_count;j++) if(str_eq(e->to,o->suggested_next_ids[j])) {
            if(!suggested || compare_edges_weight_lexical(suggested,e)>0) suggested=e;
        }
    }
    free(preferred);return conditional?conditional:label?label:suggested?suggested:fallback;
}

/* ── 9. Goal Gate Checking ───────────────────────────────────────────── */

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
        if((runner->cancel && *runner->cancel) || (runner->active_checkpoint && runner->active_checkpoint->history->array.count>=runner->max_iterations)) return fail_outcome("Cancelled or attempt budget exhausted");
        o = handler->execute(node, ctx, graph, logs_root, handler_data);
        if(o.status<STAGE_SUCCESS || o.status>STAGE_SKIPPED) {outcome_free(&o);o=fail_outcome("Handler returned invalid status");}
        if(runner->active_checkpoint) {
            JsonValue *record=outcome_json(&o);json_object_set(record,"node_id",json_new_string(node->id));json_object_set(record,"attempt",json_new_number(attempt));
            json_array_push(runner->active_checkpoint->history,record);
            StrBuf key;strbuf_init(&key);strbuf_appendf(&key,"internal.retry_count.%s",node->id);char *name=strbuf_detach(&key);
            char count[32];snprintf(count,sizeof(count),"%d",attempt);if(name) ctx_set(ctx,name,count);else ctx->failed=true;free(name);
        }

        if (o.status == STAGE_SUCCESS || o.status == STAGE_PARTIAL_SUCCESS)
            return o;

        if (o.status == STAGE_FAIL)
            return o;

        if (o.status == STAGE_RETRY && attempt < max_retries) {
            /* Exponential backoff: 100ms * 2^attempt, capped at 5s */
            double delay = 0.1 * pow(2.0, (double)(attempt>6?6:attempt));
            if (delay > 5.0) delay = 5.0;

            emit_event(runner, PIPE_EVT_STAGE_RETRYING, node->id,
                       NULL, attempt + 1);

            for(int tick=0;tick<(int)(delay*100);tick++) {if(runner->cancel && *runner->cancel) break;struct timespec ts={0,10000000};nanosleep(&ts,NULL);}

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
    if(o.status==STAGE_RETRY) {o.status=STAGE_FAIL;if(!o.failure_reason) o.failure_reason=str_dup("Retry budget exhausted");}
    return o;
}

/* ── 11. Pipeline Runner lifecycle ───────────────────────────────────── */

PipelineRunner *pipeline_runner_new(DotGraph *graph, const char *logs_root)
{
    PipelineRunner *r = mem_calloc(1, sizeof(PipelineRunner));
    if(!r) return NULL;
    r->max_iterations=10000;r->max_child_depth=8;r->owns_interviewer=true;
    r->graph    = graph;
    r->logs_root = str_dup(logs_root ? logs_root : "./logs");
    if(!r->logs_root) {free(r);return NULL;}
    handler_registry_init(&r->handler_reg);
    return r;
}

void pipeline_runner_free(PipelineRunner *r)
{
    if (!r) return;
    handler_registry_free(&r->handler_reg);
    free(r->logs_root);
    if (r->interviewer && r->owns_interviewer)
        interviewer_free(r->interviewer);
    /* Note: graph and backend are not owned by runner */
    free(r);
}

void pipeline_runner_set_execution_env(PipelineRunner *r,ExecutionEnv *env) {r->execution_env=env;}

void pipeline_runner_set_backend(PipelineRunner *r, CodergenBackend *b)
{
    r->backend = b;
}

void pipeline_runner_set_interviewer(PipelineRunner *r, Interviewer *iv)
{
    if(r->interviewer!=iv && r->owns_interviewer) interviewer_free(r->interviewer);
    r->owns_interviewer=true;
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

static char *graph_identity(const DotGraph *g) {
    JsonValue *j=json_new_object(),*nodes=json_new_array(),*edges=json_new_array();
    json_object_set(j,"label",json_new_string(str_safe(g->label)));
    json_object_set(j,"name",json_new_string(str_safe(g->name)));json_object_set(j,"goal",json_new_string(str_safe(g->goal)));
    json_object_set(j,"retry",json_new_string(str_safe(g->retry_target)));json_object_set(j,"fallback_retry",json_new_string(str_safe(g->fallback_retry_target)));
    json_object_set(j,"default_retry",json_new_number(g->default_max_retry));
    json_object_set(j,"default_model",json_new_string(str_safe(g->default_model)));json_object_set(j,"default_provider",json_new_string(str_safe(g->default_provider)));
    json_object_set(j,"default_fidelity",json_new_string(str_safe(g->default_fidelity)));json_object_set(j,"stylesheet",json_new_string(str_safe(g->model_stylesheet)));
    for(size_t i=0;i<g->node_count;i++) {
        const DotNode *n=&g->nodes[i];JsonValue *v=json_new_object(),*attrs=json_new_object();
        json_object_set(v,"id",json_new_string(n->id));json_object_set(v,"role",json_new_string(dot_node_role(n)));
        json_object_set(v,"prompt",json_new_string(str_safe(n->prompt)));json_object_set(v,"model",json_new_string(str_safe(n->llm_model)));
        json_object_set(v,"provider",json_new_string(str_safe(n->llm_provider)));json_object_set(v,"effort",json_new_string(str_safe(n->reasoning_effort)));
        for(size_t k=0;k<n->attr_count;k++) json_object_set(attrs,n->attrs[k].key,json_new_string(n->attrs[k].value));
        json_object_set(v,"attrs",attrs);json_array_push(nodes,v);
    }
    for(size_t i=0;i<g->edge_count;i++) {
        const DotEdge *e=&g->edges[i];JsonValue *v=json_new_object(),*attrs=json_new_object();
        json_object_set(v,"from",json_new_string(e->from));json_object_set(v,"to",json_new_string(e->to));
        for(size_t k=0;k<e->attr_count;k++) json_object_set(attrs,e->attrs[k].key,json_new_string(e->attrs[k].value));
        json_object_set(v,"attrs",attrs);json_array_push(edges,v);
    }
    json_object_set(j,"nodes",nodes);json_object_set(j,"edges",edges);char *identity=json_serialize(j);json_free(j);return identity;
}
static bool persist(PipelineRunner *r,Checkpoint *cp,bool committed) {
    if(!io_mkdirs(r->logs_root)) return false;
    StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"%s/checkpoint.json",r->logs_root);char *path=strbuf_detach(&b);
    bool ok=path && checkpoint_save(cp,path);
    if(ok && committed) emit_event(r,PIPE_EVT_CHECKPOINT_SAVED,cp->next_node,path,0);
    free(path);return ok;
}
static Outcome fail_outcome(const char *reason) {return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup(reason)};}
static const DotNode *unsatisfied_gate(const DotGraph *g,const Checkpoint *cp) {
    for(size_t i=0;i<g->node_count;i++) {
        const DotNode *n=&g->nodes[i];if(!n->goal_gate) continue;
        JsonValue *o=json_get(cp->latest,n->id);if(!o) continue;
        const char *status=json_get_string(o,"status");
        if(!str_eq(status,"success") && !str_eq(status,"partial_success")) return n;
    }return NULL;
}
static Outcome execute_graph(PipelineRunner *r,Checkpoint *cp,const char *boundary) {
    if(cp->complete) return outcome_copy(&cp->final_outcome);
    r->active_checkpoint=cp;
    Outcome result=fail_outcome("Iteration budget exhausted");
    size_t iteration=cp->history->array.count;
    for(;iteration<r->max_iterations && cp->history->array.count<r->max_iterations;iteration++) {
        if(r->cancel && *r->cancel) {outcome_free(&result);result=fail_outcome("Pipeline cancelled");break;}
        DotNode *node=dot_find_node(r->graph,cp->next_node);
        if(!node) {outcome_free(&result);result=fail_outcome("Invalid next node");break;}
        if(boundary && str_eq(node->id,boundary)) {outcome_free(&result);result=(Outcome){.status=STAGE_SUCCESS};break;}
        if(str_eq(dot_node_role(node),"exit")) {
            const DotNode *gate=unsatisfied_gate(r->graph,cp);
            if(gate) {
                const char *target=gate->retry_target;
                if(!target || !*target) target=r->graph->retry_target;
                if(!target || !*target) target=gate->fallback_retry_target;
                if(!target || !*target) target=r->graph->fallback_retry_target;
                if(!target || !*target) {outcome_free(&result);result=fail_outcome("Goal gate unsatisfied without recovery target");break;}
                free(cp->next_node);cp->next_node=str_dup(target);continue;
            }
        }
        Handler *h=handler_registry_resolve(&r->handler_reg,node);
        if(!h) {outcome_free(&result);result=fail_outcome("Unknown or unsupported handler");break;}
        free(cp->pending_node);cp->pending_node=str_dup(node->id);
        if(!cp->pending_node || !persist(r,cp,false)) {outcome_free(&result);result=fail_outcome("Cannot persist stage attempt; stage was not executed");break;}
        emit_event(r,PIPE_EVT_STAGE_STARTED,node->id,NULL,0);
        int retries=node->max_retries<0?r->graph->default_max_retry:node->max_retries;
        if(retries<0) retries=0;
        unsigned long failures=mem_failure_count();
        Outcome step=execute_with_retry(r,h,node,&cp->context,r->graph,r->logs_root,h->data,retries);
        if(mem_failure_count()!=failures) {outcome_free(&step);outcome_free(&result);result=fail_outcome("Stage allocation failed");break;}
        ctx_apply_updates(&cp->context,&step);ctx_set(&cp->context,"outcome",status_to_string(step.status));
        ctx_set(&cp->context,"current_node",node->id);ctx_set(&cp->context,"preferred_label",str_safe(step.preferred_label));
        json_object_set(cp->latest,node->id,outcome_json(&step));
        StrBuf log;strbuf_init(&log);strbuf_appendf(&log,"[%s] status=%s",node->id,status_to_string(step.status));char *entry=strbuf_detach(&log);
        if(entry) ctx_append_log(&cp->context,entry);else cp->context.failed=true;free(entry);
        if(!write_outcome_status(r->logs_root,node->id,&step)) {outcome_free(&step);outcome_free(&result);result=fail_outcome("Cannot write contained status artifact");break;}
        emit_event(r,step.status==STAGE_FAIL?PIPE_EVT_STAGE_FAILED:PIPE_EVT_STAGE_COMPLETED,node->id,step.failure_reason,0);
        const DotEdge *edge=NULL;const char *next=NULL;
        bool terminal=str_eq(dot_node_role(node),"exit");
        bool denied_human=str_eq(dot_node_role(node),"wait.human") && step.status!=STAGE_SUCCESS;
        if(str_eq(dot_node_role(node),"parallel") && step.status==STAGE_SUCCESS && step.suggested_next_count==1) next=step.suggested_next_ids[0];
        if(str_eq(dot_node_role(node),"wait.human") && step.status==STAGE_SUCCESS && step.suggested_next_count==1) {
            for(size_t i=0;i<r->graph->edge_count;i++) {
                const DotEdge *candidate=&r->graph->edges[i];
                if(str_eq(candidate->from,node->id) && str_eq(candidate->to,step.suggested_next_ids[0]) && evaluate_condition(candidate->condition,&step,&cp->context)) {next=candidate->to;break;}
            }
            if(!next) denied_human=true;
        }
        if(!next && !terminal && !denied_human) {edge=select_edge(r->graph,node->id,&step,&cp->context);if(edge) next=edge->to;}
        if(!next && !terminal && !denied_human && (step.status==STAGE_FAIL || step.status==STAGE_RETRY)) {
            next=node->retry_target;if(!next || !*next) next=node->fallback_retry_target;
            if(!next || !*next) next=NULL;
        }
        if(cp->context.failed || !cp->history || cp->history->failed || !cp->latest || cp->latest->failed) {
            outcome_free(&step);outcome_free(&result);result=fail_outcome("Allocation or history/context budget exhausted");break;
        }
        char *owned_next=next?str_dup(next):NULL;
        if(next && !owned_next) {outcome_free(&step);outcome_free(&result);result=fail_outcome("Transition allocation failed");break;}
        free(cp->next_node);cp->next_node=owned_next;
        free(cp->pending_node);cp->pending_node=NULL;
        if(!next) {
            outcome_free(&result);
            if(denied_human && step.status==STAGE_SUCCESS) result=fail_outcome("Human selection no longer has an eligible transition");
            else if(terminal || step.status==STAGE_FAIL || denied_human) result=outcome_copy(&step);
            else result=fail_outcome("No eligible transition from nonterminal stage");
            cp->complete=true;outcome_free(&cp->final_outcome);cp->final_outcome=outcome_copy(&result);
        }
        outcome_free(&step);
        if(!persist(r,cp,true)) {outcome_free(&result);result=fail_outcome("Checkpoint persistence failed; external effect may require recovery");break;}
        if(cp->complete) break;
        if(boundary && cp->next_node && str_eq(cp->next_node,boundary)) {
            outcome_free(&result);result=(Outcome){.status=STAGE_SUCCESS};break;
        }
    }
    if(!cp->complete && result.status==STAGE_SUCCESS && boundary && cp->next_node && str_eq(cp->next_node,boundary)) {
        cp->complete=true;outcome_free(&cp->final_outcome);cp->final_outcome=outcome_copy(&result);
        free(cp->next_node);cp->next_node=NULL;
        if(!persist(r,cp,true)) {outcome_free(&result);result=fail_outcome("Branch checkpoint persistence failed");}
    }
    emit_event(r,result.status==STAGE_FAIL?PIPE_EVT_PIPELINE_FAILED:PIPE_EVT_PIPELINE_COMPLETED,cp->next_node,result.failure_reason,0);
    r->active_checkpoint=NULL;
    return result;
}
static bool initial_checkpoint(PipelineRunner *r,Checkpoint *cp,const char *start,const PipelineContext *context) {
    *cp=(Checkpoint){.version=2,.final_outcome={.status=STAGE_FAIL}};
    cp->graph_identity=graph_identity(r->graph);cp->next_node=str_dup(start);
    uuid_t u;uuid_generate(u);char id[37];uuid_unparse_lower(u,id);cp->run_id=str_dup(id);
    cp->history=json_new_array();cp->latest=json_new_object();
    if(context) for(size_t i=0;i<context->count;i++) ctx_set(&cp->context,context->keys[i],context->values[i]);
    if(r->graph->goal) ctx_set(&cp->context,"graph.goal",r->graph->goal);
    return cp->graph_identity && cp->next_node && cp->run_id && cp->history && cp->latest && !cp->context.failed;
}
Outcome pipeline_run(PipelineRunner *r) {
    if(!r || !r->graph || r->handler_reg.failed) return fail_outcome("Runner, graph, or handler registry invalid");
    if(!validate_or_raise_with_handlers(r->graph,registered_type,r,NULL)) return fail_outcome("Graph validation failed");
    const char *start=NULL;for(size_t i=0;i<r->graph->node_count;i++) if(str_eq(dot_node_role(&r->graph->nodes[i]),"start")) {start=r->graph->nodes[i].id;break;}
    if(!start) return fail_outcome("Missing start node");
    Checkpoint cp={0};if(!initial_checkpoint(r,&cp,start,NULL)) {checkpoint_free(&cp);return fail_outcome("Cannot initialize execution state");}
    emit_event(r,PIPE_EVT_PIPELINE_STARTED,start,NULL,0);Outcome result=execute_graph(r,&cp,NULL);checkpoint_free(&cp);return result;
}
Outcome pipeline_resume(PipelineRunner *r,const char *path) {
    if(!r || !r->graph || r->handler_reg.failed || !validate_or_raise_with_handlers(r->graph,registered_type,r,NULL)) return fail_outcome("Invalid runner/graph");
    Checkpoint cp={0};if(!checkpoint_load(&cp,path)) return fail_outcome("Invalid checkpoint: version 2 required; legacy state cannot safely reconstruct routing");
    char *identity=graph_identity(r->graph);bool valid=identity && str_eq(identity,cp.graph_identity);free(identity);
    if(cp.next_node && *cp.next_node && !dot_find_node(r->graph,cp.next_node)) valid=false;
    for(size_t i=0;i<cp.latest->object.count;i++) if(!dot_find_node(r->graph,cp.latest->object.keys[i])) valid=false;
    for(size_t i=0;i<cp.history->array.count;i++) if(!dot_find_node(r->graph,json_get_string(cp.history->array.items[i],"node_id"))) valid=false;
    if(!valid) {checkpoint_free(&cp);return fail_outcome("Checkpoint graph identity/node mismatch");}
    if(cp.pending_node && *cp.pending_node) {
        const DotNode *pending=dot_find_node(r->graph,cp.pending_node);
        const char *role=pending?dot_node_role(pending):"";
        if(!str_eq(role,"parallel") && !str_eq(role,"stack.manager_loop")) {checkpoint_free(&cp);return fail_outcome("Interrupted stage has ambiguous effects; inspect attempt and explicitly reconcile before retrying");}
    }
    emit_event(r,PIPE_EVT_PIPELINE_STARTED,cp.next_node,"resumed",0);Outcome result=execute_graph(r,&cp,NULL);checkpoint_free(&cp);return result;
}

/* ── 14. Transforms ──────────────────────────────────────────────────── */

void transform_expand_variables(DotGraph *g)
{
    if (!g || !g->goal) return;
    unsigned long failures=mem_failure_count();
    for (size_t i = 0; i < g->node_count; i++) {
        DotNode *n = &g->nodes[i];
        if (n->prompt && strstr(n->prompt, "$goal")) {
            char *expanded = expand_goal(n->prompt, g->goal);
            free(n->prompt);
            n->prompt = expanded;
        }
    }
    if(mem_failure_count()!=failures) g->failed=true;
}

void transform_apply_stylesheet(DotGraph *g)
{
    if (!g || !g->model_stylesheet) return;
    unsigned long failures=mem_failure_count();
    char *err = NULL;
    Stylesheet *ss = stylesheet_parse(g->model_stylesheet, &err);
    if (!ss) {
        g->failed=true;free(err);
        return;
    }
    stylesheet_apply(ss, g);
    stylesheet_free(ss);
    if(mem_failure_count()!=failures) g->failed=true;
}

/* ── 15. Stylesheet ──────────────────────────────────────────────────── */

/* Parse a stylesheet source.
 * Format:
 *   selector { prop: value; prop: value; }
 * Selectors: "*", ".classname", "#nodeid"
 */
Stylesheet *stylesheet_parse(const char *src,char **err) {
    if(err) *err=NULL;
    if(!src || !*src || strnlen(src,ATTRACTOR_INPUT_LIMIT+1)>ATTRACTOR_INPUT_LIMIT) {if(err) *err=str_dup("Empty/oversized stylesheet");return NULL;}
    unsigned long failures=mem_failure_count();Stylesheet *sheet=mem_calloc(1,sizeof(*sheet));if(!sheet) return NULL;
    StyleRule rule={0};const char *p=src;const char *reason="Invalid or unterminated stylesheet";
    while(*p) {
        while(isspace((unsigned char)*p)) p++;if(!*p) break;
        const char *start=p;while(*p && !isspace((unsigned char)*p) && *p!='{') p++;
        if(p==start) goto fail;rule.selector=str_ndup(start,(size_t)(p-start));if(!rule.selector) goto fail;
        if(str_eq(rule.selector,"*")) rule.specificity=0;
        else if((rule.selector[0]=='.' || rule.selector[0]=='#') && rule.selector[1]) rule.specificity=rule.selector[0]=='#'?2:1;
        else goto fail;
        while(isspace((unsigned char)*p)) p++;if(*p!='{') goto fail;p++;
        for(;;) {
            while(isspace((unsigned char)*p)) p++;if(*p=='}') {p++;break;}if(!*p) goto fail;
            start=p;while(*p && *p!=':' && *p!='}' && *p!=';') p++;
            if(*p!=':') goto fail;
            char *property=str_ndup(start,(size_t)(p-start));if(!property) goto fail;p++;
            start=p;while(*p && *p!=';' && *p!='}') p++;
            if(!*p) {free(property);goto fail;}
            char *raw=str_ndup(start,(size_t)(p-start));if(!raw) {free(property);goto fail;}
            char *value=str_trim(raw),*key=str_trim(property),**field=NULL;
            if(str_eq(key,"llm_model") || str_eq(key,"model")) field=&rule.llm_model;
            else if(str_eq(key,"llm_provider") || str_eq(key,"provider")) field=&rule.llm_provider;
            else if(str_eq(key,"reasoning_effort") || str_eq(key,"reasoning")) field=&rule.reasoning_effort;
            if(!field || !*value) {free(property);free(raw);goto fail;}
            char *copy=str_dup(value);free(raw);free(property);if(!copy) goto fail;free(*field);*field=copy;
            if(*p==';') p++;
        }
        if(sheet->count>=4096) {reason="Stylesheet rule limit";goto fail;}
        StyleRule *rules=mem_reallocarray(sheet->rules,sheet->count+1,sizeof(*rules));if(!rules) goto fail;
        sheet->rules=rules;rules[sheet->count++]=rule;rule=(StyleRule){0};
    }
    if(mem_failure_count()!=failures) goto fail;return sheet;
fail:
    free(rule.selector);free(rule.llm_model);free(rule.llm_provider);free(rule.reasoning_effort);stylesheet_free(sheet);
    if(err) *err=str_dup(reason);return NULL;
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

void stylesheet_apply(const Stylesheet *s,DotGraph *g) {
    if(!s || !g) return;
    for(size_t i=0;i<g->node_count;i++) {
        DotNode *node=&g->nodes[i];
        bool explicit_model=node->llm_model && *node->llm_model,explicit_provider=node->llm_provider && *node->llm_provider,explicit_effort=node->reasoning_effort && *node->reasoning_effort;
        for(int specificity=0;specificity<3;specificity++) for(size_t k=0;k<s->count;k++) {
            const StyleRule *r=&s->rules[k];if(r->specificity!=specificity || !selector_matches(r->selector,node)) continue;
            if(r->llm_model && !explicit_model) {char *copy=str_dup(r->llm_model);if(copy) {free(node->llm_model);node->llm_model=copy;}}
            if(r->llm_provider && !explicit_provider) {char *copy=str_dup(r->llm_provider);if(copy) {free(node->llm_provider);node->llm_provider=copy;}}
            if(r->reasoning_effort && !explicit_effort) {char *copy=str_dup(r->reasoning_effort);if(copy) {free(node->reasoning_effort);node->reasoning_effort=copy;}}
        }
    }
}

/* ── 16. Interviewer Implementations ─────────────────────────────────── */

/* --- Auto-approve interviewer --- */

static Answer auto_approve_ask(Interviewer *self, const Question *q)
{
    (void)self;
    Answer a;
    memset(&a, 0, sizeof(a));
    a.option_index = -1;
    a.kind = ANSWER_SELECTION;

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
    Interviewer *iv = mem_calloc(1, sizeof(Interviewer));
    if(!iv) return NULL;
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
    char buf[1024];struct timespec began;clock_gettime(CLOCK_MONOTONIC,&began);
    long long deadline=(long long)began.tv_sec*1000+began.tv_nsec/1000000+q->timeout_ms;
read_again:;
    size_t used=0;bool overflow=false;
    for(;;) {
        int remaining=-1;
        if(q->timeout_ms>0) {
            struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
            long long left=deadline-((long long)now.tv_sec*1000+now.tv_nsec/1000000);
            if(left<=0) {a.kind=ANSWER_TIMEOUT;return a;}remaining=(int)left;
        }
        struct pollfd input={STDIN_FILENO,POLLIN,0};int ready=poll(&input,1,remaining);
        if(ready<0 && errno==EINTR) continue;
        if(ready<=0) {a.kind=ready==0?ANSWER_TIMEOUT:ANSWER_CANCELLED;return a;}
        char c;ssize_t n=read(STDIN_FILENO,&c,1);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) {a.kind=n==0?ANSWER_EOF:ANSWER_CANCELLED;return a;}
        if(c=='\n') break;
        if(used+1<sizeof(buf)) buf[used++]=c;else overflow=true;
    }
    buf[used]=0;
    if(overflow) {printf("Invalid selection. Choice: ");fflush(stdout);goto read_again;}
    if(str_eq(buf,"cancel")) {a.kind=ANSWER_CANCELLED;return a;}
    a.kind=ANSWER_SELECTION;
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

    if (q->type == QUESTION_MULTIPLE_CHOICE && a.option_index < 0) {
        free(a.text); free(a.value); a.text = a.value = NULL;
        printf("Invalid selection. Choice: "); fflush(stdout);
        goto read_again;
    }
    return a;
}

Interviewer *console_interviewer_new(void)
{
    Interviewer *iv = mem_calloc(1, sizeof(Interviewer));
    if(!iv) return NULL;
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
