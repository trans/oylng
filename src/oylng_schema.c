/*
 * oylng_schema.c — a schema compiled from a table (part 2)
 *
 * Section numbers are those of spec/yaml-easy-schema.md at trans/yaml2.
 *
 * A schema is the table's entries a document's root reaches. Each node of
 * the table is compiled once, into an xt, and a mapping's or sequence's
 * children only when something reaches it, so nothing recurses on the
 * table's depth. Reaching a root takes three passes, each through what the
 * root reaches and nothing twice:
 *
 *  1. Go through it, compiling entries as names reach them. Each value a
 *     type is written for is a place, and so is each entry reached.
 *  2. Refuse a name that only names itself, and work out each entry's full
 *     size by addition, as section 9 has it: before anything is built.
 *  3. Expand each place's names into its alternatives, which section 9's
 *     ceiling and the full size bound, and check the ceiling.
 *
 * The stream's own schema is compiled into one context for the table, kept
 * with the parser, so that roots share what they reach: an entry reached
 * can't be replaced (section 8), so what was compiled stays true.
 */

#include "oylng_yes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE    UINT32_MAX
#define CEILING 1024

/* ── Errors and memory ───────────────────────────────────── */

static _Noreturn void bad(sctx *S, size_t pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S->msg, sizeof S->msg, fmt, ap);
    va_end(ap);
    S->st = OYLNG_ERR_SCHEMA;
    S->errpos = pos;
    longjmp(S->jb, 1);
}

static _Noreturn void nomem(sctx *S) {
    snprintf(S->msg, sizeof S->msg, "out of memory");
    S->st = OYLNG_ERR_MEMORY;
    S->errpos = 0;
    longjmp(S->jb, 1);
}

static void *alloc(sctx *S, size_t n) {
    void *p = oylng_arena_alloc(S->A, n ? n : 1, 8);
    if (!p) nomem(S);
    return p;
}

static void *zalloc(sctx *S, size_t n) {
    void *p = alloc(S, n);
    memset(p, 0, n);
    return p;
}

static void grow(sctx *S, void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return;
    uint64_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    if (c > UINT32_MAX || c > SIZE_MAX / elem) nomem(S);
    void *np = realloc(*p, (size_t)c * elem);
    if (!np) nomem(S);
    *p = np;
    *cap = (uint32_t)c;
}

#define GROW(S, arr, cap, need) grow((S), (void **)&(arr), &(cap), (need), sizeof *(arr))

static void push(sctx *S, xt *t) {
    GROW(S, S->stk, S->capstk, S->nstk + 1);
    S->stk[S->nstk++] = t;
}

static xstr copy(sctx *S, const char *s, size_t n) {
    if (n > UINT32_MAX - 1) bad(S, 0, "a string of 4 GiB or more in a schema");
    char *d = alloc(S, n + 1);
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return (xstr){ d, (uint32_t)n };
}

static bool is(xstr a, const char *s, size_t n) { return a.len == n && (!n || memcmp(a.s, s, n) == 0); }

static size_t pos32(size_t pos) { return pos > UINT32_MAX ? UINT32_MAX : pos; }

static xt *new_xt(sctx *S, int kind, size_t pos) {
    xt *t = zalloc(S, sizeof *t);
    t->kind = (uint8_t)kind;
    t->pos = (uint32_t)pos32(pos);
    t->node = NONE;
    return t;
}

uint64_t oylng__hash_str(const sctx *S, const char *p, size_t n) { return oylng_hash(&S->key, p, n); }

/* ── The context ─────────────────────────────────────────── */

sctx *oylng__sctx_new(oylng_parser *P, bool stamp) {
    sctx *S = calloc(1, sizeof *S);
    if (!S) return NULL;
    S->A = oylng_arena_new(8192);
    if (!S->A) { free(S); return NULL; }
    oylng_hash_key_new(&S->key);
    S->P = P;
    S->stamp = stamp;
    S->limit = P ? oylng__parser_counts_limit(P) : OYLNG_DEFAULT_LIMIT;
    oylng__parser_input(P, &S->in, &S->inlen);
    return S;
}

void oylng__sctx_free(void *ctx) {
    sctx *S = ctx;
    if (!S) return;
    oylng_arena_free(S->A);
    free(S->buckets);
    free(S->memo);
    free(S->stk);
    free(S->scr32);
    free(S->scrpos);
    free(S->fresh);
    free(S->fresh_types);
    free(S->fresh_entries);
    free(S->fresh_deeps);
    free(S);
}

/* The context's entry for a name the table has, made the first time; NULL
 * if the table has none */
static centry *entry_of(sctx *S, const char *name, size_t len) {
    uint64_t h = oylng__hash_str(S, name, len);
    if (S->nbuckets) {
        for (centry *e = S->buckets[h & (S->nbuckets - 1)]; e; e = e->next)
            if (is(e->name, name, len)) return e;
    }
    uint32_t pe;
    uint32_t node = oylng__table_entry(S->P, name, len, &pe);
    if (node == NONE) return NULL;
    if (S->nentries >= S->nbuckets) {   /* twice the buckets, rehashed */
        uint32_t nb = S->nbuckets ? S->nbuckets * 2 : 64;
        centry **b = calloc(nb, sizeof *b);
        if (!b) nomem(S);
        for (uint32_t i = 0; i < S->nbuckets; i++)
            for (centry *e = S->buckets[i], *nx; e; e = nx) {
                nx = e->next;
                uint64_t eh = oylng__hash_str(S, e->name.s, e->name.len);
                e->next = b[eh & (nb - 1)];
                b[eh & (nb - 1)] = e;
            }
        free(S->buckets);
        S->buckets = b;
        S->nbuckets = nb;
    }
    centry *e = zalloc(S, sizeof *e);
    e->name = copy(S, name, len);
    e->node = node;
    e->pentry = pe;
    e->next = S->buckets[h & (S->nbuckets - 1)];
    S->buckets[h & (S->nbuckets - 1)] = e;
    S->nentries++;
    return e;
}

