#include "attractor/validator.h"
#include "attractor/engine.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*============================================================================
 * DiagnosticList
 *==========================================================================*/

void diagnostic_list_init(DiagnosticList *dl) {
    dl->items = NULL;
    dl->count = 0;
    dl->cap = 0;
}

void diagnostic_list_free(DiagnosticList *dl) {
    for (size_t i = 0; i < dl->count; i++) {
        free(dl->items[i].rule);
        free(dl->items[i].message);
        free(dl->items[i].node_id);
        free(dl->items[i].edge_from);
        free(dl->items[i].edge_to);
        free(dl->items[i].fix);
    }
    free(dl->items);
    dl->items = NULL;
    dl->count = dl->cap = 0;
}

void diagnostic_list_add(DiagnosticList *dl, DiagSeverity sev,
                         const char *rule, const char *msg,
                         const char *node_id, const char *fix) {
    if (dl->count >= dl->cap) {
        dl->cap = dl->cap ? dl->cap * 2 : 16;
        dl->items = realloc(dl->items, dl->cap * sizeof(Diagnostic));
    }
    Diagnostic *d = &dl->items[dl->count++];
    d->severity  = sev;
    d->rule      = str_dup(rule);
    d->message   = str_dup(msg);
    d->node_id   = str_dup(node_id);
    d->edge_from = NULL;
    d->edge_to   = NULL;
    d->fix       = str_dup(fix);
}

static void diagnostic_list_add_edge(DiagnosticList *dl, DiagSeverity sev,
                                     const char *rule, const char *msg,
                                     const char *edge_from, const char *edge_to,
                                     const char *fix) {
    if (dl->count >= dl->cap) {
        dl->cap = dl->cap ? dl->cap * 2 : 16;
        dl->items = realloc(dl->items, dl->cap * sizeof(Diagnostic));
    }
    Diagnostic *d = &dl->items[dl->count++];
    d->severity  = sev;
    d->rule      = str_dup(rule);
    d->message   = str_dup(msg);
    d->node_id   = NULL;
    d->edge_from = str_dup(edge_from);
    d->edge_to   = str_dup(edge_to);
    d->fix       = str_dup(fix);
}

bool diagnostic_list_has_errors(const DiagnosticList *dl) {
    for (size_t i = 0; i < dl->count; i++) {
        if (dl->items[i].severity == DIAG_ERROR) return true;
    }
    return false;
}

/*============================================================================
 * Internal helpers
 *==========================================================================*/

static bool is_start_node(const DotNode *n) {
    if (n->shape && str_eq(n->shape, "Mdiamond")) return true;
    if (str_eq(n->id, "start") || str_eq(n->id, "Start")) return true;
    return false;
}

static bool is_terminal_node(const DotNode *n) {
    if (n->shape && str_eq(n->shape, "Msquare")) return true;
    if (str_eq(n->id, "exit") || str_eq(n->id, "end") ||
        str_eq(n->id, "Exit") || str_eq(n->id, "End")) return true;
    return false;
}

/* Return index of start node, or -1 if not found. */
static int find_start_index(const DotGraph *g) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (is_start_node(&g->nodes[i])) return (int)i;
    }
    return -1;
}

/*
 * Check whether a node resolves to the codergen handler (i.e. it is an
 * "LLM node").  The heuristic: shape=box, or no shape and no type, and
 * not a recognized structural shape (diamond, hexagon, Mdiamond, Msquare,
 * house, invhouse, etc.).
 */
static bool is_llm_node(const DotNode *n) {
    /* Explicit type wins */
    if (n->type && n->type[0] != '\0') {
        return str_eq(n->type, "codergen");
    }
    /* Structural shapes are never LLM nodes */
    if (n->shape) {
        if (str_eq(n->shape, "Mdiamond") || str_eq(n->shape, "Msquare") ||
            str_eq(n->shape, "diamond")  || str_eq(n->shape, "hexagon") ||
            str_eq(n->shape, "house")    || str_eq(n->shape, "invhouse") ||
            str_eq(n->shape, "parallelogram") ||
            str_eq(n->shape, "doubleoctagon") ||
            str_eq(n->shape, "tripleoctagon"))
            return false;
        /* shape=box is the canonical LLM shape */
        if (str_eq(n->shape, "box") || str_eq(n->shape, "rect") ||
            str_eq(n->shape, "rectangle"))
            return true;
    }
    /* No shape, no type: start/exit names are excluded */
    if (is_start_node(n) || is_terminal_node(n)) return false;
    /* Default: no shape + no type = codergen */
    if (!n->shape || n->shape[0] == '\0') return true;
    return false;
}

