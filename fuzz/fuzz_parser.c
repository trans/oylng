/*
 * fuzz_parser.c — libFuzzer harness for the parser
 *
 * The first byte sets options: its low bits a size limit (from a few bytes
 * up to the default), its high bit 300 levels instead of 256, and the bits
 * between how the emitter writes the stream again: 0x40 flow style, and
 * bits 3 to 5 the indent. The rest is the stream. Every event sequence must
 * be well formed: the stream and each document, mapping and sequence
 * close, in order, and a mapping holds pairs. A stream that is read
 * without error is written again by the emitter, which must accept it, and
 * what it writes must read back as the same events. Read as trees, the
 * stream must give the same events, or the same error, each key's value
 * must be found, and the trees must be written as the events were. After
 * each tree, the table's tree must find each entry's value by its key, and
 * after an error, give the error. Read with references kept (section 10),
 * no events are given; the stream must be accepted where the events were,
 * and its kept trees, spread under a budget, read as the trees did; where
 * the events were refused for the grammar, it must be refused alike; each
 * -<< must name a sequence, and each <<: line a mapping. Read with its
 * documents together, the sizes add up, so it may be accepted only where
 * the events were, and refused for the grammar only alike. Typed with stamps ignored and no
 * application's schema, the stream must read as the trees did. Typed
 * (part 2), by its stamps, and by the stream's own table as an application's
 * schema rooted at `a` when it compiles as one, the stream must be refused
 * where the events were, or for a schema; each typed value must be what its
 * type says, each key's value found; every third document's stamp is
 * ignored, and it must not be typed by it. The flow bit also turns on the cut.
 * Typed so, the names the stream writes as !name are bound, up to 32 of
 * them, and a few others (section 12): the binding of a value finds
 * the objects of the bound values just below it made, a value written at
 * an offset of 5 mod 8 is refused, each bound value has its object, at one place only,
 * and every object made is let go.
 * A document the application's schema typed, or that nothing typed, is
 * written again with that schema (section 13), under a size budget: in
 * block style it must be accepted, and in flow refused only for a value
 * with no spelling there, and it must read back the same.
 * Where the stream holds a NUL, what comes before it is also a published
 * file (section 3.1), and what comes after a stream typed with a lookup
 * that gives it, once each #sha256= and the 64 hex digits after it are set
 * to the file's hash: a published document must be typed as its type says,
 * and a refusal must say why.
 *
 *     clang -fsanitize=fuzzer,address,undefined -Iinclude src/oylng_*.c fuzz/fuzz_parser.c
 */

#include "oylng/oylng.h"
#include "oylng/schema.h"
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A hash of the events, their types and values, in order */
static uint64_t ev_hash(uint64_t h, const oylng_event *e) {
    h = (h ^ (uint64_t)e->type) * 0x100000001B3u;
    h = (h ^ (uint64_t)e->value.len) * 0x100000001B3u;
    h = (h ^ (uint64_t)(e->value.data != NULL)) * 0x100000001B3u;
    for (size_t i = 0; i < e->value.len; i++) h = (h ^ (unsigned char)e->value.data[i]) * 0x100000001B3u;
    return h;
}

/* The same, of what a tree holds: no offsets, and only a name's data may
 * be NULL */
static uint64_t tv_hash(uint64_t h, oylng_event_type type, oylng_str v) {
    h = (h ^ (uint64_t)type) * 0x100000001B3u;
    h = (h ^ (uint64_t)v.len) * 0x100000001B3u;
    if (type == OYLNG_EVT_DOC_START) h = (h ^ (uint64_t)(v.data != NULL)) * 0x100000001B3u;
    for (size_t i = 0; i < v.len; i++) h = (h ^ (unsigned char)v.data[i]) * 0x100000001B3u;
    return h;
}

static uint64_t tree_hash(uint64_t h, const oylng_tree *t, oylng_node n) {
    static const oylng_str none = { NULL, 0 };
    oylng_kind k = oylng_node_kind(t, n);
    if (k == OYLNG_NODE_STRING) {
        oylng_str v = oylng_node_text(t, n);
        if (!v.data) abort();
        return tv_hash(h, OYLNG_EVT_SCALAR, v);
    }
    if (k != OYLNG_NODE_MAPPING && k != OYLNG_NODE_SEQUENCE) abort();
    bool map = k == OYLNG_NODE_MAPPING;
    h = tv_hash(h, map ? OYLNG_EVT_MAP_START : OYLNG_EVT_SEQ_START, none);
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++) {
        if (map) {
            oylng_node key = oylng_node_key(t, n, i), v = oylng_node_value(t, n, i);
            if (oylng_node_get(t, n, oylng_node_text(t, key)) != v) abort();
            h = tree_hash(tree_hash(h, t, key), t, v);
        } else h = tree_hash(h, t, oylng_node_item(t, n, i));
    }
    if (oylng_node_item(t, n, c) != OYLNG_NO_NODE || oylng_node_key(t, n, c) != OYLNG_NO_NODE) abort();
    return tv_hash(h, map ? OYLNG_EVT_MAP_END : OYLNG_EVT_SEQ_END, none);
}

/* A kept tree's mapping's pairs, as the data has them (section 9.1): each
 * <<: line's, then each written one, added at the end, or in place of the
 * value of a key there, a written key replacing the key too. False past the
 * budget, which each key compared takes from. */