/* ── Type expressions (sections 4 to 9, 14) ──────────────── */

const char *const oylng__std_names[] = { "str", "int", "bigint", "float", "decimal", "bool", "null",
                                         "date", "datetime", "binary", "any" };

static int std_of(const char *s, size_t n) {
    for (int i = 0; i <= STD_ANY; i++)
        if (strlen(oylng__std_names[i]) == n && memcmp(oylng__std_names[i], s, n) == 0) return i;
    return -1;
}

static bool name_char(char c) {
    return c != ' ' && c != '\t' && c != '(' && c != ')' && c != '[' && c != ']' && c != ',' && c != '|' &&
           c != '/';
}

/* Whether the ( at i opens a type's parentheses: !name just before it, at
 * the start or after a space, [ or , */
static bool opens_args(const char *s, size_t i) {
    size_t j = i;
    while (j > 0 && name_char(s[j - 1]) && s[j - 1] != '!') j--;
    if (j == i || j == 0 || s[j - 1] != '!') return false;
    j--;
    return j == 0 || s[j - 1] == ' ' || s[j - 1] == '[' || s[j - 1] == ',';
}

/* Where a type's parentheses, opened at i, end: at the first /) once a
 * pattern starts, else at ) */
static size_t args_end(sctx *S, const char *s, size_t n, size_t i, size_t pos) {
    size_t k = i + 1;
    while (k < n && s[k] != '/' && s[k] != ')') k++;
    if (k >= n) bad(S, pos, "unclosed parentheses: %.*s", (int)(n > 60 ? 60 : n), s);
    if (s[k] == ')') return k + 1;
    for (size_t e = k + 1; e + 1 < n; e++)
        if (s[e] == '/' && s[e + 1] == ')') return e + 2;
    bad(S, pos, "unclosed pattern: %.*s", (int)(n > 60 ? 60 : n), s);
}

typedef struct { const char *s; size_t n; } span;

/* s split at sep outside brackets and type parentheses, in the arena: read
 * twice, counting and then filling, so that a refusal on the way (an
 * unclosed parenthesis) leaves nothing on the heap */
static span *split(sctx *S, const char *s, size_t n, const char *sep, size_t pos, size_t *count) {
    size_t sl = strlen(sep);
    span *out = NULL;
    size_t k = 0;
    for (int fill = 0; fill < 2; fill++) {
        size_t b = 0, depth = 0;
        k = 0;
        for (size_t i = 0; i < n;) {
            char c = s[i];
            if (c == '(' && opens_args(s, i)) { i = args_end(S, s, n, i, pos); continue; }
            if (c == '[') depth++;
            if (c == ']' && depth) depth--;
            if (!depth && n - i >= sl && memcmp(s + i, sep, sl) == 0) {
                if (fill) out[k] = (span){ s + b, i - b };
                k++;
                i += sl;
                b = i;
                continue;
            }
            i++;
        }
        if (fill) out[k] = (span){ s + b, n - b };
        k++;
        if (!fill) out = alloc(S, k * sizeof *out);
    }
    *count = k;
    return out;
}

static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

static span strip(span p) {
    while (p.n && ws(p.s[0])) { p.s++; p.n--; }
    while (p.n && ws(p.s[p.n - 1])) p.n--;
    return p;
}

/* A number of a limit: -? digits (. digits)? ([eE] [-+]? digits)? at s,
 * its length, 0 if none */
static size_t limit_number(const char *s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    size_t d = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    if (i == d) return 0;
    if (i + 1 < n && s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (s[j] == '-' || s[j] == '+')) j++;
        size_t e = j;
        while (j < n && s[j] >= '0' && s[j] <= '9') j++;
        if (j > e) i = j;
    }
    return i;
}

static uint64_t whole(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v >= OYLNG__SATURATED) return OYLNG__SATURATED;
    }
    return v;
}

/* Sections 4.2, 4.6 and 4.7: in one pair of parentheses, in this order and
 * at most one of each, a precision (!decimal only), a range and a pattern */
