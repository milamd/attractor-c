/*============================================================================
 * DOT (Graphviz subset) Parser – Pure C11
 *
 * Implements: attractor/dot_parser.h
 * Dependencies: util/str.h (str_dup, str_eq, str_lower, etc.)
 *==========================================================================*/

#include "attractor/dot_parser.h"
#include "util/str.h"
#include "util/mem.h"
#include "util/io.h"
#include <errno.h>
#include <limits.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*============================================================================
 * Token types
 *==========================================================================*/

typedef enum {
    TOK_IDENT,
    TOK_STRING,
    TOK_NUMBER,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_ARROW,
    TOK_EQUALS,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_DOT,
    TOK_EOF,
    TOK_ERROR
} TokKind;

typedef struct {
    TokKind kind;
    char   *text;       /* heap-allocated token text */
} Token;

/*============================================================================
 * Tokenizer state
 *==========================================================================*/

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    char       *err;
} Lexer;

/*============================================================================
 * Forward declarations – parser internals
 *==========================================================================*/

typedef struct {
    Attr  *items;
    size_t count;
    size_t cap;
} AttrList;

typedef struct {
    DotNode *items;
    size_t   count;
    size_t   cap;
} NodeList;

typedef struct {
    DotEdge *items;
    size_t   count;
    size_t   cap;
} EdgeList;

/* A set of default attrs (for "node [...]" and "edge [...]") */
typedef struct {
    Attr  *items;
    size_t count;
    size_t cap;
} DefaultAttrs;

typedef struct {
    Lexer         lex;
    Token         cur;
    NodeList      nodes;
    EdgeList      edges;
    AttrList      graph_attrs;
    DefaultAttrs  node_defaults;
    DefaultAttrs  edge_defaults;
    char         *graph_name;
    char         *err;
    bool          failed;
    unsigned      depth;
} Parser;

/*============================================================================
 * Comment stripping
 *==========================================================================*/

static char *strip_comments(const char *src) {
    size_t len = strlen(src);
    StrBuf sb;
    strbuf_init(&sb);

    size_t i = 0;
    while (i < len) {
        /* Inside a double-quoted string – pass through verbatim */
        if (src[i] == '"') {
            strbuf_append(&sb, src + i, 1);
            i++;
            while (i < len && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < len) {
                    strbuf_append(&sb, src + i, 2);
                    i += 2;
                } else {
                    strbuf_append(&sb, src + i, 1);
                    i++;
                }
            }
            if (i < len) {
                strbuf_append(&sb, src + i, 1); /* closing quote */
                i++;
            }
            continue;
        }
        /* Line comment */
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
            i += 2;
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        /* Block comment */
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; /* skip closing */
            /* Replace block comment with a space to separate tokens */
            strbuf_append(&sb, " ", 1);
            continue;
        }
        strbuf_append(&sb, src + i, 1);
        i++;
    }
    return strbuf_detach(&sb);
}

/*============================================================================
 * Lexer
 *==========================================================================*/

static void lex_init(Lexer *l, const char *src, size_t len) {
    l->src = src;
    l->pos = 0;
    l->len = len;
    l->err = NULL;
}

static void lex_skip_ws(Lexer *l) {
    while (l->pos < l->len && isspace((unsigned char)l->src[l->pos]))
        l->pos++;
}

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static Token lex_string(Lexer *l) {
    /* opening " already consumed by caller check, but not advanced yet */
    l->pos++; /* skip opening " */
    StrBuf sb;
    strbuf_init(&sb);
    while (l->pos < l->len && l->src[l->pos] != '"') {
        if (l->src[l->pos] == '\\' && l->pos + 1 < l->len) {
            char esc = l->src[l->pos + 1];
            l->pos += 2;
            switch (esc) {
            case 'n':  strbuf_append(&sb, "\n", 1); break;
            case 't':  strbuf_append(&sb, "\t", 1); break;
            case '\\': strbuf_append(&sb, "\\", 1); break;
            case '"':  strbuf_append(&sb, "\"", 1); break;
            default:
                strbuf_append(&sb, "\\", 1);
                strbuf_append(&sb, &esc, 1);
                break;
            }
        } else {
            strbuf_append(&sb, l->src + l->pos, 1);
            l->pos++;
        }
    }
    if(l->pos>=l->len) {strbuf_free(&sb);l->err=str_dup("unterminated quoted string");return (Token){.kind=TOK_ERROR};}
    l->pos++;
    Token t = { .kind = TOK_STRING, .text = strbuf_detach(&sb) };
    return t;
}

static Token lex_number(Lexer *l) {
    size_t start = l->pos;
    if (l->src[l->pos] == '-') l->pos++;
    while (l->pos < l->len && isdigit((unsigned char)l->src[l->pos]))
        l->pos++;
    /* optional decimal part */
    if (l->pos < l->len && l->src[l->pos] == '.') {
        l->pos++;
        while (l->pos < l->len && isdigit((unsigned char)l->src[l->pos]))
            l->pos++;
    }
    Token t = { .kind = TOK_NUMBER, .text = str_ndup(l->src + start, l->pos - start) };
    return t;
}