typedef struct { oylng_node k, v; } kpair;
typedef struct { kpair *p; size_t n, cap; } kpairs;

static bool kp_put(const oylng_tree *t, kpairs *ps, oylng_node k, oylng_node v, bool written, size_t *budget) {
    oylng_str kt = oylng_node_text(t, k);
    if (!kt.data && kt.len) abort();
    for (size_t i = 0; i < ps->n; i++) {
        if (!*budget) return false;
        --*budget;
        oylng_str o = oylng_node_text(t, ps->p[i].k);
        if (o.len == kt.len && (!o.len || memcmp(o.data, kt.data, o.len) == 0)) {
            if (written) ps->p[i].k = k;
            ps->p[i].v = v;
            return true;
        }
    }
    if (ps->n == ps->cap) {
        size_t nc = ps->cap ? 2 * ps->cap : 8;
        kpair *np = realloc(ps->p, nc * sizeof *np);
        if (!np) return false;
        ps->p = np;
        ps->cap = nc;
    }
    ps->p[ps->n++] = (kpair){ k, v };
    return true;
}

static bool kept_pairs(const oylng_tree *t, oylng_node map, kpairs *ps, size_t *budget) {
    size_t nm = oylng_node_merges(t, map);
    if (oylng_node_merge(t, map, nm) != OYLNG_NO_NODE) abort();
    for (size_t i = 0; i < nm; i++) {
        oylng_node g = oylng_node_merge(t, map, i), src = oylng_node_target(t, g);
        if (oylng_node_kind(t, g) != OYLNG_NODE_MERGE || oylng_node_kind(t, src) != OYLNG_NODE_MAPPING) abort();
        kpairs sp = { 0 };
        bool ok = kept_pairs(t, src, &sp, budget);
        for (size_t j = 0; ok && j < sp.n; j++) ok = kp_put(t, ps, sp.p[j].k, sp.p[j].v, false, budget);
        free(sp.p);
        if (!ok) return false;
    }
    for (size_t i = 0; i < oylng_node_count(t, map); i++) {
        oylng_node k = oylng_node_key(t, map, i), v = oylng_node_value(t, map, i);
        if (oylng_node_kind(t, k) != OYLNG_NODE_STRING || oylng_node_get(t, map, oylng_node_text(t, k)) != v) abort();
        if (!kp_put(t, ps, k, v, true, budget)) return false;
    }
    return true;
}

static uint64_t kept_hash(uint64_t h, const oylng_tree *t, oylng_node n, size_t *budget);

/* A kept sequence's items, each -<< spread */
static uint64_t kept_items(uint64_t h, const oylng_tree *t, oylng_node seq, size_t *budget) {
    size_t c = oylng_node_count(t, seq);
    for (size_t i = 0; i < c && *budget; i++) {
        oylng_node it = oylng_node_item(t, seq, i);
        if (oylng_node_kind(t, it) != OYLNG_NODE_SPLICE) {
            h = kept_hash(h, t, it, budget);
            continue;
        }
        oylng_node src = oylng_node_target(t, it);
        if (oylng_node_kind(t, src) != OYLNG_NODE_SEQUENCE || oylng_node_count(t, it) || oylng_node_text(t, it).data)
            abort();
        --*budget;
        h = kept_items(h, t, src, budget);
    }
    if (oylng_node_item(t, seq, c) != OYLNG_NO_NODE) abort();
    return h;
}

/* A kept tree's node, spread, hashed as tree_hash hashes a tree's; the
 * budget 0 if it took more visits than that */
static uint64_t kept_hash(uint64_t h, const oylng_tree *t, oylng_node n, size_t *budget) {
    static const oylng_str none = { NULL, 0 };
    if (!*budget) return h;
    --*budget;
    oylng_kind k = oylng_node_kind(t, n);
    if (oylng_node_target(t, n) != OYLNG_NO_NODE) abort();   /* a -<< or <<: line is never a value */
    if (k == OYLNG_NODE_STRING) {
        if (oylng_node_merges(t, n)) abort();
        return tree_hash(h, t, n);
    }
    if (k == OYLNG_NODE_SEQUENCE) {
        if (oylng_node_merges(t, n)) abort();
        h = tv_hash(h, OYLNG_EVT_SEQ_START, none);
        h = kept_items(h, t, n, budget);
        return tv_hash(h, OYLNG_EVT_SEQ_END, none);
    }
    if (k != OYLNG_NODE_MAPPING) abort();
    kpairs ps = { 0 };
    bool ok = kept_pairs(t, n, &ps, budget);
    h = tv_hash(h, OYLNG_EVT_MAP_START, none);
    for (size_t i = 0; ok && i < ps.n && *budget; i++) h = kept_hash(kept_hash(h, t, ps.p[i].k, budget), t, ps.p[i].v, budget);
    free(ps.p);
    if (!ok) *budget = 0;
    return tv_hash(h, OYLNG_EVT_MAP_END, none);
}

/* The table's tree: each entry's value found by its key; after an error,
 * the error */
static void check_table(oylng_parser *p, oylng_status st) {
    oylng_tree *t = (oylng_tree *)1;
    oylng_status ts = oylng_parser_table(p, &t);
    if (ts == OYLNG_ERR_MEMORY) return;
    if (st != OYLNG_OK) {
        if (ts != st || t) abort();
        return;
    }
    if (ts != OYLNG_OK) abort();
    if (!t) return;
    oylng_node r = oylng_tree_root(t);
    if (oylng_node_kind(t, r) != OYLNG_NODE_MAPPING || !oylng_node_count(t, r)) abort();
    for (size_t i = 0; i < oylng_node_count(t, r); i++)
        if (oylng_node_get(t, r, oylng_node_text(t, oylng_node_key(t, r, i))) != oylng_node_value(t, r, i)) abort();
    oylng_tree_free(t);
}

