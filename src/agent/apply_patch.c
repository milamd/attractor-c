/* apply_patch: codex-style v4a patches (coding-agent spec Appendix A).
 *
 * Every operation is planned in memory before any file is touched, so a patch
 * with a bad hunk changes nothing. Hunks are located with KMP over per-line
 * hashes: each search is O(file lines + hunk lines), trying exact matches
 * first and then whitespace/Unicode-punctuation-normalized ones. */
#include "agent/agent.h"
#include "util/json.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; size_t n; } Line;

typedef struct { Line *items; size_t count, cap; } Lines;

typedef struct {
    Lines old_lines, new_lines;
    Line  hint;
    bool  has_hint, eof;
} Hunk;

typedef enum { OP_ADD, OP_DELETE, OP_UPDATE } OpKind;

typedef struct {
    OpKind kind;
    Line   path, move_to;
    bool   has_move;
    Lines  added;
    Hunk  *hunks;
    size_t hunk_count, hunk_cap;
    char  *result; /* planned content for ADD/UPDATE */
} Op;

typedef struct { Op *ops; size_t count, cap; } Patch;

#define PATCH_MAX_OPS 1000

static bool lines_push(Lines *l, Line line) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        Line *items = mem_reallocarray(l->items, cap, sizeof(*items));
        if (!items) return false;
        l->items = items; l->cap = cap;
    }
    l->items[l->count++] = line;
    return true;
}

static void patch_free(Patch *p) {
    for (size_t i = 0; i < p->count; i++) {
        Op *op = &p->ops[i];
        free(op->added.items); free(op->result);
        for (size_t h = 0; h < op->hunk_count; h++) { free(op->hunks[h].old_lines.items); free(op->hunks[h].new_lines.items); }
        free(op->hunks);
    }
    free(p->ops);
}

static bool line_is(Line l, const char *text) { size_t n = strlen(text); return l.n == n && !memcmp(l.s, text, n); }
static bool line_starts(Line l, const char *prefix, Line *rest) {
    size_t n = strlen(prefix);
    if (l.n < n || memcmp(l.s, prefix, n)) return false;
    if (rest) *rest = (Line){l.s + n, l.n - n};
    return true;
}
static Line line_trim(Line l) {
    while (l.n && (*l.s == ' ' || *l.s == '\t')) { l.s++; l.n--; }
    while (l.n && (l.s[l.n - 1] == ' ' || l.s[l.n - 1] == '\t' || l.s[l.n - 1] == '\r')) l.n--;
    return l;
}
/* Paths are workspace-relative; ".." components are refused up front so the
 * commit phase cannot fail on containment after other files changed. */
static bool path_ok(Line p) {
    if (!p.n || memchr(p.s, 0, p.n)) return false;
    for (size_t i = 0; i < p.n; ) {
        size_t j = i; while (j < p.n && p.s[j] != '/') j++;
        if (j - i == 2 && p.s[i] == '.' && p.s[i + 1] == '.') return false;
        i = j + 1;
    }
    return true;
}

/* ── Parsing ─────────────────────────────────────────────────────────── */

