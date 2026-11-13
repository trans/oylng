/*
 * oylng_regex.c — part 2's patterns: I-Regexp (RFC 9485), matched whole
 *
 * Section numbers are those of spec/yaml-easy-schema.md at trans/yaml2.
 *
 * A pattern is read twice. The first reading checks it is I-Regexp as
 * section 4.3 has it and works out its count (section 9), building
 * nothing. If the count is within what the caller will match, the second
 * reading builds a tree of it, and the tree becomes a program that a set
 * of threads runs over the text (Thompson's construction, run as Pike's VM
 * without captures). Matching never backtracks: it takes the text's length
 * times the program's, and the program is a few instructions for each
 * position the count counts.
 *
 * The tree is kept small whatever is written: a part that counts 0, such
 * as `x{0}`, matches only the empty string and is dropped as it is read,
 * and so is a group's tree past a budget, which only a part multiplied by 0
 * can reach once the whole counts no more than the ceiling. Counts that
 * multiply by 1 (`?`, `*`, `+`) written one round another become one.
 */

#include "oylng_yes.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define NONE      UINT32_MAX
#define INF       UINT32_MAX     /* a count with no upper end */
#define DISCARD   (UINT32_MAX - 1)   /* a part not built: it must count 0 */
#define MAX_DEPTH 256            /* groups inside groups */
#define BUDGET    (1u << 16)     /* tree nodes, past which a part isn't built */
#define MAX_PROG  (1u << 20)     /* instructions: never reached within the ceiling */

/* ── The program ─────────────────────────────────────────── */

enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_MATCH };

typedef struct {
    uint8_t  op;
    uint32_t x, y;    /* CHAR: the code point; CLASS: its number; SPLIT: both
                         next instructions; JMP: the next */
} ins;

typedef struct {
    struct { uint32_t lo, hi; } *r;   /* code points, sorted and joined */
    uint32_t  nr;
    uint32_t  pos;      /* categories in the class, a bit each */
    uint32_t *negs;     /* each \P{..}'s categories, which a character in
                           the class through it isn't in */
    uint32_t  nneg;
    bool      neg;      /* [^...] */
} cls;

struct oylng__rx {
    ins      *prog;
    uint32_t  n;
    cls      *cl;
    uint32_t  ncl;
};

/* ── Reading ─────────────────────────────────────────────── */

enum { N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP };

typedef struct {
    uint8_t  kind;
    bool     opt;     /* N_ALT: one branch is empty */
    uint32_t a;       /* N_CHAR: the code point; N_CLASS: its number */
    uint32_t kid;     /* N_CAT, N_ALT: the first child; N_REP: the child */
    uint32_t next;    /* the next sibling */
    uint32_t lo, hi;  /* N_REP */
} node;

typedef struct { uint32_t lo, hi; } range;

typedef struct {
    const unsigned char *p;
    size_t    n, i;
    unsigned  depth;
    const char *why;
    jmp_buf   jb;
    int       err;          /* -1 not I-Regexp, -2 out of memory */
    /* the second reading */
    node     *v;
    uint32_t  nv, capv;
    cls      *cl;
    uint32_t  ncl, capcl;
    range    *rs;           /* a class's ranges as it is read */
    uint32_t  nrs, caprs;
    uint32_t *ng;
    uint32_t  nng, capng;
} reader;

static _Noreturn void fail(reader *R, const char *why) {
    R->why = why;
    R->err = -1;
    longjmp(R->jb, 1);
}

static _Noreturn void nomem(reader *R) {
    R->why = "out of memory";
    R->err = -2;
    longjmp(R->jb, 1);
}

static void *grow(reader *R, void *p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return p;
    uint64_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    if (c > UINT32_MAX || c > SIZE_MAX / elem) nomem(R);
    void *np = realloc(p, (size_t)c * elem);
    if (!np) nomem(R);
    *cap = (uint32_t)c;
    return np;
}

static inline uint64_t add_sat(uint64_t a, uint64_t b) { return a + b < a ? UINT64_MAX : a + b; }

