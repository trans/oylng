/*
 * oylng_typed.c — a document typed by a schema (part 2)
 *
 * Section numbers are those of spec/yaml-easy-schema.md at trans/yaml2.
 *
 * A document is read as a tree, and then typed: each value of it, from the
 * root, against the type at its place. A typed value is a node of the tree
 * and what the type made of it. Nothing recurses: a collection being typed
 * is a frame on a stack, which types its children in turn.
 *
 * The tree shares a node of the table at every place that names it. A
 * shared node is typed once for each type that reaches it (section 11),
 * and each time adds its own size to typed, which the size limit applies
 * to. A typed value isn't shared between types.
 */

#include "oylng_yes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX

typedef struct {
    uint8_t  type;     /* oylng_type */
    uint8_t  neg;      /* OYLNG_T_INT: below 0 */
    uint16_t own;      /* BIGINT, DECIMAL: its text is in bytes, at a, x.n
                          long: a number a pattern matched, in the standard
                          spelling (section 4.5) */
    uint32_t node;     /* the tree's node */
    uint32_t name;     /* in names, or NONE */
    uint32_t a;        /* SEQUENCE, MAPPING: where its children start in kids;
                          BINARY, and BIGINT and DECIMAL with own: where its
                          bytes start */
    union {
        uint64_t u;    /* INT: its magnitude */
        double   f;
        uint64_t n;    /* BINARY, and BIGINT and DECIMAL with own: how many bytes */
    } x;
} tval;

struct oylng_typed {
    oylng_tree    *T;
    oylng_typed_by how;
    oylng_value    root;
    tval          *v;
    size_t         nv, capv;
    uint32_t      *kids;      /* a sequence's items, a mapping's values */
    size_t         nk, capk;
    unsigned char *bytes;
    size_t         nb, capb;
    struct { uint32_t off, len; } *names;
    size_t         nn, capn;
    char          *pool;
    size_t         np, capp;
};

/* ── Typing ──────────────────────────────────────────────── */

typedef struct {
    uint32_t node;   /* the collection */
    uint32_t val;
    const xt *x;     /* its type: a sequence or a mapping */
    uint32_t i, n;   /* the child being typed, and how many */
} frame;

typedef struct {
    oylng_typed      *Y;
    const sctx       *S;
    const oylng_tree *T;
    oylng_parser     *P;
    bool              cut;
    uint64_t          limit, typed;
    frame            *f;
    size_t            nf, capf;
    struct memo { uint32_t node, val; const xt *k; } *memo;   /* shared nodes typed */
    size_t            nmemo, capmemo;
    struct nm { const centry *e; uint32_t i; } *nms;          /* names copied */
    size_t            nnms, capnms;
    uint8_t          *ok;
    uint32_t         *req;
    size_t            capok;
    oylng__rxscr      rxs;        /* for matching patterns */
    char             *canon;      /* a number a pattern matched, as spelled */
    size_t            capcanon;
    jmp_buf           jb;
    oylng_status      st;
} typer;

static _Noreturn void nomem(typer *R) {
    R->st = oylng__parser_fail(R->P, OYLNG_ERR_MEMORY, "out of memory");
    longjmp(R->jb, 1);
}

static void *room(typer *R, void *p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 16;
    while (c < need) {
        if (c > SIZE_MAX / 2 / elem) nomem(R);
        c *= 2;
    }
    void *np = realloc(p, c * elem);
    if (!np) nomem(R);
    *cap = c;
    return np;
}

#define ROOM(R, arr, cap, need) ((arr) = room((R), (arr), &(cap), (need), sizeof *(arr)))

/* Refuses the document: the path to the value, what is wrong, and where
 * the node is written. A key's fault is its mapping's, so its path is the
 * mapping's, as the checker gives it. */
static _Noreturn void refuse_as(typer *R, oylng_status st, uint32_t node, const char *fmt, ...) {
    char path[160] = "", what[200];
    size_t pl = 0, nf = R->nf;
    if (nf) {
        const frame *f = &R->f[nf - 1];
        if (oylng_node_kind(R->T, f->node) == OYLNG_NODE_MAPPING && f->i < oylng_node_count(R->T, f->node) &&
            oylng_node_key(R->T, f->node, f->i) == node)
            nf--;
    }
    for (size_t i = 0; i < nf && pl < sizeof path - 8; i++) {
        const frame *f = &R->f[i];
        int w;
        if (oylng_node_kind(R->T, f->node) == OYLNG_NODE_MAPPING) {
            oylng_str k = oylng_node_text(R->T, oylng_node_key(R->T, f->node, f->i));
            w = snprintf(path + pl, sizeof path - pl, "/%.*s", (int)(k.len > 40 ? 40 : k.len), k.data);
        } else w = snprintf(path + pl, sizeof path - pl, "/%u", f->i);
        if (w > 0) pl += (size_t)w;
    }
    if (pl >= sizeof path) pl = sizeof path - 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    char msg[400];
    snprintf(msg, sizeof msg, "%s: %s", pl ? path : "/", what);
    R->st = oylng__parser_fail_at(R->P, st, oylng_node_mark(R->T, node).offset, msg);
    longjmp(R->jb, 1);
}