static const char *parse_patch(const char *text, Patch *p) {
    Lines lines = {0};
    for (const char *c = text; *c; ) {
        const char *e = strchr(c, '\n'); size_t n = e ? (size_t)(e - c) : strlen(c);
        Line l = {c, n}; if (l.n && l.s[l.n - 1] == '\r') l.n--;
        if (!lines_push(&lines, l)) { free(lines.items); return "Out of memory"; }
        c += n + (e ? 1 : 0);
    }
    const char *err = NULL;
    size_t i = 0;
    while (i < lines.count && !line_trim(lines.items[i]).n) i++;
    if (i == lines.count || !line_is(line_trim(lines.items[i]), "*** Begin Patch")) { err = "Patch must start with '*** Begin Patch'"; goto done; }
    i++;
    Op *op = NULL; Hunk *hunk = NULL; bool ended = false;
    for (; i < lines.count && !ended; i++) {
        Line l = lines.items[i], rest;
        if (line_is(line_trim(l), "*** End Patch")) { ended = true; break; }
        OpKind kind; bool new_op = true;
        if (line_starts(l, "*** Add File: ", &rest)) kind = OP_ADD;
        else if (line_starts(l, "*** Delete File: ", &rest)) kind = OP_DELETE;
        else if (line_starts(l, "*** Update File: ", &rest)) kind = OP_UPDATE;
        else new_op = false;
        if (new_op) {
            if (p->count >= PATCH_MAX_OPS) { err = "Too many operations in patch"; goto done; }
            if (p->count == p->cap) {
                size_t cap = p->cap ? p->cap * 2 : 8; Op *ops = mem_reallocarray(p->ops, cap, sizeof(*ops));
                if (!ops) { err = "Out of memory"; goto done; }
                p->ops = ops; p->cap = cap;
            }
            op = &p->ops[p->count++]; *op = (Op){.kind = kind, .path = line_trim(rest)};
            hunk = NULL;
            if (!path_ok(op->path)) { err = "Invalid file path in patch"; goto done; }
            continue;
        }
        if (!op) { err = "Expected a file operation after '*** Begin Patch'"; goto done; }
        if (op->kind == OP_ADD) {
            if (!line_starts(l, "+", &rest)) { err = "Add File lines must start with '+'"; goto done; }
            if (!lines_push(&op->added, rest)) { err = "Out of memory"; goto done; }
            continue;
        }
        if (op->kind == OP_DELETE) { err = "Unexpected content after Delete File"; goto done; }
        if (line_starts(l, "*** Move to: ", &rest)) {
            if (op->hunk_count || op->has_move) { err = "'*** Move to:' must directly follow Update File"; goto done; }
            op->move_to = line_trim(rest); op->has_move = true;
            if (!path_ok(op->move_to)) { err = "Invalid move path in patch"; goto done; }
            continue;
        }
        if (line_is(line_trim(l), "*** End of File")) {
            if (!hunk) { err = "'*** End of File' outside a hunk"; goto done; }
            hunk->eof = true; continue;
        }
        if (line_starts(l, "***", NULL)) { err = "Unknown patch directive"; goto done; }
        bool header = line_starts(l, "@@", &rest);
        if (header || !hunk) {
            if (op->hunk_count == op->hunk_cap) {
                size_t cap = op->hunk_cap ? op->hunk_cap * 2 : 4; Hunk *hs = mem_reallocarray(op->hunks, cap, sizeof(*hs));
                if (!hs) { err = "Out of memory"; goto done; }
                op->hunks = hs; op->hunk_cap = cap;
            }
            hunk = &op->hunks[op->hunk_count++]; *hunk = (Hunk){0};
            if (header) { hunk->hint = line_trim(rest); hunk->has_hint = hunk->hint.n > 0; continue; }
        }
        bool ok = true;
        if (!l.n) { ok = lines_push(&hunk->old_lines, l) && lines_push(&hunk->new_lines, l); } /* bare blank = context */
        else if (l.s[0] == ' ') { Line t = {l.s + 1, l.n - 1}; ok = lines_push(&hunk->old_lines, t) && lines_push(&hunk->new_lines, t); }
        else if (l.s[0] == '-') ok = lines_push(&hunk->old_lines, (Line){l.s + 1, l.n - 1});
        else if (l.s[0] == '+') ok = lines_push(&hunk->new_lines, (Line){l.s + 1, l.n - 1});
        else { err = "Hunk lines must start with ' ', '-' or '+'"; goto done; }
        if (!ok) { err = "Out of memory"; goto done; }
    }
    if (!ended) { err = "Patch must end with '*** End Patch'"; goto done; }
    for (size_t k = 0; k < p->count; k++)
        if (p->ops[k].kind == OP_UPDATE && !p->ops[k].hunk_count && !p->ops[k].has_move) { err = "Update File has no hunks"; goto done; }
done:
    free(lines.items);
    return err;
}

/* ── Matching ────────────────────────────────────────────────────────── */

/* Normalize into out (capacity >= l.n): trim, collapse whitespace runs, and
 * fold common Unicode punctuation to ASCII. Returns the normalized length. */