static void limits(sctx *S, xt *t, const char *a, size_t n, const char *whole_text, size_t wn, size_t pos) {
    int std = t->std;
    if (std != STD_STR && std != STD_INT && std != STD_BIGINT && std != STD_FLOAT && std != STD_DECIMAL &&
        std != STD_NULL)
        bad(S, pos, "%.*s: !%s takes nothing in parentheses", (int)(wn > 60 ? 60 : wn), whole_text,
            oylng__std_names[std]);
    /* a pattern is last, from the first /, with one space before it */
    const char *slash = memchr(a, '/', n);
    if (slash) {
        size_t si = (size_t)(slash - a), pl = n - si;
        if (pl < 2 || a[n - 1] != '/') bad(S, pos, "bad pattern: %.*s", (int)(wn > 60 ? 60 : wn), whole_text);
        if (si && a[si - 1] != ' ') bad(S, pos, "bad limit: %.*s", (int)(wn > 60 ? 60 : wn), whole_text);
        const char *why = NULL;
        int r = oylng__rx_compile(S->A, slash + 1, pl - 2, CEILING, &t->u.std.rx, &t->u.std.rxcount, &why);
        if (r == -2) nomem(S);
        if (r < 0) bad(S, pos, "%s: %.*s", why, (int)(wn > 60 ? 60 : wn), whole_text);
        t->flags |= XF_PAT;
        n = si ? si - 1 : 0;
        if (!n) return;
    }
    bool rng = false;
    size_t k = 0;
    for (size_t i = 0; i <= n; k++) {
        size_t j = i;
        while (j < n && a[j] != ' ') j++;
        const char *tok = a + i;
        size_t tl = j - i;
        size_t c = 0;
        while (c < tl && tok[c] >= '0' && tok[c] <= '9') c++;
        if (k == 0 && std == STD_DECIMAL && c && tok[0] != '0' && c < tl && tok[c] == ':' && c + 1 < tl) {
            size_t d = c + 1, e = d;
            while (e < tl && tok[e] >= '0' && tok[e] <= '9') e++;
            if (e == tl && (tok[d] != '0' || e == d + 1)) {
                t->u.std.prec = whole(tok, c);
                t->u.std.scale = whole(tok + d, e - d);
                if (t->u.std.scale > t->u.std.prec)
                    bad(S, pos, "a scale larger than the precision: %.*s", (int)(wn > 60 ? 60 : wn), whole_text);
                t->flags |= XF_PREC;
                i = j + 1;
                continue;
            }
        }
        size_t lo = limit_number(tok, tl), hi = 0;
        bool ok = !rng && std != STD_NULL && tl - lo >= 2 && tok[lo] == '.' && tok[lo + 1] == '.';
        if (ok) {
            hi = tl - lo - 2;
            ok = (lo || hi) && (!hi || limit_number(tok + lo + 2, hi) == hi);
        }
        if (!ok) bad(S, pos, "bad limit: %.*s", (int)(wn > 60 ? 60 : wn), whole_text);
        if (lo) { oylng__num_read(tok, lo, &t->u.std.lo); t->flags |= XF_LO; }
        if (hi) { oylng__num_read(tok + lo + 2, hi, &t->u.std.hi); t->flags |= XF_HI; }
        if (lo && hi && oylng__num_cmp(&t->u.std.lo, &t->u.std.hi) > 0)
            bad(S, pos, "bad limit: %.*s", (int)(wn > 60 ? 60 : wn), whole_text);
        rng = true;
        i = j + 1;
    }
}

#define MAX_TEXT_DEPTH 256

static xt *parse_expr(sctx *S, const char *s, size_t n, size_t pos, unsigned depth);

static xt *atom(sctx *S, span p, size_t pos, unsigned depth) {
    const char *s = p.s;
    size_t n = p.n;
    if (!n) bad(S, pos, "an empty part");
    if (n == 2 && s[0] == '\'' && s[1] == '\'') {
        xt *t = new_xt(S, X_LIT, pos);
        t->u.lit = (xstr){ "", 0 };
        return t;
    }
    if (n == 2 && s[0] == '[' && s[1] == ']') return new_xt(S, X_SEQ, pos);
    if (s[0] == '"' || s[0] == '\'')
        bad(S, pos, "a part can only start with a quote as two single quotes: %.*s", (int)(n > 60 ? 60 : n), s);
    if (s[0] == '!') {
        size_t j = 1;
        while (j < n && name_char(s[j])) j++;
        bool args = j < n;
        if (j == 1 || (args && (s[j] != '(' || s[n - 1] != ')')))
            bad(S, pos, "bad type: %.*s", (int)(n > 60 ? 60 : n), s);
        centry *e = entry_of(S, s + 1, j - 1);
        if (e) {
            if (args) bad(S, pos, "%.*s: a named type takes no limit", (int)(n > 60 ? 60 : n), s);
            xt *t = new_xt(S, X_NAME, pos);
            t->u.name = e;
            return t;
        }
        int std = std_of(s + 1, j - 1);
        if (std < 0) bad(S, pos, "unknown type %.*s", (int)(j > 60 ? 60 : j), s);
        S->std_used |= 1u << std;
        xt *t = new_xt(S, X_STD, pos);
        t->std = (uint8_t)std;
        if (args) limits(S, t, s + j + 1, n - j - 2, s, n, pos);
        return t;
    }
    if (s[0] == '[') {
        if (s[n - 1] != ']') bad(S, pos, "bad sequence: %.*s", (int)(n > 60 ? 60 : n), s);
        if (depth >= MAX_TEXT_DEPTH) bad(S, pos, "sequences in a type nested more than %d deep", MAX_TEXT_DEPTH);
        size_t ni;
        span *it = split(S, s + 1, n - 2, ",", pos, &ni);
        for (size_t i = 0; i < ni; i++) it[i] = strip(it[i]);
        xt *t = new_xt(S, X_SEQ, pos);
        if (it[ni - 1].n == 3 && memcmp(it[ni - 1].s, "...", 3) == 0) {
            ni--;
            if (!ni) bad(S, pos, "`...` repeats the item before it");
            ni--;
            t->u.seq.rest = parse_expr(S, it[ni].s, it[ni].n, pos, depth + 1);
        }
        t->u.seq.items = alloc(S, ni * sizeof *t->u.seq.items);
        t->u.seq.n = (uint32_t)ni;
        for (size_t i = 0; i < ni; i++) t->u.seq.items[i] = parse_expr(S, it[i].s, it[i].n, pos, depth + 1);
        t->flags |= XF_FILLED;
        return t;
    }
    if (s[0] == '{') bad(S, pos, "a mapping in a union must be named: %.*s", (int)(n > 60 ? 60 : n), s);
    xt *t = new_xt(S, X_LIT, pos);
    t->u.lit = (xstr){ s, (uint32_t)n };
    return t;
}

/* A type expression: part (" | " part)* */
static xt *parse_expr(sctx *S, const char *s, size_t n, size_t pos, unsigned depth) {
    size_t np;
    span *ps = split(S, s, n, " | ", pos, &np);
    for (size_t i = 0; i < np; i++)
        if (ps[i].n && (ps[i].s[0] == ' ' || ps[i].s[ps[i].n - 1] == ' '))
            bad(S, pos, "a part starts or ends with a space: %.*s", (int)(n > 60 ? 60 : n), s);
    if (np == 1) return atom(S, ps[0], pos, depth);
    xt *t = new_xt(S, X_ALT, pos);
    t->u.alt.v = alloc(S, np * sizeof *t->u.alt.v);
    t->u.alt.n = (uint32_t)np;
    for (size_t i = 0; i < np; i++) t->u.alt.v[i] = atom(S, ps[i], pos, depth);
    return t;
}