#define refuse(R, node, ...) refuse_as((R), OYLNG_ERR_SCHEMA, (node), __VA_ARGS__)

static uint32_t name_of(typer *R, const centry *e) {
    if (!e) return NONE;
    size_t cap = R->capnms;
    if (cap) {
        size_t h = ((uintptr_t)e >> 4) * 0x9E3779B97F4A7C15u >> 7;
        for (size_t i = h & (cap - 1);; i = (i + 1) & (cap - 1)) {
            if (!R->nms[i].e) break;
            if (R->nms[i].e == e) return R->nms[i].i;
        }
    }
    oylng_typed *Y = R->Y;
    ROOM(R, Y->names, Y->capn, Y->nn + 1);
    ROOM(R, Y->pool, Y->capp, Y->np + e->name.len + 1);
    memcpy(Y->pool + Y->np, e->name.s, e->name.len);
    Y->names[Y->nn].off = (uint32_t)Y->np;
    Y->names[Y->nn].len = e->name.len;
    Y->np += e->name.len;
    uint32_t idx = (uint32_t)Y->nn++;
    if (2 * (R->nnms + 1) > R->capnms) {   /* twice the room, rehashed */
        size_t nc = R->capnms ? R->capnms * 2 : 32;
        struct nm *nn = calloc(nc, sizeof *nn);
        if (!nn) nomem(R);
        for (size_t i = 0; i < R->capnms; i++)
            if (R->nms[i].e) {
                size_t h = ((uintptr_t)R->nms[i].e >> 4) * 0x9E3779B97F4A7C15u >> 7;
                size_t j = h & (nc - 1);
                while (nn[j].e) j = (j + 1) & (nc - 1);
                nn[j] = R->nms[i];
            }
        free(R->nms);
        R->nms = nn;
        R->capnms = nc;
    }
    size_t h = ((uintptr_t)e >> 4) * 0x9E3779B97F4A7C15u >> 7;
    size_t j = h & (R->capnms - 1);
    while (R->nms[j].e) j = (j + 1) & (R->capnms - 1);
    R->nms[j] = (struct nm){ e, idx };
    R->nnms++;
    return idx;
}

static uint32_t new_val(typer *R, oylng_type type, uint32_t node, const centry *from) {
    oylng_typed *Y = R->Y;
    if (Y->nv >= NONE - 1) refuse_as(R, OYLNG_ERR_LIMIT, node, "a typed document of 4G values or more");
    uint32_t name = name_of(R, from);
    ROOM(R, Y->v, Y->capv, Y->nv + 1);
    Y->v[Y->nv] = (tval){ (uint8_t)type, 0, 0, node, name, 0, { 0 } };
    return (uint32_t)Y->nv++;
}

/* ── Scalars (section 4) ─────────────────────────────────── */

enum { NO_MATCH, MATCH, REFUSED };

typedef struct {
    oylng_type type;
    bool       neg;
    uint64_t   u;
    double     f;
    const char *text;   /* BIGINT, DECIMAL: a pattern's number, as spelled */
    size_t     textlen;
    char       why[120];
} scalar;

