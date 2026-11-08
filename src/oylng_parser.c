/*
 * oylng_parser.c — reads a YAML 2.0 stream
 *
 * The language is spec/yaml-2.0.md at trans/yaml2, Draft 1. Section numbers
 * below are that draft's.
 *
 * One path: a recursive descent over the grammar (section 12), one
 * document at a time, into an array of nodes in document order. The
 * table's nodes come first in the array and stay until `...`; a document's
 * follow them and go once it has been delivered. Events are then read off
 * the array: a reference is followed where it stands, a `-<<` splices in
 * the items it names, and a merged mapping is read through its plan, the
 * order section 9.1 gives its keys.
 *
 * The two sizes of section 10 are counted as nodes are added, so the limit
 * refuses a stream before anything is built past it. A document is read
 * whole before its first event, so a document in error is never delivered
 * in part (section 2).
 *
 * Errors unwind with longjmp to oylng_parse_next. Everything the parser
 * allocates hangs off the parser, so nothing leaks when they do.
 */

#include "oylng_internal.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Nodes ───────────────────────────────────────────────── */

enum { N_STR, N_SEQ, N_MAP, N_REF, N_SPLICE, N_MERGE };

enum { S_PLAIN, S_SINGLE, S_DOUBLE, S_LITERAL, S_FOLDED, S_EMPTY };

#define F_MERGED 1u   /* a mapping with <<: lines, read through its plan */

typedef struct {
    uint8_t     kind;
    uint8_t     sub;     /* STR: how it was written (S_*). MAP: F_MERGED or 0 */
    uint16_t    lev;     /* the levels it goes down, references followed */
    uint32_t    link;    /* SEQ, MAP: the index past the node's subtree.
                            REF, SPLICE, MERGE: the node it names */
    const char *data;    /* STR: the value */
    uint32_t    len;     /* STR: its length in bytes */
    uint32_t    aux;     /* MAP with F_MERGED: its plan; other MAP: its pairs */
    size_t      pos;     /* where it is written in the input */
} node;

/* A merged mapping's plan: its pairs in delivered order */
typedef struct { uint32_t key, val; } pair;
typedef struct { uint32_t start, count; } plan;

/* An open mapping's pair, and its key's hash once the mapping has a table */
typedef struct { uint32_t key, val, hash; } spair;

#define NONE  UINT32_MAX
#define ROOT  UINT32_MAX     /* the mapping the table's entries are keys of */
#define EMPTY UINT32_MAX     /* an unused slot's key */

/* Up to this many pairs, a mapping's keys are compared one by one; past
 * it, they are found by hash */
#define SMALL_MAP 8

/* The table's key index: a hash table of (mapping, key) for following
 * paths, holding the table's entries (as keys of ROOT) and the keys of its
 * mappings of more than SMALL_MAP pairs. For an unmerged mapping val is the
 * value's node, for a merged one the pair's place in its plan, and for ROOT
 * the entry. */
typedef struct {
    uint32_t hash, map, key, val;
} slot;

typedef struct {
    slot    *s;
    uint32_t cap, count;
} keyindex;

/* An entry of the table */
typedef struct {
    uint32_t key, val, tdoc;
    uint64_t full;
} entry;

/* Reading events off the array */
enum { W_NODES, W_PLAN };

typedef struct {
    uint8_t  mode;
    uint8_t  close;      /* the event at its end, or OYLNG_EVT_NONE */
    uint8_t  want_val;   /* W_PLAN: the key is out, its value is next */
    uint32_t cur, end;
} frame;

enum { ST_START, ST_BETWEEN, ST_WALK, ST_DONE, ST_ERROR };

struct oylng_parser {
    const char *in, *end, *p;
    jmp_buf     jb;

    uint64_t limit;
    unsigned max_levels;

    int      state;
    bool     in_table;   /* reading a --- & document */
    bool     defer;      /* reading an entry's key: check the limit after */
    bool     any_doc;    /* a marker has been read */
    unsigned depth;      /* collections open, as written */

    /* nodes, and the full size of each of the table's: a document's
       aren't needed, as nothing refers to them */
    node     *nodes;
    uint64_t *full;
    uint32_t  n, cap, capfull, table_end;

    pair     *pairs;
    uint32_t  npairs, cappairs, pairs_table_end;
    plan     *plans;
    uint32_t  nplans, capplans, plans_table_end;
    spair    *scratch;   /* the open mappings' pairs, innermost last */
    uint32_t  nscratch, capscratch;
    uint32_t *lix;       /* the open mappings' hash tables, innermost last:
                            each slot a pair's place in its scratch, or EMPTY */
    uint32_t  nlix, caplix;

    entry    *entries;
    uint32_t  nentries, capentries, tdoc;

    keyindex       tix;   /* the table's keys */
    oylng_hash_key hkey;
    bool           hkey_set;

    /* section 10: the table's sizes, and the document's */
    uint64_t t_held, t_full, d_held, d_full;

    /* the document being delivered */
    uint32_t  root;
    bool      root_pending;
    oylng_str name;
    frame    *stack;
    uint32_t  sp, capstack;

    oylng_event evt;
    oylng_arena *strings;   /* values that had to be built */

    bool   stopped;
    size_t stop_offset;

    oylng_status status;
    oylng_mark   errmark;
    char         errmsg[200];
};

typedef struct oylng_parser parser;

/* ── Errors ──────────────────────────────────────────────── */

static oylng_mark mark_of(const parser *P, const char *at) {
    if (at < P->in) at = P->in;
    if (at > P->end) at = P->end;
    oylng_mark m = { (size_t)(at - P->in), 1, 1 };
    const char *line = P->in;
    for (const char *q = P->in; q < at; q++)
        if (*q == '\n') { m.line++; line = q + 1; }
    m.col = (size_t)(at - line) + 1;
    return m;
}

#if defined(__GNUC__) || defined(__clang__)
#  define PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#  define PRINTF(a, b)
#endif

static _Noreturn PRINTF(4, 5) void fail_as(parser *P, oylng_status st, const char *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P->errmsg, sizeof P->errmsg, fmt, ap);
    va_end(ap);
    P->status = st;
    P->errmark = mark_of(P, at);
    longjmp(P->jb, 1);
}

#define fail(P, at, ...)       fail_as((P), OYLNG_ERR_PARSE, (at), __VA_ARGS__)
#define fail_limit(P, at, ...) fail_as((P), OYLNG_ERR_LIMIT, (at), __VA_ARGS__)
#define fail_mem(P)            fail_as((P), OYLNG_ERR_MEMORY, (P)->p, "out of memory")

/* ── Growing arrays ──────────────────────────────────────── */

static void *grow(parser *P, void *ptr, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return ptr;
    uint64_t nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    if (nc > UINT32_MAX) fail_limit(P, P->p, "too many nodes for one parser");
    if (nc > SIZE_MAX / elem) fail_mem(P);
    void *np = realloc(ptr, (size_t)nc * elem);
    if (!np) fail_mem(P);
    *cap = (uint32_t)nc;
    return np;
}

/* ── Sizes (section 10) ──────────────────────────────────── */

static inline uint64_t sat(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    return s < a ? UINT64_MAX : s;
}

static inline uint64_t cur_full(const parser *P) {
    return P->in_table ? P->t_full : P->d_full;
}

/* Refuses the stream once held or full passes the limit. This interface
 * delivers data with no references left, so the limit applies to both. */
static void check(parser *P, const char *at) {
    if (sat(P->t_full, P->d_full) > P->limit)
        fail_limit(P, at, "past the size limit of %llu, with references expanded",
                   (unsigned long long)P->limit);
    if (sat(P->t_held, P->d_held) > P->limit)
        fail_limit(P, at, "past the size limit of %llu", (unsigned long long)P->limit);
}

/* Adds to held and full, of the table or the document */
static void count(parser *P, const char *at, uint64_t held, uint64_t full) {
    if (P->in_table) { P->t_held = sat(P->t_held, held); P->t_full = sat(P->t_full, full); }
    else             { P->d_held = sat(P->d_held, held); P->d_full = sat(P->d_full, full); }
    if (!P->defer) check(P, at);
}

static void enter(parser *P, const char *at) {
    if (++P->depth > P->max_levels)
        fail_limit(P, at, "nested more than %u levels deep", P->max_levels);
}

static void leave(parser *P) { P->depth--; }

static uint32_t new_node(parser *P, int kind, const char *at) {
    if (P->n >= P->cap) P->nodes = grow(P, P->nodes, &P->cap, P->n + 1, sizeof *P->nodes);
    uint32_t i = P->n++;
    P->nodes[i] = (node){ .kind = (uint8_t)kind, .pos = (size_t)(at - P->in) };
    if (P->in_table) {
        if (i >= P->capfull) P->full = grow(P, P->full, &P->capfull, i + 1, sizeof *P->full);
        P->full[i] = 0;
    }
    return i;
}

static inline uint32_t subtree_end(const parser *P, uint32_t i) {
    const node *nd = &P->nodes[i];
    return (nd->kind == N_SEQ || nd->kind == N_MAP) ? nd->link : i + 1;
}

static inline void set_full(parser *P, uint32_t i, uint64_t full) {
    if (P->in_table) P->full[i] = full;
}

