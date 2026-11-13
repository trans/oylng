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
 * must be found, and the trees must be written as the events were. Typed
 * (part 2), by its stamps, and by the stream's own table as an application's
 * schema rooted at `a` when it compiles as one, the stream must be refused
 * where the events were, or for a schema; each typed value must be what its
 * type says, each key's value found. The flow bit also turns on the cut.
 *
 *     clang -fsanitize=fuzzer,address,undefined -Iinclude src/oylng_*.c fuzz/fuzz_parser.c
 */

#include "oylng/oylng.h"
#include "oylng/schema.h"
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
        }
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

    /* typed: refused where the events were, or for a schema */
    static const uint64_t limits[8] = { 0, 4, 16, 64, 256, 4096, 1 << 16, 0 };
    oylng_schema *app = oylng_schema_new(in, size, (oylng_str){ "a", 1 }, limits[opt & 7]);
    if (app && oylng_schema_status(app) != OYLNG_OK) {
        if (!oylng_schema_error(app)) abort();
        oylng_schema_free(app);
        app = NULL;
    }
    oylng_parser *yp = parser_for(in, size, opt);
    if (yp) {
        oylng_parser_set_cut(yp, (opt & 0x40) != 0);
        oylng_typed *y;
        oylng_status st;
        size_t budget = 1 << 16;
        while ((st = oylng_parse_typed(yp, app, &y)) == OYLNG_OK && y) {
            oylng_typed_by how = oylng_typed_how(y);
            oylng_value root = oylng_typed_root(y);
            if (root == OYLNG_NO_VALUE) abort();
            if (how == OYLNG_TYPED_NOT && oylng_value_type(y, root) != OYLNG_T_ANY) abort();
            if (how == OYLNG_TYPED_APP && !app) abort();
            check_value(y, root, &budget);
            oylng_typed_free(y);
        }
        if (st == OYLNG_OK) {
            if (!clean && pst != OYLNG_ERR_MEMORY) abort();
        } else {
            if (!oylng_parser_error(yp)) abort();
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
