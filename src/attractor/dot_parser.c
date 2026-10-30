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
#include "util/strindex.h"
#include <stdint.h>
#include <errno.h>
#include <limits.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Distinct attributes per element or attribute block; bounds per-statement work. */
#define DOT_ATTR_LIMIT 1024u

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
    TokKind     kind;
    const char *ptr;
    size_t      len;
    char       *owned;
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

typedef struct {
    Lexer         lex;
    Token         cur;
    NodeList      nodes;
    EdgeList      edges;
    AttrList      graph_attrs;
    AttrList      node_defaults;
    AttrList      edge_defaults;
    StrIndex      node_ids;
    char         *graph_name;
    char         *err;
    bool          failed;
    unsigned      depth;
} Parser;

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
    for (;;) {
        while (l->pos < l->len && isspace((unsigned char)l->src[l->pos]))
            l->pos++;
        if (l->pos + 1 < l->len && l->src[l->pos] == '/') {
            if (l->src[l->pos + 1] == '/') {
                l->pos += 2;
                while (l->pos < l->len && l->src[l->pos] != '\n') l->pos++;
                continue;
            }
            if (l->src[l->pos + 1] == '*') {
                l->pos += 2;
                while (l->pos + 1 < l->len && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/'))
                    l->pos++;
                l->pos = (l->pos + 1 < l->len) ? l->pos + 2 : l->len;
                continue;
            }
        }
        break;
    }
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
        size_t start = l->pos;
        while (l->pos < l->len && l->src[l->pos] != '"' && l->src[l->pos] != '\\')
            l->pos++;
        if (l->pos > start)
            strbuf_append(&sb, l->src + start, l->pos - start);
        if (l->pos >= l->len || l->src[l->pos] == '"') break;
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
    size_t len = sb.len;
    char *s = strbuf_detach(&sb);
    return (Token){ .kind = TOK_STRING, .ptr = s, .len = len, .owned = s };
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
    return (Token){ .kind = TOK_NUMBER, .ptr = l->src + start, .len = l->pos - start };
}

static Token lex_ident(Lexer *l) {
    size_t start = l->pos;
    while (l->pos < l->len && is_ident_char(l->src[l->pos]))
        l->pos++;
    return (Token){ .kind = TOK_IDENT, .ptr = l->src + start, .len = l->pos - start };
}

static Token lex_next(Lexer *l) {
    if(l->err) return (Token){.kind=TOK_ERROR};
    lex_skip_ws(l);
    if (l->pos >= l->len) {
        return (Token){ .kind = TOK_EOF };
    }
    char c = l->src[l->pos];

    if (c == '"') return lex_string(l);

    if (c == '-' && l->pos + 1 < l->len && l->src[l->pos + 1] == '>') {
        l->pos += 2;
        return (Token){ .kind = TOK_ARROW, .ptr = "->", .len = 2 };
    }

    /* Negative number: '-' followed by digit */
    if (c == '-' && l->pos + 1 < l->len && isdigit((unsigned char)l->src[l->pos + 1]))
        return lex_number(l);

    if (isdigit((unsigned char)c))
        return lex_number(l);

    if (is_ident_start(c))
        return lex_ident(l);

    const char *p = l->src + l->pos++;
    switch (c) {
    case '{': return (Token){ .kind = TOK_LBRACE,    .ptr = p, .len = 1 };
    case '}': return (Token){ .kind = TOK_RBRACE,    .ptr = p, .len = 1 };
    case '[': return (Token){ .kind = TOK_LBRACKET,  .ptr = p, .len = 1 };
    case ']': return (Token){ .kind = TOK_RBRACKET,  .ptr = p, .len = 1 };
    case '=': return (Token){ .kind = TOK_EQUALS,    .ptr = p, .len = 1 };
    case ',': return (Token){ .kind = TOK_COMMA,     .ptr = p, .len = 1 };
    case ';': return (Token){ .kind = TOK_SEMICOLON, .ptr = p, .len = 1 };
    case '.': return (Token){ .kind = TOK_DOT,       .ptr = p, .len = 1 };
    default: {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_appendf(&sb, "unexpected character '%c'", c);
        free(l->err);
        l->err = strbuf_detach(&sb);
        return (Token){ .kind = TOK_ERROR };
    }
    }
}

/*============================================================================
 * Token helpers
 *==========================================================================*/