static inline void set_lev(parser *P, uint32_t i, uint32_t lv) {
    P->nodes[i].lev = (uint16_t)(lv > UINT16_MAX ? UINT16_MAX : lv);
}

/* ── Characters (section 3) ──────────────────────────────── */

#define BAD_CP UINT32_MAX

/* The code point at q, and its length; BAD_CP if it isn't UTF-8 */
static uint32_t utf8(const unsigned char *q, const unsigned char *e, size_t *len) {
    unsigned c = q[0];
    if (c < 0x80) { *len = 1; return c; }
    uint32_t cp;
    size_t n;
    if (c < 0xC2) return BAD_CP;
    else if (c < 0xE0) { n = 2; cp = c & 0x1F; }
    else if (c < 0xF0) { n = 3; cp = c & 0x0F; }
    else if (c < 0xF5) { n = 4; cp = c & 0x07; }
    else return BAD_CP;
    if ((size_t)(e - q) < n) return BAD_CP;
    for (size_t i = 1; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) return BAD_CP;
        cp = cp << 6 | (q[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return BAD_CP;
    if (cp >= 0xD800 && cp < 0xE000) return BAD_CP;
    *len = n;
    return cp;
}

/* Whether a character may appear (dq: inside double quotes). Line feeds
 * and carriage returns are breaks, and their callers deal with them. */
static bool allowed(uint32_t c, bool dq) {
    if (c < 0x80) return c == 0x09 || (c >= 0x20 && c < 0x7F) || (dq && c == 0x7F);
    if (c < 0xA0) return dq;
    if (c == 0x2028 || c == 0x2029 || c == 0xFEFF || c == 0xFFFE || c == 0xFFFF) return dq;
    return true;
}

static inline bool is_brk(const parser *P, const char *q) {
    return q < P->end && (*q == '\n' || (*q == '\r' && q + 1 < P->end && q[1] == '\n'));
}

static inline size_t brk_len(const char *q) { return *q == '\r' ? 2 : 1; }

static inline bool at_eol(const parser *P, const char *q) {
    return q >= P->end || is_brk(P, q);
}

/* The length of the character at q, which isn't a line break; refuses the
 * stream if it isn't allowed there */
static size_t char_len(parser *P, const char *q, bool dq) {
    unsigned c = (unsigned char)*q;
    if ((c >= 0x20 && c < 0x7F) || c == '\t') return 1;
    if (c == '\r') fail(P, q, "a carriage return must be followed by a line feed");
    if (c < 0x80) {
        if (dq && c == 0x7F) return 1;
        if (dq) fail(P, q, "character U+%04X must be written as an escape", c);
        fail(P, q, "character U+%04X is not allowed", c);
    }
    size_t n;
    uint32_t cp = utf8((const unsigned char *)q, (const unsigned char *)P->end, &n);
    if (cp == BAD_CP) fail(P, q, "invalid UTF-8");
    if (!allowed(cp, dq))
        fail(P, q, "character U+%04X is not allowed%s", (unsigned)cp,
             cp < 0xA0 || cp >= 0x2028 ? " outside double quotes" : "");
    return n;
}

static inline int peek(const parser *P, size_t k) {
    return P->p + k < P->end ? (unsigned char)P->p[k] : -1;
}

static inline bool eof(const parser *P) { return P->p >= P->end; }

static inline bool bol(const parser *P, const char *q) {
    return q == P->in || q[-1] == '\n';
}

static size_t col(const parser *P) {
    const char *q = P->p;
    while (q > P->in && q[-1] != '\n') q--;
    return (size_t)(P->p - q);
}

static size_t spaces(parser *P) {
    const char *b = P->p;
    while (P->p < P->end && *P->p == ' ') P->p++;
    return (size_t)(P->p - b);
}

static size_t white(parser *P) {
    const char *b = P->p;
    while (P->p < P->end && (*P->p == ' ' || *P->p == '\t')) P->p++;
    return (size_t)(P->p - b);
}

static size_t indent_here(const parser *P) {
    const char *q = P->p;
    while (q < P->end && *q == ' ') q++;
    return (size_t)(q - P->p);
}

/* At a #: to the end of the line, checking what the comment holds */
static void skip_comment(parser *P) {
    while (!at_eol(P, P->p)) P->p += char_len(P, P->p, false);
}

/* eol ::= (white+ comment?)? (break | EOF) */
static void eol(parser *P) {
    size_t n = white(P);
    if (n && peek(P, 0) == '#') skip_comment(P);
    if (eof(P)) return;
    if (is_brk(P, P->p)) { P->p += brk_len(P->p); return; }
    if (peek(P, 0) == ':')
        fail(P, P->p, "expected the end of the line, not \":\"");
    if (peek(P, 0) == '#')
        fail(P, P->p, "a comment needs a space or a tab before its #");
    fail(P, P->p, "expected the end of the line");
}

/* gap ::= (space* comment? break)* (space* comment? EOF)? */
static void gap(parser *P) {
    for (;;) {
        const char *j = P->p;
        while (P->p < P->end && *P->p == ' ') P->p++;
        if (peek(P, 0) == '#') skip_comment(P);
        if (eof(P)) return;
        if (is_brk(P, P->p)) { P->p += brk_len(P->p); continue; }
        P->p = j;
        return;
    }
}

/* Looking ahead, without refusing anything: eol and gap from q */
static const char *eol_skip(const parser *P, const char *q) {
    const char *b = q;
    while (q < P->end && (*q == ' ' || *q == '\t')) q++;
    if (q > b && q < P->end && *q == '#')
        while (!at_eol(P, q)) q++;
    if (q >= P->end) return q;
    if (is_brk(P, q)) return q + brk_len(q);
    return NULL;
}

static const char *gap_skip(const parser *P, const char *q) {
    for (;;) {
        const char *j = q;
        while (q < P->end && *q == ' ') q++;
        if (q < P->end && *q == '#')
            while (!at_eol(P, q)) q++;
        if (q >= P->end) return q;
        if (is_brk(P, q)) { q += brk_len(q); continue; }
        return j;
    }
}

/* A marker line at q: `---` or `...` at column 0, then a space, a tab or
 * the end of the line */
static bool line_is(const parser *P, const char *q, const char *m) {
    if (!bol(P, q) || P->end - q < 3 || memcmp(q, m, 3) != 0) return false;
    q += 3;
    return q >= P->end || *q == ' ' || *q == '\t' || is_brk(P, q);
}

static bool marker(const parser *P) { return line_is(P, P->p, "---"); }
static bool dots(const parser *P)   { return line_is(P, P->p, "..."); }

/* A %YAML 2.0 line with a marker after it (section 4.4) */
static bool directive_then_marker(const parser *P) {
    const char *q = P->p;
    if (!bol(P, q) || P->end - q < 9 || memcmp(q, "%YAML 2.0", 9) != 0) return false;
    q = eol_skip(P, q + 9);
    if (!q) return false;
    return line_is(P, gap_skip(P, q), "---");
}

/* ── The key index ───────────────────────────────────────── */

static inline void hash_ready(parser *P) {
    if (!P->hkey_set) { oylng_hash_key_new(&P->hkey); P->hkey_set = true; }
}

/* A key's text, for an open mapping's table */
static uint32_t text_hash(parser *P, const char *k, size_t len) {
    hash_ready(P);
    return (uint32_t)oylng_hash(&P->hkey, k, len);
}

/* A mapping and a key's text, for the table's index */
static uint32_t key_hash(parser *P, uint32_t map, const char *k, size_t len) {
    hash_ready(P);
    unsigned char m[4] = { (unsigned char)map, (unsigned char)(map >> 8),
                           (unsigned char)(map >> 16), (unsigned char)(map >> 24) };
    oylng_hasher h;
    oylng_hash_start(&h, &P->hkey);
    oylng_hash_bytes(&h, m, 4);
    oylng_hash_bytes(&h, k, len);
    return (uint32_t)oylng_hash_end(&h);
}

static slot *ix_find(const parser *P, const keyindex *ix, uint32_t map,
                     const char *k, size_t len, uint32_t h) {
    if (!ix->cap) return NULL;
    uint32_t mask = ix->cap - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        slot *s = &ix->s[i];
        if (s->key == EMPTY) return NULL;
        if (s->hash == h && s->map == map) {
            const node *kn = &P->nodes[s->key];
            if (kn->len == len && (len == 0 || memcmp(kn->data, k, len) == 0)) return s;
        }
    }
}

static void ix_place(slot *s, uint32_t cap, slot v) {
    uint32_t mask = cap - 1;
    uint32_t i = v.hash & mask;
    while (s[i].key != EMPTY) i = (i + 1) & mask;
    s[i] = v;
}

static void ix_insert(parser *P, keyindex *ix, uint32_t map, uint32_t key, uint32_t val, uint32_t h) {
    if ((uint64_t)(ix->count + 1) * 2 > ix->cap) {
        uint64_t nc = ix->cap ? (uint64_t)ix->cap * 2 : 64;
        if (nc > UINT32_MAX) fail_limit(P, P->p, "too many keys for one parser");
        slot *ns = malloc((size_t)nc * sizeof *ns);
        if (!ns) fail_mem(P);
        memset(ns, 0xFF, (size_t)nc * sizeof *ns);
        for (uint32_t i = 0; i < ix->cap; i++)
            if (ix->s[i].key != EMPTY) ix_place(ns, (uint32_t)nc, ix->s[i]);
        free(ix->s);
        ix->s = ns;
        ix->cap = (uint32_t)nc;
    }
    ix_place(ix->s, ix->cap, (slot){ h, map, key, val });
    ix->count++;
}

static void ix_clear(keyindex *ix) {
    if (ix->cap > 4096) { free(ix->s); ix->s = NULL; ix->cap = 0; }
    else if (ix->s) memset(ix->s, 0xFF, (size_t)ix->cap * sizeof *ix->s);
    ix->count = 0;
}

static inline bool key_is(const parser *P, uint32_t k, const char *s, size_t len) {
    const node *kn = &P->nodes[k];
    return kn->len == len && (len == 0 || memcmp(kn->data, s, len) == 0);
}

/* ── Strings ─────────────────────────────────────────────── */

static const char empty_text[] = "";

static uint32_t new_str(parser *P, const char *at, const char *data, size_t len, int style) {
    if (len > UINT32_MAX - 1) fail_limit(P, at, "a scalar of 4 GiB or more");
    count(P, at, 1 + (uint64_t)len, 1 + (uint64_t)len);
    uint32_t i = new_node(P, N_STR, at);
    node *nd = &P->nodes[i];
    nd->data = data;
    nd->len = (uint32_t)len;
    nd->sub = (uint8_t)style;
    set_full(P, i, 1 + (uint64_t)len);
    return i;
}

static uint32_t empty_str(parser *P) {
    return new_str(P, P->p, empty_text, 0, S_EMPTY);
}

static char *str_alloc(parser *P, size_t len) {
    char *s = oylng_arena_alloc(P->strings, len ? len : 1, 1);
    if (!s) fail_mem(P);
    return s;
}

static inline bool is_flowi(int c) {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

static inline bool is_indicator(int c) {
    switch (c) {
    case ',': case '[': case ']': case '{': case '}': case '#': case '&': case '*':
    case '|': case '>': case '\'': case '"': case '-': case '?': case ':':
        return true;
    default:
        return false;
    }
}

/* A space, a tab, a break or the end at q */
static inline bool sep_at(const parser *P, const char *q) {
    return q >= P->end || *q == ' ' || *q == '\t' || is_brk(P, q);
}

/* -<< then spaces or tabs then *: only a sequence item (section 9.2) */
static bool merge_item_at(const parser *P, const char *q) {
    if (P->end - q < 4 || memcmp(q, "-<<", 3) != 0) return false;
    q += 3;
    if (*q != ' ' && *q != '\t') return false;
    while (q < P->end && (*q == ' ' || *q == '\t')) q++;
    return q < P->end && *q == '*';
}

/* Why a plain scalar can't start at q, or NULL if it can (section 6.1) */
static const char *plain_start_bad(const parser *P, const char *q, bool flow) {
    if (q >= P->end || is_brk(P, q)) return "expected a value";
    int c = (unsigned char)*q;
    int d = q + 1 < P->end ? (unsigned char)q[1] : -1;
    if (merge_item_at(P, q)) return "-<< is only a sequence item";
    if (c == ' ') return "unexpected space";
    if (c == '\t') return "a tab can't start a value";
    if (c == '&') return "& is not allowed here: anchors are not part of YAML 2.0, and only --- & starts the table";
    if ((c == '-' || c == '?' || c == ':') &&
        (d < 0 || d == ' ' || d == '\t' || d == '\n' || d == '\r' || (flow && is_flowi(d))))
        return c == '?' ? "? is not an indicator in YAML 2.0: a key is a scalar on one line"
                        : "a plain scalar can't start with this";
    if (is_indicator(c) && c != '-' && c != '?' && c != ':') return "a plain scalar can't start with this";
    if (c == '%' && bol(P, q)) return "% at the start of a line is for directives";
    if (c == '!' && !P->in_table)
        return "a value starting with ! must be quoted: tags are not part of YAML 2.0";
    return NULL;
}

/* The bytes a plain scalar goes on over without a second look, as a bit
 * per ASCII byte (block, flow): printable but #, :, and in flow , [ ] { } */
#define ASCII_BIT(c) ((c) < 64 ? (uint64_t)1 << (c) : 0)
#define ASCII_BIT_HI(c) ((c) >= 64 ? (uint64_t)1 << ((c) - 64) : 0)
static const uint64_t plain_run_lo = 0xFFFFFFFE00000000u & ~ASCII_BIT('#') & ~ASCII_BIT(':');
static const uint64_t plain_run_hi = 0x7FFFFFFFFFFFFFFFu;
static const uint64_t flow_run_lo  = 0xFFFFFFFE00000000u & ~ASCII_BIT('#') & ~ASCII_BIT(':') & ~ASCII_BIT(',');
static const uint64_t flow_run_hi  = 0x7FFFFFFFFFFFFFFFu & ~ASCII_BIT_HI('[') & ~ASCII_BIT_HI(']')
                                     & ~ASCII_BIT_HI('{') & ~ASCII_BIT_HI('}');

static inline bool in_run(unsigned c, uint64_t lo, uint64_t hi) {
    return c < 64 ? (lo >> c) & 1 : c < 128 && (hi >> (c - 64)) & 1;
}

/* Where a plain scalar from q ends, before any spaces after it. With
 * check, every character is checked; without, it only looks ahead. */
static const char *plain_end(parser *P, const char *q, bool flow, bool check) {
    const char *e = P->end, *last = q;
    uint64_t lo = flow ? flow_run_lo : plain_run_lo, hi = flow ? flow_run_hi : plain_run_hi;
    while (q < e) {
        unsigned char c = (unsigned char)*q;
        if (in_run(c, lo, hi)) {
            do q++; while (q < e && in_run((unsigned char)*q, lo, hi));
            last = q;
            continue;
        }
        if (c == '\n' || (c == '\r' && q + 1 < e && q[1] == '\n')) break;
        if (c == '#' && (q[-1] == ' ' || q[-1] == '\t')) break;
        if (c == ':') {
            const char *r = q + 1;
            if (r >= e || *r == ' ' || *r == '\t' || is_brk(P, r) || (flow && is_flowi(*r))) break;
        }
        if (flow && is_flowi(c)) break;
        if (c == ' ' || c == '\t') { q++; continue; }
        q += check ? char_len(P, q, false) : 1;
        last = q;
    }
    return last;
}

static uint32_t plain(parser *P, bool flow) {
    const char *s = P->p;
    const char *why = plain_start_bad(P, s, flow);
    if (why) {
        if (s < P->end && *s == '\r' && !is_brk(P, s)) char_len(P, s, false);
        fail(P, s, "%s", why);
    }
    const char *e = plain_end(P, s, flow, true);
    P->p = e;
    return new_str(P, s, s, (size_t)(e - s), S_PLAIN);
}

static int hex4(const parser *P, const char *q) {
    if (P->end - q < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)q[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v << 4 | d;
    }
    return v;
}

static size_t put_utf8(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* A double-quoted scalar, as in JSON (section 6.2) */
static uint32_t dquoted(parser *P) {
    const char *s = P->p, *q = s + 1, *e = P->end;
    bool esc = false;
    for (;;) {
        if (at_eol(P, q)) fail(P, s, "a double-quoted scalar must close on its line");
        if (*q == '"') break;
        if (*q != '\\') { q += char_len(P, q, true); continue; }
        esc = true;
        int c = q + 1 < e ? (unsigned char)q[1] : -1;
        switch (c) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
            q += 2;
            break;
        case 'u': {
            int u = hex4(P, q + 2);
            if (u < 0) fail(P, q, "\\u takes four hex digits");
            if (u >= 0xDC00 && u < 0xE000) fail(P, q, "a lone surrogate, \\u%04X, names no character", u);
            if (u >= 0xD800 && u < 0xDC00) {
                int lo = (e - q >= 12 && q[6] == '\\' && q[7] == 'u') ? hex4(P, q + 8) : -1;
                if (lo < 0xDC00 || lo >= 0xE000)
                    fail(P, q, "a lone surrogate, \\u%04X, names no character", u);
                q += 12;
            } else q += 6;
            break;
        }
        default:
            if (c < 0 || at_eol(P, q + 1)) fail(P, s, "a double-quoted scalar must close on its line");
            fail(P, q, "this is not an escape in YAML 2.0, which has JSON's escapes only");
        }
    }
    P->p = q + 1;
    if (!esc) return new_str(P, s, s + 1, (size_t)(q - s - 1), S_DOUBLE);

    char *out = str_alloc(P, (size_t)(q - s - 1));
    size_t o = 0;
    for (const char *r = s + 1; r < q;) {
        if (*r != '\\') { out[o++] = *r++; continue; }
        switch (r[1]) {
        case 'b': out[o++] = '\b'; r += 2; break;
        case 'f': out[o++] = '\f'; r += 2; break;
        case 'n': out[o++] = '\n'; r += 2; break;
        case 'r': out[o++] = '\r'; r += 2; break;
        case 't': out[o++] = '\t'; r += 2; break;
        case 'u': {
            uint32_t u = (uint32_t)hex4(P, r + 2);
            r += 6;
            if (u >= 0xD800 && u < 0xDC00) {
                u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)hex4(P, r + 2) - 0xDC00);
                r += 6;
            }
            o += put_utf8(out + o, u);
            break;
        }
        default: out[o++] = r[1]; r += 2; break;   /* " \ / */
        }
    }
    return new_str(P, s, out, o, S_DOUBLE);
}