/* Each typed value is what its type says. A typed document shares a value
 * reached twice, so the walk stops after a budget of visits. */
static void check_value(const oylng_typed *t, oylng_value v, size_t *budget) {
    if (!*budget) return;
    --*budget;
    oylng_type ty = oylng_value_type(t, v);
    oylng_node n = oylng_value_node(t, v);
    const oylng_tree *tr = oylng_typed_tree(t);
    oylng_kind k = oylng_node_kind(tr, n);
    oylng_str s = oylng_value_text(t, v);
    size_t c = oylng_value_count(t, v);
    int64_t i64;
    uint64_t u64;
    oylng_datetime dt;
    switch (ty) {
    case OYLNG_T_ANY:
        if (k == OYLNG_NODE_NONE) abort();
        return;
    case OYLNG_T_SEQUENCE:
        if (k != OYLNG_NODE_SEQUENCE || c != oylng_node_count(tr, n)) abort();
        for (size_t i = 0; i < c; i++) {
            oylng_value x = oylng_value_item(t, v, i);
            if (x == OYLNG_NO_VALUE) abort();
            check_value(t, x, budget);
        }
        if (oylng_value_item(t, v, c) != OYLNG_NO_VALUE) abort();
        return;
    case OYLNG_T_MAPPING:
        if (k != OYLNG_NODE_MAPPING || c != oylng_node_count(tr, n)) abort();
        for (size_t i = 0; i < c; i++) {
            oylng_str key = oylng_value_key(t, v, i);
            oylng_value x = oylng_value_at(t, v, i);
            if (!key.data || x == OYLNG_NO_VALUE || oylng_value_get(t, v, key) != x) abort();
            check_value(t, x, budget);
        }
        if (oylng_value_at(t, v, c) != OYLNG_NO_VALUE) abort();
        return;
    default:
        break;
    }
    /* a scalar */
    if (k != OYLNG_NODE_STRING || !s.data || c) abort();
    if (s.len) { volatile char last = s.data[s.len - 1]; (void)last; }
    switch (ty) {
    case OYLNG_T_STR: case OYLNG_T_BIGINT: case OYLNG_T_DECIMAL: break;
    case OYLNG_T_INT:
        if (!oylng_value_int(t, v, &i64) && !oylng_value_uint(t, v, &u64)) abort();
        break;
    case OYLNG_T_FLOAT:
        if (!isfinite(oylng_value_float(t, v))) abort();
        break;
    case OYLNG_T_BOOL:
        if (!(s.len == 4 && !memcmp(s.data, "true", 4) && oylng_value_bool(t, v)) &&
            !(s.len == 5 && !memcmp(s.data, "false", 5) && !oylng_value_bool(t, v)))
            abort();
        break;
    case OYLNG_T_NULL:   /* "null", empty for an optional key, or what a pattern took */
        break;
    case OYLNG_T_DATE: case OYLNG_T_DATETIME:
        if (!oylng_value_datetime(t, v, &dt) || dt.year < 0 || dt.year > 9999 || dt.month < 1 ||
            dt.month > 12 || dt.day < 1 || dt.day > 31 || dt.hour > 23 || dt.minute > 59 ||
            dt.second > 60 || dt.nanosecond >= 1000000000u)
            abort();
        break;
    case OYLNG_T_BINARY: {
        oylng_str b = oylng_value_bytes(t, v);
        if (!b.data && b.len) abort();
        if (b.len) { volatile char last = b.data[b.len - 1]; (void)last; }
        break;
    }
    default:
        abort();
    }
}

/* Bindings (section 12) */
static const char *const fixed_names[] = { "a", "b", "c", "job", "point" };
#define NFIXED (sizeof fixed_names / sizeof *fixed_names)
#define NBOUND 40
static oylng_str bound_names[NBOUND];
static size_t nbound;

/* The names to bind: the fixed ones, and each !name the stream writes */
static void find_names(const char *in, size_t size) {
    nbound = 0;
    for (size_t i = 0; i < NFIXED; i++) bound_names[nbound++] = (oylng_str){ fixed_names[i], strlen(fixed_names[i]) };
    for (size_t i = 0; i + 1 < size && nbound < NBOUND; i++) {
        if (in[i] != '!') continue;
        size_t j = i + 1;
        while (j < size && j - i <= 40 && (isalnum((unsigned char)in[j]) || in[j] == '_' || in[j] == '-')) j++;
        if (j == i + 1 || j - i > 40) continue;
        oylng_str n = { in + i + 1, j - i - 1 };
        bool dup = false;
        for (size_t k = 0; k < nbound && !dup; k++)
            dup = bound_names[k].len == n.len && !memcmp(bound_names[k].data, n.data, n.len);
        if (!dup) bound_names[nbound++] = n;
        i = j - 1;
    }
}

typedef struct { const oylng_typed *t; oylng_value v; } fobj;
typedef struct { size_t made, dropped; } fbinder;
typedef struct { fbinder *b; size_t i; } fname;   /* each name's: its binder, and which */
static fname fnames[NBOUND];