static inline uint64_t mul_sat(uint64_t a, uint64_t b) {
    if (!a || !b) return 0;
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* The code point at p[i], valid UTF-8, and its length */
static uint32_t decode(const unsigned char *p, size_t n, size_t i, size_t *len) {
    unsigned char c = p[i];
    if (c < 0x80 || i + 1 >= n) { *len = 1; return c; }
    if (c < 0xE0) { *len = 2; return ((uint32_t)(c & 0x1F) << 6) | (p[i + 1] & 0x3F); }
    if (c < 0xF0 || i + 2 >= n) {
        *len = 3;
        return i + 2 < n ? ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F) : c;
    }
    if (i + 3 >= n) { *len = 1; return c; }
    *len = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12) |
           ((uint32_t)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
}

static uint32_t new_node(reader *R, int kind) {
    R->v = grow(R, R->v, &R->capv, R->nv + 1, sizeof *R->v);
    R->v[R->nv] = (node){ .kind = (uint8_t)kind, .kid = NONE, .next = NONE };
    return R->nv++;
}

/* The categories a \p{..} name stands for, a bit each; 0 if it names none.
 * RFC 9485's names: no Cs, since a string holds no surrogate. */
static uint32_t cat_mask(const unsigned char *s, size_t n) {
    static const char *const names[] = { "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl",
                                         "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc",
                                         "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn" };
    if (n == 1) {
        uint32_t m = 0;
        for (unsigned k = 0; k < 30; k++)
            if (names[k][0] == (char)s[0]) m |= 1u << k;
        return strchr("LMNPZSC", s[0]) ? m : 0;
    }
    if (n != 2 || (s[0] == 'C' && s[1] == 's')) return 0;
    for (unsigned k = 0; k < 30; k++)
        if (names[k][0] == (char)s[0] && names[k][1] == (char)s[1]) return 1u << k;
    return 0;
}

/* An escape at p[i], '\': a character (*mask 0) or \p{..} / \P{..} (*mask
 * its categories, *negated for \P) */
static uint32_t escape(reader *R, uint32_t *mask, bool *negated) {
    const unsigned char *p = R->p;
    size_t n = R->n, i = R->i;
    *mask = 0;
    if (i + 1 >= n) fail(R, "not I-Regexp");
    unsigned char c = p[i + 1];
    if (c == 'p' || c == 'P') {
        if (i + 2 >= n || p[i + 2] != '{') fail(R, "not I-Regexp");
        size_t j = i + 3;
        while (j < n && ((p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= 'a' && p[j] <= 'z'))) j++;
        if (j >= n || p[j] != '}') fail(R, "not I-Regexp");
        uint32_t m = cat_mask(p + i + 3, j - i - 3);
        if (!m) fail(R, "not I-Regexp");
        *mask = m;
        *negated = c == 'P';
        R->i = j + 1;
        return 0;
    }
    if (!strchr("()*+-.?[\\]^nrt{|}", c) || !c) fail(R, "not I-Regexp");
    R->i = i + 2;
    return c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
}

/* A class's member: a character, or a category (*mask set) */
static uint32_t member(reader *R, uint32_t *mask, bool *negated) {
    unsigned char c = R->p[R->i];
    *mask = 0;
    if (c == '\\') return escape(R, mask, negated);
    if (c == '[' || c == ']' || c == '-') fail(R, "not I-Regexp");
    size_t len;
    uint32_t cp = decode(R->p, R->n, R->i, &len);
    R->i += len;
    return cp;
}