/* A single-quoted scalar: '' is one ' (section 6.3) */
static uint32_t squoted(parser *P) {
    const char *s = P->p, *q = s + 1;
    bool doubled = false;
    for (;;) {
        if (at_eol(P, q)) fail(P, s, "a single-quoted scalar must close on its line");
        if (*q == '\'') {
            if (q + 1 < P->end && q[1] == '\'') { doubled = true; q += 2; continue; }
            break;
        }
        q += char_len(P, q, false);
    }
    P->p = q + 1;
    if (!doubled) return new_str(P, s, s + 1, (size_t)(q - s - 1), S_SINGLE);
    char *out = str_alloc(P, (size_t)(q - s - 1));
    size_t o = 0;
    for (const char *r = s + 1; r < q; r++) {
        out[o++] = *r;
        if (*r == '\'') r++;
    }
    return new_str(P, s, out, o, S_SINGLE);
}

/* A block scalar under a parent at indent n (section 6.4) */
static uint32_t block(parser *P, long n) {
    const char *h = P->p, *e = P->end;
    bool fold = *h == '>';
    P->p++;
    bool chomp = false;
    if (peek(P, 0) == '-') { chomp = true; P->p++; }
    if (!sep_at(P, P->p))
        fail(P, P->p, "a block scalar's header is |, |-, > or >-, and only a comment may follow it");
    eol(P);

    /* first, where it ends and its indent m */
    const char *body = P->p, *q = body, *stop = body;
    long m = -1;
    size_t wide = 0;
    while (q < e) {
        const char *ls = q;
        size_t ind = 0;
        while (q < e && *q == ' ') { q++; ind++; }
        bool empty = at_eol(P, q);
        if (empty && (m < 0 || (long)ind <= m)) {
            if (m < 0 && ind > wide) wide = ind;
            if (q < e) q += brk_len(q);
            stop = q;
            continue;
        }
        if (m < 0) {
            if ((long)ind <= n || ind < 1) break;
            m = (long)ind;
            if ((long)wide > m)
                fail(P, ls, "a blank line before the text holds more spaces than the text's indent");
        } else if ((long)ind < m) break;
        const char *t = ls + m;
        if (fold && (*t == ' ' || *t == '\t'))
            fail(P, t, "a line of a folded scalar can't start with a space or a tab");
        q = t;
        while (!at_eol(P, q)) q += char_len(P, q, false);
        if (q < e) q += brk_len(q);
        stop = q;
    }
    P->p = stop;
    if (m < 0) return new_str(P, h, empty_text, 0, fold ? S_FOLDED : S_LITERAL);

    /* then the value */
    char *out = str_alloc(P, (size_t)(stop - body) + 1);
    size_t o = 0, blanks = 0;
    bool seen = false;
    for (q = body; q < stop;) {
        const char *ls = q;
        size_t ind = 0;
        while (q < stop && *q == ' ') { q++; ind++; }
        if (at_eol(P, q) && (long)ind <= m) {
            blanks++;
            if (q < e) q += brk_len(q);
            continue;
        }
        const char *t = ls + m, *te = t;
        while (!at_eol(P, te)) te++;
        q = te < e ? te + brk_len(te) : te;
        if (fold && seen && blanks == 0) out[o++] = ' ';
        else {
            /* literal: a break ends each line before this one; folded:
               each blank line between two lines is one */
            size_t breaks = fold ? blanks : blanks + (seen ? 1 : 0);
            for (size_t i = 0; i < breaks; i++) out[o++] = '\n';
        }
        blanks = 0;
        memcpy(out + o, t, (size_t)(te - t));
        o += (size_t)(te - t);
        seen = true;
    }
    if (!chomp) out[o++] = '\n';
    return new_str(P, h, out, o, fold ? S_FOLDED : S_LITERAL);
}