/* ── Nodes of the table ──────────────────────────────────── */

static xt *memo_get(sctx *S, uint32_t node) {
    if (!S->memocap) return NULL;
    for (uint32_t h = (uint32_t)(node * 2654435761u) & (S->memocap - 1);; h = (h + 1) & (S->memocap - 1)) {
        if (!S->memo[h].t) return NULL;
        if (S->memo[h].node == node) return S->memo[h].t;
    }
}

static void memo_put(sctx *S, uint32_t node, xt *t) {
    if (2 * (S->nmemo + 1) > S->memocap) {
        uint32_t nc = S->memocap ? S->memocap * 2 : 256;
        void *nm = calloc(nc, sizeof *S->memo);
        if (!nm) nomem(S);
        void *old = S->memo;
        uint32_t oc = S->memocap;
        S->memo = nm;
        S->memocap = nc;
        S->nmemo = 0;
        for (uint32_t i = 0; i < oc; i++) {
            struct { uint32_t node; xt *t; } *o = (void *)((char *)old + i * sizeof *S->memo);
            if (o->t) memo_put(S, o->node, o->t);
        }
        free(old);
    }
    for (uint32_t h = (uint32_t)(node * 2654435761u) & (S->memocap - 1);; h = (h + 1) & (S->memocap - 1))
        if (!S->memo[h].t) {
            S->memo[h].node = node;
            S->memo[h].t = t;
            S->nmemo++;
            return;
        }
}

/* Node i of the table as a type: a string as an expression, a collection
 * with its children left until something reaches it (fill) */
static xt *compile_node(sctx *S, uint32_t i) {
    xt *t = memo_get(S, i);
    if (t) return t;
    oylng__tnode tn;
    oylng__table_node(S->P, i, &tn);
    if (tn.kind == OYLNG__T_STR) {
        xstr text = copy(S, tn.data, tn.len);
        if (!tn.plain || !tn.len) {   /* a quoted scalar is a literal (section 5) */
            t = new_xt(S, X_LIT, tn.pos);
            t->u.lit = text;
        } else t = parse_expr(S, text.s, text.len, tn.pos, 0);
    } else {
        t = new_xt(S, tn.kind == OYLNG__T_SEQ ? X_SEQ : X_MAP, tn.pos);
        t->node = i;
    }
    memo_put(S, i, t);
    return t;
}

typedef struct {
    sctx     *S;
    uint32_t *v;
    uint32_t  n, cap;
    bool      oom;
} kids;

static void kid(void *ctx, uint32_t i) {
    kids *k = ctx;
    if (k->oom) return;
    if (k->n == k->cap) {
        uint32_t nc = k->cap ? k->cap * 2 : 16;
        uint32_t *nv = realloc(k->v, nc * sizeof *nv);
        if (!nv) { k->oom = true; return; }
        k->v = nv;
        k->cap = nc;
    }
    k->v[k->n++] = i;
}

static uint32_t field_slot(const sctx *S, const xt *m, const char *k, size_t n, bool *found) {
    uint32_t mask = m->u.map.ixcap - 1;
    for (uint32_t h = (uint32_t)oylng__hash_str(S, k, n) & mask;; h = (h + 1) & mask) {
        uint32_t f = m->u.map.ix[h];
        if (!f) { *found = false; return h; }
        if (is(m->u.map.f[f - 1].name, k, n)) { *found = true; return h; }
    }
}

const xfield *oylng__field(const sctx *S, const xt *m, const char *k, size_t n) {
    if (!m->u.map.ixcap) {
        for (uint32_t i = 0; i < m->u.map.nf; i++)
            if (is(m->u.map.f[i].name, k, n)) return &m->u.map.f[i];
        return NULL;
    }
    bool found;
    uint32_t h = field_slot(S, m, k, n, &found);
    return found ? &m->u.map.f[m->u.map.ix[h] - 1] : NULL;
}

/* What a `!**` holds (section 10): a mapping of rules, or a type, which is
 * read as rules whose only rule is !* */
static xt *rules_of(sctx *S, uint32_t node, size_t pos) {
    xt *v = compile_node(S, node);
    if (v->kind == X_MAP) return v;
    xt *r = new_xt(S, X_MAP, pos);
    r->flags |= XF_FILLED | XF_BARE;
    r->u.map.other = v;
    return r;
}

/* A collection's children, compiled: a sequence's items and the type
 * `...` repeats, a mapping's fields, type keys, !* (section 7) and !**
 * (section 10) */