static bool int_spelling(const char *s, size_t n) {
    size_t i = n && s[0] == '-';
    if (i >= n) return false;
    if (s[i] == '0') return i + 1 == n;
    for (; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool float_spelling(const char *s, size_t n) {
    size_t i = n && s[0] == '-';
    if (i >= n || s[i] < '0' || s[i] > '9') return false;
    if (s[i] == '0') i++;
    else while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    if (i < n && s[i] == '.') {
        size_t d = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return false;
    }
    return i == n;
}

static bool outside(const xt *x, const oylng__num *v) {
    return ((x->flags & XF_LO) && oylng__num_cmp(v, &x->u.std.lo) < 0) ||
           ((x->flags & XF_HI) && oylng__num_cmp(v, &x->u.std.hi) > 0);
}

#define SHOW(n) (int)((n) > 40 ? 40 : (n))

/* Whether the whole of s matches x's pattern */
static bool pat_match(typer *R, const xt *x, const char *s, size_t n) {
    if (!x->u.std.rx) return false;   /* past the ceiling: never compiled into a schema */
    int r = oylng__rx_match(x->u.std.rx, s, n, &R->rxs);
    if (r < 0) nomem(R);
    return r == 1;
}

/* Section 4.5: what a pattern on a number matched, read as the number and
 * written in the standard spelling, into R->canon: no +, no leading zeros, a
 * digit on each side of a point, the exponent as written. For !int and
 * !bigint, [-+]? digits; for !float and !decimal, [-+]? digits with at most
 * one point and an exponent. False if it isn't that. */
static bool pat_number(typer *R, bool integer, const char *s, size_t n, size_t *len) {
    size_t i = 0;
    bool neg = false;
    if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    size_t ib = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    size_t ie = i, fb = i, fe = i;
    if (!integer && i < n && s[i] == '.') {
        fb = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        fe = i;
    }
    if (ie == ib && fe == fb) return false;
    size_t eb = i;
    if (!integer && i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (s[j] == '-' || s[j] == '+')) j++;
        size_t d = j;
        while (j < n && s[j] >= '0' && s[j] <= '9') j++;
        if (j == d) return false;
        i = j;
    }
    if (i != n) return false;
    while (ib < ie && s[ib] == '0') ib++;
    if (integer && ib == ie) neg = false;   /* -0 is 0 */
    R->canon = room(R, R->canon, &R->capcanon, n + 2, 1);
    char *o = R->canon;
    size_t k = 0;
    if (neg) o[k++] = '-';
    if (ib == ie) o[k++] = '0';
    memcpy(o + k, s + ib, ie - ib);
    k += ie - ib;
    if (fe > fb) {
        o[k++] = '.';
        memcpy(o + k, s + fb, fe - fb);
        k += fe - fb;
    }
    memcpy(o + k, s + eb, n - eb);
    k += n - eb;
    *len = k;
    return true;
}

static int match_std(typer *R, const xt *x, const char *s, size_t n, bool plain, scalar *out) {
    const char *name = oylng__std_names[x->std];
    oylng__num num;
    out->neg = false;   /* what a match doesn't set is 0 */
    out->u = 0;
    out->f = 0;
    out->text = NULL;
    out->textlen = 0;
    switch (x->std) {
    case STD_STR:
        if ((x->flags & XF_PAT) && !pat_match(R, x, s, n)) {
            snprintf(out->why, sizeof out->why, "'%.*s' does not match", SHOW(n), s);
            return NO_MATCH;
        }
        if (x->flags & (XF_LO | XF_HI)) {
            uint64_t cps = 0;
            for (size_t i = 0; i < n; i++) cps += ((unsigned char)s[i] & 0xC0) != 0x80;
            char buf[24];
            oylng__num_of_count(cps, buf, &num);
            if (outside(x, &num)) {
                snprintf(out->why, sizeof out->why, "'%.*s' is out of range", SHOW(n), s);
                return NO_MATCH;
            }
        }
        out->type = OYLNG_T_STR;
        return MATCH;
    case STD_INT:
    case STD_BIGINT:
    case STD_FLOAT:
    case STD_DECIMAL:
    case STD_BOOL:
    case STD_NULL:
        if (!plain) {
            snprintf(out->why, sizeof out->why, "'%.*s' isn't plain, so not !%s", SHOW(n), s, name);
            return NO_MATCH;
        }
        break;
    case STD_DATE:
    case STD_DATETIME: {
        oylng_datetime dt;
        int r = oylng__date(s, n, x->std == STD_DATETIME, R->cut, &dt);
        if (r < 0) {
            snprintf(out->why, sizeof out->why, "this parser can't hold %.*s: it keeps 9 fraction digits",
                     SHOW(n), s);
            return REFUSED;
        }
        if (!r) break;
        out->type = x->std == STD_DATE ? OYLNG_T_DATE : OYLNG_T_DATETIME;
        return MATCH;
    }
    case STD_BINARY: {
        size_t len;
        if (!oylng__base64(s, n, NULL, &len)) break;
        out->type = OYLNG_T_BINARY;
        out->u = len;
        return MATCH;
    }
    default:
        break;
    }
    if ((x->flags & XF_PAT) && x->std != STD_STR) {
        /* section 4.5: the pattern replaces the spelling */
        if (!pat_match(R, x, s, n)) {
            snprintf(out->why, sizeof out->why, "'%.*s' does not match", SHOW(n), s);
            return NO_MATCH;
        }
        if (x->std == STD_NULL) { out->type = OYLNG_T_NULL; return MATCH; }
        bool integer = x->std == STD_INT || x->std == STD_BIGINT;
        size_t len;
        if (!pat_number(R, integer, s, n, &len)) {
            snprintf(out->why, sizeof out->why, "'%.*s' matched, and is not %s", SHOW(n), s,
                     integer ? "an integer" : "a number");
            return REFUSED;
        }
        s = R->canon;
        n = len;
        out->text = s;
        out->textlen = n;
    }
    switch (x->std) {
    case STD_INT:
    case STD_BIGINT:
        if (!int_spelling(s, n)) break;
        /* a limit first, on the number as written (section 4.4) */
        oylng__num_read(s, n, &num);
        if (outside(x, &num)) {
            snprintf(out->why, sizeof out->why, "'%.*s' is out of range", SHOW(n), s);
            return NO_MATCH;
        }
        if (x->std == STD_BIGINT) {
            out->type = OYLNG_T_BIGINT;
            return MATCH;
        }
        if (!oylng__int(s, n, &out->neg, &out->u)) {
            snprintf(out->why, sizeof out->why, "%.*s is outside !int, -2^63 to 2^64-1", SHOW(n), s);
            return REFUSED;
        }
        out->type = OYLNG_T_INT;
        return MATCH;
    case STD_FLOAT:
        if (!float_spelling(s, n)) break;
        oylng__num_read(s, n, &num);
        if (outside(x, &num)) {
            snprintf(out->why, sizeof out->why, "'%.*s' is out of range", SHOW(n), s);
            return NO_MATCH;
        }
        if (!oylng__float(s, n, &out->f)) {
            snprintf(out->why, sizeof out->why, "%.*s is too large for binary64", SHOW(n), s);
            return REFUSED;
        }
        out->type = OYLNG_T_FLOAT;
        return MATCH;
    case STD_DECIMAL:
        if (!float_spelling(s, n)) break;
        oylng__num_read(s, n, &num);
        if ((x->flags & XF_PREC) && !oylng__num_fits(&num, x->u.std.prec, x->u.std.scale)) {
            snprintf(out->why, sizeof out->why, "'%.*s' has too many digits for %llu:%llu", SHOW(n), s,
                     (unsigned long long)x->u.std.prec, (unsigned long long)x->u.std.scale);
            return NO_MATCH;
        }
        if (outside(x, &num)) {
            snprintf(out->why, sizeof out->why, "'%.*s' is out of range", SHOW(n), s);
            return NO_MATCH;
        }
        out->type = OYLNG_T_DECIMAL;
        return MATCH;
    case STD_BOOL:
        if (n == 4 && memcmp(s, "true", 4) == 0) { out->type = OYLNG_T_BOOL; out->u = 1; return MATCH; }
        if (n == 5 && memcmp(s, "false", 5) == 0) { out->type = OYLNG_T_BOOL; out->u = 0; return MATCH; }
        break;
    case STD_NULL:
        if (n == 4 && memcmp(s, "null", 4) == 0) { out->type = OYLNG_T_NULL; return MATCH; }
        break;
    default:
        break;
    }
    snprintf(out->why, sizeof out->why, "expected !%s, got '%.*s'", name, SHOW(n), s);
    return NO_MATCH;
}

/* A scalar matched against a union's alternatives in the order written,
 * !any last: the position that took it, or NONE */
static uint32_t match_alts(typer *R, const xalts *a, uint32_t node, const char *s, size_t n, bool plain,
                           scalar *out) {
    out->neg = false;   /* a literal and !any set only the type */
    out->u = 0;
    out->f = 0;
    out->why[0] = 0;
    uint32_t lit = oylng__lit(R->S, a, s, n);
    for (uint32_t i = 0; i < a->n && (lit == NONE || i < lit); i++) {
        const xt *x = a->p[i].t;
        if (x->kind != X_STD || x->std == STD_ANY) continue;
        int r = match_std(R, x, s, n, plain, out);
        if (r == MATCH) return i;
        if (r == REFUSED) refuse(R, node, "%s", out->why);
    }
    if (lit != NONE) {
        out->type = OYLNG_T_STR;
        return lit;
    }
    if (a->any != NONE) {
        out->type = OYLNG_T_ANY;
        return a->any;
    }
    return NONE;
}

/* Whether a key fits a type key: how it was written doesn't count (section 7) */
static bool key_fits(typer *R, const xt *kt, uint32_t node, oylng_str k) {
    scalar sc;
    return match_alts(R, kt->alts, node, k.data, k.len, true, &sc) != NONE;
}

static uint32_t scalar_val(typer *R, uint32_t node, const scalar *sc, const centry *from) {
    uint32_t v = new_val(R, sc->type, node, from);
    tval *t = &R->Y->v[v];
    t->neg = sc->neg;
    if (sc->type == OYLNG_T_FLOAT) t->x.f = sc->f;
    else t->x.u = sc->u;
    if ((sc->type == OYLNG_T_BIGINT || sc->type == OYLNG_T_DECIMAL) && sc->text) {
        oylng_typed *Y = R->Y;
        if (Y->nb > UINT32_MAX - sc->textlen)
            refuse_as(R, OYLNG_ERR_LIMIT, node, "a typed document of 4 GiB of bytes or more");
        ROOM(R, Y->bytes, Y->capb, Y->nb + sc->textlen);
        memcpy(Y->bytes + Y->nb, sc->text, sc->textlen);
        t = &Y->v[v];
        t->own = 1;
        t->a = (uint32_t)Y->nb;
        t->x.n = sc->textlen;
        Y->nb += sc->textlen;
    }
    if (sc->type == OYLNG_T_BINARY) {
        oylng_typed *Y = R->Y;
        oylng_str s = oylng_node_text(R->T, node);
        if (Y->nb > UINT32_MAX - (s.len / 4 * 3 + 3))
            refuse_as(R, OYLNG_ERR_LIMIT, node, "a typed document of 4 GiB of bytes or more");
        ROOM(R, Y->bytes, Y->capb, Y->nb + s.len / 4 * 3 + 3);
        size_t len;
        oylng__base64(s.data, s.len, Y->bytes + Y->nb, &len);
        t = &Y->v[v];
        t->a = (uint32_t)Y->nb;
        t->x.n = len;
        Y->nb += len;
    }
    return v;
}

/* ── Collections (sections 6, 7 and 9) ───────────────────── */

static uint32_t open_coll(typer *R, oylng_type type, uint32_t node, const xt *x, const centry *from) {
    uint32_t v = new_val(R, type, node, from);
    oylng_typed *Y = R->Y;
    size_t n = oylng_node_count(R->T, node);
    if (n > NONE - Y->nk) refuse_as(R, OYLNG_ERR_LIMIT, node, "a typed document of 4G values or more");
    ROOM(R, Y->kids, Y->capk, Y->nk + n);
    Y->v[v].a = (uint32_t)Y->nk;
    for (size_t i = 0; i < n; i++) Y->kids[Y->nk + i] = NONE;
    Y->nk += n;
    ROOM(R, R->f, R->capf, R->nf + 1);
    R->f[R->nf++] = (frame){ node, v, x, 0, (uint32_t)n };
    return v;
}

static uint32_t open_map(typer *R, const xpos *p, uint32_t node) {
    const xt *m = p->t;
    if (m->u.map.nreq) {
        size_t n = oylng_node_count(R->T, node);
        uint32_t have = 0;
        for (size_t i = 0; i < n; i++) {
            const xfield *f = oylng__field(R->S, m, oylng_node_text(R->T, oylng_node_key(R->T, node, i)).data,
                                           oylng_node_text(R->T, oylng_node_key(R->T, node, i)).len);
            have += f && !f->opt;
        }
        if (have < m->u.map.nreq)
            for (uint32_t i = 0; i < m->u.map.nf; i++) {
                const xfield *f = &m->u.map.f[i];
                if (!f->opt && oylng__node_find(R->T, node, (oylng_str){ f->name.s, f->name.len }) == NONE)
                    refuse(R, node, "missing %.*s", SHOW(f->name.len), f->name.s);
            }
    }
    return open_coll(R, OYLNG_T_MAPPING, node, m, p->from);
}

static uint32_t open_seq(typer *R, const xpos *p, uint32_t node) {
    const xt *q = p->t;
    size_t n = oylng_node_count(R->T, node);
    if (n < q->u.seq.n || (!q->u.seq.rest && n != q->u.seq.n)) refuse(R, node, "wrong length: %zu", n);
    return open_coll(R, OYLNG_T_SEQUENCE, node, q, p->from);
}

/* Which of a union's mappings fits the mapping at node, by its own keys
 * (section 9): the one, or NONE, and how many in *count */
static uint32_t choose(typer *R, const xalts *a, uint32_t node, uint32_t *count) {
    uint32_t nm = a->nmaps;
    if (nm > R->capok) {
        uint8_t *ok = realloc(R->ok, nm);
        if (!ok) nomem(R);
        R->ok = ok;
        uint32_t *req = realloc(R->req, nm * sizeof *req);
        if (!req) nomem(R);
        R->req = req;
        R->capok = nm;
    }
    memset(R->ok, 1, nm);
    memset(R->req, 0, nm * sizeof *R->req);
    size_t n = oylng_node_count(R->T, node);
    for (size_t i = 0; i < n; i++) {
        uint32_t kn = oylng_node_key(R->T, node, i), vn = oylng_node_value(R->T, node, i);
        oylng_str k = oylng_node_text(R->T, kn);
        for (uint32_t j = 0; j < nm; j++) {
            if (!R->ok[j]) continue;
            const xt *m = a->p[a->maps[j]].t;
            const xfield *f = oylng__field(R->S, m, k.data, k.len);
            if (f) {
                R->req[j] += !f->opt;
                if (f->type->alts->all_lits) {
                    oylng_str v = oylng_node_text(R->T, vn);
                    if (oylng_node_kind(R->T, vn) != OYLNG_NODE_STRING ||
                        oylng__lit(R->S, f->type->alts, v.data, v.len) == NONE)
                        R->ok[j] = 0;
                }
                continue;
            }
            if (m->u.map.other) continue;
            bool taken = false;
            for (uint32_t r = 0; r < m->u.map.nrows && !taken; r++) taken = key_fits(R, m->u.map.rows[r].key, kn, k);
            if (!taken) R->ok[j] = 0;
        }
    }
    uint32_t fit = NONE;
    *count = 0;
    for (uint32_t j = 0; j < nm; j++)
        if (R->ok[j] && R->req[j] == a->p[a->maps[j]].t->u.map.nreq) {
            (*count)++;
            fit = a->maps[j];
        }
    return *count == 1 ? fit : NONE;
}

/* The value at node, typed with the position p of a union */
static uint32_t one(typer *R, const xpos *p, uint32_t node) {
    const xt *x = p->t;
    oylng_kind k = oylng_node_kind(R->T, node);
    switch (x->kind) {
    case X_LIT: {
        oylng_str s = oylng_node_text(R->T, node);
        if (k != OYLNG_NODE_STRING || s.len != x->u.lit.len || (s.len && memcmp(s.data, x->u.lit.s, s.len)))
            refuse(R, node, "expected '%.*s'", SHOW(x->u.lit.len), x->u.lit.s);
        scalar sc = { .type = OYLNG_T_STR };
        return scalar_val(R, node, &sc, p->from);
    }
    case X_MAP:
        if (k != OYLNG_NODE_MAPPING) refuse(R, node, "expected a mapping");
        return open_map(R, p, node);
    case X_SEQ:
        if (k != OYLNG_NODE_SEQUENCE) refuse(R, node, "expected a sequence");
        return open_seq(R, p, node);
    default:
        break;
    }
    if (x->std == STD_ANY) return new_val(R, OYLNG_T_ANY, node, p->from);
    if (k != OYLNG_NODE_STRING) refuse(R, node, "expected !%s", oylng__std_names[x->std]);
    oylng_str s = oylng_node_text(R->T, node);
    scalar sc;
    int r = match_std(R, x, s.data, s.len, oylng_node_plain(R->T, node), &sc);
    if (r != MATCH) refuse(R, node, "%s", sc.why);
    return scalar_val(R, node, &sc, p->from);
}

static uint32_t convert(typer *R, const xt *t, uint32_t node) {
    const xalts *a = t->alts;
    if (a->n == 1) return one(R, &a->p[0], node);
    oylng_kind k = oylng_node_kind(R->T, node);
    if (k == OYLNG_NODE_MAPPING) {
        uint32_t count;
        uint32_t fit = a->nmaps ? choose(R, a, node, &count) : (count = 0, NONE);
        if (fit != NONE) return open_map(R, &a->p[fit], node);
        if (!count && a->any != NONE) return new_val(R, OYLNG_T_ANY, node, a->p[a->any].from);
        refuse(R, node, "%u of %u mapping alternatives fit", count, a->nmaps);
    }
    if (k == OYLNG_NODE_SEQUENCE) {
        if (a->seq != NONE) return open_seq(R, &a->p[a->seq], node);
        if (a->any != NONE) return new_val(R, OYLNG_T_ANY, node, a->p[a->any].from);
        refuse(R, node, "a sequence is not allowed here");
    }
    oylng_str s = oylng_node_text(R->T, node);
    scalar sc;
    uint32_t i = match_alts(R, a, node, s.data, s.len, oylng_node_plain(R->T, node), &sc);
    if (i == NONE) refuse(R, node, "'%.*s' is not allowed here", SHOW(s.len), s.data);
    if (sc.type == OYLNG_T_ANY) return new_val(R, OYLNG_T_ANY, node, a->p[i].from);
    return scalar_val(R, node, &sc, a->p[i].from);
}

/* A node's own size, as held counts it (section 11) */
static uint64_t own_size(const oylng_tree *T, uint32_t node) {
    oylng_kind k = oylng_node_kind(T, node);
    if (k == OYLNG_NODE_STRING) return 1 + (uint64_t)oylng_node_text(T, node).len;
    if (k == OYLNG_NODE_SEQUENCE) return 1;
    uint64_t n = 1;
    size_t c = oylng_node_count(T, node);
    for (size_t i = 0; i < c; i++) n += 1 + (uint64_t)oylng_node_text(T, oylng_node_key(T, node, i)).len;
    return n;
}

static size_t memo_slot(const typer *R, uint32_t node, const xt *k) {
    uint64_t h = ((uint64_t)node * 0x9E3779B97F4A7C15u) ^ ((uintptr_t)k >> 4) * 0xC2B2AE3D27D4EB4Fu;
    return (size_t)(h >> 17) & (R->capmemo - 1);
}

/* The value at node typed with t: once for each type, if node is shared */
static uint32_t conv(typer *R, const xt *t, uint32_t node) {
    const xt *k = t->kind_of;
    bool shared = oylng__node_shared(R->T, node);
    if (shared && R->capmemo) {
        for (size_t i = memo_slot(R, node, k);; i = (i + 1) & (R->capmemo - 1)) {
            if (!R->memo[i].k) break;
            if (R->memo[i].node == node && R->memo[i].k == k) return R->memo[i].val;
        }
    }
    R->typed += own_size(R->T, node);
    if (R->typed > R->limit)
        refuse_as(R, OYLNG_ERR_LIMIT, node, "typed passes the size limit of %llu", (unsigned long long)R->limit);
    uint32_t v = convert(R, t, node);
    if (shared) {
        if (2 * (R->nmemo + 1) > R->capmemo) {
            size_t nc = R->capmemo ? R->capmemo * 2 : 256;
            struct memo *old = R->memo;
            size_t oc = R->capmemo;
            R->memo = calloc(nc, sizeof *R->memo);
            if (!R->memo) { R->memo = old; nomem(R); }
            R->capmemo = nc;
            for (size_t i = 0; i < oc; i++)
                if (old[i].k) {
                    size_t j = memo_slot(R, old[i].node, old[i].k);
                    while (R->memo[j].k) j = (j + 1) & (nc - 1);
                    R->memo[j] = old[i];
                }
            free(old);
        }
        size_t j = memo_slot(R, node, k);
        while (R->memo[j].k) j = (j + 1) & (R->capmemo - 1);
        R->memo[j] = (struct memo){ node, v, k };
        R->nmemo++;
    }
    return v;
}

/* The next child of the innermost collection being typed */
static uint32_t child(typer *R, frame *f) {
    const xt *x = f->x;
    if (x->kind == X_SEQ) {
        uint32_t item = oylng_node_item(R->T, f->node, f->i);
        return conv(R, f->i < x->u.seq.n ? x->u.seq.items[f->i] : x->u.seq.rest, item);
    }
    uint32_t kn = oylng_node_key(R->T, f->node, f->i), vn = oylng_node_value(R->T, f->node, f->i);
    oylng_str k = oylng_node_text(R->T, kn);
    const xfield *fd = oylng__field(R->S, x, k.data, k.len);
    if (fd) {
        /* an optional key with no value is null (section 7) */
        if (fd->opt && oylng_node_kind(R->T, vn) == OYLNG_NODE_STRING && oylng_node_plain(R->T, vn) &&
            !oylng_node_text(R->T, vn).len)
            return new_val(R, OYLNG_T_NULL, vn, NULL);
        return conv(R, fd->type, vn);
    }
    for (uint32_t r = 0; r < x->u.map.nrows; r++)
        if (key_fits(R, x->u.map.rows[r].key, kn, k)) return conv(R, x->u.map.rows[r].val, vn);
    if (x->u.map.other) return conv(R, x->u.map.other, vn);
    refuse(R, kn, "unknown key %.*s", SHOW(k.len), k.data);
}

static bool type_tree(typer *R, const xt *root) {
    if (setjmp(R->jb)) return false;
    R->Y->root = conv(R, root, oylng_tree_root(R->T));
    while (R->nf) {
        size_t fi = R->nf - 1;
        frame *f = &R->f[fi];
        if (f->i == f->n) {
            /* a collection typed: its parent goes on to its next child */
            if (--R->nf) R->f[R->nf - 1].i++;
            continue;
        }
        uint32_t v = child(R, f);
        f = &R->f[fi];   /* the frames may have moved */
        R->Y->kids[R->Y->v[f->val].a + f->i] = v;
        if (R->nf == fi + 1) f->i++;   /* else the child is a collection, being typed */
    }
    return true;
}

/* ── Reading a typed document ────────────────────────────── */

struct start {
    oylng_parser       *P;
    const oylng_schema *app;
    const sctx         *S;
    const xt           *root;
    oylng_typed_by      how;
};

/* At a document's start: the schema its stamp names, compiled and checked
 * before its data is read (section 2), or the application's */
static oylng_status at_start(void *ctx, oylng_str name) {
    struct start *c = ctx;
    oylng_parser *P = c->P;
    if (name.data && !memchr(name.data, '/', name.len)) {
        sctx *S = oylng__parser_ctx(P);
        uint32_t count = oylng__table_count(P);
        if (S && S->std_used) {
            /* an entry since named like a standard type changes what a
               name means, so the schema is compiled again */
            for (uint32_t i = S->entries_seen; i < count && S; i++) {
                const char *k;
                size_t kl;
                oylng__table_entry_at(P, i, &k, &kl);
                for (int s = 0; s <= STD_ANY; s++)
                    if ((S->std_used & 1u << s) && strlen(oylng__std_names[s]) == kl &&
                        memcmp(oylng__std_names[s], k, kl) == 0) {
                        oylng__parser_set_ctx(P, NULL, NULL);
                        S = NULL;
                        break;
                    }
            }
        }
        if (!S) {
            S = oylng__sctx_new(P, true);
            if (!S) return oylng__parser_fail(P, OYLNG_ERR_MEMORY, "out of memory");
            oylng__parser_set_ctx(P, S, oylng__sctx_free);
        }
        S->entries_seen = count;
        bool found;
        const xt *root = oylng__sctx_root(S, name.data, name.len, &found);
        if (found && !root) return oylng__parser_fail_at(P, S->st, S->errpos, S->msg);
        if (found) {
            c->S = S;
            c->root = root;
            c->how = OYLNG_TYPED_STAMP;
            return OYLNG_OK;
        }
    }
    if (c->app) {
        c->S = c->app->ctx;
        c->root = c->app->root;
        c->how = OYLNG_TYPED_APP;
    }
    return OYLNG_OK;
}

oylng_status oylng_parse_typed(oylng_parser *P, const oylng_schema *app, oylng_typed **out) {
    if (!out) return OYLNG_ERR_INPUT;
    *out = NULL;
    if (!P || (app && (app->st != OYLNG_OK || !app->root))) return OYLNG_ERR_INPUT;
    struct start c = { P, app, NULL, NULL, OYLNG_TYPED_NOT };
    oylng_tree *T;
    oylng_status st = oylng__tree_read(P, &T, at_start, &c);
    if (st != OYLNG_OK || !T) return st;
    oylng_typed *Y = calloc(1, sizeof *Y);
    if (!Y) {
        oylng_tree_free(T);
        return oylng__parser_fail(P, OYLNG_ERR_MEMORY, "out of memory");
    }
    Y->T = T;
    Y->how = c.how;
    typer R = { .Y = Y, .S = c.S, .T = T, .P = P, .cut = oylng__parser_cut(P), .limit = oylng__parser_counts_limit(P) };
    bool ok;
    if (!c.root) {
        ok = !setjmp(R.jb);
        if (ok) Y->root = new_val(&R, OYLNG_T_ANY, oylng_tree_root(T), NULL);
    } else ok = type_tree(&R, c.root);
    free(R.f);
    free(R.memo);
    free(R.nms);
    free(R.ok);
    free(R.req);
    free(R.canon);
    oylng__rxscr_free(&R.rxs);
    if (!ok) {
        oylng_typed_free(Y);
        return R.st;
    }
    *out = Y;
    return OYLNG_OK;
}

/* ── Public API ──────────────────────────────────────────── */

void oylng_typed_free(oylng_typed *Y) {
    if (!Y) return;
    oylng_tree_free(Y->T);
    free(Y->v);
    free(Y->kids);
    free(Y->bytes);
    free(Y->names);
    free(Y->pool);
    free(Y);
}

const oylng_tree *oylng_typed_tree(const oylng_typed *Y) { return Y ? Y->T : NULL; }
oylng_typed_by oylng_typed_how(const oylng_typed *Y) { return Y ? Y->how : OYLNG_TYPED_NOT; }
oylng_value oylng_typed_root(const oylng_typed *Y) { return Y ? Y->root : OYLNG_NO_VALUE; }

static const tval *val(const oylng_typed *Y, oylng_value v) { return Y && v < Y->nv ? &Y->v[v] : NULL; }

oylng_type oylng_value_type(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    return t ? (oylng_type)t->type : OYLNG_T_NONE;
}

oylng_node oylng_value_node(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    return t ? t->node : OYLNG_NO_NODE;
}

oylng_str oylng_value_text(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    if (t && t->own) return (oylng_str){ (const char *)Y->bytes + t->a, (size_t)t->x.n };
    return t ? oylng_node_text(Y->T, t->node) : (oylng_str){ NULL, 0 };
}

oylng_str oylng_value_name(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    if (!t || t->name == NONE) return (oylng_str){ NULL, 0 };
    return (oylng_str){ Y->pool + Y->names[t->name].off, Y->names[t->name].len };
}

bool oylng_value_int(const oylng_typed *Y, oylng_value v, int64_t *out) {
    const tval *t = val(Y, v);
    if (!t || t->type != OYLNG_T_INT || !out) return false;
    if (t->neg) {
        if (t->x.u > (uint64_t)INT64_MAX + 1) return false;
        *out = t->x.u == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)t->x.u;
        return true;
    }
    if (t->x.u > INT64_MAX) return false;
    *out = (int64_t)t->x.u;
    return true;
}