/* ── References (section 8.2) ────────────────────────────── */

static inline bool ref_end(const parser *P, const char *q, bool flow) {
    return sep_at(P, q) || (flow && (*q == ',' || *q == ']' || *q == '}'));
}

/* One step of a path: a bare key, or <...> */
static void step(parser *P, bool flow, const char **k, size_t *kl) {
    const char *q = P->p, *e = P->end;
    if (q < e && *q == '<') {
        const char *b = q + 1, *r = b;
        while (!at_eol(P, r) && *r != '>') r += char_len(P, r, false);
        if (at_eol(P, r) || r == b) fail(P, q, "a <...> step holds one key, and closes with >");
        *k = b;
        *kl = (size_t)(r - b);
        P->p = r + 1;
        if (!ref_end(P, P->p, flow) && *P->p != ':')
            fail(P, P->p, "a <...> step ends at its >");
        if (peek(P, 0) == ':' && peek(P, 1) == ':')
            fail(P, P->p, "after <...>, a : starts the next step, which can't start with :");
        return;
    }
    const char *b = q;
    while (!ref_end(P, q, flow)) {
        if (*q == '>') fail(P, q, "a key holding > can't be written in a path");
        if (*q == ':') {
            const char *r = q;
            while (r < e && *r == ':') r++;
            if (r - q == 1) break;
            q = r;
            continue;
        }
        q += char_len(P, q, false);
    }
    if (q == b) fail(P, b, "a reference needs a key after * and after each :");
    *k = b;
    *kl = (size_t)(q - b);
    P->p = q;
}

static uint32_t entry_get(parser *P, const char *k, size_t kl) {
    slot *s = ix_find(P, &P->tix, ROOT, k, kl, key_hash(P, ROOT, k, kl));
    return s ? P->entries[s->val].val : NONE;
}

/* The value of key k in mapping m of the table, or NONE */
static uint32_t map_get(parser *P, uint32_t m, const char *k, size_t kl) {
    const node *nd = &P->nodes[m];
    bool merged = (nd->sub & F_MERGED) != 0;
    const pair *pr = merged ? P->pairs + P->plans[nd->aux].start : NULL;
    uint32_t cnt = merged ? P->plans[nd->aux].count : nd->aux;
    if (cnt > SMALL_MAP) {
        slot *s = ix_find(P, &P->tix, m, k, kl, key_hash(P, m, k, kl));
        if (!s) return NONE;
        return merged ? pr[s->val].val : s->val;
    }
    if (merged) {
        for (uint32_t i = 0; i < cnt; i++)
            if (key_is(P, pr[i].key, k, kl)) return pr[i].val;
    } else
        for (uint32_t i = m + 1; i < nd->link; i = subtree_end(P, i + 1))
            if (key_is(P, i, k, kl)) return i + 1;
    return NONE;
}

static inline uint32_t deref(const parser *P, uint32_t v) {
    while (P->nodes[v].kind == N_REF) v = P->nodes[v].link;
    return v;
}

/* A reference: the node in the table it names (section 8.3) */
static uint32_t ref(parser *P, bool flow) {
    const char *s = P->p;
    if (peek(P, 0) != '*') fail(P, s, "expected a reference, *name");
    P->p++;
    const char *k;
    size_t kl;
    step(P, flow, &k, &kl);
    uint32_t v = entry_get(P, k, kl);
    if (v == NONE) fail(P, s, "the table has no entry \"%.*s\" here", (int)(kl > 60 ? 60 : kl), k);
    while (peek(P, 0) == ':' && !ref_end(P, P->p + 1, flow)) {
        P->p++;
        const char *ks = P->p;
        step(P, flow, &k, &kl);
        v = deref(P, v);
        if (P->nodes[v].kind != N_MAP)
            fail(P, ks, "the path steps through a %s, which has no keys",
                 P->nodes[v].kind == N_SEQ ? "sequence" : "string");
        uint32_t w = map_get(P, v, k, kl);
        if (w == NONE) fail(P, ks, "no key \"%.*s\" there", (int)(kl > 60 ? 60 : kl), k);
        v = w;
    }
    return deref(P, v);
}

static uint32_t plainref(parser *P, bool flow) {
    const char *s = P->p;
    uint32_t t = ref(P, flow);
    count(P, s, 1, P->full[t]);
    uint32_t r = new_node(P, N_REF, s);
    P->nodes[r].link = t;
    set_full(P, r, P->full[t]);
    P->nodes[r].lev = P->nodes[t].lev;
    return r;
}

/* ── Mappings, with merges (sections 7.1, 7.4, 9.1) ──────── */

/* An open mapping. Its pairs are in the scratch from s0, in the order they
 * are delivered: for a merged one, its plan as it is built. Past SMALL_MAP
 * pairs it has a hash table of tcap slots in lix from t0. Mappings close in
 * the reverse of the order they open, and a mapping takes no pair while one
 * opened inside it is open, so the mapping taking a pair has the scratch's
 * last pairs and lix's last table. */
typedef struct {
    uint32_t M;
    uint32_t s0;       /* where its pairs start in the scratch */
    uint32_t t0, tcap; /* its table in lix; tcap 0 until it has one */
    uint32_t lv;       /* the most levels below it */
    uint64_t before;   /* full before it */
    bool     merged, started;
} mapctx;