/*============================================================================
 * Rule: start_node
 *==========================================================================*/

static void rule_start_node(const DotGraph *g, DiagnosticList *dl) {
    size_t count = 0;
    for (size_t i = 0; i < g->node_count; i++) {
        if (is_start_node(&g->nodes[i])) count++;
    }
    if (count == 0) {
        diagnostic_list_add(dl, DIAG_ERROR, "start_node",
            "No start node found. Add a node with shape=Mdiamond or id=\"start\".",
            NULL, "Add a node: start [shape=Mdiamond]");
    } else if (count > 1) {
        StrBuf msg;
        strbuf_init(&msg);
        strbuf_appendf(&msg, "Multiple start nodes found (%zu). Exactly one is required.", count);
        diagnostic_list_add(dl, DIAG_ERROR, "start_node", msg.data, NULL,
            "Remove extra start nodes so only one remains.");
        strbuf_free(&msg);
    }
}

/*============================================================================
 * Rule: terminal_node
 *==========================================================================*/

static void rule_terminal_node(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (is_terminal_node(&g->nodes[i])) return;
    }
    diagnostic_list_add(dl, DIAG_ERROR, "terminal_node",
        "No terminal node found. Add a node with shape=Msquare or id=\"exit\"/\"end\".",
        NULL, "Add a node: exit [shape=Msquare]");
}

/*============================================================================
 * Rule: reachability (BFS from start)
 *==========================================================================*/

static void rule_reachability(const DotGraph *g, DiagnosticList *dl) {
    int start = find_start_index(g);
    if (start < 0) return; /* start_node rule already fires */

    size_t n = g->node_count;
    if (n == 0) return;

    bool *visited = calloc(n, sizeof(bool));
    size_t *queue = calloc(n, sizeof(size_t));
    size_t head = 0, tail = 0;

    visited[(size_t)start] = true;
    queue[tail++] = (size_t)start;

    /* BFS */
    while (head < tail) {
        size_t cur = queue[head++];
        const DotEdge *out[256];
        size_t out_count = dot_outgoing_edges(g, g->nodes[cur].id, out, 256);
        for (size_t e = 0; e < out_count; e++) {
            /* Find target node index */
            for (size_t j = 0; j < n; j++) {
                if (str_eq(g->nodes[j].id, out[e]->to) && !visited[j]) {
                    visited[j] = true;
                    queue[tail++] = j;
                }
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (!visited[i]) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Node \"%s\" is not reachable from the start node.",
                           g->nodes[i].id);
            diagnostic_list_add(dl, DIAG_ERROR, "reachability", msg.data,
                                g->nodes[i].id, "Add an edge from a reachable node to this node.");
            strbuf_free(&msg);
        }
    }

    free(visited);
    free(queue);
}

/*============================================================================
 * Rule: edge_target_exists
 *==========================================================================*/

