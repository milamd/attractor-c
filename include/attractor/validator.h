#ifndef ATTRACTOR_VALIDATOR_H
#define ATTRACTOR_VALIDATOR_H

#include "attractor/dot_parser.h"

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_INFO
} DiagSeverity;

typedef struct {
    char         *rule;
    DiagSeverity  severity;
    char         *message;
    char         *node_id;          /* may be NULL */
    char         *edge_from;        /* may be NULL */
    char         *edge_to;          /* may be NULL */
    char         *fix;              /* may be NULL */
} Diagnostic;

typedef struct {
    Diagnostic *items;
    size_t      count;
    size_t      cap;
} DiagnosticList;

void diagnostic_list_init(DiagnosticList *dl);
void diagnostic_list_free(DiagnosticList *dl);
void diagnostic_list_add(DiagnosticList *dl, DiagSeverity sev,
                         const char *rule, const char *msg,
                         const char *node_id, const char *fix);
bool diagnostic_list_has_errors(const DiagnosticList *dl);

/* Validate a graph. Returns diagnostics. */
DiagnosticList validate_graph(const DotGraph *g);

/* Validate and raise (return false) if errors exist */
bool validate_or_raise(const DotGraph *g, char **err_msg);

#endif