static void map_open(parser *P, mapctx *c, const char *at) {
    c->before = cur_full(P);
    enter(P, at);
    count(P, at, 1, 1);
    c->M = new_node(P, N_MAP, at);
    c->s0 = P->nscratch;
    c->t0 = P->nlix;
    c->tcap = 0;
    c->lv = 0;
    c->merged = c->started = false;
}

/* The open mapping's pair whose key is k, as its place from s0, or NONE;
 * h is k's hash if the mapping has a table */
static uint32_t pair_find(const parser *P, const mapctx *c, const char *k, size_t len, uint32_t h) {
    const spair *sp = P->scratch + c->s0;
    if (!c->tcap) {
        uint32_t cnt = P->nscratch - c->s0;
        for (uint32_t i = 0; i < cnt; i++)
            if (key_is(P, sp[i].key, k, len)) return i;
        return NONE;
    }
    const uint32_t *t = P->lix + c->t0;
    uint32_t mask = c->tcap - 1;
    for (uint32_t j = h & mask;; j = (j + 1) & mask) {
        uint32_t i = t[j];
        if (i == EMPTY) return NONE;
        if (sp[i].hash == h && key_is(P, sp[i].key, k, len)) return i;
    }
}

static void table_put(parser *P, const mapctx *c, uint32_t i) {
    uint32_t *t = P->lix + c->t0, mask = c->tcap - 1;
    uint32_t j = P->scratch[c->s0 + i].hash & mask;
    while (t[j] != EMPTY) j = (j + 1) & mask;
    t[j] = i;
}

/* Gives the open mapping a table for its cnt pairs, hashing them, or a
 * bigger table, from the hashes kept with them */
static void table_build(parser *P, mapctx *c, uint32_t cnt) {
    spair *sp = P->scratch + c->s0;
    if (!c->tcap)
        for (uint32_t i = 0; i < cnt; i++) {
            const node *kn = &P->nodes[sp[i].key];
            sp[i].hash = text_hash(P, kn->data, kn->len);
        }
    uint64_t cap = c->tcap ? (uint64_t)c->tcap * 2 : 32;
    while (cap < (uint64_t)cnt * 2) cap *= 2;
    if (c->t0 + cap > UINT32_MAX) fail_limit(P, P->p, "too many keys for one parser");
    P->lix = grow(P, P->lix, &P->caplix, (uint32_t)(c->t0 + cap), sizeof *P->lix);
    P->nlix = (uint32_t)(c->t0 + cap);
    c->tcap = (uint32_t)cap;
    memset(P->lix + c->t0, 0xFF, (size_t)cap * sizeof *P->lix);
    for (uint32_t i = 0; i < cnt; i++) table_put(P, c, i);
}

/* A new pair for the open mapping, whose key hashes to h if the mapping
 * has a table; returns its place from s0 */
static uint32_t pair_add(parser *P, mapctx *c, uint32_t k, uint32_t v, uint32_t h) {
    P->scratch = grow(P, P->scratch, &P->capscratch, P->nscratch + 1, sizeof *P->scratch);
    uint32_t r = P->nscratch - c->s0;
    P->scratch[P->nscratch++] = (spair){ k, v, h };
    if (r + 1 <= SMALL_MAP) return r;
    if ((uint64_t)(r + 1) * 2 > c->tcap) table_build(P, c, r + 1);
    else table_put(P, c, r);
    return r;
}

static inline uint32_t pair_hash(parser *P, const mapctx *c, uint32_t k) {
    return c->tcap ? text_hash(P, P->nodes[k].data, P->nodes[k].len) : 0;
}

/* <<: *src, before any written key */
static void map_merge(parser *P, mapctx *c, uint32_t src, const char *at) {
    if (P->nodes[src].kind != N_MAP) fail(P, at, "<<: names a mapping");
    count(P, at, 1, P->full[src] - 1);
    uint32_t g = new_node(P, N_MERGE, at);
    P->nodes[g].link = src;
    c->merged = true;
    P->nodes[c->M].sub |= F_MERGED;

    const node *sn = &P->nodes[src];
    uint32_t i, end, k, v;
    bool via_plan = (sn->sub & F_MERGED) != 0;
    if (via_plan) { i = P->plans[sn->aux].start; end = i + P->plans[sn->aux].count; }
    else { i = src + 1; end = sn->link; }
    while (i < end) {
        if (via_plan) { k = P->pairs[i].key; v = P->pairs[i].val; i++; }
        else { k = i; v = i + 1; i = subtree_end(P, v); }
        const node *kn = &P->nodes[k];
        uint32_t h = pair_hash(P, c, k);
        uint32_t r = pair_find(P, c, kn->data, kn->len, h);
        if (r != NONE) P->scratch[c->s0 + r].val = v;
        else pair_add(P, c, k, v, h);
    }
}

/* A written key, node k: refused if written before. Returns its pair's
 * place from s0. */
static uint32_t map_key(parser *P, mapctx *c, uint32_t k) {
    const node *kn = &P->nodes[k];
    uint32_t h = pair_hash(P, c, k);
    uint32_t r = pair_find(P, c, kn->data, kn->len, h);
    c->started = true;
    if (r == NONE) return pair_add(P, c, k, NONE, h);
    /* a merged key has its node in the source, before M */
    if (!c->merged || P->scratch[c->s0 + r].key > c->M)
        fail(P, P->in + kn->pos, "the key \"%.*s\" is repeated",
             (int)(kn->len > 60 ? 60 : kn->len), kn->data);
    P->scratch[c->s0 + r].key = k;
    return r;
}

static void map_value(parser *P, mapctx *c, uint32_t r, uint32_t v) {
    P->scratch[c->s0 + r].val = v;
    if (!c->merged && P->nodes[v].lev > c->lv) c->lv = P->nodes[v].lev;
}

static uint32_t map_close(parser *P, mapctx *c) {
    uint32_t lv = c->lv, cnt = P->nscratch - c->s0;
    const spair *sp = P->scratch + c->s0;
    if (c->merged) {
        P->plans = grow(P, P->plans, &P->capplans, P->nplans + 1, sizeof *P->plans);
        P->pairs = grow(P, P->pairs, &P->cappairs, P->npairs + cnt, sizeof *P->pairs);
        P->plans[P->nplans] = (plan){ P->npairs, cnt };
        P->nodes[c->M].aux = P->nplans++;
        for (uint32_t i = 0; i < cnt; i++) {
            P->pairs[P->npairs++] = (pair){ sp[i].key, sp[i].val };
            if (P->nodes[sp[i].val].lev > lv) lv = P->nodes[sp[i].val].lev;
        }
    } else P->nodes[c->M].aux = cnt;
    /* paths look keys up in the table's mappings until ... */
    if (P->in_table && cnt > SMALL_MAP)
        for (uint32_t i = 0; i < cnt; i++) {
            const node *kn = &P->nodes[sp[i].key];
            ix_insert(P, &P->tix, c->M, sp[i].key, c->merged ? i : sp[i].val,
                      key_hash(P, c->M, kn->data, kn->len));
        }
    P->nscratch = c->s0;
    P->nlix = c->t0;
    P->nodes[c->M].link = P->n;
    set_lev(P, c->M, lv + 1);
    set_full(P, c->M, cur_full(P) - c->before);
    leave(P);
    return c->M;
}

static uint32_t splice(parser *P, uint32_t src, const char *at, uint32_t *lv) {
    if (P->nodes[src].kind != N_SEQ) fail(P, at, "-<< names a sequence");
    count(P, at, 1, P->full[src] - 1);
    uint32_t s = new_node(P, N_SPLICE, at);
    P->nodes[s].link = src;
    if (P->nodes[src].lev > *lv) *lv = P->nodes[src].lev;   /* its items sit one level below src */
    return s;
}

static bool starts_merge(const parser *P, bool flow) {
    const char *q = P->p;
    if (P->end - q < 3 || memcmp(q, "<<:", 3) != 0) return false;
    q += 3;
    return q >= P->end || *q == ' ' || is_brk(P, q) || (flow && *q == '\t');
}

/* ── Block nodes (sections 7.1, 7.2) ─────────────────────── */

static uint32_t node_at(parser *P, long n);
static uint32_t value(parser *P, long m, bool inmap);
static uint32_t line_node(parser *P, long n);
static uint32_t flow(parser *P);

static bool is_seq(const parser *P) {
    const char *q = P->p;
    if (q >= P->end || *q != '-') return false;
    if (sep_at(P, q + 1)) return true;
    if (P->end - q < 4 || q[1] != '<' || q[2] != '<' || q[3] != ' ') return false;
    q += 3;
    while (q < P->end && *q == ' ') q++;
    return q < P->end && *q == '*';
}

/* Where a key from q ends, looking ahead without refusing; NULL if no
 * key starts there */
static const char *key_end(parser *P, const char *q) {
    const char *e = P->end;
    if (q >= e) return NULL;
    if (*q == '"') {
        for (q++; !at_eol(P, q); q++) {
            if (*q == '\\') { if (at_eol(P, q + 1)) return NULL; q++; }
            else if (*q == '"') return q + 1;
        }
        return NULL;
    }
    if (*q == '\'') {
        for (q++; !at_eol(P, q); q++) {
            if (*q != '\'') continue;
            if (q + 1 < e && q[1] == '\'') { q++; continue; }
            return q + 1;
        }
        return NULL;
    }
    if (plain_start_bad(P, q, false)) return NULL;
    const char *r = plain_end(P, q, false, false);
    if (r - q == 2 && q[0] == '<' && q[1] == '<') return NULL;
    return r;
}

