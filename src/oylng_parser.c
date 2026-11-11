/*
 * oylng_parser.c — reads a YAML 2.0 stream
 *
 * The language is spec/yaml-2.0.md at trans/yaml2, Draft 1. Section numbers
 * below are that draft's.
 *
 * The grammar (section 12) is read by a state machine with a stack of
 * frames, one per open collection. It makes events a few dozen at a time,
 * and hands them out one a call. So a document is delivered as it is read,
 * and what the parser holds is what the draft makes it hold:
 *
 * - the table, as an array of nodes in document order, until `...`, so
 *   that references can be followed: a reference is delivered by walking
 *   the nodes it names, and a `-<<` by walking the items;
 * - the keys of each open mapping, to refuse a repeated one (section 7.4);
 * - a mapping with `<<:` lines, built as nodes until it closes, because a
 *   key written after them can replace a merged one in place (section
 *   9.1). It is then delivered through its plan, the order section 9.1
 *   gives its keys, and let go.
 *
 * The table is read by the same machine, building. The two sizes of
 * section 10, and the levels with references followed, are counted as the
 * stream is read, so a stream is refused before anything is built past
 * the limit. A document in error may have delivered events already, but
 * the error comes before its end (section 2).
 *
 * Errors unwind with longjmp to oylng_parse_next. Everything the parser
 * allocates hangs off the parser, so nothing leaks when they do.
 */

#include "oylng_internal.h"
#include "oylng_simd.h"
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

/* An open mapping's pair: its key's text, the text's hash once the mapping
 * has a table, and while building, the key's node and the value's */
typedef struct {
    const char *k;
    uint32_t    klen, hash, key, val;
} spair;

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

/* An open mapping, or (M, lv and before only) an open sequence. A
 * mapping's pairs are in the scratch from s0, in the order they are
 * delivered: for a merged one, its plan as it is built. Past SMALL_MAP
 * pairs it has a hash table of tcap slots in lix from t0. Collections
 * close in the reverse of the order they open, and a mapping takes no pair
 * while one opened inside it is open, so the mapping taking a pair has the
 * scratch's last pairs and lix's last table. */
typedef struct {
    uint32_t M;        /* its node while building, or NONE */
    uint32_t s0;       /* where its pairs start in the scratch */
    uint32_t t0, tcap; /* its table in lix; tcap 0 until it has one */
    uint32_t lv;       /* building: the most levels below it */
    uint64_t before;   /* building: full before it */
    bool     merged, started;
} mapctx;

/* The grammar's frames: a document, the collections open in it, or the
 * table's own mapping, block or flow */
enum { G_DOC, G_BMAP, G_BSEQ, G_FSEQ, G_FMAP, G_TMAP, G_TFLOW };

/* Where a frame is: at an entry (or a document's value), at a mapping's
 * value, or after an entry */
enum { A_ENTRY, A_VALUE, A_AFTER };

typedef struct {
    uint8_t     kind, at_;
    bool        eol_after;    /* opened by line_node, so an eol follows its close */
    bool        build_root;   /* the merged mapping a document's build began at */
    bool        inline_root;  /* G_DOC: its value is on the marker's line */
    bool        has_mk;       /* mapping, delivered: it has kept a key's text, from mk */
    long        m;            /* block: its indent */
    const char *at;           /* where it opens */
    mapctx      c;
    uint32_t    r;            /* mapping: the pair whose value is next */
    uint32_t    k, e;         /* table: the entry's key node and its place */
    oylng_arena_mark mk;      /* the strings before its first kept key */
} gframe;

/* The value a frame was given, when one opened a frame of its own instead */
#define PUSHED (UINT32_MAX - 1)

enum { ST_START, ST_BETWEEN, ST_DOC, ST_DONE, ST_ERROR };

/* How many events are made at a time, and the room past them for the
 * flow fast path's (flow_fast) */
#define QN 64
#define QX 64

struct oylng_parser {
    const char *in, *end, *p;
    jmp_buf     jb;

    uint64_t limit;
    unsigned max_levels;

    int      state;
    bool     in_table;   /* reading a --- & document */
    bool     building;   /* making nodes, not delivering: the table, or a merged mapping */
    bool     in_key;     /* reading a key, whose text is kept until its mapping closes */
    bool     defer;      /* reading an entry's key: check the limit after */
    bool     any_doc;    /* a marker has been read */
    unsigned depth;      /* collections open, as written */

    /* the last look for a block mapping's key, at look_at: whether one
       and its : start there, and, for a plain key of printable ASCII,
       where it ends, so that reading it needn't scan it again */
    const char *look_at, *look_end;
    bool        look_map;

    gframe  *g;          /* the grammar's frames */
    uint32_t ng, capg;

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

    /* the document being delivered, and the walk delivering nodes */
    oylng_str name;
    frame    *stack;
    uint32_t  sp, capstack;

    /* a merged mapping built in a document, let go once delivered: what
       was in use before it */
    bool             release_pending;
    uint32_t         b_n, b_pairs, b_plans;
    oylng_arena_mark b_mark, doc_mark;

    /* events are made QN at a time, and handed out one a call */
    oylng_event q[QN + QX];
    uint32_t    qn, qi;
    bool        qstop;        /* make no more this time */
    bool        rel_pending;  /* strings to let go before the next ones */
    oylng_arena_mark rel_mark;

    oylng_arena *strings;   /* built text that lasts: the table's, keys', a merged mapping's */
    oylng_arena *vals;      /* built text of values being delivered, until they are */

    bool   stopped;
    size_t stop_offset;