static void fill(sctx *S, xt *t) {
    if (t->flags & XF_FILLED) return;
    t->flags |= XF_FILLED;
    if (t->node == NONE) return;   /* [] */
    kids k = { S, NULL, 0, 0, false };
    bool ok = oylng__table_children(S->P, t->node, kid, &k);
    if (!ok || k.oom) { free(k.v); nomem(S); }
    uint32_t *c = oylng_arena_alloc(S->A, k.n ? (size_t)k.n * sizeof *c : 1, 8);
    if (!c) { free(k.v); nomem(S); }
    if (k.n) memcpy(c, k.v, (size_t)k.n * sizeof *c);
    uint32_t n = k.n;
    free(k.v);
    if (t->kind == X_SEQ) {
        if (n) {
            oylng__tnode last;
            oylng__table_node(S->P, c[n - 1], &last);
            if (last.kind == OYLNG__T_STR && last.plain && last.len == 3 && memcmp(last.data, "...", 3) == 0) {
                if (n == 1) bad(S, last.pos, "`...` repeats the item before it");
                n -= 2;
                t->u.seq.rest = compile_node(S, c[n]);
            }
        }
        t->u.seq.items = alloc(S, (size_t)n * sizeof *t->u.seq.items);
        t->u.seq.n = n;
        for (uint32_t i = 0; i < n; i++) t->u.seq.items[i] = compile_node(S, c[i]);
        return;
    }
    uint32_t pairs = n / 2, nf = 0, nrows = 0;
    t->u.map.f = alloc(S, (size_t)pairs * sizeof *t->u.map.f);
    t->u.map.rows = alloc(S, (size_t)pairs * sizeof *t->u.map.rows);
    if (pairs > 8) {
        uint32_t cap = 16;
        while (cap < 2 * pairs) cap *= 2;
        t->u.map.ix = zalloc(S, (size_t)cap * sizeof *t->u.map.ix);
        t->u.map.ixcap = cap;
    }
    for (uint32_t p = 0; p < pairs; p++) {
        oylng__tnode kn;
        oylng__table_node(S->P, c[2 * p], &kn);
        const char *ks = kn.data;
        size_t kl = kn.len;
        bool opt = false;
        if (kn.plain && kl && ks[0] == '!') {
            if (kl == 3 && memcmp(ks, "!**", 3) == 0) {
                t->u.map.deep = rules_of(S, c[2 * p + 1], kn.pos);
                continue;
            }
            xt *v = compile_node(S, c[2 * p + 1]);
            if (kl == 2 && ks[1] == '*') { t->u.map.other = v; continue; }
            xstr kt = copy(S, ks, kl);
            t->u.map.rows[nrows].key = parse_expr(S, kt.s, kt.len, kn.pos, 0);
            t->u.map.rows[nrows++].val = v;
            continue;
        }
        if (kn.plain && kl && ks[kl - 1] == '?') { opt = true; kl--; }
        xfield f = { copy(S, ks, kl), compile_node(S, c[2 * p + 1]), opt };
        /* a key named twice (a and a?): the later wins, in the earlier's place */
        xfield *have = NULL;
        if (t->u.map.ixcap) {
            bool found;
            uint32_t h = field_slot(S, t, f.name.s, f.name.len, &found);
            if (found) have = &t->u.map.f[t->u.map.ix[h] - 1];
            else t->u.map.ix[h] = nf + 1;
        } else
            for (uint32_t i = 0; i < nf; i++)
                if (is(t->u.map.f[i].name, f.name.s, f.name.len)) have = &t->u.map.f[i];
        if (have) *have = f;
        else t->u.map.f[nf++] = f;
    }
    t->u.map.nf = nf;
    t->u.map.nrows = nrows;
    for (uint32_t i = 0; i < nf; i++) t->u.map.nreq += !t->u.map.f[i].opt;
}

/* ── Pass 1: what a root reaches ─────────────────────────── */

static void fresh_add(sctx *S, xt ***arr, uint32_t *n, uint32_t *cap, xt *t) {
    grow(S, (void **)arr, cap, *n + 1, sizeof **arr);
    (*arr)[(*n)++] = t;
}

static void make_place(sctx *S, xt *c, bool types) {
    if (!(c->flags & XF_PLACE)) {
        c->flags |= XF_PLACE;
        fresh_add(S, &S->fresh, &S->nfresh, &S->capfresh, c);
    }
    if (types && !(c->flags & XF_TYPES)) {
        c->flags |= XF_TYPES;
        fresh_add(S, &S->fresh_types, &S->nfresh_types, &S->capfresh_types, c);
    }
    if (!(c->flags & XF_SEEN)) push(S, c);
}

static void reach(sctx *S, centry *e) {
    if (e->reached) return;
    e->reached = true;
    if (!e->top) e->top = compile_node(S, e->node);
    if (S->stamp) oylng__entry_reach(S->P, e->pentry);
    GROW(S, S->fresh_entries, S->capfresh_entries, S->nfresh_entries + 1);
    S->fresh_entries[S->nfresh_entries++] = e;
    make_place(S, e->top, false);
}

static void go_through(sctx *S, xt *root) {
    S->nstk = 0;
    push(S, root);
    while (S->nstk) {
        xt *t = S->stk[--S->nstk];
        if (t->flags & XF_SEEN) continue;
        t->flags |= XF_SEEN;
        switch (t->kind) {
        case X_ALT:
            for (uint32_t i = 0; i < t->u.alt.n; i++)
                if (!(t->u.alt.v[i]->flags & XF_SEEN)) push(S, t->u.alt.v[i]);
            break;
        case X_NAME:
            reach(S, t->u.name);
            break;
        case X_SEQ:
            fill(S, t);
            for (uint32_t i = 0; i < t->u.seq.n; i++) make_place(S, t->u.seq.items[i], true);
            if (t->u.seq.rest) make_place(S, t->u.seq.rest, true);
            break;
        case X_MAP:
            fill(S, t);
            for (uint32_t i = 0; i < t->u.map.nf; i++) make_place(S, t->u.map.f[i].type, true);
            for (uint32_t i = 0; i < t->u.map.nrows; i++) {
                make_place(S, t->u.map.rows[i].key, true);
                make_place(S, t->u.map.rows[i].val, true);
            }
            if (t->u.map.other) make_place(S, t->u.map.other, true);
            if (t->u.map.deep) {
                /* the rules aren't a place, but what they hold is */
                xt *d = t->u.map.deep;
                fill(S, d);
                if (d->u.map.deep) bad(S, d->pos, "a `!**` holds no `!**`");
                GROW(S, S->fresh_deeps, S->capfresh_deeps, S->nfresh_deeps + 1);
                S->fresh_deeps[S->nfresh_deeps++] = d;
                if (!(d->flags & XF_SEEN)) push(S, d);
            }
            break;
        default:
            break;
        }
    }
}