static bool is_map(parser *P) {
    const char *q = P->p, *e = P->end;
    if (starts_merge(P, false)) return true;
    const char *r = key_end(P, q);
    if (!r) return false;
    while (r < e && *r == ' ') r++;
    return r < e && *r == ':' && sep_at(P, r + 1);
}

/* At an entry at indent m: mid-line at that column, or a line indented m */
static bool at_entry(parser *P, long m) {
    if (eof(P)) return false;
    if (bol(P, P->p)) {
        if (marker(P) || dots(P) || directive_then_marker(P)) return false;
        if ((long)indent_here(P) != m) return false;
        P->p += m;
        return true;
    }
    return (long)col(P) == m;
}

static uint32_t key(parser *P) {
    int c = peek(P, 0);
    if (c == '"') return dquoted(P);
    if (c == '\'') return squoted(P);
    const char *s = P->p;
    uint32_t k = plain(P, false);
    if (P->nodes[k].len == 2 && memcmp(P->nodes[k].data, "<<", 2) == 0)
        fail(P, s, "a plain << is the merge indicator; the key is written \"<<\"");
    return k;
}

static void colon(parser *P) {
    spaces(P);
    if (peek(P, 0) != ':') fail(P, P->p, "expected : after the key");
    P->p++;
}

static uint32_t block_map(parser *P, long m) {
    mapctx c;
    map_open(P, &c, P->p);
    for (;;) {
        const char *j = P->p;
        if (!at_entry(P, m)) { P->p = j; break; }
        if (starts_merge(P, false)) {
            const char *at = P->p;
            if (c.started) fail(P, at, "<<: comes before the mapping's other keys");
            P->p += 3;
            if (!spaces(P)) fail(P, P->p, "<<: needs a reference after a space");
            uint32_t src = ref(P, false);
            eol(P);
            map_merge(P, &c, src, at);
        } else {
            if (!is_map(P)) {
                if (is_seq(P)) fail(P, P->p, "a sequence item where this mapping expects a key");
                fail(P, P->p, "expected a key and :, at this mapping's indent");
            }
            uint32_t k = key(P);
            colon(P);
            uint32_t r = map_key(P, &c, k);
            map_value(P, &c, r, value(P, m, true));
        }
        gap(P);
    }
    return map_close(P, &c);
}

static uint32_t block_seq(parser *P, long m) {
    uint64_t before = cur_full(P);
    const char *at = P->p;
    enter(P, at);
    count(P, at, 1, 1);
    uint32_t S = new_node(P, N_SEQ, at);
    uint32_t lv = 0;
    for (;;) {
        const char *j = P->p;
        if (!at_entry(P, m) || !is_seq(P)) { P->p = j; break; }
        if (peek(P, 1) == '<') {
            const char *sp = P->p;
            P->p += 3;
            spaces(P);
            uint32_t src = ref(P, false);
            eol(P);
            splice(P, src, sp, &lv);
        } else {
            P->p++;
            const char *k0 = P->p;
            size_t k = spaces(P);
            uint32_t v;
            if (k && peek(P, 0) != '#' && !at_eol(P, P->p)) {
                long c = (long)col(P);
                if (is_seq(P)) v = block_seq(P, c);
                else if (is_map(P)) v = block_map(P, c);
                else v = line_node(P, m);
            } else {
                P->p = k0;
                eol(P);
                gap(P);
                v = node_at(P, m);
            }
            if (P->nodes[v].lev > lv) lv = P->nodes[v].lev;
        }
        gap(P);
    }
    P->nodes[S].link = P->n;
    set_lev(P, S, lv + 1);
    set_full(P, S, cur_full(P) - before);
    leave(P);
    return S;
}

/* node(n) ::= map(m) | seq(m) | indent(m) line-node(max(n, 0)), m > n */
static uint32_t node_at(parser *P, long n) {
    gap(P);
    if (eof(P) || marker(P) || dots(P)) return empty_str(P);
    long m = (long)indent_here(P);
    if (m <= n) return empty_str(P);
    P->p += m;
    if (is_seq(P)) return block_seq(P, m);
    if (is_map(P)) return block_map(P, m);
    return line_node(P, n < 0 ? 0 : n);
}

/* What follows a key's : or a table entry's (section 7.1) */
static uint32_t value(parser *P, long m, bool inmap) {
    const char *j = P->p;
    size_t n = spaces(P);
    if (n && peek(P, 0) != '#' && !at_eol(P, P->p)) return line_node(P, m);
    P->p = j;
    if (!sep_at(P, P->p) && peek(P, 0) != '#')
        fail(P, P->p, "a : needs a space or the end of the line after it");
    if (peek(P, 0) == '\t') {
        const char *t = P->p;
        white(P);
        if (peek(P, 0) != '#' && !at_eol(P, P->p)) fail(P, t, "a tab can't come before a value");
        P->p = t;
    }
    eol(P);
    gap(P);
    if (eof(P) || marker(P) || dots(P)) return empty_str(P);
    long k = (long)indent_here(P);
    if (inmap && k == m) {
        P->p += k;
        if (is_seq(P)) return block_seq(P, m);
        P->p -= k;
    }
    return node_at(P, m);
}

static uint32_t line_node(parser *P, long n) {
    int c = peek(P, 0);
    uint32_t v;
    if (c == '|' || c == '>') return block(P, n);
    if (c == '[' || c == '{') v = flow(P);
    else if (c == '*') v = plainref(P, false);
    else if (c == '"') v = dquoted(P);
    else if (c == '\'') v = squoted(P);
    else v = plain(P, false);
    eol(P);
    return v;
}

/* ── Flow collections (section 7.3) ──────────────────────── */

/* fs ::= (white | break | comment break)* */
static void fs(parser *P) {
    for (;;) {
        const char *q = P->p;
        if (bol(P, q) && (line_is(P, q, "---") || line_is(P, q, "...")))
            fail(P, q, "a flow collection can't hold a --- or ... line");
        if (q >= P->end) return;
        if (*q == ' ' || *q == '\t' || *q == '\n') P->p++;
        else if (is_brk(P, q)) P->p += 2;
        else if (*q == '#' && (bol(P, q) || q[-1] == ' ' || q[-1] == '\t')) skip_comment(P);
        else return;
    }
}

static uint32_t fnode(parser *P) {
    int c = peek(P, 0);
    if (c == '[' || c == '{') return flow(P);
    if (c == '"') return dquoted(P);
    if (c == '\'') return squoted(P);
    if (c == '*') return plainref(P, true);
    return plain(P, true);
}

static uint32_t flow_seq(parser *P) {
    uint64_t before = cur_full(P);
    const char *at = P->p;
    enter(P, at);
    count(P, at, 1, 1);
    uint32_t S = new_node(P, N_SEQ, at);
    uint32_t lv = 0;
    P->p++;
    fs(P);
    while (peek(P, 0) != ']') {
        if (eof(P)) fail(P, at, "this [ is never closed");
        const char *j = P->p;
        if (P->end - j > 3 && memcmp(j, "-<<", 3) == 0 && (j[3] == ' ' || j[3] == '\t' || is_brk(P, j + 3))) {
            P->p += 3;
            fs(P);
            if (peek(P, 0) == '*') {
                uint32_t src = ref(P, true);
                splice(P, src, j, &lv);
            } else {
                P->p = j;
                uint32_t v = fnode(P);
                if (P->nodes[v].lev > lv) lv = P->nodes[v].lev;
            }
        } else {
            uint32_t v = fnode(P);
            if (P->nodes[v].lev > lv) lv = P->nodes[v].lev;
        }
        fs(P);
        if (peek(P, 0) == ',') { P->p++; fs(P); }
        else if (peek(P, 0) != ']') {
            if (eof(P)) fail(P, at, "this [ is never closed");
            fail(P, P->p, "expected , or ] here");
        }
    }
    P->p++;
    P->nodes[S].link = P->n;
    set_lev(P, S, lv + 1);
    set_full(P, S, cur_full(P) - before);
    leave(P);
    return S;
}

/* A flow pair's key and its :, leaving the cursor after the : */
static uint32_t flow_key(parser *P, bool *quoted) {
    int c = peek(P, 0);
    *quoted = c == '"' || c == '\'';
    uint32_t k;
    if (*quoted) {
        k = c == '"' ? dquoted(P) : squoted(P);
        fs(P);
    } else {
        const char *s = P->p;
        k = plain(P, true);
        if (P->nodes[k].len == 2 && memcmp(P->nodes[k].data, "<<", 2) == 0)
            fail(P, s, "a plain << is the merge indicator; the key is written \"<<\"");
        white(P);
    }
    if (peek(P, 0) != ':') {
        if (!*quoted && (peek(P, 0) == ',' || peek(P, 0) == '}'))
            fail(P, P->p, "a key in a flow mapping needs a :");
        fail(P, P->p, "expected : after the key");
    }
    P->p++;
    int d = peek(P, 0);
    if (!*quoted && !(sep_at(P, P->p) || d == ',' || d == '}'))
        fail(P, P->p, "a : needs a space after it, or , or }");
    return k;
}