bool oylng_value_uint(const oylng_typed *Y, oylng_value v, uint64_t *out) {
    const tval *t = val(Y, v);
    if (!t || t->type != OYLNG_T_INT || t->neg || !out) return false;
    *out = t->x.u;
    return true;
}

double oylng_value_float(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    return t && t->type == OYLNG_T_FLOAT ? t->x.f : 0;
}

bool oylng_value_bool(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    return t && t->type == OYLNG_T_BOOL && t->x.u;
}

oylng_str oylng_value_bytes(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    if (!t || t->type != OYLNG_T_BINARY) return (oylng_str){ NULL, 0 };
    return (oylng_str){ Y->bytes ? (const char *)Y->bytes + t->a : "", (size_t)t->x.n };
}

bool oylng_value_datetime(const oylng_typed *Y, oylng_value v, oylng_datetime *out) {
    const tval *t = val(Y, v);
    if (!t || (t->type != OYLNG_T_DATE && t->type != OYLNG_T_DATETIME) || !out) return false;
    oylng_str s = oylng_node_text(Y->T, t->node);
    return oylng__date(s.data, s.len, t->type == OYLNG_T_DATETIME, true, out) == 1;
}

size_t oylng_value_count(const oylng_typed *Y, oylng_value v) {
    const tval *t = val(Y, v);
    return t && (t->type == OYLNG_T_SEQUENCE || t->type == OYLNG_T_MAPPING) ? oylng_node_count(Y->T, t->node) : 0;
}