static void tok_free(Token *t) {
    free(t->owned);
    t->owned = NULL;
    t->ptr = NULL;
    t->len = 0;
}

/*============================================================================
 * Parser – helpers
 *==========================================================================*/

static void parser_advance(Parser *p) {
    tok_free(&p->cur);
    p->cur = lex_next(&p->lex);
    if(p->cur.kind!=TOK_EOF && !p->cur.ptr) p->failed=true;
    if (p->cur.kind == TOK_ERROR && !p->err && !p->failed) {
        p->err = p->lex.err;
        p->lex.err = NULL;
    }
}

static char *parser_take_text(Parser *p) {
    char *s = p->cur.owned ? p->cur.owned : str_ndup(p->cur.ptr, p->cur.len);
    p->cur.owned = NULL;
    if (!s) p->failed = true;
    parser_advance(p);
    return s;
}

static bool parser_at(const Parser *p, TokKind k) {
    return p->cur.kind == k;
}

static bool parser_at_ident_val(const Parser *p, const char *val) {
    return p->cur.kind == TOK_IDENT && p->cur.ptr && strlen(val) == p->cur.len &&
           memcmp(p->cur.ptr, val, p->cur.len) == 0;
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

static bool attrlist_push_owned(AttrList *al, char *key, char *value) {
    if (!key || !value) {
        free(key);
        free(value);
        return false;
    }
    if (al->count == al->cap) {
        size_t cap = al->cap ? al->cap * 2 : 8;
        Attr *items = mem_reallocarray(al->items, cap, sizeof(*items));
        if (!items) {
            free(key);
            free(value);
            return false;
        }
        al->items = items;
        al->cap = cap;
    }
    al->items[al->count].key   = key;
    al->items[al->count].value = value;
    al->count++;
    return true;
}

static bool attrlist_set(AttrList *d, const char *key, const char *value) {
    /* Overwrite existing if key already present */
    for (size_t i = 0; i < d->count; i++) {
        if (str_eq(d->items[i].key, key)) {
            char *v = str_dup(value);
            if (!v) return false;
            free(d->items[i].value);
            d->items[i].value = v;
            return true;
        }
    }
    if (d->count >= DOT_ATTR_LIMIT) return false;
    return attrlist_push_owned(d, str_dup(key), str_dup(value));
}

static void attrlist_free(AttrList *al) {
    for (size_t i = 0; i < al->count; i++) {
        free(al->items[i].key);
        free(al->items[i].value);
    }
    free(al->items);
    al->items = NULL;
    al->count = al->cap = 0;
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
        char *key = parser_take_text(p);
        if (!key) { parser_error(p, "Attribute allocation failed"); return; }

        /* Handle qualified identifiers: key.subkey.subkey */
        if (parser_at(p, TOK_DOT)) {
            StrBuf keybuf;
            strbuf_init(&keybuf);
            strbuf_append_cstr(&keybuf, key);
            free(key);
            while (parser_at(p, TOK_DOT)) {
                strbuf_append(&keybuf, ".", 1);
                parser_advance(p); /* consume dot */
                if (parser_at(p, TOK_IDENT)) {
                    strbuf_append(&keybuf, p->cur.ptr, p->cur.len);
                    parser_advance(p);
                } else {
                    parser_error(p, "expected identifier after '.'");
                    strbuf_free(&keybuf);
                    return;
                }
            }
            key = strbuf_detach(&keybuf);
            if (!key) { parser_error(p, "Attribute allocation failed"); return; }
        }

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
        if (attrs->count >= DOT_ATTR_LIMIT) {
            parser_error(p, "DOT attribute limit exceeded");
            free(key);
            return;
        }
        char *value = parser_take_text(p);
        if (!attrlist_push_owned(attrs, key, value))
            p->failed = true;

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
    size_t at;
    if (strindex_get(&p->node_ids, id, &at))
        return &p->nodes.items[at];
    DotNode *n = nodelist_add(&p->nodes);
    if(!n) {parser_error(p,"Node allocation failed");return NULL;}
    n->id = str_dup(id);
    /* Node ids are separately allocated, so they stay valid as the array grows. */
    if(!n->id || !strindex_put(&p->node_ids, n->id, p->nodes.count - 1)) {parser_error(p,"Node allocation failed");return NULL;}
    return n;
}

/*============================================================================
 * Parser – merge attrs into node (append, don't deduplicate yet)
 *==========================================================================*/

/* Later values replace earlier ones in place, so repeated statements cannot
 * multiply attributes; lookups keep last-wins semantics. */
static bool merge_attrs_into_node(DotNode *n, Attr *items, size_t count, bool take) {
    for (size_t i = 0; i < count; i++) {
        Attr *existing = NULL;
        for (size_t j = 0; j < n->attr_count && !existing; j++)
            if (str_eq(n->attrs[j].key, items[i].key)) existing = &n->attrs[j];
        if (existing) {
            if (str_eq(existing->value, items[i].value)) continue;
            char *value = take ? items[i].value : str_dup(items[i].value);
            if (!value) return false;
            if (take) items[i].value = NULL;
            free(existing->value);
            existing->value = value;
            continue;
        }
        if (n->attr_count >= DOT_ATTR_LIMIT) return false;
        size_t idx = n->attr_count;
        Attr *grown=mem_reallocarray(n->attrs,n->attr_count+1,sizeof(*grown));
        if(!grown) return false;n->attrs=grown;n->attr_count++;
        if (take) {
            n->attrs[idx].key   = items[i].key;
            n->attrs[idx].value = items[i].value;
            items[i].key = items[i].value = NULL;
        } else {
            n->attrs[idx].key   = str_dup(items[i].key);
            n->attrs[idx].value = str_dup(items[i].value);
        }
    }
    return true;
}

/*============================================================================
 * Parser – statements
 *==========================================================================*/

static void parse_statements(Parser *p, const AttrList *scope_node_defaults,
                             const char *scope_class);

static void parse_subgraph(Parser *p, const AttrList *parent_node_defaults) {
    /* 'subgraph' already consumed */
    char *sg_name = NULL;
    if (parser_at(p, TOK_IDENT) || parser_at(p, TOK_STRING)) {
        sg_name = parser_take_text(p);
    }

    if (!parser_consume(p, TOK_LBRACE)) {
        parser_error(p, "expected '{' after subgraph");
        free(sg_name);
        return;
    }

    /* Create scoped node defaults from parent */
    AttrList scoped;
    attrlist_init(&scoped);
    if (parent_node_defaults) {
        for (size_t i = 0; i < parent_node_defaults->count; i++)
            attrlist_set(&scoped, parent_node_defaults->items[i].key,
                         parent_node_defaults->items[i].value);
    }

    /* Derive class from subgraph name if present (strip "cluster_" prefix). */
    char *scope_class = NULL;
    if (sg_name) {
        const char *base = str_starts_with(sg_name, "cluster_") ? sg_name + 8 : sg_name;
        StrBuf cls;
        strbuf_init(&cls);
        for (size_t i = 0; base[i]; i++) {
            char c = (char)tolower((unsigned char)base[i]);
            if (c == ' ' || c == '_')
                strbuf_append(&cls, "-", 1);
            else if (isalnum((unsigned char)c) || c == '-')
                strbuf_append(&cls, &c, 1);
        }
        scope_class = strbuf_detach(&cls);
    }

    if(p->depth>=64) parser_error(p,"DOT subgraph nesting limit");
    else {p->depth++;parse_statements(p, &scoped, scope_class);p->depth--;}

    if (!parser_consume(p, TOK_RBRACE)) {
        parser_error(p, "expected '}' closing subgraph");
    }

    attrlist_free(&scoped);
    free(sg_name);
    free(scope_class);
}

static void parse_statements(Parser *p, const AttrList *scope_node_defaults,
                             const char *scope_class) {
    /* Copy scoped defaults into a mutable version so "node [...]" inside scope
     * can modify them */
    AttrList local_nd;
    attrlist_init(&local_nd);
    const AttrList *src_nd = scope_node_defaults ? scope_node_defaults : &p->node_defaults;
    for (size_t i = 0; i < src_nd->count; i++)
        attrlist_set(&local_nd, src_nd->items[i].key, src_nd->items[i].value);

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
                for (size_t i = 0; i < attrs.count; i++) {
                    if (attrlist_push_owned(&p->graph_attrs, attrs.items[i].key, attrs.items[i].value))
                        attrs.items[i].key = attrs.items[i].value = NULL;
                }
                attrlist_free(&attrs);
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
                    bool ok = attrlist_set(&local_nd, attrs.items[i].key,
                                           attrs.items[i].value);
                    /* Also update parser-global defaults when not in subgraph */
                    if (ok && !scope_node_defaults) {
                        ok = attrlist_set(&p->node_defaults, attrs.items[i].key,
                                          attrs.items[i].value);
                    }
                    if (!ok) { parser_error(p, "DOT attribute limit exceeded"); break; }
                }
                attrlist_free(&attrs);
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
                    if (!attrlist_set(&p->edge_defaults, attrs.items[i].key,
                                      attrs.items[i].value)) {
                        parser_error(p, "DOT attribute limit exceeded");
                        break;
                    }
                attrlist_free(&attrs);
            }
            parser_skip_semi(p);
            continue;
        }

        /* Must be an identifier (node/edge stmt) or graph-level key=value */
        if (!parser_at(p, TOK_IDENT) && !parser_at(p, TOK_STRING)) {
            StrBuf sb;
            strbuf_init(&sb);
            strbuf_append_cstr(&sb, "unexpected token: ");
            if (p->cur.ptr) strbuf_append(&sb, p->cur.ptr, p->cur.len);
            else strbuf_append_cstr(&sb, "<null>");
            parser_error(p, sb.data);
            strbuf_free(&sb);
            goto done;
        }

        char *first_id = parser_take_text(p);
        if(!first_id) {parser_error(p,"Identifier allocation failed");goto done;}

        /* Graph-level attr: identifier '=' value (not inside bracket) */
        if (parser_at(p, TOK_EQUALS)) {
            parser_advance(p);
            if (parser_at(p, TOK_STRING) || parser_at(p, TOK_IDENT) ||
                parser_at(p, TOK_NUMBER)) {
                char *val = parser_take_text(p);
                if (!attrlist_push_owned(&p->graph_attrs, first_id, val))
                    p->failed = true;
                first_id = NULL;
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
                ids[id_count++] = parser_take_text(p);
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

                size_t total_attrs = p->edge_defaults.count + edge_attrs.count;
                if (total_attrs > 0) {
                    e->attrs = mem_calloc(total_attrs, sizeof(*e->attrs));
                    if (!e->attrs) { parser_error(p, "Edge attributes allocation failed"); break; }
                    for (size_t d = 0; d < p->edge_defaults.count; d++) {
                        e->attrs[e->attr_count].key   = str_dup(p->edge_defaults.items[d].key);
                        e->attrs[e->attr_count].value = str_dup(p->edge_defaults.items[d].value);
                        e->attr_count++;
                    }
                    for (size_t a = 0; a < edge_attrs.count; a++) {
                        e->attrs[e->attr_count].key   = str_dup(edge_attrs.items[a].key);
                        e->attrs[e->attr_count].value = str_dup(edge_attrs.items[a].value);
                        e->attr_count++;
                    }
                }
            }

            attrlist_free(&edge_attrs);
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

            /* Apply current node defaults (scoped) directly without copying into a temporary list */
            if (!merge_attrs_into_node(node, local_nd.items, local_nd.count, false))
                parser_error(p, "DOT attribute limit exceeded");

            /* Parse explicit attrs */
            if (parser_at(p, TOK_LBRACKET)) {
                AttrList explicit_attrs;
                attrlist_init(&explicit_attrs);
                parse_attr_block(p, &explicit_attrs);
                if (!p->err && !p->failed && !merge_attrs_into_node(node, explicit_attrs.items, explicit_attrs.count, true))
                    parser_error(p, "DOT attribute limit exceeded");
                attrlist_free(&explicit_attrs);
            }
            free(first_id);
            parser_skip_semi(p);
            continue;
        }
    }