/* ── Pass 2: names, and full sizes ───────────────────────── */

static inline uint64_t sat(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    return s < a ? UINT64_MAX : s;
}

/* What one type expression adds to the full size (section 9), its names'
 * entries' sizes known */
static uint64_t full_of(const xt *t) {
    switch (t->kind) {
    case X_ALT: {
        uint64_t n = 0;
        for (uint32_t i = 0; i < t->u.alt.n; i++) n = sat(n, full_of(t->u.alt.v[i]));
        return n;
    }
    case X_NAME:
        return sat(1, t->u.name->full);
    case X_MAP: {
        uint64_t n = 1 + (uint64_t)t->u.map.nf + t->u.map.nrows + (t->u.map.other != NULL);
        const xt *d = t->u.map.deep;
        /* a `!**` 1, and a mapping of rules its keys too */
        if (d) n += 1 + ((d->flags & XF_BARE) ? 0 : (uint64_t)d->u.map.nf + d->u.map.nrows + (d->u.map.other != NULL));
        return n;
    }
    case X_STD:
        return (t->flags & XF_PAT) ? t->u.std.rxcount : 1;   /* a pattern its count */
    default:
        return 1;
    }
}

/* Positions in a union (section 9): a literal none, a type with a pattern
 * its pattern's count, any other 1 */
static uint64_t positions(const xt *x) {
    if (x->kind == X_LIT) return 0;
    if (x->kind == X_STD && (x->flags & XF_PAT)) return x->u.std.rxcount;
    return 1;
}

/* The names an entry's type is, written bare or in a union: the edges of
 * the graph a name that only names itself goes round */
static uint32_t bare_count(const xt *t) { return t->kind == X_NAME ? 1 : t->kind == X_ALT ? t->u.alt.n : 0; }

static centry *bare_at(const xt *t, uint32_t i) {
    const xt *x = t->kind == X_NAME ? t : t->u.alt.v[i];
    return x->kind == X_NAME ? x->u.name : NULL;
}

enum { E_NEW, E_ON_PATH, E_DONE };

static void check_names(sctx *S, centry *start) {
    if (start->state == E_DONE) return;
    struct dfs { centry *e; uint32_t i; } *st;
    uint32_t n = 0, cap;
    /* the scratch holds the walk, so a failure frees it with the context */
    grow(S, &S->scr32, &S->capscr32, 1, sizeof *st);
    st = S->scr32;
    cap = S->capscr32;
    st[n++] = (struct dfs){ start, 0 };
    start->state = E_ON_PATH;
    while (n) {
        struct dfs *d = &st[n - 1];
        xt *top = d->e->top;
        if (d->i < bare_count(top)) {
            centry *f = bare_at(top, d->i++);
            if (!f) continue;
            if (f->state == E_ON_PATH) bad(S, top->pos, "!%.*s only names itself", (int)f->name.len, f->name.s);
            if (f->state == E_DONE) continue;
            if (!f->top) reach(S, f);
            if (n == cap) {
                S->capscr32 = cap;
                grow(S, &S->scr32, &S->capscr32, n + 1, sizeof *st);
                st = S->scr32;
                cap = S->capscr32;
            }
            st[n++] = (struct dfs){ f, 0 };
            f->state = E_ON_PATH;
            continue;
        }
        d->e->full = full_of(top);
        d->e->state = E_DONE;
        n--;
    }
}

/* The full size of what root reaches, through its places */
static uint64_t root_full(sctx *S, xt *root) {
    uint32_t gen = ++S->gen;
    uint64_t total = 0;
    S->nstk = 0;
    push(S, root);
    while (S->nstk) {
        xt *t = S->stk[--S->nstk];
        if (t->mark == gen) continue;
        t->mark = gen;
        if ((t->flags & XF_PLACE) && t != root) total = sat(total, full_of(t));
        switch (t->kind) {
        case X_ALT:
            for (uint32_t i = 0; i < t->u.alt.n; i++) push(S, t->u.alt.v[i]);
            break;
        case X_NAME:
            push(S, t->u.name->top);
            break;
        case X_SEQ:
            for (uint32_t i = 0; i < t->u.seq.n; i++) push(S, t->u.seq.items[i]);
            if (t->u.seq.rest) push(S, t->u.seq.rest);
            break;
        case X_MAP:
            for (uint32_t i = 0; i < t->u.map.nf; i++) push(S, t->u.map.f[i].type);
            for (uint32_t i = 0; i < t->u.map.nrows; i++) {
                push(S, t->u.map.rows[i].key);
                push(S, t->u.map.rows[i].val);
            }
            if (t->u.map.other) push(S, t->u.map.other);
            if (t->u.map.deep) {   /* what the rules hold, not the rules, which t counts */
                xt *d = t->u.map.deep;
                for (uint32_t i = 0; i < d->u.map.nf; i++) push(S, d->u.map.f[i].type);
                for (uint32_t i = 0; i < d->u.map.nrows; i++) {
                    push(S, d->u.map.rows[i].key);
                    push(S, d->u.map.rows[i].val);
                }
                if (d->u.map.other) push(S, d->u.map.other);
            }
            break;
        default:
            break;
        }
    }
    return total;
}

/* ── Pass 3: alternatives (section 9) ────────────────────── */

uint32_t oylng__lit(const sctx *S, const xalts *a, const char *k, size_t n) {
    if (!a->litcap) return NONE;
    uint32_t mask = a->litcap - 1;
    for (uint32_t h = (uint32_t)oylng__hash_str(S, k, n) & mask;; h = (h + 1) & mask) {
        uint32_t i = a->lits[h];
        if (!i) return NONE;
        if (is(a->p[i - 1].t->u.lit, k, n)) return i - 1;
    }
}