static void rule_edge_target_exists(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->edge_count; i++) {
        const DotEdge *e = &g->edges[i];
        if (!dot_find_node(g, e->from)) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Edge source \"%s\" does not reference an existing node.",
                           str_safe(e->from));
            diagnostic_list_add_edge(dl, DIAG_ERROR, "edge_target_exists", msg.data,
                                     e->from, e->to, "Define the missing node or fix the edge.");
            strbuf_free(&msg);
        }
        if (!dot_find_node(g, e->to)) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Edge target \"%s\" does not reference an existing node.",
                           str_safe(e->to));
            diagnostic_list_add_edge(dl, DIAG_ERROR, "edge_target_exists", msg.data,
                                     e->from, e->to, "Define the missing node or fix the edge.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: start_no_incoming
 *==========================================================================*/

static void rule_start_no_incoming(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (!is_start_node(&g->nodes[i])) continue;
        const DotEdge *inc[256];
        size_t inc_count = dot_incoming_edges(g, g->nodes[i].id, inc, 256);
        if (inc_count > 0) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg,
                "Start node \"%s\" has %zu incoming edge(s). Start nodes must have none.",
                g->nodes[i].id, inc_count);
            diagnostic_list_add(dl, DIAG_ERROR, "start_no_incoming", msg.data,
                                g->nodes[i].id, "Remove incoming edges to the start node.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: exit_no_outgoing
 *==========================================================================*/

static void rule_exit_no_outgoing(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (!is_terminal_node(&g->nodes[i])) continue;
        const DotEdge *out[256];
        size_t out_count = dot_outgoing_edges(g, g->nodes[i].id, out, 256);
        if (out_count > 0) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg,
                "Terminal node \"%s\" has %zu outgoing edge(s). Terminal nodes must have none.",
                g->nodes[i].id, out_count);
            diagnostic_list_add(dl, DIAG_ERROR, "exit_no_outgoing", msg.data,
                                g->nodes[i].id, "Remove outgoing edges from this terminal node.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: condition_syntax
 *
 * Valid condition: empty, or clauses separated by "&&" / "||", optionally
 * grouped with parentheses. Each leaf clause must contain "=" or "!="
 * with a non-empty key (left side).  Bare truthy keys (no operator) are
 * also accepted.
 *==========================================================================*/

static bool validate_single_clause(const char *start, size_t len) {
    char *clause = str_ndup(start, len);
    char *trimmed = str_trim(clause);

    if (trimmed[0] == '\0') { free(clause); return false; }

    /* A bare key (no operator) is valid: means truthy check */
    char *neq = strstr(trimmed, "!=");
    char *eq  = strchr(trimmed, '=');
    if (!eq) {
        /* Bare key - valid if non-empty */
        free(clause);
        return true;
    }

    /* Key is everything before the operator */
    size_t key_len;
    if (neq && neq < eq) {
        key_len = (size_t)(neq - trimmed);
    } else if (eq > trimmed && *(eq - 1) == '!') {
        key_len = (size_t)(eq - 1 - trimmed);
    } else {
        key_len = (size_t)(eq - trimmed);
    }

    char *key = str_ndup(trimmed, key_len);
    char *tkey = str_trim(key);
    bool valid = (tkey[0] != '\0');
    free(key);
    free(clause);
    return valid;
}

static bool validate_condition_expr(const char *cond) {
    if (!cond || cond[0] == '\0') return true;

    /* Walk the expression, stripping parens and logical operators,
     * validating each leaf clause. */
    const char *p = cond;
    int paren_depth = 0;

    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (*p == '(') { paren_depth++; p++; continue; }
        if (*p == ')') {
            if (paren_depth <= 0) return false; /* unmatched */
            paren_depth--; p++; continue;
        }
        if (*p == '&' && *(p+1) == '&') { p += 2; continue; }
        if (*p == '|' && *(p+1) == '|') { p += 2; continue; }

        /* Read a clause: stop at &&, ||, ), or end */
        const char *clause_start = p;
        while (*p && *p != ')' &&
               !(*p == '&' && *(p+1) == '&') &&
               !(*p == '|' && *(p+1) == '|'))
            p++;

        if (!validate_single_clause(clause_start, (size_t)(p - clause_start)))
            return false;
    }

    return paren_depth == 0;
}

static void rule_condition_syntax(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->edge_count; i++) {
        const DotEdge *e = &g->edges[i];
        if (!e->condition || e->condition[0] == '\0') continue;
        if (!validate_condition_expr(e->condition)) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg,
                "Edge %s -> %s has unparseable condition: \"%s\".",
                str_safe(e->from), str_safe(e->to), e->condition);
            diagnostic_list_add_edge(dl, DIAG_ERROR, "condition_syntax", msg.data,
                                     e->from, e->to,
                                     "Conditions must be clauses separated by && "
                                     "where each clause contains = or !=.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: type_known
 *==========================================================================*/

static const char *known_types[] = {
    "start", "exit", "codergen", "wait.human", "conditional",
    "parallel", "parallel.fan_in", "tool", "stack.manager_loop",
    NULL
};

static void rule_type_known(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        const DotNode *n = &g->nodes[i];
        if (!n->type || n->type[0] == '\0') continue;
        bool found = false;
        for (const char **t = known_types; *t; t++) {
            if (str_eq(n->type, *t)) { found = true; break; }
        }
        if (!found) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Node \"%s\" has unknown type \"%s\".",
                           n->id, n->type);
            diagnostic_list_add(dl, DIAG_WARNING, "type_known", msg.data,
                                n->id,
                                "Use one of: start, exit, codergen, wait.human, "
                                "conditional, parallel, parallel.fan_in, tool, "
                                "stack.manager_loop.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: fidelity_valid
 *==========================================================================*/

static const char *valid_fidelities[] = {
    "full", "truncate", "compact",
    "summary:low", "summary:medium", "summary:high",
    NULL
};

static bool is_valid_fidelity(const char *val) {
    if (!val || val[0] == '\0') return true;
    for (const char **f = valid_fidelities; *f; f++) {
        if (str_eq(val, *f)) return true;
    }
    return false;
}

static void rule_fidelity_valid(const DotGraph *g, DiagnosticList *dl) {
    /* Check node fidelities */
    for (size_t i = 0; i < g->node_count; i++) {
        const DotNode *n = &g->nodes[i];
        if (!is_valid_fidelity(n->fidelity)) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Node \"%s\" has invalid fidelity \"%s\".",
                           n->id, n->fidelity);
            diagnostic_list_add(dl, DIAG_WARNING, "fidelity_valid", msg.data,
                                n->id,
                                "Use one of: full, truncate, compact, "
                                "summary:low, summary:medium, summary:high.");
            strbuf_free(&msg);
        }
    }
    /* Check edge fidelities */
    for (size_t i = 0; i < g->edge_count; i++) {
        const DotEdge *e = &g->edges[i];
        if (!is_valid_fidelity(e->fidelity)) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg, "Edge %s -> %s has invalid fidelity \"%s\".",
                           str_safe(e->from), str_safe(e->to), e->fidelity);
            diagnostic_list_add_edge(dl, DIAG_WARNING, "fidelity_valid", msg.data,
                                     e->from, e->to,
                                     "Use one of: full, truncate, compact, "
                                     "summary:low, summary:medium, summary:high.");
            strbuf_free(&msg);
        }
    }
    /* Check graph-level default fidelity */
    if (!is_valid_fidelity(g->default_fidelity)) {
        StrBuf msg;
        strbuf_init(&msg);
        strbuf_appendf(&msg, "Graph default_fidelity \"%s\" is invalid.",
                       g->default_fidelity);
        diagnostic_list_add(dl, DIAG_WARNING, "fidelity_valid", msg.data,
                            NULL,
                            "Use one of: full, truncate, compact, "
                            "summary:low, summary:medium, summary:high.");
        strbuf_free(&msg);
    }
}