static uint32_t flow_map(parser *P) {
    const char *at = P->p;
    mapctx c;
    map_open(P, &c, at);
    P->p++;
    fs(P);
    while (peek(P, 0) != '}') {
        if (eof(P)) fail(P, at, "this { is never closed");
        if (starts_merge(P, true)) {
            const char *mp = P->p;
            if (c.started) fail(P, mp, "<<: comes before the mapping's other keys");
            P->p += 3;
            fs(P);
            uint32_t src = ref(P, true);
            map_merge(P, &c, src, mp);
        } else {
            bool q;
            uint32_t k = flow_key(P, &q);
            uint32_t r = map_key(P, &c, k);
            fs(P);
            int d = peek(P, 0);
            map_value(P, &c, r, (d == ',' || d == '}') ? empty_str(P) : fnode(P));
        }
        fs(P);
        if (peek(P, 0) == ',') { P->p++; fs(P); }
        else if (peek(P, 0) != '}') {
            if (eof(P)) fail(P, at, "this { is never closed");
            fail(P, P->p, "expected , or } here");
        }
    }
    P->p++;
    return map_close(P, &c);
}

static uint32_t flow(parser *P) {
    return peek(P, 0) == '[' ? flow_seq(P) : flow_map(P);
}

/* ── The table (section 8.1) ─────────────────────────────── */

/* A key of the table's own mapping has been read: a key an earlier --- &
 * defined is replaced, and its size comes off full now (section 10). */
static uint32_t entry_key(parser *P, uint32_t k) {
    const node *kn = &P->nodes[k];
    slot *s = ix_find(P, &P->tix, ROOT, kn->data, kn->len, key_hash(P, ROOT, kn->data, kn->len));
    if (!s) return NONE;
    entry *e = &P->entries[s->val];
    if (e->tdoc == P->tdoc)
        fail(P, P->in + kn->pos, "the key \"%.*s\" is repeated in this --- & document",
             (int)(kn->len > 60 ? 60 : kn->len), kn->data);
    P->t_full = P->t_full > e->full ? P->t_full - e->full : 0;
    return s->val;
}

static void entry_set(parser *P, uint32_t k, uint32_t v, uint32_t e) {
    if (P->nodes[v].lev > P->max_levels)
        fail_limit(P, P->in + P->nodes[k].pos, "this entry goes more than %u levels deep", P->max_levels);
    entry en = { k, v, P->tdoc, sat(P->full[k], P->full[v]) };
    if (e != NONE) { P->entries[e] = en; return; }
    P->entries = grow(P, P->entries, &P->capentries, P->nentries + 1, sizeof *P->entries);
    P->entries[P->nentries] = en;
    const node *kn = &P->nodes[k];
    ix_insert(P, &P->tix, ROOT, k, P->nentries++, key_hash(P, ROOT, kn->data, kn->len));
}

static void table_flow(parser *P) {
    const char *at = P->p;
    P->p++;
    fs(P);
    while (peek(P, 0) != '}') {
        if (eof(P)) fail(P, at, "this { is never closed");
        if (P->end - P->p >= 3 && memcmp(P->p, "<<:", 3) == 0)
            fail(P, P->p, "<<: belongs inside an entry, not at the top of the table");
        bool q;
        const char *ks = P->p;
        P->defer = true;
        uint32_t k = flow_key(P, &q);
        uint32_t e = entry_key(P, k);
        P->defer = false;
        check(P, ks);
        fs(P);
        int d = peek(P, 0);
        entry_set(P, k, (d == ',' || d == '}') ? empty_str(P) : fnode(P), e);
        fs(P);
        if (peek(P, 0) == ',') { P->p++; fs(P); }
        else if (peek(P, 0) != '}') {
            if (eof(P)) fail(P, at, "this { is never closed");
            fail(P, P->p, "expected , or } here");
        }
    }
    P->p++;
}

static void table_map(parser *P) {
    for (;;) {
        gap(P);
        int c = peek(P, 0);
        if (eof(P) || marker(P) || dots(P) || c == '%') break;
        if (c == ' ') fail(P, P->p, "the table's entries start at column 0");
        if (P->end - P->p >= 3 && memcmp(P->p, "<<:", 3) == 0)
            fail(P, P->p, "<<: belongs inside an entry, not at the top of the table");
        const char *ks = P->p;
        P->defer = true;
        uint32_t k = key(P);
        colon(P);
        uint32_t e = entry_key(P, k);
        P->defer = false;
        check(P, ks);
        entry_set(P, k, value(P, 0, true), e);
    }
}

/* ── Streams (section 4) ─────────────────────────────────── */

static void reset_table(parser *P) {
    P->n = P->table_end = 0;
    P->npairs = P->pairs_table_end = 0;
    P->nplans = P->plans_table_end = 0;
    P->nentries = 0;
    ix_clear(&P->tix);
    P->t_held = P->t_full = 0;
}

static void discard_doc(parser *P) {
    P->n = P->table_end;
    P->npairs = P->pairs_table_end;
    P->nplans = P->plans_table_end;
    P->nscratch = P->nlix = 0;
    P->d_held = P->d_full = 0;
    P->name = (oylng_str){ NULL, 0 };
}

/* The length of a name character at q, or 0 (sections 4.3, 12) */
static size_t name_char(const parser *P, const char *q) {
    if (q >= P->end) return 0;
    unsigned c = (unsigned char)*q;
    if (c < 0x80)
        return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '_' || c == '/' || c == '-') ? 1 : 0;
    size_t n;
    uint32_t u = utf8((const unsigned char *)q, (const unsigned char *)P->end, &n);
    if (u == BAD_CP) return 0;
    bool ok = u == 0xB7 || (u >= 0xC0 && u <= 0xD6) || (u >= 0xD8 && u <= 0xF6) ||
              (u >= 0xF8 && u <= 0x37D) || (u >= 0x37F && u <= 0x1FFF) ||
              (u >= 0x200C && u <= 0x200D) || (u >= 0x203F && u <= 0x2040) ||
              (u >= 0x2070 && u <= 0x218F) || (u >= 0x2C00 && u <= 0x2FEF) ||
              (u >= 0x3001 && u <= 0xD7FF) || (u >= 0xF900 && u <= 0xFDCF) ||
              (u >= 0xFDF0 && u <= 0xFEFE) || (u >= 0xFF00 && u <= 0xFFFD) ||
              (u >= 0x10000 && u <= 0xEFFFF);
    return ok ? n : 0;
}

/* After a marker: a document, true, or a table, false */
static bool document(parser *P) {
    P->p += 3;
    const char *j = P->p;
    size_t n = spaces(P);
    P->in_table = false;
    if (n && peek(P, 0) == '&') {
        P->in_table = true;
        P->tdoc++;
        P->p++;
        const char *k = P->p;
        if (spaces(P) && peek(P, 0) == '{') {
            table_flow(P);
            eol(P);
            gap(P);
        } else {
            P->p = k;
            if (!sep_at(P, P->p)) fail(P, P->p, "--- & is followed by a space, a tab or the end of the line");
            eol(P);
            gap(P);
            k = P->p;
            spaces(P);
            if (peek(P, 0) == '{') {
                table_flow(P);
                eol(P);
                gap(P);
            } else {
                P->p = k;
                table_map(P);
            }
        }
        P->in_table = false;
        P->table_end = P->n;
        P->pairs_table_end = P->npairs;
        P->plans_table_end = P->nplans;
        return false;
    }
    if (n && peek(P, 0) == '!') {
        const char *b = ++P->p;
        size_t c;
        while ((c = name_char(P, P->p)) > 0) P->p += c;
        if (P->p == b) fail(P, b, "a document name, after !, is letters, digits and . _ - /");
        P->name = (oylng_str){ b, (size_t)(P->p - b) };
        count(P, b, 1 + P->name.len, 1 + P->name.len);
        j = P->p;
        n = spaces(P);
        if (!n && !sep_at(P, P->p)) {
            if (peek(P, 0) == ':' || peek(P, 0) == '%')
                fail(P, P->p, "a document name can't hold : or %%");
            fail(P, P->p, "a document name is letters, digits and . _ - /");
        }
    }
    uint32_t root;
    if (n && peek(P, 0) != '#' && !at_eol(P, P->p)) root = line_node(P, 0);
    else {
        P->p = j;
        eol(P);
        gap(P);
        root = node_at(P, -1);
    }
    if (P->nodes[root].lev > P->max_levels)
        fail_limit(P, P->in + P->nodes[root].pos, "with references followed, this document goes more than %u levels deep",
                   P->max_levels);
    P->root = root;
    return true;
}

static void stream_start(parser *P) {
    if (P->end - P->in >= 3 && memcmp(P->in, "\xEF\xBB\xBF", 3) == 0)
        fail(P, P->in, "a YAML 2.0 stream has no byte order mark");
    if (P->end - P->in < 9 || memcmp(P->in, "%YAML 2.0", 9) != 0) {
        if (P->end - P->in >= 6 && memcmp(P->in, "%YAML ", 6) == 0)
            fail(P, P->in, "this parser reads YAML 2.0 only, and the stream says another version");
        fail(P, P->in, "a YAML 2.0 stream starts with the line %%YAML 2.0");
    }
    P->p += 9;
    if (!sep_at(P, P->p))
        fail(P, P->in, "this parser reads YAML 2.0 only, and the stream says another version");
    eol(P);
    gap(P);
}