static int range_cmp(const void *a, const void *b) {
    const range *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* [...] at p[i]: its number, if building */
static uint32_t read_class(reader *R, bool build) {
    const unsigned char *p = R->p;
    size_t n = R->n;
    R->i++;
    bool neg = R->i < n && p[R->i] == '^';
    if (neg) R->i++;
    R->nrs = R->nng = 0;
    uint32_t pos = 0;
    bool any = false;
#define ADD(a, b)                                                                  \
    do {                                                                           \
        if (build) {                                                               \
            R->rs = grow(R, R->rs, &R->caprs, R->nrs + 1, sizeof *R->rs);         \
            R->rs[R->nrs++] = (range){ (a), (b) };                                 \
        }                                                                          \
    } while (0)
    if (R->i < n && p[R->i] == '-') { ADD('-', '-'); R->i++; any = true; }
    for (;;) {
        if (R->i >= n) fail(R, "not I-Regexp");
        if (p[R->i] == ']') {
            if (!any) fail(R, "not I-Regexp");
            R->i++;
            break;
        }
        if (p[R->i] == '-' && R->i + 1 < n && p[R->i + 1] == ']') { ADD('-', '-'); R->i++; any = true; continue; }
        uint32_t m;
        bool ng = false;
        uint32_t a = member(R, &m, &ng);
        any = true;
        if (m) {
            if (!ng) pos |= m;
            else if (build) {
                R->ng = grow(R, R->ng, &R->capng, R->nng + 1, sizeof *R->ng);
                R->ng[R->nng++] = m;
            }
            continue;
        }
        if (R->i + 1 < n && p[R->i] == '-' && p[R->i + 1] != ']') {
            R->i++;
            uint32_t m2;
            bool ng2;
            uint32_t b = member(R, &m2, &ng2);
            if (m2 || b < a) fail(R, "not I-Regexp");
            ADD(a, b);
        } else ADD(a, a);
    }
#undef ADD
    if (!build) return NONE;
    /* sorted and joined */
    if (R->nrs > 1) qsort(R->rs, R->nrs, sizeof *R->rs, range_cmp);
    uint32_t k = 0;
    for (uint32_t j = 0; j < R->nrs; j++) {
        if (k && R->rs[j].lo <= R->rs[k - 1].hi + 1) {
            if (R->rs[j].hi > R->rs[k - 1].hi) R->rs[k - 1].hi = R->rs[j].hi;
        } else R->rs[k++] = R->rs[j];
    }
    R->cl = grow(R, R->cl, &R->capcl, R->ncl + 1, sizeof *R->cl);
    cls *c = &R->cl[R->ncl];
    *c = (cls){ .pos = pos, .neg = neg, .nr = k, .nneg = R->nng };
    if (k) {
        c->r = malloc(k * sizeof *c->r);
        if (!c->r) nomem(R);
        memcpy(c->r, R->rs, k * sizeof *c->r);
    }
    if (R->nng) {
        c->negs = malloc(R->nng * sizeof *c->negs);
        if (!c->negs) { free(c->r); nomem(R); }
        memcpy(c->negs, R->ng, R->nng * sizeof *c->negs);
    }
    return R->ncl++;
}

/* A class of one category, for \p{..} and \P{..} outside brackets */
static uint32_t cat_class(reader *R, uint32_t mask, bool negated) {
    R->cl = grow(R, R->cl, &R->capcl, R->ncl + 1, sizeof *R->cl);
    R->cl[R->ncl] = (cls){ .pos = mask, .neg = negated };
    return R->ncl++;
}

/* A digit run's value, saturating, and where it ends */
static uint64_t number(reader *R, size_t *len) {
    size_t i = R->i, s = i;
    uint64_t v = 0;
    while (i < R->n && R->p[i] >= '0' && R->p[i] <= '9') {
        v = add_sat(mul_sat(v, 10), (uint64_t)(R->p[i] - '0'));
        i++;
    }
    *len = i - s;
    return v;
}

/* Whether the digits at a (al of them) are fewer than those at b, as numbers */
static bool digits_less(const unsigned char *a, size_t al, const unsigned char *b, size_t bl) {
    while (al > 1 && *a == '0') a++, al--;
    while (bl > 1 && *b == '0') b++, bl--;
    if (al != bl) return al < bl;
    return memcmp(a, b, al) < 0;
}

typedef struct {
    uint64_t count;
    uint32_t node;    /* NONE: matches only the empty string; DISCARD */
} part;

static part regexp(reader *R, bool build);

/* Section 9: a pattern's count, as written */
static part atom(reader *R, bool build) {
    const unsigned char *p = R->p;
    unsigned char c = p[R->i];
    part out = { 1, NONE };
    uint32_t m;
    bool ng;
    switch (c) {
    case '(':
        if (++R->depth > MAX_DEPTH) fail(R, "groups nested more than 256 deep");
        R->i++;
        out = regexp(R, build);
        if (R->i >= R->n || p[R->i] != ')') fail(R, "not I-Regexp");
        R->i++;
        R->depth--;
        return out;
    case '.':
        R->i++;
        if (build) out.node = new_node(R, N_ANY);
        return out;
    case '[': {
        uint32_t k = read_class(R, build);
        if (build) { out.node = new_node(R, N_CLASS); R->v[out.node].a = k; }
        return out;
    }
    case '\\': {
        uint32_t ch = escape(R, &m, &ng);
        if (!build) return out;
        if (m) {
            uint32_t k = cat_class(R, m, ng);
            out.node = new_node(R, N_CLASS);
            R->v[out.node].a = k;
        } else {
            out.node = new_node(R, N_CHAR);
            R->v[out.node].a = ch;
        }
        return out;
    }
    case '^': fail(R, "^ outside a class");
    case '$': fail(R, "$ outside a class");
    case '*': case '+': case '?': case '{': case '}': case ']':
        fail(R, "not I-Regexp");
    default: {
        size_t len;
        uint32_t ch = decode(p, R->n, R->i, &len);
        R->i += len;
        if (build) { out.node = new_node(R, N_CHAR); R->v[out.node].a = ch; }
        return out;
    }
    }
}

/* Whether a count multiplies by 1: ?, *, + or {1} */
static bool by_one(uint32_t lo, uint32_t hi) { return lo <= 1 && (hi == 1 || hi == INF); }

/* An atom and its count */
static part piece(reader *R, bool build) {
    uint32_t mark = R->nv, cmark = R->ncl;
    bool b = build && R->nv < BUDGET;
    part a = atom(R, b);
    if (build && !b) a.node = DISCARD;
    const unsigned char *p = R->p;
    size_t n = R->n;
    uint64_t mult = 1;
    uint32_t lo = 1, hi = 1;
    if (R->i < n && (p[R->i] == '*' || p[R->i] == '+' || p[R->i] == '?')) {
        lo = p[R->i] == '+';
        hi = p[R->i] == '?' ? 1 : INF;
        R->i++;
    } else if (R->i < n && p[R->i] == '{') {
        R->i++;
        size_t ll, hl = 0;
        const unsigned char *ls = p + R->i;
        uint64_t l = number(R, &ll), h;
        if (!ll) fail(R, "not I-Regexp");
        R->i += ll;
        bool comma = R->i < n && p[R->i] == ',', open = false;
        const unsigned char *hs = NULL;
        if (comma) {
            R->i++;
            hs = p + R->i;
            h = number(R, &hl);
            R->i += hl;
            open = !hl;
        } else h = l;
        if (R->i >= n || p[R->i] != '}') fail(R, "not I-Regexp");
        R->i++;
        if (comma && !open && digits_less(hs, hl, ls, ll)) fail(R, "not I-Regexp");
        mult = open ? (l ? l : 1) : h;
        lo = l > INF - 1 ? INF - 1 : (uint32_t)l;
        hi = open ? INF : h > INF - 1 ? INF - 1 : (uint32_t)h;
    }
    part out = { mul_sat(a.count, mult), a.node };
    if (!build) return out;
    if (!out.count) {
        /* matches only the empty string: drop what it built */
        R->nv = mark;
        for (uint32_t k = cmark; k < R->ncl; k++) { free(R->cl[k].r); free(R->cl[k].negs); }
        R->ncl = cmark;
        out.node = NONE;
        return out;
    }
    if (out.node == DISCARD || out.node == NONE || (lo == 1 && hi == 1)) return out;
    node *x = &R->v[out.node];
    if (x->kind == N_REP && by_one(x->lo, x->hi) && by_one(lo, hi)) {
        x->lo = x->lo && lo;
        x->hi = x->hi == INF || hi == INF ? INF : 1;
        return out;
    }
    uint32_t r = new_node(R, N_REP);
    R->v[r].kid = out.node;
    R->v[r].lo = lo;
    R->v[r].hi = hi;
    out.node = r;
    return out;
}

/* Pieces up to | or ) */
static part branch(reader *R, bool build) {
    part out = { 0, NONE };
    uint32_t first = NONE, last = NONE, kids = 0;
    bool discard = false;
    while (R->i < R->n && R->p[R->i] != '|' && R->p[R->i] != ')') {
        part q = piece(R, build);
        out.count = add_sat(out.count, q.count);
        if (!build || q.node == NONE) continue;
        if (q.node == DISCARD) { discard = true; continue; }
        if (discard) continue;
        if (last == NONE) first = q.node;
        else R->v[last].next = q.node;
        last = q.node;
        kids++;
    }
    if (!build) return out;
    if (discard) { out.node = DISCARD; return out; }
    if (kids <= 1) { out.node = first; return out; }
    out.node = new_node(R, N_CAT);
    R->v[out.node].kid = first;
    return out;
}

/* Branches joined by | */
static part regexp(reader *R, bool build) {
    part out = branch(R, build);
    if (R->i >= R->n || R->p[R->i] != '|') return out;
    uint32_t first = NONE, last = NONE, kids = 0;
    bool opt = false, discard = out.node == DISCARD;
#define TAKE(q)                                                     \
    do {                                                            \
        if ((q).node == DISCARD) discard = true;                    \
        else if ((q).node == NONE) opt = true;                      \
        else if (!discard) {                                        \
            if (last == NONE) first = (q).node;                     \
            else R->v[last].next = (q).node;                        \
            last = (q).node;                                        \
            kids++;                                                 \
        }                                                           \
    } while (0)
    if (build) TAKE(out);
    while (R->i < R->n && R->p[R->i] == '|') {
        R->i++;
        part q = branch(R, build);
        out.count = add_sat(out.count, q.count);
        if (build) TAKE(q);
    }
#undef TAKE
    if (!build) return out;
    if (discard) { out.node = DISCARD; return out; }
    if (!kids) { out.node = NONE; return out; }
    if (kids == 1 && !opt) { out.node = first; return out; }
    if (kids == 1) {
        /* x| is x? */
        node *x = &R->v[first];
        if (x->kind == N_REP && by_one(x->lo, x->hi)) {
            x->lo = 0;
            out.node = first;
            return out;
        }
        out.node = new_node(R, N_REP);
        R->v[out.node].kid = first;
        R->v[out.node].lo = 0;
        R->v[out.node].hi = 1;
        return out;
    }
    out.node = new_node(R, N_ALT);
    R->v[out.node].kid = first;
    R->v[out.node].opt = opt;
    return out;
}

/* ── Building the program ────────────────────────────────── */

typedef struct {
    reader   *R;
    ins      *prog;
    uint32_t  n, cap;
} builder;

static uint32_t emit(builder *B, uint8_t op, uint32_t x, uint32_t y) {
    if (B->n >= MAX_PROG) fail(B->R, "a pattern too large to match");
    B->prog = grow(B->R, B->prog, &B->cap, B->n + 1, sizeof *B->prog);
    B->prog[B->n] = (ins){ op, x, y };
    return B->n++;
}

static void build(builder *B, uint32_t at) {
    const node *x = &B->R->v[at];
    switch (x->kind) {
    case N_CHAR: emit(B, I_CHAR, x->a, 0); return;
    case N_ANY: emit(B, I_ANY, 0, 0); return;
    case N_CLASS: emit(B, I_CLASS, x->a, 0); return;
    case N_CAT:
        for (uint32_t k = x->kid; k != NONE; k = B->R->v[k].next) build(B, k);
        return;
    case N_ALT: {
        /* each branch but the last: SPLIT to it or on; it ends with a JMP
           to the end, patched once known */
        uint32_t jumps = NONE;
        bool opt = x->opt;
        for (uint32_t k = x->kid; k != NONE; k = B->R->v[k].next) {
            bool last = B->R->v[k].next == NONE && !opt;
            uint32_t s = last ? NONE : emit(B, I_SPLIT, B->n + 1, 0);
            build(B, k);
            if (!last) {
                uint32_t j = emit(B, I_JMP, jumps, 0);   /* a chain of jumps to patch */
                jumps = j;
                B->prog[s].y = B->n;
            }
        }
        while (jumps != NONE) {
            uint32_t nx = B->prog[jumps].x;
            B->prog[jumps].x = B->n;
            jumps = nx;
        }
        return;
    }
    case N_REP: {
        uint32_t kid = x->kid, lo = x->lo, hi = x->hi;
        if (hi == INF) {
            for (uint32_t c = 1; c < lo; c++) build(B, kid);
            if (lo == 0) {
                uint32_t s = emit(B, I_SPLIT, B->n + 1, 0);
                build(B, kid);
                emit(B, I_JMP, s, 0);
                B->prog[s].y = B->n;
            } else {
                uint32_t start = B->n;
                build(B, kid);
                emit(B, I_SPLIT, start, B->n + 1);
            }
            return;
        }
        for (uint32_t c = 0; c < lo; c++) build(B, kid);
        uint32_t splits = NONE;
        for (uint32_t c = lo; c < hi; c++) {
            uint32_t s = emit(B, I_SPLIT, B->n + 1, splits);
            splits = s;
            build(B, kid);
        }
        while (splits != NONE) {
            uint32_t nx = B->prog[splits].y;
            B->prog[splits].y = B->n;
            splits = nx;
        }
        return;
    }
    }
}

/* ── Compiling ───────────────────────────────────────────── */

static void reader_free(reader *R, bool classes) {
    free(R->v);
    free(R->rs);
    free(R->ng);
    if (classes)
        for (uint32_t k = 0; k < R->ncl; k++) { free(R->cl[k].r); free(R->cl[k].negs); }
    free(R->cl);
}

static void *arena_copy(oylng_arena *A, const void *p, size_t n) {
    void *d = oylng_arena_alloc(A, n ? n : 1, 8);
    if (d && n) memcpy(d, p, n);
    return d;
}

int oylng__rx_compile(oylng_arena *A, const char *pat, size_t n, uint64_t build_max, oylng__rx **out,
                      uint64_t *count, const char **why) {
    *out = NULL;
    *count = 0;
    reader R = { .p = (const unsigned char *)pat, .n = n };
    if (setjmp(R.jb)) {
        *why = R.why;
        reader_free(&R, true);
        return R.err;
    }
    part all = regexp(&R, false);
    if (R.i != n) fail(&R, "not I-Regexp");
    *count = all.count;
    if (all.count > build_max) return 0;

    R.i = 0;
    R.depth = 0;
    all = regexp(&R, true);
    if (all.node == DISCARD) fail(&R, "a pattern too large to match");   /* can't be, within the ceiling */
    builder B = { .R = &R };
    if (setjmp(R.jb)) {
        *why = R.why;
        free(B.prog);
        reader_free(&R, true);
        return R.err;
    }
    if (all.node != NONE) build(&B, all.node);
    emit(&B, I_MATCH, 0, 0);

    oylng__rx *rx = oylng_arena_alloc(A, sizeof *rx, 8);
    if (!rx) nomem(&R);
    rx->n = B.n;
    rx->prog = arena_copy(A, B.prog, (size_t)B.n * sizeof *B.prog);
    rx->ncl = R.ncl;
    rx->cl = arena_copy(A, R.cl, (size_t)R.ncl * sizeof *R.cl);
    if (!rx->prog || !rx->cl) nomem(&R);
    for (uint32_t k = 0; k < R.ncl; k++) {
        cls *c = &rx->cl[k];
        void *r = c->nr ? arena_copy(A, R.cl[k].r, (size_t)c->nr * sizeof *c->r) : NULL;
        void *g = c->nneg ? arena_copy(A, R.cl[k].negs, (size_t)c->nneg * sizeof *c->negs) : NULL;
        if ((c->nr && !r) || (c->nneg && !g)) nomem(&R);
        c->r = r;
        c->negs = g;
    }
    free(B.prog);
    reader_free(&R, true);
    *out = rx;
    return 0;
}

/* ── Matching ────────────────────────────────────────────── */

static bool in_class(const cls *c, uint32_t cp) {
    bool in = false;
    uint32_t lo = 0, hi = c->nr;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cp < c->r[mid].lo) hi = mid;
        else if (cp > c->r[mid].hi) lo = mid + 1;
        else { in = true; break; }
    }
    if (!in && (c->pos || c->nneg)) {
        uint32_t bit = 1u << oylng__ucd_cat(cp);
        if (c->pos & bit) in = true;
        for (uint32_t k = 0; !in && k < c->nneg; k++)
            if (!(c->negs[k] & bit)) in = true;
    }
    return in != c->neg;
}