static bool same_name(oylng_str a, oylng_str b) {
    return a.data && b.data && a.len == b.len && (!a.len || !memcmp(a.data, b.data, a.len));
}

static bool is_bound(oylng_str n) {
    for (size_t i = 0; n.data && i < nbound; i++)
        if (bound_names[i].len == n.len && !memcmp(bound_names[i].data, n.data, n.len)) return true;
    return false;
}

/* The code of the value's bound name runs, the outermost bound one of
   the names it was typed through, and no other's */
static const char *fuzz_bind(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    fname *f = ctx;
    fbinder *b = f->b;
    if (!same_name(oylng_value_bound(t, v), bound_names[f->i]) || oylng_value_object(t, v)) abort();
    oylng_type ty = oylng_value_type(t, v);
    if (ty == OYLNG_T_SEQUENCE || ty == OYLNG_T_MAPPING)
        for (size_t i = 0; i < oylng_value_count(t, v); i++) {
            oylng_value c = ty == OYLNG_T_SEQUENCE ? oylng_value_item(t, v, i) : oylng_value_at(t, v, i);
            if (oylng_value_bound(t, c).data && !oylng_value_object(t, c)) abort();   /* innermost first */
        }
    if (oylng_node_mark(oylng_typed_tree(t), oylng_value_node(t, v)).offset % 8 == 5) return "refused by the binding";
    fobj *o = malloc(sizeof *o);
    if (!o) return "out of memory";
    *o = (fobj){ t, v };
    b->made++;
    *obj = o;
    return NULL;
}

static void fuzz_drop(void *ctx, void *o) {
    ((fname *)ctx)->b->dropped++;
    free(o);
}

/* Each bound value has its own object, and is at one place only */
static void check_objects(const oylng_typed *t, oylng_value v, uint8_t **seen, size_t *cap, size_t *budget) {
    if (!*budget) return;
    --*budget;
    oylng_str bn = oylng_value_bound(t, v);
    /* the nearest name is one of those typed through: bound, it leaves
       the value a bound name */
    if (is_bound(oylng_value_name(t, v)) && !bn.data) abort();
    if (bn.data && !is_bound(bn)) abort();
    if (bn.data) {
        const fobj *o = oylng_value_object(t, v);
        if (!o || o->t != t || o->v != v) abort();
        if (v >= *cap) {
            size_t nc = *cap ? *cap : 256;
            while (nc <= v) nc *= 2;
            uint8_t *ns = realloc(*seen, nc);
            if (!ns) { *budget = 0; return; }
            memset(ns + *cap, 0, nc - *cap);
            *seen = ns;
            *cap = nc;
        }
        if ((*seen)[v]) abort();
        (*seen)[v] = 1;
    } else if (oylng_value_object(t, v)) abort();
    oylng_type ty = oylng_value_type(t, v);
    if (ty == OYLNG_T_SEQUENCE || ty == OYLNG_T_MAPPING)
        for (size_t i = 0; i < oylng_value_count(t, v); i++)
            check_objects(t, ty == OYLNG_T_SEQUENCE ? oylng_value_item(t, v, i) : oylng_value_at(t, v, i), seen, cap, budget);
}

/* The published file, for any hash: the parser checks it */
/* ── Typed documents written again (section 13) ─────────── */

static bool same_nodes(const oylng_tree *a, oylng_node n, const oylng_tree *b, oylng_node m) {
    oylng_kind k = oylng_node_kind(a, n);
    if (k != oylng_node_kind(b, m)) return false;
    if (k == OYLNG_NODE_STRING) {
        oylng_str x = oylng_node_text(a, n), y = oylng_node_text(b, m);
        return x.len == y.len && (!x.len || !memcmp(x.data, y.data, x.len));
    }
    size_t c = oylng_node_count(a, n);
    if (c != oylng_node_count(b, m)) return false;
    for (size_t i = 0; i < c; i++)
        if (k == OYLNG_NODE_SEQUENCE ? !same_nodes(a, oylng_node_item(a, n, i), b, oylng_node_item(b, m, i))
                                     : !same_nodes(a, oylng_node_key(a, n, i), b, oylng_node_key(b, m, i)) ||
                                           !same_nodes(a, oylng_node_value(a, n, i), b, oylng_node_value(b, m, i)))
            return false;
    return true;
}

static bool same_str(oylng_str x, oylng_str y) { return x.len == y.len && (!x.len || !memcmp(x.data, y.data, x.len)); }

