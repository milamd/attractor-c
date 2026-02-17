#ifndef ATTRACTOR_DOT_PARSER_H
#define ATTRACTOR_DOT_PARSER_H

#include <stddef.h>
#include <stdbool.h>

/*============================================================================
 * Graph Model
 *==========================================================================*/

typedef struct {
    char *key;
    char *value;
} Attr;

typedef struct {
    char   *id;
    Attr   *attrs;
    size_t  attr_count;
    /* Resolved attributes (convenience accessors) */
    char   *label;
    char   *shape;
    char   *type;
    char   *prompt;
    int     max_retries;
    bool    goal_gate;
    char   *retry_target;
    char   *fallback_retry_target;
    char   *fidelity;
    char   *thread_id;
    char   *class_attr;
    char   *timeout;
    char   *llm_model;
    char   *llm_provider;
    char   *reasoning_effort;
    bool    auto_status;
    bool    allow_partial;
} DotNode;

typedef struct {
    char   *from;
    char   *to;
    Attr   *attrs;
    size_t  attr_count;
    /* Resolved attributes */
    char   *label;
    char   *condition;
    int     weight;
    char   *fidelity;
    char   *thread_id;
    bool    loop_restart;
} DotEdge;

typedef struct {
    char    *name;              /* digraph name */
    DotNode *nodes;
    size_t   node_count;
    DotEdge *edges;
    size_t   edge_count;

    /* Graph-level attributes */
    char   *goal;
    char   *label;
    char   *model_stylesheet;
    int     default_max_retry;
    char   *retry_target;
    char   *fallback_retry_target;
    char   *default_fidelity;
    char   *default_model;
    char   *default_provider;
} DotGraph;

/* Parse a DOT source string. Returns NULL on error, sets *err_msg. */
DotGraph   *dot_parse(const char *source, char **err_msg);
void        dot_graph_free(DotGraph *g);

/* Find nodes/edges */
DotNode    *dot_find_node(const DotGraph *g, const char *id);
size_t      dot_outgoing_edges(const DotGraph *g, const char *node_id,
                               const DotEdge **out, size_t max);
size_t      dot_incoming_edges(const DotGraph *g, const char *node_id,
                               const DotEdge **out, size_t max);

/* Get attribute value from a node or return def */
const char *dot_node_attr(const DotNode *n, const char *key, const char *def);
const char *dot_edge_attr(const DotEdge *e, const char *key, const char *def);

#endif