void oylng__rxscr_free(oylng__rxscr *scr) {
    free(scr->buf);
    scr->buf = NULL;
    scr->cap = 0;
    scr->gen = 0;
}

/* The threads at pc, past splits and jumps, onto list */
static void closure(const oylng__rx *rx, uint32_t pc, uint32_t *list, uint32_t *nl, uint32_t *mark, uint32_t gen,
                    uint32_t *stack) {
    uint32_t ns = 0;
    if (mark[pc] == gen) return;
    mark[pc] = gen;
    stack[ns++] = pc;
    while (ns) {
        pc = stack[--ns];
        const ins *in = &rx->prog[pc];
        if (in->op == I_JMP || in->op == I_SPLIT) {
            uint32_t a = in->x, b = in->op == I_SPLIT ? in->y : NONE;
            if (b != NONE && mark[b] != gen) { mark[b] = gen; stack[ns++] = b; }
            if (mark[a] != gen) { mark[a] = gen; stack[ns++] = a; }
        } else list[(*nl)++] = pc;
    }
}

int oylng__rx_match(const oylng__rx *rx, const char *text, size_t n, oylng__rxscr *scr) {
    if (n > UINT32_MAX / 2) return -1;   /* the parser refuses inputs of 4 GiB */
    size_t m = rx->n, need = 4 * m;
    if (scr->cap < need) {
        uint32_t *nb = realloc(scr->buf, need * sizeof *nb);
        if (!nb) return -1;
        scr->buf = nb;
        scr->cap = need;
        memset(nb, 0, need * sizeof *nb);
        scr->gen = 0;
    }
    uint32_t *cl = scr->buf, *nl = cl + m, *mark = nl + m, *stack = mark + m;
    uint32_t ncl = 0, nnl;
    if (scr->gen > UINT32_MAX - 2 - n) {
        memset(mark, 0, m * sizeof *mark);
        scr->gen = 0;
    }
    closure(rx, 0, cl, &ncl, mark, ++scr->gen, stack);
    const unsigned char *p = (const unsigned char *)text;
    for (size_t i = 0; i < n && ncl;) {
        size_t len;
        uint32_t cp = decode(p, n, i, &len);
        i += len;
        uint32_t gen = ++scr->gen;
        nnl = 0;
        for (uint32_t t = 0; t < ncl; t++) {
            const ins *in = &rx->prog[cl[t]];
            bool ok = in->op == I_CHAR    ? in->x == cp
                      : in->op == I_ANY   ? cp != '\n' && cp != '\r'
                      : in->op == I_CLASS ? in_class(&rx->cl[in->x], cp)
                                          : false;
            if (ok) closure(rx, cl[t] + 1, nl, &nnl, mark, gen, stack);
        }
        uint32_t *tmp = cl;
        cl = nl;
        nl = tmp;
        ncl = nnl;
    }
    for (uint32_t t = 0; t < ncl; t++)
        if (rx->prog[cl[t]].op == I_MATCH) return 1;
    return 0;
}