/* Reads up to the next document to deliver: true, or false at the end */
static bool next_document(parser *P) {
    for (;;) {
        gap(P);
        if (eof(P)) return false;
        bool ended = dots(P);
        if (ended) {
            P->p += 3;
            eol(P);
            const char *after = P->p;
            for (;;) {
                const char *j = P->p;
                spaces(P);
                if (is_brk(P, P->p)) P->p += brk_len(P->p);
                else { P->p = j; break; }
            }
            if (eof(P)) return false;
            if (!marker(P) && !directive_then_marker(P)) {
                P->stopped = true;
                P->stop_offset = (size_t)(after - P->in);
                return false;
            }
            discard_doc(P);
            reset_table(P);
        }
        if (directive_then_marker(P)) {
            if (!ended) fail(P, P->p, "%%YAML 2.0 comes again only after ...");
            P->p += 9;
            eol(P);
            gap(P);
        }
        if (!marker(P)) {
            if (!P->any_doc) fail(P, P->p, "every document starts with ---, the first one too");
            if (peek(P, 0) == '%') fail(P, P->p, "a directive other than %%YAML 2.0, or one in the wrong place");
            fail(P, P->p, "this line doesn't fit the document above it: check its indent");
        }
        P->any_doc = true;
        discard_doc(P);
        if (document(P)) return true;
    }
}

/* ── Delivering events ───────────────────────────────────── */

static void emit(parser *P, oylng_event_type t, const char *data, size_t len, size_t pos) {
    P->evt.type = t;
    P->evt.value.data = data;
    P->evt.value.len = len;
    P->evt.offset = pos;
}

/* The walk runs without setjmp, so its one failure, the stack not
 * growing, sets the error here and the walk stops on it */
static void push(parser *P, uint8_t mode, uint32_t cur, uint32_t end, uint8_t close) {
    frame *top = P->sp ? &P->stack[P->sp - 1] : NULL;
    if (top && top->close == OYLNG_EVT_NONE && top->mode == W_NODES && top->cur == top->end) {
        *top = (frame){ mode, close, 0, cur, end };   /* a spent splice: reuse it */
        return;
    }
    if (P->sp >= P->capstack) {
        uint64_t nc = P->capstack ? (uint64_t)P->capstack * 2 : 64;
        frame *ns = nc <= UINT32_MAX ? realloc(P->stack, (size_t)nc * sizeof *ns) : NULL;
        if (!ns) {
            P->status = OYLNG_ERR_MEMORY;
            P->errmark = mark_of(P, P->p);
            snprintf(P->errmsg, sizeof P->errmsg, "out of memory");
            P->state = ST_ERROR;
            return;
        }
        P->stack = ns;
        P->capstack = (uint32_t)nc;
    }
    P->stack[P->sp++] = (frame){ mode, close, 0, cur, end };
}

/* Starts node i: true if that gave an event */
static bool handle(parser *P, uint32_t i) {
    i = deref(P, i);
    const node *nd = &P->nodes[i];
    switch (nd->kind) {
    case N_STR:
        emit(P, OYLNG_EVT_SCALAR, nd->data, nd->len, nd->pos);
        return true;
    case N_SEQ:
        emit(P, OYLNG_EVT_SEQ_START, NULL, 0, nd->pos);
        push(P, W_NODES, i + 1, nd->link, OYLNG_EVT_SEQ_END);
        return true;
    case N_MAP:
        emit(P, OYLNG_EVT_MAP_START, NULL, 0, nd->pos);
        if (nd->sub & F_MERGED) {
            const plan *pl = &P->plans[nd->aux];
            push(P, W_PLAN, pl->start, pl->start + pl->count, OYLNG_EVT_MAP_END);
        } else push(P, W_NODES, i + 1, nd->link, OYLNG_EVT_MAP_END);
        return true;
    case N_SPLICE: {
        uint32_t s = nd->link;
        push(P, W_NODES, s + 1, P->nodes[s].link, OYLNG_EVT_NONE);
        return false;
    }
    default:   /* N_MERGE: its pairs are in the plan */
        return false;
    }
}

static void walk(parser *P) {
    if (P->root_pending) {
        P->root_pending = false;
        if (handle(P, P->root)) return;
    }
    for (;;) {
        if (P->state == ST_ERROR) return;
        if (!P->sp) {
            emit(P, OYLNG_EVT_DOC_END, NULL, 0, (size_t)(P->p - P->in));
            P->state = ST_BETWEEN;
            return;
        }
        frame *f = &P->stack[P->sp - 1];
        if (f->mode == W_PLAN && f->want_val) {
            f->want_val = 0;
            uint32_t v = P->pairs[f->cur++].val;
            if (handle(P, v)) return;
            continue;
        }
        if (f->cur == f->end) {
            uint8_t c = f->close;
            P->sp--;
            if (c != OYLNG_EVT_NONE) {
                emit(P, (oylng_event_type)c, NULL, 0, (size_t)(P->p - P->in));
                return;
            }
            continue;
        }
        if (f->mode == W_PLAN) {
            const node *k = &P->nodes[P->pairs[f->cur].key];
            f->want_val = 1;
            emit(P, OYLNG_EVT_SCALAR, k->data, k->len, k->pos);
            return;
        }
        uint32_t i = f->cur;
        f->cur = subtree_end(P, i);
        if (handle(P, i)) return;
    }
}

/* ── Public API ──────────────────────────────────────────── */

oylng_parser *oylng_parser_new(const char *input, size_t len) {
    if (!input && len) return NULL;
    parser *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    P->strings = oylng_arena_new(4096);
    if (!P->strings) { free(P); return NULL; }
    if (!input) input = empty_text;
    P->in = P->p = input;
    P->end = input + len;
    P->limit = OYLNG_DEFAULT_LIMIT;
    P->max_levels = OYLNG_DEFAULT_LEVELS;
    P->state = ST_START;
    return P;
}

void oylng_parser_set_limit(oylng_parser *P, uint64_t limit) {
    if (P) P->limit = limit;
}

void oylng_parser_set_levels(oylng_parser *P, unsigned levels) {
    if (!P) return;
    if (levels < OYLNG_DEFAULT_LEVELS) levels = OYLNG_DEFAULT_LEVELS;
    if (levels > OYLNG_MAX_LEVELS) levels = OYLNG_MAX_LEVELS;
    P->max_levels = levels;
}

oylng_status oylng_parse_next(oylng_parser *P, const oylng_event **evt) {
    if (!P || !evt) return OYLNG_ERR_INPUT;
    *evt = NULL;
    if (P->state == ST_ERROR) return P->status;
    if (P->state == ST_WALK) {
        walk(P);
        if (P->state == ST_ERROR) return P->status;
        *evt = &P->evt;
        return OYLNG_OK;
    }
    if (setjmp(P->jb)) {
        P->state = ST_ERROR;
        return P->status;
    }
    switch (P->state) {
    case ST_START:
        stream_start(P);
        emit(P, OYLNG_EVT_STREAM_START, NULL, 0, 0);
        P->state = ST_BETWEEN;
        break;
    case ST_BETWEEN:
        if (next_document(P)) {
            emit(P, OYLNG_EVT_DOC_START, P->name.data, P->name.len,
                 P->nodes[P->root].pos);
            P->root_pending = true;
            P->sp = 0;
            P->state = ST_WALK;
        } else {
            emit(P, OYLNG_EVT_STREAM_END, NULL, 0, (size_t)(P->p - P->in));
            P->state = ST_DONE;
        }
        break;
    default:
        emit(P, OYLNG_EVT_STREAM_END, NULL, 0, (size_t)(P->p - P->in));
        break;
    }
    *evt = &P->evt;
    return OYLNG_OK;
}

const char *oylng_parser_error(const oylng_parser *P) {
    return P && P->state == ST_ERROR ? P->errmsg : NULL;
}

oylng_mark oylng_parser_error_mark(const oylng_parser *P) {
    if (!P || P->state != ST_ERROR) return (oylng_mark){ 0, 0, 0 };
    return P->errmark;
}

bool oylng_parser_stopped(const oylng_parser *P, size_t *offset) {
    if (!P || P->state != ST_DONE || !P->stopped) return false;
    if (offset) *offset = P->stop_offset;
    return true;
}

void oylng_parser_free(oylng_parser *P) {
    if (!P) return;
    free(P->nodes);
    free(P->full);
    free(P->pairs);
    free(P->plans);
    free(P->scratch);
    free(P->entries);
    free(P->tix.s);
    free(P->lix);
    free(P->stack);
    oylng_arena_free(P->strings);
    free(P);
}

const char *oylng_event_type_str(oylng_event_type t) {
    switch (t) {
    case OYLNG_EVT_STREAM_START: return "STREAM_START";
    case OYLNG_EVT_STREAM_END:   return "STREAM_END";
    case OYLNG_EVT_DOC_START:    return "DOC_START";
    case OYLNG_EVT_DOC_END:      return "DOC_END";
    case OYLNG_EVT_MAP_START:    return "MAP_START";
    case OYLNG_EVT_MAP_END:      return "MAP_END";
    case OYLNG_EVT_SEQ_START:    return "SEQ_START";
    case OYLNG_EVT_SEQ_END:      return "SEQ_END";
    case OYLNG_EVT_SCALAR:       return "SCALAR";
    default:                     return "NONE";
    }
}