    /* making events for a tree: a node of the table is one OYLNG__EVT_REF,
       which the tree copies once and shares, not its events */
    bool   tree_mode;
    /* for each node of the table, the tree it was copied into last
       (high 32 bits) and its node there; kept here so a tree needn't
       clear one of its own */
    uint64_t *tmemo;
    uint32_t  captmemo, tgen;

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

/* With references followed: refuses a node reaching lev levels below the
 * collections open around it */
static void levels_check(parser *P, const char *at, uint32_t lev) {
    if ((uint64_t)P->depth + lev > P->max_levels)
        fail_limit(P, at, "with references followed, this document goes more than %u levels deep",
                   P->max_levels);
}

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
    uint32_t cp = oylng_utf8((const unsigned char *)q, (const unsigned char *)P->end, &n);
    if (cp == OYLNG_BAD_CP) fail(P, q, "invalid UTF-8");
    if (!oylng_allowed(cp, dq))
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
    for (;;) {
        P->p += oylng_ascii_run(P->p, (size_t)(P->end - P->p), '\0', '\0');
        if (at_eol(P, P->p)) return;
        P->p += char_len(P, P->p, false);
    }
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

/* A scalar as read: its value, how it was written, and where */
typedef struct {
    const char *data;
    size_t      len;
    const char *at;
    int         style;
} scal;

static inline size_t pos(const parser *P, const char *at) { return (size_t)(at - P->in); }

static void emit(parser *P, oylng_event_type t, const char *data, size_t len, size_t at) {
    oylng_event *e = &P->q[P->qn++];
    e->type = t;
    e->value.data = data;
    e->value.len = len;
    e->offset = at;
}

/* Lets the strings after m go, once the events made so far, which may
 * point to them, are handed out: the next ones aren't made until then */
static void release_later(parser *P, oylng_arena_mark m) {
    oylng_arena_mark now = oylng_arena_get_mark(P->strings);
    if (now.block == m.block && now.used == m.used) return;
    P->rel_pending = true;
    P->rel_mark = m;
    P->qstop = true;
}

static void emit_str(parser *P, const scal *s) {
    emit(P, OYLNG_EVT_SCALAR, s->data, s->len, pos(P, s->at));
}

/* Counts a scalar (section 10), and makes its node if building */
static uint32_t put_str(parser *P, const scal *s) {
    if (s->len > UINT32_MAX - 1) fail_limit(P, s->at, "a scalar of 4 GiB or more");
    count(P, s->at, 1 + (uint64_t)s->len, 1 + (uint64_t)s->len);
    if (!P->building) return NONE;
    uint32_t i = new_node(P, N_STR, s->at);
    node *nd = &P->nodes[i];
    nd->data = s->data;
    nd->len = (uint32_t)s->len;
    nd->sub = (uint8_t)s->style;
    set_full(P, i, 1 + (uint64_t)s->len);
    return i;
}

/* A value: its node while building; otherwise it is delivered, NONE */
static uint32_t put_value(parser *P, scal s) {
    uint32_t i = put_str(P, &s);
    if (!P->building) emit_str(P, &s);
    return i;
}

static uint32_t put_empty(parser *P) {
    return put_value(P, (scal){ empty_text, 0, P->p, S_EMPTY });
}

/* Room for text built from escapes or lines. A key's lasts until its
 * mapping closes, and what is built lasts as long as its node; a value
 * delivered as read lasts until the next call. */
static char *str_alloc(parser *P, size_t len) {
    oylng_arena *a = P->vals;
    if (P->building) a = P->strings;
    else if (P->in_key) {
        /* the mapping reading it, on top, lets it go when it closes */
        gframe *f = &P->g[P->ng - 1];
        if (!f->has_mk) {
            f->mk = oylng_arena_get_mark(P->strings);
            f->has_mk = true;
        }
        a = P->strings;
    }
    char *s = oylng_arena_alloc(a, len ? len : 1, 1);
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

/* Where a plain scalar from q ends, before any spaces after it, every
 * character checked */
static const char *plain_end(parser *P, const char *q, bool flow) {
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
        q += char_len(P, q, false);
        last = q;
    }
    return last;
}

/* Looking ahead, without refusing anything: where a plain scalar from q
 * ends in a block, as plain_end finds it, and whether every byte of it is
 * printable ASCII, so that it needs no check */
static const char *plain_look(const parser *P, const char *q, bool *ascii) {
    const char *e = P->end, *last = q;
    bool ok = true;
    while (q < e) {
        unsigned char c = (unsigned char)*q;
        if (in_run(c, plain_run_lo, plain_run_hi)) {
            do q++; while (q < e && in_run((unsigned char)*q, plain_run_lo, plain_run_hi));
            last = q;
            continue;
        }
        if (c == '\n' || (c == '\r' && q + 1 < e && q[1] == '\n')) break;
        if (c == '#' && (q[-1] == ' ' || q[-1] == '\t')) break;
        if (c == ':') {
            const char *r = q + 1;
            if (r >= e || *r == ' ' || *r == '\t' || is_brk(P, r)) break;
        }
        if (c == ' ' || c == '\t') { q++; continue; }
        if (c != '#' && c != ':') ok = false;
        last = ++q;
    }
    *ascii = ok;
    return last;
}

static scal plain(parser *P, bool flow) {
    const char *s = P->p;
    const char *why = plain_start_bad(P, s, flow);
    if (why) {
        if (s < P->end && *s == '\r' && !is_brk(P, s)) char_len(P, s, false);
        fail(P, s, "%s", why);
    }
    const char *e = plain_end(P, s, flow);
    P->p = e;
    return (scal){ s, (size_t)(e - s), s, S_PLAIN };
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
static scal dquoted(parser *P) {
    const char *s = P->p, *q = s + 1, *e = P->end;
    bool esc = false;
    for (;;) {
        q += oylng_ascii_run(q, (size_t)(e - q), '"', '\\');
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
    if (!esc) return (scal){ s + 1, (size_t)(q - s - 1), s, S_DOUBLE };

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
    return (scal){ out, o, s, S_DOUBLE };
}

/* A single-quoted scalar: '' is one ' (section 6.3) */
static scal squoted(parser *P) {
    const char *s = P->p, *q = s + 1;
    bool doubled = false;
    for (;;) {
        q += oylng_ascii_run(q, (size_t)(P->end - q), '\'', '\'');
        if (at_eol(P, q)) fail(P, s, "a single-quoted scalar must close on its line");
        if (*q == '\'') {
            if (q + 1 < P->end && q[1] == '\'') { doubled = true; q += 2; continue; }
            break;
        }
        q += char_len(P, q, false);
    }
    P->p = q + 1;
    if (!doubled) return (scal){ s + 1, (size_t)(q - s - 1), s, S_SINGLE };
    char *out = str_alloc(P, (size_t)(q - s - 1));
    size_t o = 0;
    for (const char *r = s + 1; r < q; r++) {
        out[o++] = *r;
        if (*r == '\'') r++;
    }
    return (scal){ out, o, s, S_SINGLE };
}

/* A block scalar under a parent at indent n (section 6.4) */
static scal block(parser *P, long n) {
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
        for (;;) {
            q += oylng_ascii_run(q, (size_t)(e - q), '\0', '\0');
            if (at_eol(P, q)) break;
            q += char_len(P, q, false);
        }
        if (q < e) q += brk_len(q);
        stop = q;
    }
    P->p = stop;
    if (m < 0) return (scal){ empty_text, 0, h, fold ? S_FOLDED : S_LITERAL };

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
        te += oylng_find_break(te, (size_t)(e - te));   /* the lines are checked: a CR is a break's */
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
    return (scal){ out, o, h, fold ? S_FOLDED : S_LITERAL };
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
    uint32_t cnt = merged ? P->plans[nd->aux].count : nd->aux;
    /* a merge of {} has no pairs, and pairs may be NULL */
    const pair *pr = merged && cnt ? P->pairs + P->plans[nd->aux].start : NULL;
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

static bool handle(parser *P, uint32_t i);

/* A reference as a value: its node while building; otherwise the node it
 * names is delivered, NONE */
static uint32_t plainref(parser *P, bool flow) {
    const char *s = P->p;
    uint32_t t = ref(P, flow);
    count(P, s, 1, P->full[t]);
    if (!P->in_table) levels_check(P, s, P->nodes[t].lev);
    if (!P->building) {
        handle(P, t);
        return NONE;
    }
    uint32_t r = new_node(P, N_REF, s);
    P->nodes[r].link = t;
    set_full(P, r, P->full[t]);
    P->nodes[r].lev = P->nodes[t].lev;
    return r;
}

/* ── Mappings, with merges (sections 7.1, 7.4, 9.1) ──────── */

static inline bool same(const char *a, size_t al, const char *b, size_t bl) {
    return al == bl && (al == 0 || (a[0] == b[0] && memcmp(a + 1, b + 1, al - 1) == 0));
}

/* The open mapping's pair whose key is k, as its place from s0, or NONE;
 * h is k's hash if the mapping has a table */
static uint32_t pair_find(const parser *P, const mapctx *c, const char *k, size_t len, uint32_t h) {
    uint32_t cnt = P->nscratch - c->s0;
    if (!cnt) return NONE;   /* before the first pair, scratch may be NULL, and NULL + 0 is undefined */
    const spair *sp = P->scratch + c->s0;
    if (!c->tcap) {
        for (uint32_t i = 0; i < cnt; i++)
            if (same(sp[i].k, sp[i].klen, k, len)) return i;
        return NONE;
    }
    const uint32_t *t = P->lix + c->t0;
    uint32_t mask = c->tcap - 1;
    for (uint32_t j = h & mask;; j = (j + 1) & mask) {
        uint32_t i = t[j];
        if (i == EMPTY) return NONE;
        if (sp[i].hash == h && same(sp[i].k, sp[i].klen, k, len)) return i;
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
        for (uint32_t i = 0; i < cnt; i++) sp[i].hash = text_hash(P, sp[i].k, sp[i].klen);
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
static uint32_t pair_add(parser *P, mapctx *c, const char *k, size_t klen, uint32_t key,
                         uint32_t val, uint32_t h) {
    if (P->nscratch >= P->capscratch)
        P->scratch = grow(P, P->scratch, &P->capscratch, P->nscratch + 1, sizeof *P->scratch);
    uint32_t r = P->nscratch - c->s0;
    P->scratch[P->nscratch++] = (spair){ k, (uint32_t)klen, h, key, val };
    if (r + 1 <= SMALL_MAP) return r;
    if ((uint64_t)(r + 1) * 2 > c->tcap) table_build(P, c, r + 1);
    else table_put(P, c, r);
    return r;
}

static inline uint32_t pair_hash(parser *P, const mapctx *c, const char *k, size_t len) {
    return c->tcap ? text_hash(P, k, len) : 0;
}

/* <<: *src, before any written key. Only a mapping being built has them. */
static void map_merge(parser *P, mapctx *c, uint32_t src, const char *at) {
    if (P->nodes[src].kind != N_MAP) fail(P, at, "<<: names a mapping");
    count(P, at, 1, P->full[src] - 1);
    if (!P->in_table) levels_check(P, at, P->nodes[src].lev - 1u);   /* its values sit in this mapping */
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
        uint32_t h = pair_hash(P, c, kn->data, kn->len);
        uint32_t r = pair_find(P, c, kn->data, kn->len, h);
        if (r != NONE) P->scratch[c->s0 + r].val = v;
        else pair_add(P, c, kn->data, kn->len, k, v, h);
    }
}

/* A written key, with its node k while building: refused if written
 * before. Returns its pair's place from s0. */
static uint32_t map_key(parser *P, mapctx *c, const scal *s, uint32_t k) {
    uint32_t h = pair_hash(P, c, s->data, s->len);
    uint32_t r = pair_find(P, c, s->data, s->len, h);
    c->started = true;
    if (r == NONE) return pair_add(P, c, s->data, s->len, k, NONE, h);
    /* a merged key has its node in the source, before M */
    if (!c->merged || P->scratch[c->s0 + r].key > c->M)
        fail(P, s->at, "the key \"%.*s\" is repeated", (int)(s->len > 60 ? 60 : s->len), s->data);
    P->scratch[c->s0 + r].key = k;
    return r;
}

static void map_value(parser *P, mapctx *c, uint32_t r, uint32_t v) {
    P->scratch[c->s0 + r].val = v;
    if (!c->merged && P->nodes[v].lev > c->lv) c->lv = P->nodes[v].lev;
}

/* Closes a mapping being built: its plan if merged, its keys in the
 * table's index if it is the table's and big */
static uint32_t map_close(parser *P, mapctx *c) {
    uint32_t lv = c->lv, cnt = P->nscratch - c->s0;
    const spair *sp = cnt ? P->scratch + c->s0 : NULL;   /* {}: scratch may be NULL */
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
        for (uint32_t i = 0; i < cnt; i++)
            ix_insert(P, &P->tix, c->M, sp[i].key, c->merged ? i : sp[i].val,
                      key_hash(P, c->M, sp[i].k, sp[i].klen));
    P->nscratch = c->s0;
    P->nlix = c->t0;
    P->nodes[c->M].link = P->n;
    set_lev(P, c->M, lv + 1);
    set_full(P, c->M, cur_full(P) - c->before);
    return c->M;
}

static void push(parser *P, uint8_t mode, uint32_t cur, uint32_t end, uint8_t close);

/* -<< *src in a sequence: a node while building; otherwise its items are
 * delivered */
static void splice(parser *P, mapctx *c, uint32_t src, const char *at) {
    if (P->nodes[src].kind != N_SEQ) fail(P, at, "-<< names a sequence");
    count(P, at, 1, P->full[src] - 1);
    uint32_t il = P->nodes[src].lev - 1u;   /* its items' levels; a sequence has at least 1 */
    if (!P->in_table) levels_check(P, at, il);
    if (!P->building) {
        push(P, W_NODES, src + 1, P->nodes[src].link, OYLNG_EVT_NONE);
        return;
    }
    uint32_t s = new_node(P, N_SPLICE, at);
    P->nodes[s].link = src;
    if (il > c->lv) c->lv = il;
}

static bool starts_merge(const parser *P, bool flow) {
    const char *q = P->p;
    if (P->end - q < 3 || memcmp(q, "<<:", 3) != 0) return false;
    q += 3;
    return q >= P->end || *q == ' ' || is_brk(P, q) || (flow && *q == '\t');
}

/* ── Block nodes (sections 7.1, 7.2) ─────────────────────── */

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
    bool ascii;
    const char *r = plain_look(P, q, &ascii);
    if (r - q == 2 && q[0] == '<' && q[1] == '<') return NULL;
    if (ascii) P->look_end = r;
    return r;
}

/* Whether a block mapping starts at the cursor: a <<: or a key and its :.
 * What it finds is kept for the key() that follows. */
static bool is_map(parser *P) {
    const char *q = P->p, *e = P->end;
    if (q == P->look_at) return P->look_map;
    P->look_at = q;
    P->look_end = NULL;
    bool m;
    if (starts_merge(P, false)) m = true;
    else {
        const char *r = key_end(P, q);
        if (r) while (r < e && *r == ' ') r++;
        m = r && r < e && *r == ':' && sep_at(P, r + 1);
    }
    P->look_map = m;
    return m;
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

static void colon(parser *P) {
    spaces(P);
    if (peek(P, 0) != ':') fail(P, P->p, "expected : after the key");
    P->p++;
}

/* fs ::= (white | break | comment break)* */
static void flow_line(parser *P, const char *q) {
    if (line_is(P, q, "---") || line_is(P, q, "..."))
        fail(P, q, "a flow collection can't hold a --- or ... line");
}

static void fs(parser *P) {
    const char *q = P->p, *e = P->end;
    if (bol(P, q)) flow_line(P, q);
    for (;;) {
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (q >= e) break;
        if (*q == '\n' || (*q == '\r' && q + 1 < e && q[1] == '\n')) {
            q += *q == '\r' ? 2 : 1;
            flow_line(P, q);
        } else if (*q == '#' && (bol(P, q) || q[-1] == ' ' || q[-1] == '\t')) {
            P->p = q;
            skip_comment(P);
            q = P->p;
        } else break;
    }
    P->p = q;
}

static void entry_set(parser *P, uint32_t k, uint32_t v, uint32_t e);

/* A key, counted, with its node while building */
static scal key(parser *P, uint32_t *node) {
    int c = peek(P, 0);
    P->in_key = true;
    scal s;
    if (P->p == P->look_at && P->look_end) {   /* found by is_map, needing no check */
        s = (scal){ P->p, (size_t)(P->look_end - P->p), P->p, S_PLAIN };
        P->p = P->look_end;
    } else s = c == '"' ? dquoted(P) : c == '\'' ? squoted(P) : plain(P, false);
    P->in_key = false;
    *node = put_str(P, &s);
    if (s.style == S_PLAIN && s.len == 2 && memcmp(s.data, "<<", 2) == 0)
        fail(P, s.at, "a plain << is the merge indicator; the key is written \"<<\"");
    return s;
}

/* ── Frames ──────────────────────────────────────────────── */

static gframe *gpush(parser *P, int kind, long m, const char *at, bool eol_after) {
    if (P->ng >= P->capg) P->g = grow(P, P->g, &P->capg, P->ng + 1, sizeof *P->g);
    gframe *f = &P->g[P->ng++];
    f->kind = (uint8_t)kind;
    f->at_ = A_ENTRY;
    f->eol_after = eol_after;
    f->build_root = f->inline_root = f->has_mk = false;
    f->m = m;
    f->at = at;
    f->c.tcap = f->c.lv = 0;
    f->c.merged = f->c.started = false;
    return f;
}

static inline gframe *gtop(parser *P) { return &P->g[P->ng - 1]; }

/* A value has been read for frame f, the one on top: its node while
 * building */
static void value_done(parser *P, gframe *f, uint32_t v) {
    switch (f->kind) {
    case G_BMAP: case G_FMAP:
        if (P->building) map_value(P, &f->c, f->r, v);
        break;
    case G_BSEQ: case G_FSEQ:
        if (P->building && P->nodes[v].lev > f->c.lv) f->c.lv = P->nodes[v].lev;
        break;
    case G_TMAP: case G_TFLOW:
        entry_set(P, f->k, v, f->e);
        break;
    default:
        break;
    }
}

/* A sequence at P->p: [ for a flow one */
static void open_seq(parser *P, int kind, long m, bool eol_after) {
    const char *at = P->p;
    uint64_t before = cur_full(P);
    enter(P, at);
    count(P, at, 1, 1);
    gframe *f = gpush(P, kind, m, at, eol_after);
    f->c.before = before;
    if (P->building) f->c.M = new_node(P, N_SEQ, at);
    else {
        f->c.M = NONE;
        emit(P, OYLNG_EVT_SEQ_START, NULL, 0, pos(P, at));
    }
    if (kind == G_FSEQ) {
        P->p++;
        fs(P);
    }
}

/* A mapping at P->p: { for a flow one. A mapping with <<: lines is built
 * until it closes, as a key written after them can still replace a merged
 * one (section 9.1); one that isn't is delivered as it is read. */
static void open_map(parser *P, int kind, long m, bool eol_after) {
    const char *at = P->p;
    uint64_t before = cur_full(P);
    enter(P, at);
    count(P, at, 1, 1);
    if (kind == G_FMAP) {
        P->p++;
        fs(P);
    }
    gframe *f = gpush(P, kind, m, at, eol_after);
    f->c.before = before;
    f->c.s0 = P->nscratch;
    f->c.t0 = P->nlix;
    if (!P->building && starts_merge(P, kind == G_FMAP)) {
        f->build_root = true;
        P->building = true;
        P->b_n = P->n;
        P->b_pairs = P->npairs;
        P->b_plans = P->nplans;
        P->b_mark = oylng_arena_get_mark(P->strings);
    }
    if (P->building) f->c.M = new_node(P, N_MAP, at);
    else {
        f->c.M = NONE;
        emit(P, OYLNG_EVT_MAP_START, NULL, 0, pos(P, at));
    }
}

/* Closes the collection on top. A merged mapping a build began at is then
 * delivered from its nodes, which go once it has been. */
static void close_coll(parser *P) {
    gframe *f = gtop(P);
    bool map = f->kind == G_BMAP || f->kind == G_FMAP;
    bool eol_after = f->eol_after, root = f->build_root;
    uint32_t v = NONE;
    if (P->building) {
        if (map) v = map_close(P, &f->c);
        else {
            v = f->c.M;
            P->nodes[v].link = P->n;
            set_lev(P, v, f->c.lv + 1);
            set_full(P, v, cur_full(P) - f->c.before);
        }
    } else {
        if (map) {
            P->nscratch = f->c.s0;
            P->nlix = f->c.t0;
            if (f->has_mk) release_later(P, f->mk);
        }
        emit(P, map ? OYLNG_EVT_MAP_END : OYLNG_EVT_SEQ_END, NULL, 0, pos(P, P->p));
    }
    leave(P);
    P->ng--;
    if (root) {
        P->building = false;
        P->release_pending = true;
        handle(P, v);
        v = NONE;
    }
    if (eol_after) eol(P);
    value_done(P, gtop(P), v);
}

/* ── Values ──────────────────────────────────────────────────
 * Each gives the value's node while building, NONE once it is delivered,
 * or PUSHED when a collection opened a frame: value_done comes when it
 * closes. */

static uint32_t line_node(parser *P, long n);

/* node(n) ::= map(m) | seq(m) | indent(m) line-node(max(n, 0)), m > n */
static uint32_t node_at(parser *P, long n) {
    gap(P);
    if (eof(P) || marker(P) || dots(P)) return put_empty(P);
    long m = (long)indent_here(P);
    if (m <= n) return put_empty(P);
    P->p += m;
    if (is_seq(P)) { open_seq(P, G_BSEQ, m, false); return PUSHED; }
    if (is_map(P)) { open_map(P, G_BMAP, m, false); return PUSHED; }
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
    if (eof(P) || marker(P) || dots(P)) return put_empty(P);
    long k = (long)indent_here(P);
    if (inmap && k == m) {
        P->p += k;
        if (is_seq(P)) { open_seq(P, G_BSEQ, m, false); return PUSHED; }
        P->p -= k;
    }
    return node_at(P, m);
}

/* ── The flow fast path ──────────────────────────────────────
 * A flow collection written on one line, of plain scalars of letters,
 * digits and . _ + - and of double-quoted ones of printable ASCII and no
 * escapes, as JSON mostly is, is read in one pass that makes its events
 * straight into the queue, with no frames. Anything else, anything the
 * general path would refuse (a repeated key, the limits) and a collection
 * with more events than the queue has room for are left to it: nothing is
 * kept, and it reads the collection from the start. */

#if defined(OYLNG_NO_FAST)   /* the tests' build of the general path alone */
#  define FAST_PATHS 0
#else
#  define FAST_PATHS 1
#endif

static inline bool ff_alnum(unsigned c) {
    return (unsigned)((c | 0x20) - 'a') < 26 || c - '0' < 10;
}

static inline bool ff_plain(unsigned c) {
    return ff_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-';
}

static inline void ff_emit(oylng_event *e, oylng_event_type t, const char *data, size_t len, size_t at) {
    e->type = t;
    e->value.data = data;
    e->value.len = len;
    e->offset = at;
}

/* At a [ or a {: true once the collection is read and its events made */
static bool flow_fast(parser *P) {
    if (!FAST_PATHS || P->building || P->defer) return false;
    enum { ITEM, COLON, VALUE, SEP };
    struct { const char *k; size_t len; } keys[QN + QX];
    bool map[QN + QX];
    uint32_t k0[QN + QX];
    const char *q = P->p, *e = P->end;
    oylng_event *out = &P->q[P->qn];
    uint32_t room = QN + QX - P->qn, n = 0, nk = 0, depth = 0;
    uint64_t add = 0;
    int st = ITEM;
    for (;;) {
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (q >= e || n == room) return false;
        unsigned c = (unsigned char)*q;
        if (st == COLON) {
            if (c != ':') return false;
            q++;
            st = VALUE;
            continue;
        }
        if (st == SEP && c == ',') {
            q++;
            st = ITEM;
            continue;
        }
        if (c == '}' || c == ']') {
            if (st == VALUE || (c == '}') != map[depth - 1]) return false;
            q++;
            ff_emit(&out[n++], c == '}' ? OYLNG_EVT_MAP_END : OYLNG_EVT_SEQ_END, NULL, 0, pos(P, q));
            nk = k0[--depth];
            if (!depth) break;
            st = SEP;
            continue;
        }
        if (st == SEP) return false;
        bool is_key = depth && st == ITEM && map[depth - 1];
        if (c == '{' || c == '[') {
            if (is_key || P->depth + depth + 1 > P->max_levels) return false;
            map[depth] = c == '{';
            k0[depth++] = nk;
            ff_emit(&out[n++], c == '{' ? OYLNG_EVT_MAP_START : OYLNG_EVT_SEQ_START, NULL, 0, pos(P, q));
            add++;
            q++;
            st = ITEM;
            continue;
        }
        const char *a = q, *d, *z;   /* the scalar at a: its text from d to z */
        if (c == '"') {
            d = q + 1;
            z = d + oylng_ascii_run(d, (size_t)(e - d), '"', '\\');
            if (z >= e || *z != '"') return false;
            q = z + 1;
        } else {
            if (!(ff_alnum(c) || c == '.' || c == '_' ||
                  ((c == '-' || c == '+') && q + 1 < e && ff_plain((unsigned char)q[1]))))
                return false;
            d = q;
            z = q + 1;
            while (z < e && ff_plain((unsigned char)*z)) z++;
            q = z;
            while (q < e && (*q == ' ' || *q == '\t')) q++;
            if (q >= e) return false;
            if (*q == ':') {
                if (!is_key || q + 1 >= e || (q[1] != ' ' && q[1] != '\t')) return false;
            } else if (*q != ',' && *q != ']' && *q != '}') return false;
        }
        size_t len = (size_t)(z - d);
        if (is_key) {
            for (uint32_t i = k0[depth - 1]; i < nk; i++)
                if (same(keys[i].k, keys[i].len, d, len)) return false;
            keys[nk].k = d;
            keys[nk++].len = len;
        }
        ff_emit(&out[n++], OYLNG_EVT_SCALAR, d, len, pos(P, a));
        add = sat(add, 1 + (uint64_t)len);
        st = is_key ? COLON : SEP;
    }
    if (sat(sat(P->t_full, P->d_full), add) > P->limit || sat(sat(P->t_held, P->d_held), add) > P->limit)
        return false;
    P->d_held = sat(P->d_held, add);
    P->d_full = sat(P->d_full, add);
    P->qn += n;
    P->p = q;
    return true;
}

static uint32_t line_node(parser *P, long n) {
    int c = peek(P, 0);
    uint32_t v;
    if (c == '|' || c == '>') return put_value(P, block(P, n));
    if ((c == '[' || c == '{') && flow_fast(P)) { eol(P); return NONE; }
    if (c == '[') { open_seq(P, G_FSEQ, 0, true); return PUSHED; }
    if (c == '{') { open_map(P, G_FMAP, 0, true); return PUSHED; }
    if (c == '*') v = plainref(P, false);
    else if (c == '"') v = put_value(P, dquoted(P));
    else if (c == '\'') v = put_value(P, squoted(P));
    else v = put_value(P, plain(P, false));
    eol(P);
    return v;
}

/* ── Flow collections (section 7.3) ──────────────────────── */

static uint32_t fnode(parser *P) {
    int c = peek(P, 0);
    if ((c == '[' || c == '{') && flow_fast(P)) return NONE;
    if (c == '[') { open_seq(P, G_FSEQ, 0, false); return PUSHED; }
    if (c == '{') { open_map(P, G_FMAP, 0, false); return PUSHED; }
    if (c == '"') return put_value(P, dquoted(P));
    if (c == '\'') return put_value(P, squoted(P));
    if (c == '*') return plainref(P, true);
    return put_value(P, plain(P, true));
}

/* A flow pair's key and its :, leaving the cursor after the : */
static scal flow_key(parser *P, uint32_t *node) {
    int c = peek(P, 0);
    bool quoted = c == '"' || c == '\'';
    P->in_key = true;
    scal s = c == '"' ? dquoted(P) : c == '\'' ? squoted(P) : plain(P, true);
    P->in_key = false;
    *node = put_str(P, &s);
    if (quoted) fs(P);
    else {
        if (s.len == 2 && memcmp(s.data, "<<", 2) == 0)
            fail(P, s.at, "a plain << is the merge indicator; the key is written \"<<\"");
        white(P);
    }
    if (peek(P, 0) != ':') {
        if (!quoted && (peek(P, 0) == ',' || peek(P, 0) == '}'))
            fail(P, P->p, "a key in a flow mapping needs a :");
        fail(P, P->p, "expected : after the key");
    }
    P->p++;
    int d = peek(P, 0);
    if (!quoted && !(sep_at(P, P->p) || d == ',' || d == '}'))
        fail(P, P->p, "a : needs a space after it, or , or }");
    return s;
}

/* After a flow entry: a , or the close */
static void flow_next(parser *P, const gframe *f, int close) {
    fs(P);
    if (peek(P, 0) == ',') { P->p++; fs(P); }
    else if (peek(P, 0) != close) {
        if (eof(P)) fail(P, f->at, close == ']' ? "this [ is never closed" : "this { is never closed");
        fail(P, P->p, close == ']' ? "expected , or ] here" : "expected , or } here");
    }
}

/* ── Steps ───────────────────────────────────────────────────
 * Each moves the frame on top on, until it opens or closes a frame, a
 * walk has nodes to deliver, or there is no room for more events: each
 * pass makes at most one. A frame that starts a value moves itself on
 * first, as the value may open a frame above it. */

static inline bool go_on(const parser *P) { return P->qn < QN && !P->sp; }

/* ── The block fast path ─────────────────────────────────────
 * Lines of a block mapping that are a plain key of letters, digits and
 * . _ + -, ": ", and a value of printable ASCII to the end of the line,
 * plain or quoted with no escapes, and then perhaps a comment, are read
 * with no frames or look ahead. Their keys and values are counted, the
 * keys checked and their events made by the functions the general path
 * uses, in its order. A line that doesn't fit is left to it, from its
 * start. */

/* Where a key the fast path takes starts at q: not ---, ... or %YAML at a
 * line's start, nor -<< */
static inline bool fb_key_start(const char *q, const char *e) {
    unsigned c = (unsigned char)*q;
    if (c == '-' || c == '.' || c == '+') return q + 1 < e && ff_alnum((unsigned char)q[1]);
    return ff_alnum(c) || c == '_';
}

/* A plain value can start at q, in a line after its start, as
 * plain_start_bad sees it */
static inline bool fb_value_start(const char *q, const char *e) {
    unsigned c = (unsigned char)*q;
    if (c == '-' || c == '?' || c == ':') return q + 1 < e && ff_alnum((unsigned char)q[1]);
    return c > ' ' && c < 0x7F && !is_indicator((int)c) && c != '!';
}

/* The end of a line at q, after a value and any spaces: past its break,
 * or NULL if there's anything else there but a comment of printable
 * ASCII and tabs */
static inline const char *fb_eol(const char *q, const char *e) {
    if (q < e && *q == '#' && q[-1] == ' ') {
        q++;
        q += oylng_ascii_run(q, (size_t)(e - q), '\0', '\0');
    }
    if (q < e && *q == '\n') return q + 1;
    if (q + 1 < e && *q == '\r' && q[1] == '\n') return q + 2;
    return NULL;
}

/* A one-line value at q the fast paths take, plain or quoted with no
 * escapes, then perhaps a comment: true, with the value in *vs and the
 * next line's start in *nl */
static bool fb_value(const char *q, const char *e, scal *vs, const char **nl) {
    if (q >= e) return false;
    const char *v = q, *vz, *d = q;
    int style = S_PLAIN;
    if (*q == '"' || *q == '\'') {
        char t = *q++;
        d = q;
        q += oylng_ascii_run(q, (size_t)(e - q), t, t == '"' ? '\\' : t);
        if (q >= e || *q != t || (t == '\'' && q + 1 < e && q[1] == '\'')) return false;
        vz = q++;
        style = t == '"' ? S_DOUBLE : S_SINGLE;
    } else {
        if (!fb_value_start(q, e)) return false;
        vz = ++q;
        for (;;) {
            while (q < e && in_run((unsigned char)*q, plain_run_lo, plain_run_hi)) q++;
            if (q > vz && q[-1] != ' ') vz = q;
            if (q >= e) break;
            if (*q == ' ') { q++; continue; }
            if (*q == ':' && q + 1 < e && in_run((unsigned char)q[1], plain_run_lo, plain_run_hi)) {
                vz = ++q;
                continue;
            }
            if (*q == '#' && q[-1] != ' ') { vz = ++q; continue; }
            break;
        }
    }
    q = vz + (style != S_PLAIN);
    while (q < e && *q == ' ') q++;
    if (!(*nl = fb_eol(q, e))) return false;   /* a # right after a quote isn't a comment's */
    *vs = (scal){ d, (size_t)(vz - d), v, style };
    return true;
}

/* A key the fast path read, its : at cz: counted, checked and delivered */
static void fb_key(parser *P, gframe *f, const scal *ks, const char *cz) {
    P->p = cz;
    put_str(P, ks);
    f->r = map_key(P, &f->c, ks, NONE);
    emit_str(P, ks);
}

enum { FB_NONE, FB_LINES, FB_OPENED };

/* At a block mapping's key, after its indent: reads lines while they fit,
 * and the next is at the mapping's indent, leaving the mapping after the
 * last: FB_LINES if it read any. A key with a block collection on the
 * next line opens it, FB_OPENED. */
static int block_fast(parser *P, gframe *f) {
    if (!FAST_PATHS || P->building || P->defer) return FB_NONE;
    const char *q = P->p, *e = P->end;
    int r = FB_NONE;
    for (;;) {
        const char *k = q;
        if (q >= e || !fb_key_start(q, e)) break;
        q++;
        while (q < e && ff_plain((unsigned char)*q)) q++;
        const char *kz = q;
        while (q < e && *q == ' ') q++;
        if (q >= e || *q != ':') break;
        const char *cz = ++q;
        while (q < e && *q == ' ') q++;
        scal ks = { k, (size_t)(kz - k), k, S_PLAIN }, vs;
        const char *nl = fb_eol(q, e);
        if (nl) {
            /* the line ends at the key: a collection on the next line, a
               sequence perhaps at the mapping's indent (value, node_at) */
            const char *c = nl, *p0 = P->p;
            while (c < e && *c == ' ') c++;
            long n = (long)(c - nl);
            P->p = c;
            int kind = n >= f->m && is_seq(P) ? G_BSEQ : n > f->m && is_map(P) ? G_BMAP : -1;
            P->p = p0;
            if (kind < 0) break;
            fb_key(P, f, &ks, cz);
            f->at_ = A_AFTER;
            P->p = c;
            if (kind == G_BSEQ) open_seq(P, G_BSEQ, n, false);
            else open_map(P, G_BMAP, n, false);
            return FB_OPENED;
        }
        if (q == cz || !fb_value(q, e, &vs, &nl)) break;
        fb_key(P, f, &ks, cz);
        put_value(P, vs);
        P->p = q = nl;
        f->at_ = A_AFTER;
        r = FB_LINES;
        if (P->qn >= QN) break;
        while (q < e && *q == ' ') q++;
        if (q - nl != f->m) break;
    }
    return r;
}

static void bmap_step(parser *P, gframe *f) {
    do {
        if (f->at_ == A_AFTER) {
            gap(P);
            f->at_ = A_ENTRY;
        } else if (f->at_ == A_VALUE) {
            long m = f->m;
            f->at_ = A_AFTER;
            uint32_t v = value(P, m, true);
            if (v == PUSHED) return;
            value_done(P, f, v);
        } else {
            const char *j = P->p;
            if (!at_entry(P, f->m)) {
                P->p = j;
                close_coll(P);
                return;
            }
            int fb = block_fast(P, f);
            if (fb == FB_OPENED) return;
            if (fb) continue;
            if (starts_merge(P, false)) {
                const char *at = P->p;
                if (f->c.started) fail(P, at, "<<: comes before the mapping's other keys");
                P->p += 3;
                if (!spaces(P)) fail(P, P->p, "<<: needs a reference after a space");
                uint32_t src = ref(P, false);
                eol(P);
                map_merge(P, &f->c, src, at);
                f->at_ = A_AFTER;
                continue;
            }
            if (!is_map(P)) {
                if (is_seq(P)) fail(P, P->p, "a sequence item where this mapping expects a key");
                fail(P, P->p, "expected a key and :, at this mapping's indent");
            }
            uint32_t k;
            scal s = key(P, &k);
            colon(P);
            f->r = map_key(P, &f->c, &s, k);
            if (!P->building) emit_str(P, &s);
            f->at_ = A_VALUE;
        }
    } while (go_on(P));
}

static void bseq_step(parser *P, gframe *f) {
    do {
        if (f->at_ == A_AFTER) {
            gap(P);
            f->at_ = A_ENTRY;
            continue;
        }
        const char *j = P->p;
        if (!at_entry(P, f->m) || !is_seq(P)) {
            P->p = j;
            close_coll(P);
            return;
        }
        f->at_ = A_AFTER;
        if (peek(P, 1) == '<') {
            const char *sp = P->p;
            P->p += 3;
            spaces(P);
            uint32_t src = ref(P, false);
            eol(P);
            splice(P, &f->c, src, sp);
            continue;
        }
        long m = f->m;
        P->p++;
        const char *k0 = P->p;
        size_t k = spaces(P);
        uint32_t v;
        if (k && peek(P, 0) != '#' && !at_eol(P, P->p)) {
            long c = m + 1 + (long)k;   /* at_entry left the - at column m */
            scal vs;
            const char *nl;
            if (FAST_PATHS && !P->building && !P->defer && fb_value(P->p, P->end, &vs, &nl)) {
                put_value(P, vs);   /* a value to the line's end: no key or - in it */
                P->p = nl;
                continue;
            }
            if (is_seq(P)) { open_seq(P, G_BSEQ, c, false); return; }
            if (is_map(P)) { open_map(P, G_BMAP, c, false); return; }
            v = line_node(P, m);
        } else {
            P->p = k0;
            eol(P);
            gap(P);
            v = node_at(P, m);
        }
        if (v == PUSHED) return;
        value_done(P, f, v);
    } while (go_on(P));
}

static void fseq_step(parser *P, gframe *f) {
    do {
        if (f->at_ == A_AFTER) {
            flow_next(P, f, ']');
            f->at_ = A_ENTRY;
            continue;
        }
        if (peek(P, 0) == ']') {
            P->p++;
            close_coll(P);
            return;
        }
        if (eof(P)) fail(P, f->at, "this [ is never closed");
        const char *j = P->p;
        f->at_ = A_AFTER;
        if (P->end - j > 3 && memcmp(j, "-<<", 3) == 0 && (j[3] == ' ' || j[3] == '\t' || is_brk(P, j + 3))) {
            P->p += 3;
            fs(P);
            if (peek(P, 0) == '*') {
                uint32_t src = ref(P, true);
                splice(P, &f->c, src, j);
                continue;
            }
            P->p = j;
        }
        uint32_t v = fnode(P);
        if (v == PUSHED) return;
        value_done(P, f, v);
    } while (go_on(P));
}

static void fmap_step(parser *P, gframe *f) {
    do {
        if (f->at_ == A_AFTER) {
            flow_next(P, f, '}');
            f->at_ = A_ENTRY;
            continue;
        }
        if (f->at_ == A_VALUE) {
            int d = peek(P, 0);
            f->at_ = A_AFTER;
            uint32_t v = (d == ',' || d == '}') ? put_empty(P) : fnode(P);
            if (v == PUSHED) return;
            value_done(P, f, v);
            continue;
        }
        if (peek(P, 0) == '}') {
            P->p++;
            close_coll(P);
            return;
        }
        if (eof(P)) fail(P, f->at, "this { is never closed");
        if (starts_merge(P, true)) {
            const char *mp = P->p;
            if (f->c.started) fail(P, mp, "<<: comes before the mapping's other keys");
            P->p += 3;
            fs(P);
            uint32_t src = ref(P, true);
            map_merge(P, &f->c, src, mp);
            f->at_ = A_AFTER;
            continue;
        }
        uint32_t k;
        scal s = flow_key(P, &k);
        f->r = map_key(P, &f->c, &s, k);
        fs(P);
        if (!P->building) emit_str(P, &s);
        f->at_ = A_VALUE;
    } while (go_on(P));
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


static void tmap_step(parser *P, gframe *f) {
    if (f->at_ == A_VALUE) {
        f->at_ = A_ENTRY;
        uint32_t v = value(P, 0, true);
        if (v != PUSHED) value_done(P, f, v);
        return;
    }
    gap(P);
    int c = peek(P, 0);
    if (eof(P) || marker(P) || dots(P) || c == '%') {
        P->ng--;
        return;
    }
    if (c == ' ') fail(P, P->p, "the table's entries start at column 0");
    if (P->end - P->p >= 3 && memcmp(P->p, "<<:", 3) == 0)
        fail(P, P->p, "<<: belongs inside an entry, not at the top of the table");
    const char *ks = P->p;
    P->defer = true;
    key(P, &f->k);
    colon(P);
    f->e = entry_key(P, f->k);
    P->defer = false;
    check(P, ks);
    f->at_ = A_VALUE;
}

static void tflow_step(parser *P, gframe *f) {
    if (f->at_ == A_AFTER) {
        flow_next(P, f, '}');
        f->at_ = A_ENTRY;
        return;
    }
    if (f->at_ == A_VALUE) {
        int d = peek(P, 0);
        f->at_ = A_AFTER;
        uint32_t v = (d == ',' || d == '}') ? put_empty(P) : fnode(P);
        if (v != PUSHED) value_done(P, f, v);
        return;
    }
    if (peek(P, 0) == '}') {
        P->p++;
        P->ng--;
        eol(P);
        gap(P);
        return;
    }
    if (eof(P)) fail(P, f->at, "this { is never closed");
    if (P->end - P->p >= 3 && memcmp(P->p, "<<:", 3) == 0)
        fail(P, P->p, "<<: belongs inside an entry, not at the top of the table");
    const char *ks = P->p;
    P->defer = true;
    flow_key(P, &f->k);
    f->e = entry_key(P, f->k);
    P->defer = false;
    check(P, ks);
    fs(P);
    f->at_ = A_VALUE;
}

/* ── Streams (section 4) ─────────────────────────────────── */

static void reset_table(parser *P) {
    P->n = P->table_end = 0;
    P->npairs = P->pairs_table_end = 0;
    P->nplans = P->plans_table_end = 0;
    P->nentries = 0;
    ix_clear(&P->tix);
    P->t_held = P->t_full = 0;
    oylng_arena_reset(P->strings);
}

static void discard_doc(parser *P) {
    P->n = P->table_end;
    P->npairs = P->pairs_table_end;
    P->nplans = P->plans_table_end;
    P->nscratch = P->nlix = 0;
    P->d_held = P->d_full = 0;
    P->name = (oylng_str){ NULL, 0 };
}

static void doc_step(parser *P, gframe *f) {
    if (f->at_ == A_ENTRY) {
        bool inl = f->inline_root;
        f->at_ = A_AFTER;
        uint32_t v = inl ? line_node(P, 0) : node_at(P, -1);
        if (v != PUSHED) value_done(P, f, v);
        return;
    }
    emit(P, OYLNG_EVT_DOC_END, NULL, 0, pos(P, P->p));
    P->ng--;
    release_later(P, P->doc_mark);
    P->qstop = true;   /* a ... next lets the table's strings go */
    discard_doc(P);
    P->state = ST_BETWEEN;
}

/* Moves the grammar on: until an event, while delivering */
static void advance(parser *P) {
    gframe *f = gtop(P);
    switch (f->kind) {
    case G_BMAP:  bmap_step(P, f); break;
    case G_BSEQ:  bseq_step(P, f); break;
    case G_FSEQ:  fseq_step(P, f); break;
    case G_FMAP:  fmap_step(P, f); break;
    case G_TMAP:  tmap_step(P, f); break;
    case G_TFLOW: tflow_step(P, f); break;
    default:      doc_step(P, f); break;
    }
}

/* The length of a name character at q, or 0 (sections 4.3, 12) */
static size_t name_char(const parser *P, const char *q) {
    if (q >= P->end) return 0;
    unsigned c = (unsigned char)*q;
    if (c < 0x80) return oylng_name_cp(c) ? 1 : 0;
    size_t n;
    uint32_t u = oylng_utf8((const unsigned char *)q, (const unsigned char *)P->end, &n);
    return u != OYLNG_BAD_CP && oylng_name_cp(u) ? n : 0;
}

/* After a marker: reads a table whole, false, or starts a document,
 * delivering its start, true */
static bool document(parser *P) {
    P->p += 3;
    const char *j = P->p;
    size_t n = spaces(P);
    P->in_table = false;
    if (n && peek(P, 0) == '&') {
        P->in_table = P->building = true;
        P->tdoc++;
        P->p++;
        const char *k = P->p;
        bool flow = spaces(P) && peek(P, 0) == '{';
        if (!flow) {
            P->p = k;
            if (!sep_at(P, P->p)) fail(P, P->p, "--- & is followed by a space, a tab or the end of the line");
            eol(P);
            gap(P);
            k = P->p;
            spaces(P);
            flow = peek(P, 0) == '{';
            if (!flow) P->p = k;
        }
        if (flow) {
            gpush(P, G_TFLOW, 0, P->p, false);
            P->p++;
            fs(P);
        } else gpush(P, G_TMAP, 0, P->p, false);
        while (P->ng) advance(P);
        P->in_table = P->building = false;
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
    bool inl = n && peek(P, 0) != '#' && !at_eol(P, P->p);
    if (!inl) {
        P->p = j;
        eol(P);
        gap(P);
    }
    /* the start is where the document's value is written */
    const char *at = P->p;
    if (!inl && !eof(P) && !marker(P) && !dots(P)) at += indent_here(P);
    P->doc_mark = oylng_arena_get_mark(P->strings);
    gpush(P, G_DOC, -1, at, false)->inline_root = inl;
    emit(P, OYLNG_EVT_DOC_START, P->name.data, P->name.len, pos(P, at));
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

/* Reads up to the next document to deliver, delivering its start: true,
 * or false at the end */
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

/* ── Delivering nodes ────────────────────────────────────────
 * A reference, a -<< and a merged mapping are delivered by walking nodes:
 * the table's, or a mapping's just built. The walk's frames are the
 * stack, and the grammar waits until it is empty. */

static void push(parser *P, uint8_t mode, uint32_t cur, uint32_t end, uint8_t close) {
    frame *top = P->sp ? &P->stack[P->sp - 1] : NULL;
    if (top && top->close == OYLNG_EVT_NONE && top->mode == W_NODES && top->cur == top->end) {
        *top = (frame){ mode, close, 0, cur, end };   /* a spent splice: reuse it */
        return;
    }
    if (P->sp >= P->capstack) P->stack = grow(P, P->stack, &P->capstack, P->sp + 1, sizeof *P->stack);
    P->stack[P->sp++] = (frame){ mode, close, 0, cur, end };
}

/* Starts node i: true if that gave an event */
static bool handle(parser *P, uint32_t i) {
    i = deref(P, i);
    const node *nd = &P->nodes[i];
    if (P->tree_mode && i < P->table_end && nd->kind <= N_MAP) {
        emit(P, (oylng_event_type)OYLNG__EVT_REF, NULL, i, nd->pos);
        return true;
    }
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

/* The walk's next event: false once it has none */
static bool walk(parser *P) {
    while (P->sp) {
        frame *f = &P->stack[P->sp - 1];
        if (f->mode == W_PLAN && f->want_val) {
            f->want_val = 0;
            uint32_t v = P->pairs[f->cur++].val;
            if (handle(P, v)) return true;
            continue;
        }
        if (f->cur == f->end) {
            uint8_t c = f->close;
            P->sp--;
            if (c != OYLNG_EVT_NONE) {
                emit(P, (oylng_event_type)c, NULL, 0, pos(P, P->p));
                return true;
            }
            continue;
        }
        if (f->mode == W_PLAN) {
            const node *k = &P->nodes[P->pairs[f->cur].key];
            f->want_val = 1;
            emit(P, OYLNG_EVT_SCALAR, k->data, k->len, k->pos);
            return true;
        }
        uint32_t i = f->cur;
        f->cur = subtree_end(P, i);
        if (handle(P, i)) return true;
    }
    return false;
}

/* A merged mapping built in a document has been delivered: its nodes and
 * text go */
static void release_build(parser *P) {
    P->release_pending = false;
    P->n = P->b_n;
    P->npairs = P->b_pairs;
    P->nplans = P->b_plans;
    oylng_arena_release(P->strings, P->b_mark);
}

/* ── Public API ──────────────────────────────────────────── */

oylng_parser *oylng_parser_new(const char *input, size_t len) {
    if (!input && len) return NULL;
    parser *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    P->strings = oylng_arena_new(4096);
    P->vals = oylng_arena_new(4096);
    if (!P->strings || !P->vals) {
        oylng_arena_free(P->strings);
        oylng_arena_free(P->vals);
        free(P);
        return NULL;
    }
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

/* Makes the next events: at least one, at most QN, stopping before
 * anything they may point to is let go */
static void fill(parser *P) {
    oylng_arena_reset(P->vals);
    if (P->rel_pending) {
        P->rel_pending = false;
        oylng_arena_release(P->strings, P->rel_mark);
    }
    do {
        if (P->sp) {
            walk(P);
            continue;
        }
        if (P->release_pending) {
            if (P->qn) break;
            release_build(P);
        }
        switch (P->state) {
        case ST_DOC:
            advance(P);
            break;
        case ST_START:
            stream_start(P);
            emit(P, OYLNG_EVT_STREAM_START, NULL, 0, 0);
            P->state = ST_BETWEEN;
            P->qstop = true;   /* so a tree can start with the first document */
            break;
        case ST_BETWEEN:
            if (next_document(P)) P->state = ST_DOC;
            else {
                emit(P, OYLNG_EVT_STREAM_END, NULL, 0, pos(P, P->p));
                P->state = ST_DONE;
                P->qstop = true;
            }
            break;
        default:
            emit(P, OYLNG_EVT_STREAM_END, NULL, 0, pos(P, P->p));
            P->qstop = true;
            break;
        }
    } while (P->qn < QN && !P->qstop);
}

oylng_status oylng_parse_next(oylng_parser *P, const oylng_event **evt) {
    if (!P || !evt) return OYLNG_ERR_INPUT;
    if (P->qi < P->qn) {
        *evt = &P->q[P->qi++];
        return OYLNG_OK;
    }
    *evt = NULL;
    if (P->state == ST_ERROR) return P->status;
    P->qn = P->qi = 0;
    P->qstop = false;
    /* an error hands out the events made before it first */
    if (setjmp(P->jb)) {
        P->state = ST_ERROR;
        if (!P->qn) return P->status;
    } else fill(P);
    *evt = &P->q[P->qi++];
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
    free(P->g);
    free(P->tmemo);
    oylng_arena_free(P->strings);
    oylng_arena_free(P->vals);
    free(P);
}

/* ── For the tree (oylng_internal.h) ─────────────────────── */

void oylng__parser_tree_mode(oylng_parser *P, bool on) { P->tree_mode = on; }

void oylng__parser_input(const oylng_parser *P, const char **in, size_t *len) {
    *in = P->in;
    *len = (size_t)(P->end - P->in);
}

bool oylng__parser_between(const oylng_parser *P) {
    if (P->qi < P->qn) {
        oylng_event_type t = P->q[P->qi].type;
        return t == OYLNG_EVT_STREAM_START || t == OYLNG_EVT_DOC_START || t == OYLNG_EVT_STREAM_END;
    }
    return P->state != ST_DOC;
}

oylng_status oylng__parser_fail(oylng_parser *P, oylng_status st, const char *msg) {
    if (P->state == ST_ERROR) return P->status;
    P->qi = P->qn;   /* the events not handed out go */
    P->state = ST_ERROR;
    P->status = st;
    P->errmark = mark_of(P, P->p);
    snprintf(P->errmsg, sizeof P->errmsg, "%s", msg);
    return st;
}

uint64_t *oylng__tree_memo(oylng_parser *P, uint32_t *gen) {
    if (P->captmemo < P->table_end) {
        uint64_t *m = realloc(P->tmemo, (size_t)P->table_end * sizeof *m);
        if (!m) return NULL;
        memset(m + P->captmemo, 0, (size_t)(P->table_end - P->captmemo) * sizeof *m);
        P->tmemo = m;
        P->captmemo = P->table_end;
    }
    if (++P->tgen == 0) {   /* once in 4G trees */
        memset(P->tmemo, 0, (size_t)P->captmemo * sizeof *P->tmemo);
        P->tgen = 1;
    }
    *gen = P->tgen;
    return P->tmemo;
}

void oylng__table_node(const oylng_parser *P, uint32_t i, oylng__tnode *out) {
    const node *nd = &P->nodes[i];
    out->kind = nd->kind == N_STR ? OYLNG__T_STR : nd->kind == N_SEQ ? OYLNG__T_SEQ : OYLNG__T_MAP;
    out->data = nd->kind == N_STR ? nd->data : NULL;
    out->len = nd->kind == N_STR ? nd->len : 0;
    out->pos = nd->pos;
}

bool oylng__table_children(const oylng_parser *P, uint32_t i, void (*fn)(void *, uint32_t), void *ctx) {
    const node *nd = &P->nodes[i];
    if (nd->kind == N_MAP) {
        if (nd->sub & F_MERGED) {
            const plan *pl = &P->plans[nd->aux];
            for (uint32_t k = 0; k < pl->count; k++) {
                const pair *pr = &P->pairs[pl->start + k];
                fn(ctx, deref(P, pr->key));
                fn(ctx, deref(P, pr->val));
            }
        } else
            for (uint32_t k = i + 1; k < nd->link; k = subtree_end(P, k + 1)) {
                fn(ctx, deref(P, k));
                fn(ctx, deref(P, k + 1));
            }
        return true;
    }
    /* a sequence's items, each -<< spread where it is: a splice's source
       may hold splices too, so the sources being read are a stack */
    uint32_t local[64][2], (*st)[2] = local, cap = 64, n = 0;
    st[n][0] = i + 1;
    st[n++][1] = nd->link;
    bool ok = true;
    while (n) {
        uint32_t *top = st[n - 1];
        if (top[0] >= top[1]) { n--; continue; }
        uint32_t k = top[0];
        top[0] = subtree_end(P, k);
        const node *kn = &P->nodes[k];
        if (kn->kind != N_SPLICE) { fn(ctx, deref(P, k)); continue; }
        uint32_t src = deref(P, kn->link);
        if (n == cap) {
            uint32_t (*ns)[2] = malloc((size_t)cap * 2 * sizeof *ns);
            if (!ns) { ok = false; break; }
            memcpy(ns, st, (size_t)n * sizeof *ns);
            if (st != local) free(st);
            st = ns;
            cap *= 2;
        }
        st[n][0] = src + 1;
        st[n++][1] = P->nodes[src].link;
    }
    if (st != local) free(st);
    return ok;
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