/* Whether two typed values are the same: type, value and name */
static bool same_typed(const oylng_typed *a, oylng_value v, const oylng_typed *b, oylng_value w) {
    oylng_type t = oylng_value_type(a, v);
    if (t != oylng_value_type(b, w)) return false;
    oylng_str na = oylng_value_name(a, v), nb = oylng_value_name(b, w);
    if (!na.data != !nb.data || (na.data && !same_str(na, nb))) return false;
    switch (t) {
    case OYLNG_T_ANY:
        return same_nodes(oylng_typed_tree(a), oylng_value_node(a, v), oylng_typed_tree(b), oylng_value_node(b, w));
    case OYLNG_T_SEQUENCE:
    case OYLNG_T_MAPPING: {
        size_t c = oylng_value_count(a, v);
        if (c != oylng_value_count(b, w)) return false;
        for (size_t i = 0; i < c; i++) {
            if (t == OYLNG_T_MAPPING && !same_str(oylng_value_key(a, v, i), oylng_value_key(b, w, i))) return false;
            if (t == OYLNG_T_SEQUENCE ? !same_typed(a, oylng_value_item(a, v, i), b, oylng_value_item(b, w, i))
                                      : !same_typed(a, oylng_value_at(a, v, i), b, oylng_value_at(b, w, i)))
                return false;
        }
        return true;
    }
    case OYLNG_T_STR:
    case OYLNG_T_BIGINT:
    case OYLNG_T_DECIMAL:
        return same_str(oylng_value_text(a, v), oylng_value_text(b, w));
    case OYLNG_T_INT: {
        int64_t i, j;
        uint64_t u, k;
        bool si = oylng_value_int(a, v, &i), sj = oylng_value_int(b, w, &j);
        if (si || sj) return si && sj && i == j;
        return oylng_value_uint(a, v, &u) && oylng_value_uint(b, w, &k) && u == k;
    }
    case OYLNG_T_FLOAT: {
        double f = oylng_value_float(a, v), g = oylng_value_float(b, w);
        return !memcmp(&f, &g, sizeof f);
    }
    case OYLNG_T_BOOL: return oylng_value_bool(a, v) == oylng_value_bool(b, w);
    case OYLNG_T_NULL: return true;
    case OYLNG_T_DATE:
    case OYLNG_T_DATETIME: {
        oylng_datetime d, e;
        if (!oylng_value_datetime(a, v, &d) || !oylng_value_datetime(b, w, &e)) return false;
        return d.year == e.year && d.month == e.month && d.day == e.day && d.hour == e.hour &&
               d.minute == e.minute && d.second == e.second && d.nanosecond == e.nanosecond &&
               d.offset == e.offset && d.zulu == e.zulu && d.unknown_offset == e.unknown_offset;
    }
    case OYLNG_T_BINARY: return same_str(oylng_value_bytes(a, v), oylng_value_bytes(b, w));
    default: return false;
    }
}

/* Whether node n and what is in it, every place counted, fit the budget */
static bool nodes_fit(const oylng_tree *t, oylng_node n, size_t *budget) {
    if (!*budget) return false;
    --*budget;
    oylng_kind k = oylng_node_kind(t, n);
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++)
        if (!nodes_fit(t, k == OYLNG_NODE_SEQUENCE ? oylng_node_item(t, n, i) : oylng_node_value(t, n, i), budget))
            return false;
    return true;
}

/* The same, of value v */
static bool fits_budget(const oylng_typed *y, oylng_value v, size_t *budget) {
    if (!*budget) return false;
    --*budget;
    oylng_type t = oylng_value_type(y, v);
    if (t == OYLNG_T_ANY) return nodes_fit(oylng_typed_tree(y), oylng_value_node(y, v), budget);
    size_t c = oylng_value_count(y, v);
    for (size_t i = 0; i < c; i++)
        if (!fits_budget(y, t == OYLNG_T_SEQUENCE ? oylng_value_item(y, v, i) : oylng_value_at(y, v, i), budget))
            return false;
    return true;
}

static void write_again(const oylng_typed *y, const oylng_schema *sc, unsigned opt) {
    size_t budget = 2048;
    if (!fits_budget(y, oylng_typed_root(y), &budget)) return;
    for (int flow = 0; flow < 2; flow++) {
        oylng_emitter *e = oylng_emitter_new();
        if (!e) return;
        oylng_emitter_set_flow(e, flow);
        oylng_emitter_set_indent(e, ((opt >> 3) & 7) + 1);
        if (sc && oylng_emitter_set_schema(e, sc) != OYLNG_OK) abort();
        oylng_status st = oylng_emit_stream_start(e);
        if (st == OYLNG_OK) st = oylng_emit_doc_start(e, oylng_tree_name(oylng_typed_tree(y)));
        if (st == OYLNG_OK) st = oylng_emit_value(e, y, oylng_typed_root(y));
        if (st == OYLNG_OK) st = oylng_emit_doc_end(e);
        if (st == OYLNG_OK) st = oylng_emit_stream_end(e);
        if (st != OYLNG_OK) {
            const char *why = oylng_emitter_error(e);
            if (st == OYLNG_ERR_MEMORY) {
                oylng_emitter_free(e);
                return;
            }
            if (!flow || st != OYLNG_ERR_EMIT || !why || !strstr(why, "no spelling of")) abort();
            oylng_emitter_free(e);
            continue;
        }
        oylng_str out = oylng_emitter_output(e);
        char *copy = malloc(out.len ? out.len : 1);
        if (copy) {
            memcpy(copy, out.data, out.len);
            oylng_parser *q = oylng_parser_new(copy, out.len);
            if (q) {
                oylng_parser_set_limit(q, UINT64_MAX);
                oylng_parser_set_levels(q, OYLNG_MAX_LEVELS);   /* references may have made it deeper */
                oylng_parser_set_cut(q, (opt & 0x40) != 0);
                oylng_typed *back = NULL;
                st = oylng_parse_typed(q, sc, &back);
                if (st != OYLNG_ERR_MEMORY &&
                    (st != OYLNG_OK || !back || !same_typed(y, oylng_typed_root(y), back, oylng_typed_root(back))))
                    abort();
                oylng_typed_free(back);
                oylng_parser_free(q);
            }
            free(copy);
        }
        oylng_emitter_free(e);
    }
}