static size_t normalize(Line l, char *out) {
    size_t n = 0; bool space = false;
    for (size_t i = 0; i < l.n; ) {
        unsigned char c = (unsigned char)l.s[i];
        char mapped = 0; size_t width = 1;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') mapped = ' ';
        else if (c == 0xC2 && i + 1 < l.n && (unsigned char)l.s[i + 1] == 0xA0) { mapped = ' '; width = 2; }
        else if (c == 0xE2 && i + 2 < l.n && (unsigned char)l.s[i + 1] == 0x80) {
            unsigned char d = (unsigned char)l.s[i + 2];
            if (d >= 0x98 && d <= 0x9B) mapped = '\'';
            else if (d >= 0x9C && d <= 0x9F) mapped = '"';
            else if (d >= 0x90 && d <= 0x95) mapped = '-';
            if (mapped) width = 3;
        }
        if (mapped == ' ') { space = n > 0; i += width; continue; }
        if (space) { out[n++] = ' '; space = false; }
        out[n++] = mapped ? mapped : (char)c; i += width;
    }
    return n;
}

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

typedef struct {
    const Line *lines; size_t count;
    uint64_t *exact, *norm;
} Seq;

typedef struct { char *a, *b; } Scratch; /* each sized to the longest line */

static bool seq_init(Seq *q, const Line *lines, size_t count, char *scratch) {
    q->lines = lines; q->count = count;
    q->exact = mem_calloc(count ? count : 1, sizeof(uint64_t));
    q->norm = mem_calloc(count ? count : 1, sizeof(uint64_t));
    if (!q->exact || !q->norm) return false;
    for (size_t i = 0; i < count; i++) {
        q->exact[i] = fnv(lines[i].s, lines[i].n);
        q->norm[i] = fnv(scratch, normalize(lines[i], scratch));
    }
    return true;
}
static void seq_free(Seq *q) { free(q->exact); free(q->norm); }

static bool lines_equal(const Seq *x, size_t i, const Seq *y, size_t j, bool fuzzy, Scratch *s) {
    if (!fuzzy) return x->exact[i] == y->exact[j] && x->lines[i].n == y->lines[j].n && !memcmp(x->lines[i].s, y->lines[j].s, x->lines[i].n);
    if (x->norm[i] != y->norm[j]) return false;
    size_t a = normalize(x->lines[i], s->a), b = normalize(y->lines[j], s->b);
    return a == b && !memcmp(s->a, s->b, a);
}

/* KMP: first index >= from where pat occurs in text, or -1. fail has pat->count slots. */
static long kmp_find(const Seq *text, size_t from, const Seq *pat, bool fuzzy, size_t *fail, Scratch *s) {
    size_t m = pat->count;
    if (!m) return (long)from;
    fail[0] = 0;
    for (size_t i = 1, k = 0; i < m; i++) {
        while (k && !lines_equal(pat, i, pat, k, fuzzy, s)) k = fail[k - 1];
        if (lines_equal(pat, i, pat, k, fuzzy, s)) k++;
        fail[i] = k;
    }
    for (size_t i = from, k = 0; i < text->count; i++) {
        while (k && !lines_equal(text, i, pat, k, fuzzy, s)) k = fail[k - 1];
        if (lines_equal(text, i, pat, k, fuzzy, s)) k++;
        if (k == m) return (long)(i + 1 - m);
    }
    return -1;
}
static bool match_at(const Seq *text, size_t at, const Seq *pat, bool fuzzy, Scratch *s) {
    if (at + pat->count > text->count) return false;
    for (size_t j = 0; j < pat->count; j++) if (!lines_equal(text, at + j, pat, j, fuzzy, s)) return false;
    return true;
}

typedef struct { size_t at, remove; const Lines *insert; } Edit;