static Token lex_ident(Lexer *l) {
    size_t start = l->pos;
    while (l->pos < l->len && is_ident_char(l->src[l->pos]))
        l->pos++;
    Token t = { .kind = TOK_IDENT, .text = str_ndup(l->src + start, l->pos - start) };
    return t;
}

static Token lex_next(Lexer *l) {
    if(l->err) return (Token){.kind=TOK_ERROR};
    lex_skip_ws(l);
    if (l->pos >= l->len) {
        Token t = { .kind = TOK_EOF, .text = NULL };
        return t;
    }
    char c = l->src[l->pos];

    if (c == '"') return lex_string(l);

    if (c == '-' && l->pos + 1 < l->len && l->src[l->pos + 1] == '>') {
        l->pos += 2;
        Token t = { .kind = TOK_ARROW, .text = str_dup("->") };
        return t;
    }

    /* Negative number: '-' followed by digit */
    if (c == '-' && l->pos + 1 < l->len && isdigit((unsigned char)l->src[l->pos + 1]))
        return lex_number(l);

    if (isdigit((unsigned char)c))
        return lex_number(l);

    if (is_ident_start(c))
        return lex_ident(l);

    l->pos++;
    switch (c) {
    case '{': { Token t = { .kind = TOK_LBRACE,     .text = str_dup("{") }; return t; }
    case '}': { Token t = { .kind = TOK_RBRACE,     .text = str_dup("}") }; return t; }
    case '[': { Token t = { .kind = TOK_LBRACKET,   .text = str_dup("[") }; return t; }
    case ']': { Token t = { .kind = TOK_RBRACKET,   .text = str_dup("]") }; return t; }
    case '=': { Token t = { .kind = TOK_EQUALS,     .text = str_dup("=") }; return t; }
    case ',': { Token t = { .kind = TOK_COMMA,      .text = str_dup(",") }; return t; }
    case ';': { Token t = { .kind = TOK_SEMICOLON,  .text = str_dup(";") }; return t; }
    case '.': { Token t = { .kind = TOK_DOT,        .text = str_dup(".") }; return t; }
    default: {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_appendf(&sb, "unexpected character '%c'", c);
        free(l->err);
        l->err = strbuf_detach(&sb);
        Token t = { .kind = TOK_ERROR, .text = NULL };
        return t;
    }
    }
}

/*============================================================================
 * Token helpers
 *==========================================================================*/

static void tok_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

/*============================================================================
 * Parser – helpers
 *==========================================================================*/

static void parser_advance(Parser *p) {
    tok_free(&p->cur);
    p->cur = lex_next(&p->lex);
    if(p->cur.kind!=TOK_EOF && !p->cur.text) p->failed=true;
    if (p->cur.kind == TOK_ERROR && !p->err && !p->failed) {
        p->err = p->lex.err;
        p->lex.err = NULL;
    }
}

static bool parser_at(const Parser *p, TokKind k) {
    return p->cur.kind == k;
}

static bool parser_at_ident_val(const Parser *p, const char *val) {
    return p->cur.kind == TOK_IDENT && str_eq(p->cur.text, val);
}

static bool parser_consume(Parser *p, TokKind k) {
    if (p->cur.kind == k) {
        parser_advance(p);
        return true;
    }
    return false;
}

static void parser_skip_semi(Parser *p) {
    while (parser_at(p, TOK_SEMICOLON))
        parser_advance(p);
}

static void parser_error(Parser *p, const char *msg) {
    p->failed=true;
    if (!p->err)
        p->err = str_dup(msg);
}

/*============================================================================
 * Dynamic array helpers
 *==========================================================================*/

static void attrlist_init(AttrList *al) {
    al->items = NULL;
    al->count = 0;
    al->cap = 0;
}

static void attrlist_add(AttrList *al, const char *key, const char *value) {
    if (al->count == al->cap) {
        size_t cap=al->cap?al->cap*2:8;
        Attr *items=mem_reallocarray(al->items,cap,sizeof(*items));
        if(!items) return ;
        al->items=items;al->cap=cap;
    }
    al->items[al->count].key   = str_dup(key);
    al->items[al->count].value = str_dup(value);
    al->count++;
}

static void attrlist_free_contents(AttrList *al) {
    for (size_t i = 0; i < al->count; i++) {
        free(al->items[i].key);
        free(al->items[i].value);
    }
    free(al->items);
    al->items = NULL;
    al->count = al->cap = 0;
}

static void defaultattrs_init(DefaultAttrs *d) {
    d->items = NULL;
    d->count = 0;
    d->cap = 0;
}

static void defaultattrs_add(DefaultAttrs *d, const char *key, const char *value) {
    /* Overwrite existing if key already present */
    for (size_t i = 0; i < d->count; i++) {
        if (str_eq(d->items[i].key, key)) {
            free(d->items[i].value);
            d->items[i].value = str_dup(value);
            return;
        }
    }
    if (d->count == d->cap) {
        size_t cap=d->cap?d->cap*2:8;
        Attr *items=mem_reallocarray(d->items,cap,sizeof(*items));
        if(!items) return ;
        d->items=items;d->cap=cap;
    }
    d->items[d->count].key   = str_dup(key);
    d->items[d->count].value = str_dup(value);
    d->count++;
}