static xt *kind_of(sctx *S, xt *t) {
    if (t->kind != X_NAME) return t;
    centry *e = t->u.name;
    if (e->kind_of) return e->kind_of;
    /* along the chain of names to the type, which pass 2 found has an end */
    uint32_t n = 0;
    S->nstk = 0;
    for (xt *x = t; x->kind == X_NAME && !x->u.name->kind_of; x = x->u.name->top, n++) push(S, x);
    xt *x = S->stk[n - 1]->u.name->top;
    xt *k = x->kind == X_NAME ? x->u.name->kind_of : x;
    for (uint32_t i = 0; i < n; i++) S->stk[i]->u.name->kind_of = k;
    return k;
}

/* t's alternatives: names expanded, each counted once in the union, at its
 * first place (section 9) */
static void expand(sctx *S, xt *t) {
    struct item { xt *t; centry *from; } *st;
    uint32_t n = 0, gen = ++S->gen;
    xpos *out = NULL;
    uint32_t no = 0;
    grow(S, &S->scr32, &S->capscr32, 1, sizeof *st);
    st = S->scr32;
    st[n++] = (struct item){ t, NULL };
    while (n) {
        struct item it = st[--n];
        if (it.t->kind == X_ALT) {
            grow(S, &S->scr32, &S->capscr32, n + it.t->u.alt.n, sizeof *st);
            st = S->scr32;
            for (uint32_t i = it.t->u.alt.n; i-- > 0;) st[n++] = (struct item){ it.t->u.alt.v[i], it.from };
            continue;
        }
        if (it.t->kind == X_NAME) {
            centry *e = it.t->u.name;
            if (e->mark == gen) continue;
            e->mark = gen;
            grow(S, &S->scr32, &S->capscr32, n + 1, sizeof *st);
            st = S->scr32;
            st[n++] = (struct item){ e->top, e };
            continue;
        }
        grow(S, &S->scrpos, &S->capscrpos, no + 1, sizeof *out);
        out = S->scrpos;
        out[no++] = (xpos){ it.t, it.from };
    }
    xalts *a = zalloc(S, sizeof *a);
    a->p = alloc(S, (size_t)no * sizeof *a->p);
    if (no) memcpy(a->p, out, (size_t)no * sizeof *a->p);
    a->n = no;
    a->seq = a->any = NONE;
    uint32_t nlit = 0;
    uint64_t count = 0;
    for (uint32_t i = 0; i < no; i++) {
        xt *x = a->p[i].t;
        if (x->kind == X_LIT) { nlit++; continue; }
        count = sat(count, positions(x));
        if (x->kind == X_SEQ) {
            if (a->seq != NONE) bad(S, t->pos, "more than one sequence alternative");
            a->seq = i;
        } else if (x->kind == X_MAP) a->nmaps++;
        else if (x->std == STD_ANY && a->any == NONE) a->any = i;
    }
    if (count > CEILING)
        bad(S, t->pos, "a union of %llu positions, past the ceiling of %d", (unsigned long long)count, CEILING);
    a->all_lits = no && nlit == no;
    if (a->nmaps) {
        a->maps = alloc(S, a->nmaps * sizeof *a->maps);
        uint32_t m = 0;
        for (uint32_t i = 0; i < no; i++)
            if (a->p[i].t->kind == X_MAP) a->maps[m++] = i;
    }
    if (nlit) {
        uint32_t cap = 16;
        while (cap < 2 * nlit) cap *= 2;
        a->lits = zalloc(S, (size_t)cap * sizeof *a->lits);
        a->litcap = cap;
        for (uint32_t i = 0; i < no; i++) {
            if (a->p[i].t->kind != X_LIT) continue;
            xstr l = a->p[i].t->u.lit;
            uint32_t h = (uint32_t)oylng__hash_str(S, l.s, l.len) & (cap - 1);
            bool dup = false;
            for (; a->lits[h]; h = (h + 1) & (cap - 1))
                if (is(a->p[a->lits[h] - 1].t->u.lit, l.s, l.len)) { dup = true; break; }
            if (!dup) a->lits[h] = i + 1;
        }
    }
    t->alts = a;
    t->kind_of = kind_of(S, t);
}

/* The type keys of mappings, which a key is tried against as one union */
static uint64_t key_count(const xt *m) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < m->u.map.nrows; i++) {
        const xalts *a = m->u.map.rows[i].key->alts;
        for (uint32_t j = 0; j < a->n; j++) n = sat(n, positions(a->p[j].t));
    }
    return n;
}

static void key_ceilings(sctx *S, xt *t) {
    const xalts *a = t->alts;
    uint64_t all = 0;
    for (uint32_t i = 0; i < a->nmaps; i++) {
        uint64_t k = key_count(a->p[a->maps[i]].t);
        if (k > CEILING)
            bad(S, a->p[a->maps[i]].t->pos, "type keys of %llu positions, past the ceiling of %d",
                (unsigned long long)k, CEILING);
        all = sat(all, k);
        if (all > CEILING && a->nmaps > 1)
            bad(S, t->pos, "the type keys of a union's mappings have %llu positions, past the ceiling of %d",
                (unsigned long long)all, CEILING);
    }
}

/* Section 10: a `!**` walks every collection, so the type its !* gives the
 * scalars it reaches can't hold a collection; and its type keys are tried
 * as one union, under the ceiling */
static void check_deep(sctx *S, const xt *d) {
    const xt *o = d->u.map.other;
    if (o)
        for (uint32_t i = 0; i < o->alts->n; i++)
            if (o->alts->p[i].t->kind == X_SEQ || o->alts->p[i].t->kind == X_MAP)
                bad(S, o->pos, "a `!**` takes a scalar type, or a mapping of rules");
    uint64_t k = key_count(d);
    if (k > CEILING)
        bad(S, d->pos, "the type keys of a `!**` have %llu positions, past the ceiling of %d", (unsigned long long)k,
            CEILING);
}