/* Plans the edits for one file and renders the new content into *out. */
static const char *apply_hunks(const char *raw, const Op *op, char **out) {
    Lines file = {0}; size_t longest = 0;
    for (const char *c = raw; *c; ) {
        const char *e = strchr(c, '\n'); size_t n = e ? (size_t)(e - c) : strlen(c);
        if (!lines_push(&file, (Line){c, n})) { free(file.items); return "Out of memory"; }
        if (n > longest) longest = n;
        c += n + (e ? 1 : 0);
    }
    size_t max_old = 0;
    for (size_t h = 0; h < op->hunk_count; h++) {
        const Hunk *k = &op->hunks[h];
        if (k->old_lines.count > max_old) max_old = k->old_lines.count;
        for (size_t i = 0; i < k->old_lines.count; i++) if (k->old_lines.items[i].n > longest) longest = k->old_lines.items[i].n;
        if (k->hint.n > longest) longest = k->hint.n;
    }
    const char *err = NULL;
    Scratch s = {mem_calloc(longest + 1, 1), mem_calloc(longest + 1, 1)};
    Edit *edits = mem_calloc(op->hunk_count ? op->hunk_count : 1, sizeof(*edits));
    size_t *fail = max_old ? mem_calloc(max_old, sizeof(*fail)) : NULL;
    Seq text = {0};
    if (!s.a || !s.b || !edits || (max_old && !fail) || !seq_init(&text, file.items, file.count, s.a)) { err = "Out of memory"; goto done; }

    size_t cursor = 0;
    for (size_t h = 0; h < op->hunk_count && !err; h++) {
        const Hunk *k = &op->hunks[h];
        long hint_at = -1;
        if (k->has_hint) {
            uint64_t h_exact = fnv(k->hint.s, k->hint.n), h_norm = fnv(s.a, normalize(k->hint, s.a));
            Seq hint = {.lines = &k->hint, .exact = &h_exact, .norm = &h_norm, .count = 1};
            for (size_t i = cursor; i < text.count && hint_at < 0; i++) if (lines_equal(&text, i, &hint, 0, true, &s)) hint_at = (long)i;
        }
        long at = -1;
        if (!k->old_lines.count) {
            at = k->eof || hint_at < 0 ? (long)text.count : hint_at + 1;
        } else {
            Seq pat = {0};
            if (!seq_init(&pat, k->old_lines.items, k->old_lines.count, s.a)) { seq_free(&pat); err = "Out of memory"; break; }
            size_t starts[2] = {hint_at >= 0 ? (size_t)hint_at : cursor, cursor};
            for (int pass = 0; pass < 2 && at < 0; pass++) {
                if (pass && starts[1] == starts[0]) break;
                for (int fuzzy = 0; fuzzy < 2 && at < 0; fuzzy++) {
                    if (k->eof) {
                        size_t end = text.count >= pat.count ? text.count - pat.count : 0;
                        if (end >= starts[pass] && match_at(&text, end, &pat, fuzzy, &s)) at = (long)end;
                    } else at = kmp_find(&text, starts[pass], &pat, fuzzy, fail, &s);
                }
            }
            seq_free(&pat);
            if (at < 0) { err = "A hunk's context or removed lines were not found in the file"; break; }
        }
        edits[h] = (Edit){(size_t)at, k->old_lines.count, &k->new_lines};
        cursor = (size_t)at + k->old_lines.count;
    }
    if (!err) {
        StrBuf b; strbuf_init(&b);
        size_t prev = 0;
        for (size_t h = 0; h < op->hunk_count; h++) {
            for (size_t i = prev; i < edits[h].at; i++) { strbuf_append(&b, file.items[i].s, file.items[i].n); strbuf_append(&b, "\n", 1); }
            for (size_t i = 0; i < edits[h].insert->count; i++) { strbuf_append(&b, edits[h].insert->items[i].s, edits[h].insert->items[i].n); strbuf_append(&b, "\n", 1); }
            prev = edits[h].at + edits[h].remove;
        }
        for (size_t i = prev; i < file.count; i++) { strbuf_append(&b, file.items[i].s, file.items[i].n); strbuf_append(&b, "\n", 1); }
        size_t raw_len = strlen(raw);
        if (raw_len && raw[raw_len - 1] != '\n' && b.len) b.len--, b.data[b.len] = 0; /* keep a missing final newline missing */
        *out = strbuf_detach(&b);
        if (!*out) err = "Out of memory";
    }
done:
    seq_free(&text); free(fail); free(edits); free(s.a); free(s.b); free(file.items);
    return err;
}

/* ── Plan and commit ─────────────────────────────────────────────────── */

static char *line_dup(Line l) { return str_ndup(l.s, l.n); }

static char *patch_error(bool *is_error, const char *message, Line path) {
    if (is_error) *is_error = true;
    StrBuf b; strbuf_init(&b);
    strbuf_append_cstr(&b, "Error: ");
    strbuf_append_cstr(&b, message);
    if (path.n) { strbuf_append_cstr(&b, ": "); strbuf_append(&b, path.s, path.n); }
    char *s = strbuf_detach(&b);
    return s ? s : str_dup("Error: out of memory");
}