/*============================================================================
 * Rule: retry_target_exists
 *==========================================================================*/

static void check_retry_ref(const DotGraph *g, DiagnosticList *dl,
                             const char *target_val, const char *context_id,
                             const char *field_name) {
    if (!target_val || target_val[0] == '\0') return;
    if (!dot_find_node(g, target_val)) {
        StrBuf msg;
        strbuf_init(&msg);
        if (context_id) {
            strbuf_appendf(&msg, "Node \"%s\" %s references non-existent node \"%s\".",
                           context_id, field_name, target_val);
        } else {
            strbuf_appendf(&msg, "Graph-level %s references non-existent node \"%s\".",
                           field_name, target_val);
        }
        diagnostic_list_add(dl, DIAG_WARNING, "retry_target_exists", msg.data,
                            context_id, "Define the referenced node or correct the reference.");
        strbuf_free(&msg);
    }
}

static void rule_retry_target_exists(const DotGraph *g, DiagnosticList *dl) {
    /* Node-level */
    for (size_t i = 0; i < g->node_count; i++) {
        const DotNode *n = &g->nodes[i];
        check_retry_ref(g, dl, n->retry_target, n->id, "retry_target");
        check_retry_ref(g, dl, n->fallback_retry_target, n->id, "fallback_retry_target");
    }
    /* Graph-level */
    check_retry_ref(g, dl, g->retry_target, NULL, "retry_target");
    check_retry_ref(g, dl, g->fallback_retry_target, NULL, "fallback_retry_target");
}

/*============================================================================
 * Rule: goal_gate_has_retry
 *==========================================================================*/