static bool lookup(void *ctx, oylng_str address, oylng_str sha256, oylng_str *file) {
    (void)address;
    (void)sha256;
    *file = *(const oylng_str *)ctx;
    return true;
}

void oylng__sha256(const void *data, size_t len, unsigned char out[32]);

/* FILE NUL STREAM: STREAM typed, its published names looked up in FILE */
static void published(const char *in, size_t size, unsigned opt);

static oylng_parser *parser_for(const char *in, size_t size, unsigned opt) {
    oylng_parser *p = oylng_parser_new(in, size);
    if (!p) return NULL;
    static const uint64_t limits[8] = { 0, 4, 16, 64, 256, 4096, 1 << 16, 0 };
    if (limits[opt & 7]) oylng_parser_set_limit(p, limits[opt & 7]);
    if (opt & 0x80) oylng_parser_set_levels(p, 300);
    return p;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    unsigned opt = data[0];
    data++, size--;

    /* a copy of its own, so reading past the end is caught */
    char *in = malloc(size ? size : 1);
    if (!in) return 0;
    memcpy(in, data, size);

    oylng_parser *p = parser_for(in, size, opt);
    if (!p) { free(in); return 0; }
    oylng_emitter *em = oylng_emitter_new();
    if (!em) { oylng_parser_free(p); free(in); return 0; }
    oylng_emitter_set_flow(em, (opt & 0x40) != 0);
    oylng_emitter_set_indent(em, ((opt >> 3) & 7) + 1);
    uint64_t h1 = 0xCBF29CE484222325u, h3 = h1;
    bool clean = false;

    /* for each open collection, whether it's a mapping and how many nodes
       it has had */
    static unsigned char ismap[OYLNG_MAX_LEVELS + 8];
    static size_t nodes[OYLNG_MAX_LEVELS + 8];
    int depth = 0, in_doc = 0, started = 0;
    const oylng_event *e;
    oylng_status pst;
    for (;;) {
        if ((pst = oylng_parse_next(p, &e)) != OYLNG_OK) {
            if (!oylng_parser_error(p)) abort();
            break;
        }
        h1 = ev_hash(h1, e);
        if (e->type != OYLNG_EVT_STREAM_START && e->type != OYLNG_EVT_STREAM_END)
            h3 = tv_hash(h3, e->type, e->value);
        /* everything the parser delivers, the emitter can write */
        oylng_status est = oylng_emit(em, e);
        if (est != OYLNG_OK && est != OYLNG_ERR_MEMORY) abort();
        switch (e->type) {
        case OYLNG_EVT_STREAM_START:
            if (started) abort();
            started = 1;
            break;
        case OYLNG_EVT_DOC_START:
            if (in_doc || depth) abort();
            in_doc = 1;
            break;
        case OYLNG_EVT_DOC_END:
            if (!in_doc || depth) abort();
            in_doc = 0;
            break;
        case OYLNG_EVT_MAP_START:
        case OYLNG_EVT_SEQ_START:
            if (!in_doc || depth >= OYLNG_MAX_LEVELS) abort();
            if (depth) nodes[depth]++;
            depth++;
            ismap[depth] = e->type == OYLNG_EVT_MAP_START;
            nodes[depth] = 0;
            break;
        case OYLNG_EVT_MAP_END:
        case OYLNG_EVT_SEQ_END:
            if (!depth || ismap[depth] != (e->type == OYLNG_EVT_MAP_END)) abort();
            if (ismap[depth] && nodes[depth] % 2) abort();
            depth--;
            break;
        case OYLNG_EVT_SCALAR:
            if (!in_doc) abort();
            if (depth) nodes[depth]++;
            if (e->value.len && !e->value.data) abort();
            if (e->value.len) { volatile char c = e->value.data[e->value.len - 1]; (void)c; }
            break;
        case OYLNG_EVT_STREAM_END:
            if (in_doc || depth) abort();
            break;
        default:
            abort();
        }
        if (e->type == OYLNG_EVT_STREAM_END) { clean = true; break; }
    }

    /* read as trees, the same, or the same error; written, the same text */
    oylng_parser *tp = parser_for(in, size, opt);
    oylng_emitter *te = oylng_emitter_new();
    if (tp && te) {
        oylng_emitter_set_flow(te, (opt & 0x40) != 0);
        oylng_emitter_set_indent(te, ((opt >> 3) & 7) + 1);
        oylng_emit_stream_start(te);
        uint64_t h4 = 0xCBF29CE484222325u;
        oylng_tree *t;
        oylng_status st;
        while ((st = oylng_parse_tree(tp, &t)) == OYLNG_OK && t) {
            h4 = tv_hash(h4, OYLNG_EVT_DOC_START, oylng_tree_name(t));
            h4 = tree_hash(h4, t, oylng_tree_root(t));
            h4 = tv_hash(h4, OYLNG_EVT_DOC_END, (oylng_str){ NULL, 0 });
            oylng_status est = oylng_emit_tree(te, t);
            if (est != OYLNG_OK && est != OYLNG_ERR_MEMORY) abort();
            oylng_tree_free(t);
            check_table(tp, OYLNG_OK);
        }
        check_table(tp, st);
        if (st == OYLNG_ERR_MEMORY || pst == OYLNG_ERR_MEMORY) {
            /* one of them ran out where the other didn't */
        } else if (clean) {
            if (st != OYLNG_OK || h4 != h3) abort();
            oylng_emit_stream_end(te);
            oylng_str a = oylng_emitter_output(em), b = oylng_emitter_output(te);
            if (!oylng_emitter_error(em) && !oylng_emitter_error(te) &&
                (a.len != b.len || memcmp(a.data, b.data, a.len) != 0))
                abort();
        } else {
            const char *x = oylng_parser_error(p), *y = oylng_parser_error(tp);
            if (st == OYLNG_OK || !x || !y || strcmp(x, y) != 0 ||
                oylng_parser_error_mark(p).offset != oylng_parser_error_mark(tp).offset)
                abort();
        }
    }
    oylng_emitter_free(te);
    oylng_parser_free(tp);

    /* with references kept: no events; accepted where the events were, and
       spread, the same; refused for the grammar where they were, alike */
    oylng_parser *kp = parser_for(in, size, opt);
    if (kp) {
        oylng_parser_set_keep(kp, true);
        const oylng_event *ke;
        if (oylng_parse_next(kp, &ke) != OYLNG_ERR_INPUT || ke) abort();
        uint64_t h6 = 0xCBF29CE484222325u;
        size_t budget = 1 << 20;
        oylng_tree *t;
        oylng_status st;
        while ((st = oylng_parse_tree(kp, &t)) == OYLNG_OK && t) {
            if (budget) {
                h6 = tv_hash(h6, OYLNG_EVT_DOC_START, oylng_tree_name(t));
                h6 = kept_hash(h6, t, oylng_tree_root(t), &budget);
                h6 = tv_hash(h6, OYLNG_EVT_DOC_END, (oylng_str){ NULL, 0 });
            }
            oylng_emitter *ke2 = oylng_emitter_new();
            if (ke2) {
                oylng_emit_stream_start(ke2);
                if (oylng_emit_tree(ke2, t) != OYLNG_ERR_INPUT) abort();
                oylng_emitter_free(ke2);
            }
            oylng_tree_free(t);
            check_table(kp, OYLNG_OK);
        }
        check_table(kp, st);
        if (st == OYLNG_ERR_MEMORY || pst == OYLNG_ERR_MEMORY) {
        } else if (clean) {
            if (st != OYLNG_OK || (budget && h6 != h3)) abort();
        } else if (pst == OYLNG_ERR_PARSE) {
            const char *x = oylng_parser_error(p), *y = oylng_parser_error(kp);
            if (st != OYLNG_ERR_PARSE || !x || !y || strcmp(x, y) != 0 ||
                oylng_parser_error_mark(p).offset != oylng_parser_error_mark(kp).offset)
                abort();
        }
        oylng_parser_free(kp);
    }

    /* together: the sizes add up, so accepted only where the events were,
       and refused for the grammar alike, or for size before it */
    oylng_parser *gp = parser_for(in, size, opt);
    if (gp) {
        oylng_parser_set_together(gp, true);
        uint64_t h7 = 0xCBF29CE484222325u;
        oylng_status st;
        const oylng_event *ge;
        while ((st = oylng_parse_next(gp, &ge)) == OYLNG_OK) {
            h7 = ev_hash(h7, ge);
            if (ge->type == OYLNG_EVT_STREAM_END) break;
        }
        if (st == OYLNG_ERR_MEMORY || pst == OYLNG_ERR_MEMORY) {
        } else if (st == OYLNG_OK) {
            if (!clean || h7 != h1) abort();
        } else if (pst == OYLNG_ERR_PARSE && st != OYLNG_ERR_LIMIT) {
            const char *x = oylng_parser_error(p), *y = oylng_parser_error(gp);
            if (st != OYLNG_ERR_PARSE || !x || !y || strcmp(x, y) != 0 ||
                oylng_parser_error_mark(p).offset != oylng_parser_error_mark(gp).offset)
                abort();
        }
        oylng_parser_free(gp);
    }

    /* typed with stamps ignored and no schema of the application's: as the
       trees, or the same error */
    oylng_parser *np = parser_for(in, size, opt);
    if (np) {
        oylng_parser_set_stamps(np, false);
        uint64_t h5 = 0xCBF29CE484222325u;
        oylng_typed *y;
        oylng_status st;
        while ((st = oylng_parse_typed(np, NULL, &y)) == OYLNG_OK && y) {
            if (oylng_typed_how(y) != OYLNG_TYPED_NOT) abort();
            const oylng_tree *t = oylng_typed_tree(y);
            h5 = tv_hash(h5, OYLNG_EVT_DOC_START, oylng_tree_name(t));
            h5 = tree_hash(h5, t, oylng_tree_root(t));
            h5 = tv_hash(h5, OYLNG_EVT_DOC_END, (oylng_str){ NULL, 0 });
            oylng_typed_free(y);
        }
        if (st == OYLNG_ERR_MEMORY || pst == OYLNG_ERR_MEMORY) {
        } else if (clean) {
            if (st != OYLNG_OK || h5 != h3) abort();
        } else {
            const char *x = oylng_parser_error(p), *z = oylng_parser_error(np);
            if (st == OYLNG_OK || !x || !z || strcmp(x, z) != 0 ||
                oylng_parser_error_mark(p).offset != oylng_parser_error_mark(np).offset)
                abort();
        }
        oylng_parser_free(np);
    }

    /* typed: refused where the events were, or for a schema */
    static const uint64_t limits[8] = { 0, 4, 16, 64, 256, 4096, 1 << 16, 0 };
    oylng_schema *app = oylng_schema_new(in, size, (oylng_str){ "a", 1 }, limits[opt & 7]);
    if (app && oylng_schema_status(app) != OYLNG_OK) {
        if (!oylng_schema_error(app)) abort();
        oylng_schema_free(app);
        app = NULL;
    }
    oylng_parser *yp = parser_for(in, size, opt);
    fbinder fb = { 0 };
    if (yp) {
        oylng_parser_set_cut(yp, (opt & 0x40) != 0);
        find_names(in, size);
        for (size_t i = 0; i < nbound; i++) {
            fnames[i] = (fname){ &fb, i };
            if (oylng_parser_bind(yp, bound_names[i], fuzz_bind, fuzz_drop, &fnames[i]) != OYLNG_OK) abort();
        }
        oylng_typed *y;
        oylng_status st;
        size_t budget = 1 << 16;
        unsigned ndoc = 0;
        bool off = false;
        while ((oylng_parser_set_stamps(yp, !(off = ndoc++ % 3 == 2)),
                st = oylng_parse_typed(yp, app, &y)) == OYLNG_OK && y) {
            oylng_typed_by how = oylng_typed_how(y);
            oylng_value root = oylng_typed_root(y);
            if (root == OYLNG_NO_VALUE) abort();
            if (off && (how == OYLNG_TYPED_STAMP || how == OYLNG_TYPED_PUBLISHED)) abort();
            if (how == OYLNG_TYPED_NOT && oylng_value_type(y, root) != OYLNG_T_ANY) abort();
            if (how == OYLNG_TYPED_APP && !app) abort();
            check_value(y, root, &budget);
            uint8_t *seen = NULL;
            size_t cap = 0, ob = 1 << 16;
            check_objects(y, root, &seen, &cap, &ob);
            free(seen);
            if (how == OYLNG_TYPED_APP || how == OYLNG_TYPED_NOT) write_again(y, how == OYLNG_TYPED_APP ? app : NULL, opt);
            oylng_typed_free(y);
        }
        if (fb.made != fb.dropped) abort();
        if (st == OYLNG_OK) {
            if (!clean && pst != OYLNG_ERR_MEMORY) abort();
        } else {
            if (!oylng_parser_error(yp)) abort();
            /* a type's names walked again differently: the whole message, as
               a refusal may quote the input */
            if (!strcmp(oylng_parser_error(yp), "/: a type's names walked again differently (a bug)")) abort();
            if (st != OYLNG_ERR_SCHEMA && st != OYLNG_ERR_LIMIT && st != OYLNG_ERR_MEMORY &&
                pst != OYLNG_ERR_MEMORY) {
                /* not for a schema: the events' error */
                if (clean || pst != st || strcmp(oylng_parser_error(p), oylng_parser_error(yp)) != 0) abort();
            }
        }
        oylng_parser_free(yp);
    }
    oylng_schema_free(app);
    oylng_parser_free(p);
    published(in, size, opt);

    /* written again, it reads back the same */
    oylng_str out = oylng_emitter_output(em);
    if (clean && !oylng_emitter_error(em)) {
        char *copy = malloc(out.len ? out.len : 1);
        if (copy) {
            memcpy(copy, out.data, out.len);
            oylng_parser *q = parser_for(copy, out.len, opt);
            uint64_t h2 = 0xCBF29CE484222325u;
            for (;;) {
                if (!q) break;
                if (oylng_parse_next(q, &e) != OYLNG_OK) abort();
                h2 = ev_hash(h2, e);
                if (e->type == OYLNG_EVT_STREAM_END) break;
            }
            if (q && h1 != h2) abort();
            oylng_parser_free(q);
            free(copy);
        }
    }
    oylng_emitter_free(em);
    free(in);
    return 0;
}