done:
    attrlist_free(&local_nd);
}

/*============================================================================
 * Resolve convenience fields from attrs
 *==========================================================================*/

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
    const char *label = NULL, *shape = NULL, *type = NULL, *prompt = NULL;
    const char *max_retries = NULL, *goal_gate = NULL, *retry_target = NULL;
    const char *fallback_retry_target = NULL, *fidelity = NULL, *thread_id = NULL;
    const char *class_attr = NULL, *timeout = NULL, *llm_model = NULL, *llm_dot_model = NULL;
    const char *llm_provider = NULL, *llm_dot_provider = NULL, *reasoning_effort = NULL;
    const char *auto_status = NULL, *allow_partial = NULL;

    for (size_t i = 0; i < n->attr_count; i++) {
        const char *k = n->attrs[i].key;
        const char *v = n->attrs[i].value;
        if (str_eq(k, "label")) label = v;
        else if (str_eq(k, "shape")) shape = v;
        else if (str_eq(k, "type")) type = v;
        else if (str_eq(k, "prompt")) prompt = v;
        else if (str_eq(k, "max_retries")) max_retries = v;
        else if (str_eq(k, "goal_gate")) goal_gate = v;
        else if (str_eq(k, "retry_target")) retry_target = v;
        else if (str_eq(k, "fallback_retry_target")) fallback_retry_target = v;
        else if (str_eq(k, "fidelity")) fidelity = v;
        else if (str_eq(k, "thread_id")) thread_id = v;
        else if (str_eq(k, "class")) class_attr = v;
        else if (str_eq(k, "timeout")) timeout = v;
        else if (str_eq(k, "llm_model")) llm_model = v;
        else if (str_eq(k, "llm.model")) llm_dot_model = v;
        else if (str_eq(k, "llm_provider")) llm_provider = v;
        else if (str_eq(k, "llm.provider")) llm_dot_provider = v;
        else if (str_eq(k, "reasoning_effort")) reasoning_effort = v;
        else if (str_eq(k, "auto_status")) auto_status = v;
        else if (str_eq(k, "allow_partial")) allow_partial = v;
    }
    if (!llm_model) llm_model = llm_dot_model;
    if (!llm_provider) llm_provider = llm_dot_provider;
    if (label) { free(n->label); n->label = str_dup(label); }
    if (shape) { free(n->shape); n->shape = str_dup(shape); }
    if (type) { free(n->type); n->type = str_dup(type); }
    if (prompt) { free(n->prompt); n->prompt = str_dup(prompt); }
    n->max_retries = parse_int(max_retries, -1);  /* -1 = not set, use graph default */
    n->goal_gate = parse_bool(goal_gate, false);
    if (retry_target) { free(n->retry_target); n->retry_target = str_dup(retry_target); }
    if (fallback_retry_target) { free(n->fallback_retry_target); n->fallback_retry_target = str_dup(fallback_retry_target); }
    if (fidelity) { free(n->fidelity); n->fidelity = str_dup(fidelity); }
    if (thread_id) { free(n->thread_id); n->thread_id = str_dup(thread_id); }
    if (class_attr) { free(n->class_attr); n->class_attr = str_dup(class_attr); }
    if (timeout) { free(n->timeout); n->timeout = str_dup(timeout); }
    if (llm_model) { free(n->llm_model); n->llm_model = str_dup(llm_model); }
    if (llm_provider) { free(n->llm_provider); n->llm_provider = str_dup(llm_provider); }
    if (reasoning_effort) { free(n->reasoning_effort); n->reasoning_effort = str_dup(reasoning_effort); }
    n->auto_status = parse_bool(auto_status, false);
    n->allow_partial = parse_bool(allow_partial, false);
}