oylng_value oylng_value_item(const oylng_typed *Y, oylng_value seq, size_t i) {
    const tval *t = val(Y, seq);
    if (!t || t->type != OYLNG_T_SEQUENCE || i >= oylng_node_count(Y->T, t->node)) return OYLNG_NO_VALUE;
    return Y->kids[t->a + i];
}

oylng_str oylng_value_key(const oylng_typed *Y, oylng_value map, size_t i) {
    const tval *t = val(Y, map);
    if (!t || t->type != OYLNG_T_MAPPING) return (oylng_str){ NULL, 0 };
    return oylng_node_text(Y->T, oylng_node_key(Y->T, t->node, i));
}

oylng_value oylng_value_at(const oylng_typed *Y, oylng_value map, size_t i) {
    const tval *t = val(Y, map);
    if (!t || t->type != OYLNG_T_MAPPING || i >= oylng_node_count(Y->T, t->node)) return OYLNG_NO_VALUE;
    return Y->kids[t->a + i];
}

oylng_value oylng_value_get(const oylng_typed *Y, oylng_value map, oylng_str key) {
    const tval *t = val(Y, map);
    if (!t || t->type != OYLNG_T_MAPPING) return OYLNG_NO_VALUE;
    uint32_t i = oylng__node_find(Y->T, t->node, key);
    return i == NONE ? OYLNG_NO_VALUE : Y->kids[t->a + i];
}