static void published(const char *in, size_t size, unsigned opt) {
    const char *nul = memchr(in, '\0', size);
    if (!nul) return;
    oylng_str file = { in, (size_t)(nul - in) };
    size_t n = size - file.len - 1;
    char *s = malloc(n ? n : 1);
    if (!s) return;
    memcpy(s, nul + 1, n);
    unsigned char h[32];
    oylng__sha256(file.data, file.len, h);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i + 8 + 64 <= n; i++)
        if (!memcmp(s + i, "#sha256=", 8))
            for (int j = 0; j < 32; j++) {
                s[i + 8 + 2 * j] = digits[h[j] >> 4];
                s[i + 9 + 2 * j] = digits[h[j] & 15];
            }
    oylng_parser *yp = parser_for(s, n, opt);
    if (yp) {
        oylng_parser_set_lookup(yp, lookup, &file);
        oylng_typed *y;
        oylng_status st;
        size_t budget = 1 << 16;
        while ((st = oylng_parse_typed(yp, NULL, &y)) == OYLNG_OK && y) {
            oylng_typed_by how = oylng_typed_how(y);
            oylng_value root = oylng_typed_root(y);
            if (root == OYLNG_NO_VALUE || how == OYLNG_TYPED_APP) abort();
            if (how == OYLNG_TYPED_NOT && oylng_value_type(y, root) != OYLNG_T_ANY) abort();
            check_value(y, root, &budget);
            oylng_typed_free(y);
        }
        if (st != OYLNG_OK && !oylng_parser_error(yp)) abort();
        oylng_parser_free(yp);
    }
    free(s);
}