static void resolve_edge(DotEdge *e) {
    const char *label = NULL, *condition = NULL, *weight = NULL;
    const char *fidelity = NULL, *thread_id = NULL, *loop_restart = NULL;
    for (size_t i = 0; i < e->attr_count; i++) {
        const char *k = e->attrs[i].key;
        const char *v = e->attrs[i].value;
        if (str_eq(k, "label")) label = v;
        else if (str_eq(k, "condition")) condition = v;
        else if (str_eq(k, "weight")) weight = v;
        else if (str_eq(k, "fidelity")) fidelity = v;
        else if (str_eq(k, "thread_id")) thread_id = v;
        else if (str_eq(k, "loop_restart")) loop_restart = v;
    }
    if (label) { free(e->label); e->label = str_dup(label); }
    if (condition) { free(e->condition); e->condition = str_dup(condition); }
    e->weight = parse_int(weight, 0);
    if (fidelity) { free(e->fidelity); e->fidelity = str_dup(fidelity); }
    if (thread_id) { free(e->thread_id); e->thread_id = str_dup(thread_id); }
    e->loop_restart = parse_bool(loop_restart, false);
}

/*============================================================================
 * Resolve graph-level attrs
 *==========================================================================*/

static void resolve_graph_attrs(DotGraph *g, const AttrList *ga) {
    g->default_max_retry = 0; /* spec default: no retries */

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
        } else if (str_eq(k, "default_max_retries") || str_eq(k, "default_max_retry")) {
            g->default_max_retry = parse_int(v, 0); /* legacy singular key accepted */
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

static void dot_node_clear(DotNode *n) {
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

static void dot_edge_clear(DotEdge *e) {
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

static void dot_graph_clear(DotGraph *g);
static bool build_csr(const DotGraph *g, DotAdjacency *a, bool incoming);

DotGraph *dot_parse(const char *source, char **err_msg) {
    unsigned long failures=mem_failure_count();
    if (!source) {
        if (err_msg) *err_msg = str_dup("null source");
        return NULL;
    }
    size_t source_len = strnlen(source, ATTRACTOR_INPUT_LIMIT + 1);
    if (source_len > ATTRACTOR_INPUT_LIMIT) {
        if (err_msg) *err_msg = str_dup("null source");
        return NULL;
    }

    /* Phase 1: tokenize + parse (comments skipped directly by lexer) */
    Parser p;
    memset(&p, 0, sizeof(p));
    lex_init(&p.lex, source, source_len);
    nodelist_init(&p.nodes);
    edgelist_init(&p.edges);
    attrlist_init(&p.graph_attrs);
    attrlist_init(&p.node_defaults);
    attrlist_init(&p.edge_defaults);
    strindex_init(&p.node_ids);

    /* Read first token */
    p.cur = lex_next(&p.lex);
    if(p.cur.kind!=TOK_EOF && !p.cur.ptr) p.failed=true;

    /* Expect: 'digraph' */
    if (!parser_at_ident_val(&p, "digraph")) {
        parser_error(&p, "expected 'digraph'");
    }
    parser_advance(&p);

    /* Graph name */
    if (parser_at(&p, TOK_IDENT) || parser_at(&p, TOK_STRING)) {
        p.graph_name = parser_take_text(&p);
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
        for (size_t i = 0; i < p.nodes.count; i++)
            dot_node_clear(&p.nodes.items[i]);
        free(p.nodes.items);
        for (size_t i = 0; i < p.edges.count; i++)
            dot_edge_clear(&p.edges.items[i]);
        free(p.edges.items);
        attrlist_free(&p.graph_attrs);
        attrlist_free(&p.node_defaults);
        attrlist_free(&p.edge_defaults);
        strindex_free(&p.node_ids);
        free(p.graph_name);
        tok_free(&p.cur);
        free(p.lex.err);
        return NULL;
    }

    /* Phase 2: resolve convenience fields */
    for (size_t i = 0; i < p.nodes.count; i++)
        resolve_node(&p.nodes.items[i]);

    for (size_t i = 0; i < p.edges.count; i++)
        resolve_edge(&p.edges.items[i]);

    /* Phase 3: build the graph */
    DotGraph *g = mem_calloc(1, sizeof(DotGraph));
    if(!g) {
        DotGraph partial={.name=p.graph_name,.nodes=p.nodes.items,.node_count=p.nodes.count,.edges=p.edges.items,.edge_count=p.edges.count};
        attrlist_free(&p.graph_attrs);attrlist_free(&p.node_defaults);attrlist_free(&p.edge_defaults);strindex_free(&p.node_ids);tok_free(&p.cur);
        /* Allocation-free graph destruction supports stack storage internally. */
        dot_graph_clear(&partial);return NULL;
    }
    g->name       = p.graph_name;
    g->nodes      = p.nodes.items;
    g->node_count = p.nodes.count;
    g->edges      = p.edges.items;
    g->edge_count = p.edges.count;
    g->node_index = p.node_ids;

    resolve_graph_attrs(g, &p.graph_attrs);

    /* Cleanup temporaries */
    attrlist_free(&p.graph_attrs);
    attrlist_free(&p.node_defaults);
    attrlist_free(&p.edge_defaults);
    tok_free(&p.cur);

    if(!build_csr(g,&g->out_adj,false) || !build_csr(g,&g->in_adj,true) || mem_failure_count()!=failures) {
        dot_graph_free(g);if(err_msg) *err_msg=str_dup("DOT allocation failure");return NULL;
    }
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

    for (size_t i = 0; i < g->node_count; i++)
        dot_node_clear(&g->nodes[i]);
    free(g->nodes);

    for (size_t i = 0; i < g->edge_count; i++)
        dot_edge_clear(&g->edges[i]);
    free(g->edges);
    strindex_free(&g->node_index);
    dot_adjacency_free(&g->out_adj);
    dot_adjacency_free(&g->in_adj);

    memset(g,0,sizeof(*g));
}
void dot_graph_free(DotGraph *g) {if(g) {dot_graph_clear(g);free(g);}}

/*============================================================================
 * Public API: dot_find_node
 *==========================================================================*/

DotNode *dot_find_node(const DotGraph *g, const char *id) {
    if (!g || !id) return NULL;
    if (g->node_index.cap) {
        size_t at;
        return strindex_get(&g->node_index, id, &at) && at < g->node_count ? &g->nodes[at] : NULL;
    }
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
    if (g->out_adj.offset && g->node_index.cap) {
        size_t idx;
        if (!strindex_get(&g->node_index, node_id, &idx) || idx >= g->node_count) return 0;
        size_t start = g->out_adj.offset[idx], end = g->out_adj.offset[idx + 1];
        size_t count = end - start, n = count < max ? count : max;
        if (out) for (size_t k = 0; k < n; k++) out[k] = &g->edges[g->out_adj.edge[start + k]];
        return count;
    }
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
    if (g->in_adj.offset && g->node_index.cap) {
        size_t idx;
        if (!strindex_get(&g->node_index, node_id, &idx) || idx >= g->node_count) return 0;
        size_t start = g->in_adj.offset[idx], end = g->in_adj.offset[idx + 1];
        size_t count = end - start, n = count < max ? count : max;
        if (out) for (size_t k = 0; k < n; k++) out[k] = &g->edges[g->in_adj.edge[start + k]];
        return count;
    }
    size_t found = 0;
    for (size_t i = 0; i < g->edge_count; i++) {
        if (str_eq(g->edges[i].to, node_id))
            {if(out && found<max) out[found]=&g->edges[i];found++;}
    }
    return found;
}

static bool build_csr(const DotGraph *g, DotAdjacency *a, bool incoming) {
    *a = (DotAdjacency){0};
    size_t n = g->node_count, m = g->edge_count, slots;
    size_t *endpoint = mem_calloc(m ? m : 1, sizeof(*endpoint));
    if (!size_add(n, 1, &slots)) { free(endpoint); return false; }
    a->offset = mem_calloc(slots, sizeof(*a->offset));
    a->edge = mem_calloc(m ? m : 1, sizeof(*a->edge));
    a->target = mem_calloc(m ? m : 1, sizeof(*a->target));
    if (!endpoint || !a->offset || !a->edge || !a->target) goto fail;
    for (size_t k = 0; k < m; k++) {
        const DotNode *node = dot_find_node(g, incoming ? g->edges[k].to : g->edges[k].from);
        endpoint[k] = node ? (size_t)(node - g->nodes) : SIZE_MAX;
        if (node) a->offset[endpoint[k]]++;
    }
    size_t sum = 0;
    for (size_t i = 0; i <= n; i++) {
        size_t c = a->offset[i];
        a->offset[i] = sum;
        sum += c;
    }
    /* Stable counting sort keeps each node's edges in declaration order. */
    for (size_t k = 0; k < m; k++) {
        if (endpoint[k] == SIZE_MAX) continue;
        size_t at = a->offset[endpoint[k]]++;
        const DotNode *other = dot_find_node(g, incoming ? g->edges[k].from : g->edges[k].to);
        a->edge[at] = k;
        a->target[at] = other ? (size_t)(other - g->nodes) : SIZE_MAX;
    }
    for (size_t i = n; i > 0; i--) a->offset[i] = a->offset[i - 1];
    a->offset[0] = 0;
    free(endpoint);
    return true;
fail:
    free(endpoint); dot_adjacency_free(a);
    return false;
}

bool dot_adjacency_build(const DotGraph *g, DotAdjacency *a) {
    return build_csr(g, a, false);
}

void dot_adjacency_free(DotAdjacency *a) {
    if (!a) return;
    free(a->offset); free(a->edge); free(a->target);
    *a = (DotAdjacency){0};
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