static void rule_goal_gate_has_retry(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        const DotNode *n = &g->nodes[i];
        if (!n->goal_gate) continue;

        /* Check node-level retry targets first, then graph-level fallbacks */
        bool has_retry = false;
        if (n->retry_target && n->retry_target[0] != '\0') has_retry = true;
        if (n->fallback_retry_target && n->fallback_retry_target[0] != '\0') has_retry = true;
        if (g->retry_target && g->retry_target[0] != '\0') has_retry = true;
        if (g->fallback_retry_target && g->fallback_retry_target[0] != '\0') has_retry = true;

        if (!has_retry) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg,
                "Node \"%s\" has goal_gate=true but no retry_target or "
                "fallback_retry_target is set (node-level or graph-level).",
                n->id);
            diagnostic_list_add(dl, DIAG_WARNING, "goal_gate_has_retry", msg.data,
                                n->id,
                                "Set retry_target or fallback_retry_target on this "
                                "node or at the graph level.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: prompt_on_llm_nodes
 *==========================================================================*/

static void rule_prompt_on_llm_nodes(const DotGraph *g, DiagnosticList *dl) {
    for (size_t i = 0; i < g->node_count; i++) {
        const DotNode *n = &g->nodes[i];
        if (!is_llm_node(n)) continue;

        bool has_prompt = (n->prompt && n->prompt[0] != '\0');
        bool has_label  = (n->label  && n->label[0]  != '\0');

        if (!has_prompt && !has_label) {
            StrBuf msg;
            strbuf_init(&msg);
            strbuf_appendf(&msg,
                "LLM node \"%s\" has neither prompt nor label set.",
                n->id);
            diagnostic_list_add(dl, DIAG_WARNING, "prompt_on_llm_nodes", msg.data,
                                n->id, "Add a prompt or label attribute to this node.");
            strbuf_free(&msg);
        }
    }
}

/*============================================================================
 * Rule: stylesheet_syntax
 *==========================================================================*/

static void rule_stylesheet_syntax(const DotGraph *g, DiagnosticList *dl) {
    if (!g->model_stylesheet || g->model_stylesheet[0] == '\0') return;

    char *err = NULL;
    Stylesheet *ss = stylesheet_parse(g->model_stylesheet, &err);
    if (!ss) {
        StrBuf msg;
        strbuf_init(&msg);
        strbuf_appendf(&msg,
            "The model_stylesheet attribute does not parse as valid stylesheet: %s",
            err ? err : "unknown error");
        diagnostic_list_add(dl, DIAG_ERROR, "stylesheet_syntax", msg.data,
                            NULL, "Fix the model_stylesheet syntax.");
        strbuf_free(&msg);
        free(err);
    } else {
        stylesheet_free(ss);
    }
}

/*============================================================================
 * validate_graph
 *==========================================================================*/

DiagnosticList validate_graph(const DotGraph *g) {
    DiagnosticList dl;
    diagnostic_list_init(&dl);

    if (!g) {
        diagnostic_list_add(&dl, DIAG_ERROR, "graph_null",
            "Graph pointer is NULL.", NULL, NULL);
        return dl;
    }

    /* ERROR rules */
    rule_start_node(g, &dl);
    rule_terminal_node(g, &dl);
    rule_edge_target_exists(g, &dl);
    rule_start_no_incoming(g, &dl);
    rule_exit_no_outgoing(g, &dl);
    rule_reachability(g, &dl);
    rule_condition_syntax(g, &dl);
    rule_stylesheet_syntax(g, &dl);

    /* WARNING rules */
    rule_type_known(g, &dl);
    rule_fidelity_valid(g, &dl);
    rule_retry_target_exists(g, &dl);
    rule_goal_gate_has_retry(g, &dl);
    rule_prompt_on_llm_nodes(g, &dl);

    return dl;
}

/*============================================================================
 * validate_or_raise
 *==========================================================================*/

bool validate_or_raise(const DotGraph *g, char **err_msg) {
    DiagnosticList dl = validate_graph(g);

    if (!diagnostic_list_has_errors(&dl)) {
        diagnostic_list_free(&dl);
        if (err_msg) *err_msg = NULL;
        return true;
    }

    StrBuf sb;
    strbuf_init(&sb);
    strbuf_appendf(&sb, "Graph validation failed:\n");

    size_t err_num = 0;
    for (size_t i = 0; i < dl.count; i++) {
        if (dl.items[i].severity != DIAG_ERROR) continue;
        err_num++;
        strbuf_appendf(&sb, "  %zu. [%s] %s\n",
                       err_num, dl.items[i].rule, dl.items[i].message);
    }

    if (err_msg) {
        *err_msg = strbuf_detach(&sb);
    } else {
        strbuf_free(&sb);
    }

    diagnostic_list_free(&dl);
    return false;
}