static void defaultattrs_free(DefaultAttrs *d) {
    for (size_t i = 0; i < d->count; i++) {
        free(d->items[i].key);
        free(d->items[i].value);
    }
    free(d->items);
    d->items = NULL;
    d->count = d->cap = 0;
}

static void nodelist_init(NodeList *nl) {
    nl->items = NULL;
    nl->count = 0;
    nl->cap = 0;
}

static DotNode *nodelist_add(NodeList *nl) {
    if(nl->count>=10000) return NULL;
    if (nl->count == nl->cap) {
        size_t cap=nl->cap?nl->cap*2:16;
        DotNode *items=mem_reallocarray(nl->items,cap,sizeof(*items));
        if(!items) return NULL;
        nl->items=items;nl->cap=cap;
    }
    DotNode *n = &nl->items[nl->count++];
    memset(n, 0, sizeof(*n));
    return n;
}

static void edgelist_init(EdgeList *el) {
    el->items = NULL;
    el->count = 0;
    el->cap = 0;
}

static DotEdge *edgelist_add(EdgeList *el) {
    if(el->count>=100000) return NULL;
    if (el->count == el->cap) {
        size_t cap=el->cap?el->cap*2:16;
        DotEdge *items=mem_reallocarray(el->items,cap,sizeof(*items));
        if(!items) return NULL;
        el->items=items;el->cap=cap;
    }
    DotEdge *e = &el->items[el->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

/*============================================================================
 * Parser – attr block parsing  [key=value, ...]
 *==========================================================================*/

static void parse_attr_block(Parser *p, AttrList *attrs) {
    if (!parser_consume(p, TOK_LBRACKET)) return;

    while (!parser_at(p, TOK_RBRACKET) && !parser_at(p, TOK_EOF) && !p->err && !p->failed) {
        /* Key: may be qualified (e.g. llm.model) */
        if (!parser_at(p, TOK_IDENT) && !parser_at(p, TOK_STRING)) {
            parser_error(p, "expected attribute key");
            return;
        }
        StrBuf keybuf;
        strbuf_init(&keybuf);
        strbuf_append_cstr(&keybuf, p->cur.text);
        parser_advance(p);

        /* Handle qualified identifiers: key.subkey.subkey */
        while (parser_at(p, TOK_DOT)) {
            strbuf_append_cstr(&keybuf, ".");
            parser_advance(p); /* consume dot */
            if (parser_at(p, TOK_IDENT)) {
                strbuf_append_cstr(&keybuf, p->cur.text);
                parser_advance(p);
            } else {
                parser_error(p, "expected identifier after '.'");
                strbuf_free(&keybuf);
                return;
            }
        }
        char *key = strbuf_detach(&keybuf);
        if(!key) {parser_error(p,"Attribute allocation failed");return;}

        if (!parser_consume(p, TOK_EQUALS)) {
            parser_error(p, "expected '=' in attribute");
            free(key);
            return;
        }

        /* Value: string, number, ident (including true/false) */
        if (!parser_at(p, TOK_STRING) && !parser_at(p, TOK_NUMBER) &&
            !parser_at(p, TOK_IDENT)) {
            parser_error(p, "expected attribute value");
            free(key);
            return;
        }
        char *value = str_dup(p->cur.text);
        parser_advance(p);

        attrlist_add(attrs, key, value);
        free(key);
        free(value);

        /* Optional comma separator; also allow semicolon inside attr blocks */
        if (parser_at(p, TOK_COMMA) || parser_at(p, TOK_SEMICOLON))
            parser_advance(p);
    }
    parser_consume(p, TOK_RBRACKET);
}

/*============================================================================
 * Parser – find or create node
 *==========================================================================*/

static DotNode *find_or_create_node(Parser *p, const char *id) {
    for (size_t i = 0; i < p->nodes.count; i++) {
        if (str_eq(p->nodes.items[i].id, id))
            return &p->nodes.items[i];
    }
    DotNode *n = nodelist_add(&p->nodes);
    if(!n) {parser_error(p,"Node allocation failed");return NULL;}
    n->id = str_dup(id);
    return n;
}

/*============================================================================
 * Parser – merge attrs into node (append, don't deduplicate yet)
 *==========================================================================*/

static void merge_attrs_into_node(DotNode *n, const AttrList *al) {
    for (size_t i = 0; i < al->count; i++) {
        /* Grow node's attrs array */
        size_t idx = n->attr_count;
        Attr *items=mem_reallocarray(n->attrs,n->attr_count+1,sizeof(*items));
        if(!items) return;n->attrs=items;n->attr_count++;
        n->attrs[idx].key   = str_dup(al->items[i].key);
        n->attrs[idx].value = str_dup(al->items[i].value);
    }
}

/*============================================================================
 * Parser – statements
 *==========================================================================*/

static void parse_statements(Parser *p, const DefaultAttrs *scope_node_defaults,
                             const char *scope_class);

static void parse_subgraph(Parser *p, const DefaultAttrs *parent_node_defaults) {
    /* 'subgraph' already consumed */
    char *sg_name = NULL;
    if (parser_at(p, TOK_IDENT) || parser_at(p, TOK_STRING)) {
        sg_name = str_dup(p->cur.text);
        parser_advance(p);
    }

    if (!parser_consume(p, TOK_LBRACE)) {
        parser_error(p, "expected '{' after subgraph");
        free(sg_name);
        return;
    }

    /* Create scoped node defaults from parent */
    DefaultAttrs scoped;
    defaultattrs_init(&scoped);
    if (parent_node_defaults) {
        for (size_t i = 0; i < parent_node_defaults->count; i++)
            defaultattrs_add(&scoped, parent_node_defaults->items[i].key,
                             parent_node_defaults->items[i].value);
    }

    /* Derive class from subgraph label. First, check for a label attr inside.
     * But we need to parse statements first. We'll derive the class from
     * sg_name or look for a label in the subgraph's graph attrs.
     *
     * Strategy: we parse into the subgraph, looking for "graph [label=...]"
     * or "label = ..." that sets the subgraph label, then derive class from it.
     * For simplicity, derive from sg_name if present (strip "cluster_" prefix). */
    char *scope_class = NULL;
    if (sg_name) {
        /* Strip "cluster_" prefix if present */
        const char *base = sg_name;
        if (str_starts_with(base, "cluster_"))
            base = sg_name + 8;
        /* Lowercase, spaces to hyphens, strip non-alphanumeric except hyphens */
        char *tmp = str_dup(base);
        str_lower(tmp);
        StrBuf cls;
        strbuf_init(&cls);
        for (size_t i = 0; tmp && tmp[i]; i++) {
            char c = tmp[i];
            if (c == ' ' || c == '_')
                strbuf_append(&cls, "-", 1);
            else if (isalnum((unsigned char)c) || c == '-')
                strbuf_append(&cls, &c, 1);
        }
        free(tmp);
        scope_class = strbuf_detach(&cls);
    }

    if(p->depth>=64) parser_error(p,"DOT subgraph nesting limit");
    else {p->depth++;parse_statements(p, &scoped, scope_class);p->depth--;}

    if (!parser_consume(p, TOK_RBRACE)) {
        parser_error(p, "expected '}' closing subgraph");
    }

    defaultattrs_free(&scoped);
    free(sg_name);
    free(scope_class);
}

static void parse_statements(Parser *p, const DefaultAttrs *scope_node_defaults,
                             const char *scope_class) {
    /* Copy scoped defaults into a mutable version so "node [...]" inside scope
     * can modify them */
    DefaultAttrs local_nd;
    defaultattrs_init(&local_nd);
    if (scope_node_defaults) {
        for (size_t i = 0; i < scope_node_defaults->count; i++)
            defaultattrs_add(&local_nd, scope_node_defaults->items[i].key,
                             scope_node_defaults->items[i].value);
    } else {
        /* Copy from parser-level node defaults */
        for (size_t i = 0; i < p->node_defaults.count; i++)
            defaultattrs_add(&local_nd, p->node_defaults.items[i].key,
                             p->node_defaults.items[i].value);
    }

    while (!parser_at(p, TOK_RBRACE) && !parser_at(p, TOK_EOF) && !p->err && !p->failed) {
        parser_skip_semi(p);
        if (parser_at(p, TOK_RBRACE) || parser_at(p, TOK_EOF)) break;

        /* subgraph */
        if (parser_at_ident_val(p, "subgraph")) {
            parser_advance(p);
            parse_subgraph(p, &local_nd);
            parser_skip_semi(p);
            continue;
        }

        /* graph [...] */
        if (parser_at_ident_val(p, "graph")) {
            parser_advance(p);
            if (parser_at(p, TOK_LBRACKET)) {
                AttrList attrs;
                attrlist_init(&attrs);
                parse_attr_block(p, &attrs);
                for (size_t i = 0; i < attrs.count; i++)
                    attrlist_add(&p->graph_attrs, attrs.items[i].key,
                                 attrs.items[i].value);
                attrlist_free_contents(&attrs);
            }
            parser_skip_semi(p);
            continue;
        }

        /* node [...] – set node defaults */
        if (parser_at_ident_val(p, "node")) {
            parser_advance(p);
            if (parser_at(p, TOK_LBRACKET)) {
                AttrList attrs;
                attrlist_init(&attrs);
                parse_attr_block(p, &attrs);
                for (size_t i = 0; i < attrs.count; i++) {
                    defaultattrs_add(&local_nd, attrs.items[i].key,
                                     attrs.items[i].value);
                    /* Also update parser-global defaults when not in subgraph */
                    if (!scope_node_defaults) {
                        defaultattrs_add(&p->node_defaults, attrs.items[i].key,
                                         attrs.items[i].value);
                    }
                }
                attrlist_free_contents(&attrs);
            }
            parser_skip_semi(p);
            continue;
        }

        /* edge [...] – set edge defaults */
        if (parser_at_ident_val(p, "edge")) {
            parser_advance(p);
            if (parser_at(p, TOK_LBRACKET)) {
                AttrList attrs;
                attrlist_init(&attrs);
                parse_attr_block(p, &attrs);
                for (size_t i = 0; i < attrs.count; i++)
                    defaultattrs_add(&p->edge_defaults, attrs.items[i].key,
                                     attrs.items[i].value);
                attrlist_free_contents(&attrs);
            }
            parser_skip_semi(p);
            continue;
        }

        /* Must be an identifier (node/edge stmt) or graph-level key=value */
        if (!parser_at(p, TOK_IDENT) && !parser_at(p, TOK_STRING)) {
            StrBuf sb;
            strbuf_init(&sb);
            strbuf_appendf(&sb, "unexpected token: %s",
                           p->cur.text ? p->cur.text : "<null>");
            parser_error(p, sb.data);
            strbuf_free(&sb);
            goto done;
        }

        char *first_id = str_dup(p->cur.text);
        if(!first_id) {parser_error(p,"Identifier allocation failed");goto done;}
        parser_advance(p);

        /* Graph-level attr: identifier '=' value (not inside bracket) */
        if (parser_at(p, TOK_EQUALS)) {
            parser_advance(p);
            if (parser_at(p, TOK_STRING) || parser_at(p, TOK_IDENT) ||
                parser_at(p, TOK_NUMBER)) {
                attrlist_add(&p->graph_attrs, first_id, p->cur.text);
                parser_advance(p);
            } else {
                parser_error(p, "expected value after '='");
            }
            free(first_id);
            parser_skip_semi(p);
            continue;
        }

        /* Edge statement: A -> B -> C ... [attrs] */
        if (parser_at(p, TOK_ARROW)) {
            /* Collect chain of node ids */
            size_t id_cap = 8;
            size_t id_count = 1;
            char **ids = mem_calloc(id_cap,sizeof(char *));
            if(!ids) {free(first_id);parser_error(p,"Chain allocation failed");goto done;}
            ids[0] = first_id;

            while (parser_at(p, TOK_ARROW)) {
                parser_advance(p); /* consume -> */
                if (!parser_at(p, TOK_IDENT) && !parser_at(p, TOK_STRING)) {
                    parser_error(p, "expected node id after '->'");
                    for (size_t i = 0; i < id_count; i++) free(ids[i]);
                    free(ids);
                    goto done;
                }
                if (id_count == id_cap) {
                    id_cap *= 2;
                    char **items=mem_reallocarray(ids,id_cap,sizeof(*items));
                    if(!items) {
                        for(size_t k=0;k<id_count;k++) free(ids[k]);free(ids);parser_error(p,"Chain allocation failed");goto done;
                    }ids=items;
                }
                ids[id_count++] = str_dup(p->cur.text);
                parser_advance(p);
            }

            /* Optional attr block */
            AttrList edge_attrs;
            attrlist_init(&edge_attrs);
            if (parser_at(p, TOK_LBRACKET))
                parse_attr_block(p, &edge_attrs);

            /* Ensure all endpoint nodes exist */
            for (size_t i = 0; i < id_count; i++) {
                DotNode *n = find_or_create_node(p, ids[i]);
                if(!n) break;
                /* If the node has no attrs yet and we have scope class, assign it */
                if (scope_class && !n->class_attr) {
                    n->class_attr = str_dup(scope_class);
                }
            }

            /* Create one edge per consecutive pair */
            for (size_t i = 0; i + 1 < id_count && !p->failed; i++) {
                DotEdge *e = edgelist_add(&p->edges);
                if(!e) {parser_error(p,"Edge allocation failed");break;}
                e->from = str_dup(ids[i]);
                e->to   = str_dup(ids[i + 1]);

                /* Apply edge defaults */
                for (size_t d = 0; d < p->edge_defaults.count; d++) {
                    size_t idx=e->attr_count;
                    Attr *items=mem_reallocarray(e->attrs,e->attr_count+1,sizeof(*items));
                    if(!items) {parser_error(p,"Edge attributes allocation failed");break;}e->attrs=items;e->attr_count++;
                    e->attrs[idx].key   = str_dup(p->edge_defaults.items[d].key);
                    e->attrs[idx].value = str_dup(p->edge_defaults.items[d].value);
                }

                /* Apply explicit attrs (will override defaults on resolve) */
                for (size_t a = 0; a < edge_attrs.count; a++) {
                    size_t idx=e->attr_count;
                    Attr *items=mem_reallocarray(e->attrs,e->attr_count+1,sizeof(*items));
                    if(!items) {parser_error(p,"Edge attributes allocation failed");break;}e->attrs=items;e->attr_count++;
                    e->attrs[idx].key   = str_dup(edge_attrs.items[a].key);
                    e->attrs[idx].value = str_dup(edge_attrs.items[a].value);
                }
            }

            attrlist_free_contents(&edge_attrs);
            for (size_t i = 0; i < id_count; i++) free(ids[i]);
            free(ids);
            parser_skip_semi(p);
            continue;
        }

        /* Node statement: NodeId [attrs]? ;? */
        {
            DotNode *node = find_or_create_node(p, first_id);
            if(!node) {free(first_id);goto done;}

            /* Apply scope class if set and node doesn't have one yet */
            if (scope_class && !node->class_attr) {
                node->class_attr = str_dup(scope_class);
            }

            /* Apply current node defaults (scoped) as base attrs */
            AttrList combined;
            attrlist_init(&combined);
            for (size_t d = 0; d < local_nd.count; d++)
                attrlist_add(&combined, local_nd.items[d].key,
                             local_nd.items[d].value);

            /* Parse explicit attrs */
            if (parser_at(p, TOK_LBRACKET))
                parse_attr_block(p, &combined);

            /* Merge into node */
            merge_attrs_into_node(node, &combined);
            attrlist_free_contents(&combined);
            free(first_id);
            parser_skip_semi(p);
            continue;
        }
    }

done:
    defaultattrs_free(&local_nd);
}

/*============================================================================
 * Resolve convenience fields from attrs
 *==========================================================================*/

/* Helper: find the last value for a key in an attrs array (last wins). */
static const char *attrs_find_last(const Attr *attrs, size_t count,
                                   const char *key) {
    const char *result = NULL;
    for (size_t i = 0; i < count; i++) {
        if (str_eq(attrs[i].key, key))
            result = attrs[i].value;
    }
    return result;
}

static int parse_int(const char *s, int def) {
    if (!s) return def;
    char *end = NULL;
    errno=0;long v = strtol(s, &end, 10);
    if(end==s || *end || errno==ERANGE || v<INT_MIN || v>INT_MAX) return def;
    return (int)v;
}

static bool parse_bool(const char *s, bool def) {
    if (!s) return def;
    if (str_eq(s, "true") || str_eq(s, "1") || str_eq(s, "yes")) return true;
    if (str_eq(s, "false") || str_eq(s, "0") || str_eq(s, "no")) return false;
    return def;
}

static void resolve_node(DotNode *n) {
    const char *v;

    v = attrs_find_last(n->attrs, n->attr_count, "label");
    if (v) { free(n->label); n->label = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "shape");
    if (v) { free(n->shape); n->shape = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "type");
    if (v) { free(n->type); n->type = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "prompt");
    if (v) { free(n->prompt); n->prompt = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "max_retries");
    n->max_retries = parse_int(v, -1);  /* -1 = not set, use graph default */

    v = attrs_find_last(n->attrs, n->attr_count, "goal_gate");
    n->goal_gate = parse_bool(v, false);

    v = attrs_find_last(n->attrs, n->attr_count, "retry_target");
    if (v) { free(n->retry_target); n->retry_target = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "fallback_retry_target");
    if (v) { free(n->fallback_retry_target); n->fallback_retry_target = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "fidelity");
    if (v) { free(n->fidelity); n->fidelity = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "thread_id");
    if (v) { free(n->thread_id); n->thread_id = str_dup(v); }

    /* class_attr may already be set from subgraph scope; attr overrides */
    v = attrs_find_last(n->attrs, n->attr_count, "class");
    if (v) { free(n->class_attr); n->class_attr = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "timeout");
    if (v) { free(n->timeout); n->timeout = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "llm_model");
    if (!v) v = attrs_find_last(n->attrs, n->attr_count, "llm.model");
    if (v) { free(n->llm_model); n->llm_model = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "llm_provider");
    if (!v) v = attrs_find_last(n->attrs, n->attr_count, "llm.provider");
    if (v) { free(n->llm_provider); n->llm_provider = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "reasoning_effort");
    if (v) { free(n->reasoning_effort); n->reasoning_effort = str_dup(v); }

    v = attrs_find_last(n->attrs, n->attr_count, "auto_status");
    n->auto_status = parse_bool(v, false);

    v = attrs_find_last(n->attrs, n->attr_count, "allow_partial");
    n->allow_partial = parse_bool(v, false);
}

static void resolve_edge(DotEdge *e) {
    const char *v;

    v = attrs_find_last(e->attrs, e->attr_count, "label");
    if (v) { free(e->label); e->label = str_dup(v); }

    v = attrs_find_last(e->attrs, e->attr_count, "condition");
    if (v) { free(e->condition); e->condition = str_dup(v); }

    v = attrs_find_last(e->attrs, e->attr_count, "weight");
    e->weight = parse_int(v, 0);

    v = attrs_find_last(e->attrs, e->attr_count, "fidelity");
    if (v) { free(e->fidelity); e->fidelity = str_dup(v); }

    v = attrs_find_last(e->attrs, e->attr_count, "thread_id");
    if (v) { free(e->thread_id); e->thread_id = str_dup(v); }

    v = attrs_find_last(e->attrs, e->attr_count, "loop_restart");
    e->loop_restart = parse_bool(v, false);
}

/*============================================================================
 * Resolve graph-level attrs
 *==========================================================================*/

static void resolve_graph_attrs(DotGraph *g, const AttrList *ga) {
    g->default_max_retry = 50; /* default */

    for (size_t i = 0; i < ga->count; i++) {
        const char *k = ga->items[i].key;
        const char *v = ga->items[i].value;

        if (str_eq(k, "goal")) {
            free(g->goal);
            g->goal = str_dup(v);
        } else if (str_eq(k, "label")) {
            free(g->label);
            g->label = str_dup(v);
        } else if (str_eq(k, "model_stylesheet")) {
            free(g->model_stylesheet);
            g->model_stylesheet = str_dup(v);
        } else if (str_eq(k, "default_max_retry")) {
            g->default_max_retry = parse_int(v, 50);
        } else if (str_eq(k, "retry_target")) {
            free(g->retry_target);
            g->retry_target = str_dup(v);
        } else if (str_eq(k, "fallback_retry_target")) {
            free(g->fallback_retry_target);
            g->fallback_retry_target = str_dup(v);
        } else if (str_eq(k, "default_fidelity")) {
            free(g->default_fidelity);
            g->default_fidelity = str_dup(v);
        } else if (str_eq(k, "default_model")) {
            free(g->default_model);
            g->default_model = str_dup(v);
        } else if (str_eq(k, "default_provider")) {
            free(g->default_provider);
            g->default_provider = str_dup(v);
        }
    }
}

/*============================================================================
 * Public API: dot_parse
 *==========================================================================*/

static void dot_graph_clear(DotGraph *g);

DotGraph *dot_parse(const char *source, char **err_msg) {
    unsigned long failures=mem_failure_count();
    if (!source || strnlen(source,ATTRACTOR_INPUT_LIMIT+1)>ATTRACTOR_INPUT_LIMIT) {
        if (err_msg) *err_msg = str_dup("null source");
        return NULL;
    }

    /* Phase 1: strip comments */
    char *clean = strip_comments(source);
    if(!clean) {if(err_msg) *err_msg=str_dup("DOT allocation/size limit");return NULL;}

    /* Phase 2: tokenize + parse */
    Parser p;
    memset(&p, 0, sizeof(p));
    lex_init(&p.lex, clean, strlen(clean));
    nodelist_init(&p.nodes);
    edgelist_init(&p.edges);
    attrlist_init(&p.graph_attrs);
    defaultattrs_init(&p.node_defaults);
    defaultattrs_init(&p.edge_defaults);

    /* Read first token */
    p.cur = lex_next(&p.lex);
    if(p.cur.kind!=TOK_EOF && !p.cur.text) p.failed=true;

    /* Expect: 'digraph' */
    if (!parser_at_ident_val(&p, "digraph")) {
        parser_error(&p, "expected 'digraph'");
    }
    parser_advance(&p);

    /* Graph name */
    if (parser_at(&p, TOK_IDENT) || parser_at(&p, TOK_STRING)) {
        p.graph_name = str_dup(p.cur.text);
        parser_advance(&p);
    }

    /* Opening brace */
    if (!parser_consume(&p, TOK_LBRACE)) {
        parser_error(&p, "expected '{' after digraph name");
    }

    /* Parse statements (top-level scope: no parent defaults, no scope class) */
    if (!p.err && !p.failed) {
        parse_statements(&p, NULL, NULL);
    }

    /* Closing brace */
    if (!p.err && !parser_consume(&p, TOK_RBRACE)) {
        parser_error(&p, "expected '}' at end of digraph");
    }

    if(!p.err && !p.failed && !parser_at(&p,TOK_EOF)) parser_error(&p,"Trailing DOT input");
    /* Check for errors */
    if (p.err || p.failed || mem_failure_count()!=failures) {
        if (err_msg) *err_msg = p.err;
        else free(p.err);

        /* Cleanup */
        for (size_t i = 0; i < p.nodes.count; i++) {
            DotNode *n = &p.nodes.items[i];
            free(n->id);
            for (size_t j = 0; j < n->attr_count; j++) {
                free(n->attrs[j].key);
                free(n->attrs[j].value);
            }
            free(n->attrs);
            free(n->label);
            free(n->shape);
            free(n->type);
            free(n->prompt);
            free(n->retry_target);
            free(n->fallback_retry_target);
            free(n->fidelity);
            free(n->thread_id);
            free(n->class_attr);
            free(n->timeout);
            free(n->llm_model);
            free(n->llm_provider);
            free(n->reasoning_effort);
        }
        free(p.nodes.items);
        for (size_t i = 0; i < p.edges.count; i++) {
            DotEdge *e = &p.edges.items[i];
            free(e->from);
            free(e->to);
            for (size_t j = 0; j < e->attr_count; j++) {
                free(e->attrs[j].key);
                free(e->attrs[j].value);
            }
            free(e->attrs);
            free(e->label);
            free(e->condition);
            free(e->fidelity);
            free(e->thread_id);
        }
        free(p.edges.items);
        attrlist_free_contents(&p.graph_attrs);
        defaultattrs_free(&p.node_defaults);
        defaultattrs_free(&p.edge_defaults);
        free(p.graph_name);
        tok_free(&p.cur);
        free(p.lex.err);
        free(clean);
        return NULL;
    }

    /* Phase 3: resolve convenience fields */
    for (size_t i = 0; i < p.nodes.count; i++)
        resolve_node(&p.nodes.items[i]);

    for (size_t i = 0; i < p.edges.count; i++)
        resolve_edge(&p.edges.items[i]);

    /* Phase 4: build the graph */
    DotGraph *g = mem_calloc(1, sizeof(DotGraph));
    if(!g) {
        DotGraph partial={.name=p.graph_name,.nodes=p.nodes.items,.node_count=p.nodes.count,.edges=p.edges.items,.edge_count=p.edges.count};
        attrlist_free_contents(&p.graph_attrs);defaultattrs_free(&p.node_defaults);defaultattrs_free(&p.edge_defaults);tok_free(&p.cur);free(clean);
        /* Allocation-free graph destruction supports stack storage internally. */
        dot_graph_clear(&partial);return NULL;
    }
    g->name       = p.graph_name;
    g->nodes      = p.nodes.items;
    g->node_count = p.nodes.count;
    g->edges      = p.edges.items;
    g->edge_count = p.edges.count;

    resolve_graph_attrs(g, &p.graph_attrs);

    /* Cleanup temporaries */
    attrlist_free_contents(&p.graph_attrs);
    defaultattrs_free(&p.node_defaults);
    defaultattrs_free(&p.edge_defaults);
    tok_free(&p.cur);
    free(clean);

    if(mem_failure_count()!=failures) {dot_graph_free(g);if(err_msg) *err_msg=str_dup("DOT allocation failure");return NULL;}
    if (err_msg) *err_msg = NULL;
    return g;
}

/*============================================================================
 * Public API: dot_graph_free
 *==========================================================================*/

static void dot_graph_clear(DotGraph *g) {
    if (!g) return;

    free(g->name);
    free(g->goal);
    free(g->label);
    free(g->model_stylesheet);
    free(g->retry_target);
    free(g->fallback_retry_target);
    free(g->default_fidelity);
    free(g->default_model);
    free(g->default_provider);

    for (size_t i = 0; i < g->node_count; i++) {
        DotNode *n = &g->nodes[i];
        free(n->id);
        for (size_t j = 0; j < n->attr_count; j++) {
            free(n->attrs[j].key);
            free(n->attrs[j].value);
        }
        free(n->attrs);
        free(n->label);
        free(n->shape);
        free(n->type);
        free(n->prompt);
        free(n->retry_target);
        free(n->fallback_retry_target);
        free(n->fidelity);
        free(n->thread_id);
        free(n->class_attr);
        free(n->timeout);
        free(n->llm_model);
        free(n->llm_provider);
        free(n->reasoning_effort);
    }
    free(g->nodes);

    for (size_t i = 0; i < g->edge_count; i++) {
        DotEdge *e = &g->edges[i];
        free(e->from);
        free(e->to);
        for (size_t j = 0; j < e->attr_count; j++) {
            free(e->attrs[j].key);
            free(e->attrs[j].value);
        }
        free(e->attrs);
        free(e->label);
        free(e->condition);
        free(e->fidelity);
        free(e->thread_id);
    }
    free(g->edges);

    memset(g,0,sizeof(*g));
}
void dot_graph_free(DotGraph *g) {if(g) {dot_graph_clear(g);free(g);}}

/*============================================================================
 * Public API: dot_find_node
 *==========================================================================*/

DotNode *dot_find_node(const DotGraph *g, const char *id) {
    if (!g || !id) return NULL;
    for (size_t i = 0; i < g->node_count; i++) {
        if (str_eq(g->nodes[i].id, id))
            return &g->nodes[i];
    }
    return NULL;
}

/*============================================================================
 * Public API: dot_outgoing_edges
 *==========================================================================*/

size_t dot_outgoing_edges(const DotGraph *g, const char *node_id,
                          const DotEdge **out, size_t max) {
    if (!g || !node_id) return 0;
    size_t found = 0;
    for (size_t i = 0; i < g->edge_count; i++) {
        if (str_eq(g->edges[i].from, node_id))
            {if(out && found<max) out[found]=&g->edges[i];found++;}
    }
    return found;
}

/*============================================================================
 * Public API: dot_incoming_edges
 *==========================================================================*/

size_t dot_incoming_edges(const DotGraph *g, const char *node_id,
                          const DotEdge **out, size_t max) {
    if (!g || !node_id) return 0;
    size_t found = 0;
    for (size_t i = 0; i < g->edge_count; i++) {
        if (str_eq(g->edges[i].to, node_id))
            {if(out && found<max) out[found]=&g->edges[i];found++;}
    }
    return found;
}

/*============================================================================
 * Public API: dot_node_attr / dot_edge_attr
 *==========================================================================*/

const char *dot_node_attr(const DotNode *n, const char *key, const char *def) {
    if (!n || !key) return def;
    /* Search backwards so last-wins semantics apply */
    for (size_t i = n->attr_count; i > 0; i--) {
        if (str_eq(n->attrs[i - 1].key, key))
            return n->attrs[i - 1].value;
    }
    return def;
}

const char *dot_edge_attr(const DotEdge *e, const char *key, const char *def) {
    if (!e || !key) return def;
    /* Search backwards so last-wins semantics apply */
    for (size_t i = e->attr_count; i > 0; i--) {
        if (str_eq(e->attrs[i - 1].key, key))
            return e->attrs[i - 1].value;
    }
    return def;
}

const char *dot_node_role(const DotNode *n) {
    if(n->type && *n->type) return n->type;
    const char *shapes[]={"Mdiamond","Msquare","box","hexagon","diamond","component","tripleoctagon","parallelogram","house"};
    const char *roles[]={"start","exit","codergen","wait.human","conditional","parallel","parallel.fan_in","tool","stack.manager_loop"};
    for(size_t i=0;i<sizeof(shapes)/sizeof(*shapes);i++) if(str_eq(n->shape,shapes[i])) return roles[i];
    if(str_eq(n->id,"start") || str_eq(n->id,"Start")) return "start";
    if(str_eq(n->id,"exit") || str_eq(n->id,"Exit") || str_eq(n->id,"end") || str_eq(n->id,"End")) return "exit";
    if(!n->shape || !*n->shape || str_eq(n->shape,"rect") || str_eq(n->shape,"rectangle")) return "codergen";
    return "unsupported";
}