/* ── Roots ───────────────────────────────────────────────── */

static void compile_root(sctx *S, centry *e) {
    if (!e->root) {
        e->root = new_xt(S, X_NAME, 0);
        e->root->u.name = e;
        oylng__tnode tn;
        oylng__table_node(S->P, e->node, &tn);
        e->root->pos = (uint32_t)pos32(tn.pos);
    }
    xt *root = e->root;
    S->nfresh = S->nfresh_types = S->nfresh_entries = S->nfresh_deeps = 0;
    if (!(root->flags & XF_TYPES)) {
        root->flags |= XF_TYPES;
        fresh_add(S, &S->fresh_types, &S->nfresh_types, &S->capfresh_types, root);
    }
    go_through(S, root);
    for (uint32_t i = 0; i < S->nfresh_entries; i++) check_names(S, S->fresh_entries[i]);
    for (uint32_t i = 0; i < S->nfresh; i++) S->full_all = sat(S->full_all, full_of(S->fresh[i]));
    if (S->full_all > S->limit) {
        uint64_t n = root_full(S, root);
        if (n > S->limit)
            bad(S, root->pos, "a schema of full size %llu, past the size limit of %llu", (unsigned long long)n,
                (unsigned long long)S->limit);
    }
    for (uint32_t i = 0; i < S->nfresh_types; i++) expand(S, S->fresh_types[i]);
    for (uint32_t i = 0; i < S->nfresh_types; i++) key_ceilings(S, S->fresh_types[i]);
    for (uint32_t i = 0; i < S->nfresh_deeps; i++) check_deep(S, S->fresh_deeps[i]);
    e->root_ok = true;
}

xt *oylng__sctx_root(sctx *S, const char *name, size_t len, bool *found) {
    *found = false;
    if (setjmp(S->jb)) return NULL;
    centry *e = entry_of(S, name, len);
    if (!e) return NULL;
    *found = true;
    if (!e->root_ok) compile_root(S, e);
    return e->root;
}

/* ── An application's schema ─────────────────────────────── */

static oylng_mark mark_in(const char *in, size_t len, size_t at) {
    if (at > len) at = len;
    oylng_mark m = { at, 1, 1 };
    const char *line = in;
    for (const char *q = in; q < in + at; q++)
        if (*q == '\n') { m.line++; line = q + 1; }
    m.col = (size_t)(in + at - line) + 1;
    return m;
}

static void schema_fail(oylng_schema *sc, oylng_status st, const char *msg, oylng_mark m) {
    sc->st = st;
    snprintf(sc->msg, sizeof sc->msg, "%s", msg);
    sc->mark = m;
}

/* Compiles root, or every entry as a root if root.data is NULL: false if
 * that failed, and S says why; missing says if there is no root */
static bool compile_app(sctx *S, oylng_parser *P, oylng_str root, xt **out, char *missing, size_t mn) {
    missing[0] = '\0';
    if (setjmp(S->jb)) return false;
    if (root.data) {
        centry *en = entry_of(S, root.data, root.len);
        if (!en) snprintf(missing, mn, "the schema has no entry %.*s", (int)(root.len > 60 ? 60 : root.len), root.data);
        else {
            compile_root(S, en);
            *out = en->root;
        }
        return true;
    }
    uint32_t n = oylng__table_count(P);
    for (uint32_t i = 0; i < n; i++) {
        const char *k;
        size_t kl;
        oylng__table_entry_at(P, i, &k, &kl);
        centry *en = entry_of(S, k, kl);
        if (en && !en->root_ok) compile_root(S, en);
    }
    return true;
}

oylng_schema *oylng_schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit) {
    return oylng__schema_new(yaml, len, root, limit, 0);
}

oylng_schema *oylng__schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit,
                                uint64_t counts_limit) {
    oylng_schema *sc = calloc(1, sizeof *sc);
    if (!sc) return NULL;
    if ((!yaml && len) || (root.len && !root.data)) {
        schema_fail(sc, OYLNG_ERR_INPUT, "no schema", (oylng_mark){ 0, 0, 0 });
        return sc;
    }
    oylng_parser *P = oylng_parser_new(yaml ? yaml : "", len);
    if (!P) { free(sc); return NULL; }
    if (limit) oylng_parser_set_limit(P, limit);
    oylng__parser_set_counts_limit(P, counts_limit);
    const oylng_event *e;
    oylng_status st;
    while ((st = oylng_parse_next(P, &e)) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) {}
    if (st != OYLNG_OK) {
        schema_fail(sc, st, oylng_parser_error(P), oylng_parser_error_mark(P));
        oylng_parser_free(P);
        return sc;
    }
    sctx *S = oylng__sctx_new(P, false);
    if (!S) {
        oylng_parser_free(P);
        free(sc);
        return NULL;
    }
    sc->ctx = S;
    char missing[120];
    if (!compile_app(S, P, root, &sc->root, missing, sizeof missing))
        schema_fail(sc, S->st, S->msg, mark_in(yaml, len, S->errpos));
    else if (missing[0])
        schema_fail(sc, OYLNG_ERR_SCHEMA, missing, (oylng_mark){ 0, 1, 1 });
    S->P = NULL;
    S->in = NULL;
    oylng_parser_free(P);
    return sc;
}

oylng_status oylng_schema_status(const oylng_schema *s) { return s ? s->st : OYLNG_ERR_INPUT; }

const char *oylng_schema_error(const oylng_schema *s) { return s && s->st != OYLNG_OK ? s->msg : NULL; }

oylng_mark oylng_schema_error_mark(const oylng_schema *s) {
    return s ? s->mark : (oylng_mark){ 0, 0, 0 };
}

void oylng_schema_free(oylng_schema *s) {
    if (!s) return;
    oylng__sctx_free(s->ctx);
    free(s);
}