char *agent_apply_patch(ExecutionEnv *env, const char *patch_text, bool *is_error) {
    if (is_error) *is_error = false;
    if (!env || !env->read_raw || !env->write_file || !env->delete_file || !env->file_exists)
        return patch_error(is_error, "apply_patch is unsupported by this environment", (Line){0});
    if (!patch_text) return patch_error(is_error, "patch required", (Line){0});
    Patch p = {0};
    const char *err = parse_patch(patch_text, &p);
    if (err) { patch_free(&p); return patch_error(is_error, err, (Line){0}); }

    /* Plan: verify every operation and compute new contents. */
    for (size_t i = 0; i < p.count; i++) {
        Op *op = &p.ops[i];
        char *path = line_dup(op->path); if (!path) { patch_free(&p); return patch_error(is_error, "Out of memory", (Line){0}); }
        bool exists = env->file_exists(env, path);
        if (op->kind == OP_ADD) {
            if (exists) err = "File already exists";
            else {
                StrBuf b; strbuf_init(&b);
                for (size_t k = 0; k < op->added.count; k++) { strbuf_append(&b, op->added.items[k].s, op->added.items[k].n); strbuf_append(&b, "\n", 1); }
                op->result = strbuf_detach(&b); if (!op->result) err = "Out of memory";
            }
        } else if (op->kind == OP_DELETE) {
            if (!exists) err = "File not found";
        } else {
            char *raw = exists ? env->read_raw(env, path) : NULL;
            if (!raw) err = "File not found or unreadable";
            else if (op->hunk_count) err = apply_hunks(raw, op, &op->result);
            else op->result = raw, raw = NULL;
            free(raw);
            if (!err && op->has_move) {
                char *dest = line_dup(op->move_to);
                if (!dest) err = "Out of memory";
                else if (!line_is(op->move_to, path) && env->file_exists(env, dest)) err = "Move destination already exists";
                free(dest);
            }
        }
        free(path);
        if (err) { char *msg = patch_error(is_error, err, op->path); patch_free(&p); return msg; }
    }

    /* Commit. */
    StrBuf summary; strbuf_init(&summary);
    strbuf_append_cstr(&summary, "Success. Updated the following files:\n");
    for (size_t i = 0; i < p.count; i++) {
        Op *op = &p.ops[i];
        Line target = op->kind == OP_UPDATE && op->has_move ? op->move_to : op->path;
        char *path = line_dup(op->path), *dest = line_dup(target);
        bool ok = path && dest;
        if (ok && op->kind == OP_DELETE) ok = env->delete_file(env, path);
        else if (ok) {
            ok = env->write_file(env, dest, op->result);
            if (ok && op->kind == OP_UPDATE && op->has_move && !str_eq(path, dest)) ok = env->delete_file(env, path);
        }
        free(path); free(dest);
        if (!ok) {
            strbuf_free(&summary);
            char *msg = patch_error(is_error, i ? "Write failed after earlier files were updated" : "Write failed", target);
            patch_free(&p); return msg;
        }
        strbuf_appendf(&summary, "%c %.*s\n", op->kind == OP_ADD ? 'A' : op->kind == OP_DELETE ? 'D' : 'M', (int)target.n, target.s);
    }
    patch_free(&p);
    char *s = strbuf_detach(&summary);
    return s ? s : patch_error(is_error, "Out of memory", (Line){0});
}

static char *tool_apply_patch(const char *args_json, ExecutionEnv *env, bool *is_error) {
    JsonValue *args = json_parse(args_json, NULL);
    char *result = agent_apply_patch(env, json_get_string(args, "patch"), is_error);
    json_free(args);
    return result;
}

void agent_register_apply_patch(ToolRegistry *reg) {
    RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
    if (!t) { reg->failed = true; return; }
    t->def.name = str_dup("apply_patch");
    t->def.description = str_dup(
        "Apply code changes using the patch format. Supports creating, deleting, and modifying files "
        "in a single operation. The patch must start with '*** Begin Patch' and end with '*** End Patch'; "
        "use '*** Add File: <path>' (lines prefixed '+'), '*** Delete File: <path>', or "
        "'*** Update File: <path>' (optional '*** Move to: <path>') followed by '@@ <context>' hunks of "
        "' ' context, '-' removed and '+' added lines.");
    t->def.parameters_json = str_dup(
        "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"string\"}},\"required\":[\"patch\"]}");
    t->execute = tool_apply_patch;
    tool_registry_register(reg, t);
}
