/*
 * test_schema.c — part 2, YAML Easy Schema: the draft's examples, and the
 * API's checks
 *
 *   test_schema path/to/yaml2/spec/yaml-easy-schema.md
 *
 * Every example in the draft is a stream and what it delivers, one line of
 * JSON per document, or a stream marked refused (section 1). Each is read
 * here as the draft's checker reads it: a stream with documents is read
 * typed, and one with only a table has each entry checked as a root. A
 * typed value is compared with the JSON read as YAML 2.0, where a JSON
 * number, true, false and null are plain and a string is quoted.
 */

#include <ctype.h>
#include "oylng/schema.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

static oylng_str S(const char *s) { return (oylng_str){ s, strlen(s) }; }

/* The library's own SHA-256, for published names' hashes */
void oylng__sha256(const void *data, size_t len, unsigned char out[32]);

static void hex_of(const void *s, size_t n, char out[65]) {
    unsigned char h[32];
    oylng__sha256(s, n, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

static bool eq(oylng_str a, const char *b, size_t bl) { return a.len == bl && (!bl || memcmp(a.data, b, bl) == 0); }

/* ── Comparing with JSON ─────────────────────────────────── */

static bool same_any(const oylng_tree *t, oylng_node n, const oylng_tree *j, oylng_node m) {
    oylng_kind k = oylng_node_kind(t, n);
    if (k != oylng_node_kind(j, m)) return false;
    if (k == OYLNG_NODE_STRING) {
        oylng_str a = oylng_node_text(t, n), b = oylng_node_text(j, m);
        return !oylng_node_plain(j, m) && eq(a, b.data, b.len);
    }
    size_t c = oylng_node_count(t, n);
    if (c != oylng_node_count(j, m)) return false;
    for (size_t i = 0; i < c; i++) {
        if (k == OYLNG_NODE_SEQUENCE) {
            if (!same_any(t, oylng_node_item(t, n, i), j, oylng_node_item(j, m, i))) return false;
            continue;
        }
        oylng_str a = oylng_node_text(t, oylng_node_key(t, n, i)), b = oylng_node_text(j, oylng_node_key(j, m, i));
        if (!eq(a, b.data, b.len) || !same_any(t, oylng_node_value(t, n, i), j, oylng_node_value(j, m, i)))
            return false;
    }
    return true;
}

static bool same(const oylng_typed *y, oylng_value v, const oylng_tree *j, oylng_node m) {
    oylng_type ty = oylng_value_type(y, v);
    const oylng_tree *t = oylng_typed_tree(y);
    if (ty == OYLNG_T_ANY) return same_any(t, oylng_value_node(y, v), j, m);
    oylng_kind jk = oylng_node_kind(j, m);
    if (ty == OYLNG_T_SEQUENCE || ty == OYLNG_T_MAPPING) {
        if (jk != (ty == OYLNG_T_SEQUENCE ? OYLNG_NODE_SEQUENCE : OYLNG_NODE_MAPPING)) return false;
        size_t c = oylng_value_count(y, v);
        if (c != oylng_node_count(j, m)) return false;
        for (size_t i = 0; i < c; i++) {
            if (ty == OYLNG_T_SEQUENCE) {
                if (!same(y, oylng_value_item(y, v, i), j, oylng_node_item(j, m, i))) return false;
                continue;
            }
            oylng_str a = oylng_value_key(y, v, i), b = oylng_node_text(j, oylng_node_key(j, m, i));
            if (!eq(a, b.data, b.len) || !same(y, oylng_value_at(y, v, i), j, oylng_node_value(j, m, i)))
                return false;
        }
        return true;
    }
    if (jk != OYLNG_NODE_STRING) return false;
    oylng_str w = oylng_node_text(j, m), text = oylng_value_text(y, v);
    bool plain = oylng_node_plain(j, m);
    char buf[64];
    switch (ty) {
    case OYLNG_T_STR:
    case OYLNG_T_DATE:
    case OYLNG_T_DATETIME:
        return !plain && eq(text, w.data, w.len);
    case OYLNG_T_BINARY: {
        /* the base64 as written, its line feeds skipped */
        size_t k = 0;
        for (size_t i = 0; i < text.len; i++) {
            if (text.data[i] == '\n') continue;
            if (k >= w.len || text.data[i] != w.data[k]) return false;
            k++;
        }
        return !plain && k == w.len;
    }
    case OYLNG_T_INT: {
        int64_t i;
        uint64_t u;
        if (oylng_value_uint(y, v, &u)) snprintf(buf, sizeof buf, "%llu", (unsigned long long)u);
        else if (oylng_value_int(y, v, &i)) snprintf(buf, sizeof buf, "%lld", (long long)i);
        else return false;
        return plain && eq(w, buf, strlen(buf));
    }
    case OYLNG_T_BIGINT:
    case OYLNG_T_DECIMAL:
        return plain && eq(text, w.data, w.len);
    case OYLNG_T_FLOAT: {
        if (!plain || w.len >= sizeof buf) return false;
        memcpy(buf, w.data, w.len);
        buf[w.len] = '\0';
        return strtod(buf, NULL) == oylng_value_float(y, v);
    }
    case OYLNG_T_BOOL:
        return plain && eq(w, oylng_value_bool(y, v) ? "true" : "false", oylng_value_bool(y, v) ? 4 : 5);
    case OYLNG_T_NULL:
        return plain && eq(w, "null", 4);
    default:
        return false;
    }
}

/* ── Typed values compared, for emitting (section 13) ────── */

static bool same_nodes(const oylng_tree *a, oylng_node n, const oylng_tree *b, oylng_node m) {
    oylng_kind k = oylng_node_kind(a, n);
    if (k != oylng_node_kind(b, m)) return false;
    if (k == OYLNG_NODE_STRING) {
        oylng_str x = oylng_node_text(a, n), y = oylng_node_text(b, m);
        return eq(x, y.data, y.len);
    }
    size_t c = oylng_node_count(a, n);
    if (c != oylng_node_count(b, m)) return false;
    for (size_t i = 0; i < c; i++) {
        if (k == OYLNG_NODE_SEQUENCE) {
            if (!same_nodes(a, oylng_node_item(a, n, i), b, oylng_node_item(b, m, i))) return false;
            continue;
        }
        oylng_str x = oylng_node_text(a, oylng_node_key(a, n, i)), y = oylng_node_text(b, oylng_node_key(b, m, i));
        if (!eq(x, y.data, y.len) || !same_nodes(a, oylng_node_value(a, n, i), b, oylng_node_value(b, m, i)))
            return false;
    }
    return true;
}

/* Whether two typed values are the same: type, value and name */
static bool same_typed(const oylng_typed *a, oylng_value v, const oylng_typed *b, oylng_value w) {
    oylng_type t = oylng_value_type(a, v);
    if (t != oylng_value_type(b, w)) return false;
    oylng_str na = oylng_value_name(a, v), nb = oylng_value_name(b, w);
    if (!na.data != !nb.data || (na.data && !eq(na, nb.data, nb.len))) return false;
    oylng_str x = oylng_value_text(a, v), y = oylng_value_text(b, w);
    switch (t) {
    case OYLNG_T_ANY:
        return same_nodes(oylng_typed_tree(a), oylng_value_node(a, v), oylng_typed_tree(b), oylng_value_node(b, w));
    case OYLNG_T_SEQUENCE:
    case OYLNG_T_MAPPING: {
        size_t c = oylng_value_count(a, v);
        if (c != oylng_value_count(b, w)) return false;
        for (size_t i = 0; i < c; i++) {
            if (t == OYLNG_T_SEQUENCE) {
                if (!same_typed(a, oylng_value_item(a, v, i), b, oylng_value_item(b, w, i))) return false;
                continue;
            }
            oylng_str ka = oylng_value_key(a, v, i), kb = oylng_value_key(b, w, i);
            if (!eq(ka, kb.data, kb.len) || !same_typed(a, oylng_value_at(a, v, i), b, oylng_value_at(b, w, i)))
                return false;
        }
        return true;
    }
    case OYLNG_T_STR:
    case OYLNG_T_BIGINT:
    case OYLNG_T_DECIMAL:
        return eq(x, y.data, y.len);
    case OYLNG_T_INT: {
        int64_t i, j;
        uint64_t u, k;
        bool si = oylng_value_int(a, v, &i), sj = oylng_value_int(b, w, &j);
        if (si || sj) return si && sj && i == j;
        return oylng_value_uint(a, v, &u) && oylng_value_uint(b, w, &k) && u == k;
    }
    case OYLNG_T_FLOAT: {
        double f = oylng_value_float(a, v), g = oylng_value_float(b, w);
        return memcmp(&f, &g, sizeof f) == 0;
    }
    case OYLNG_T_BOOL:
        return oylng_value_bool(a, v) == oylng_value_bool(b, w);
    case OYLNG_T_NULL:
        return true;
    case OYLNG_T_DATE:
    case OYLNG_T_DATETIME: {
        oylng_datetime d, e;
        oylng_value_datetime(a, v, &d);
        oylng_value_datetime(b, w, &e);
        return d.year == e.year && d.month == e.month && d.day == e.day && d.hour == e.hour &&
               d.minute == e.minute && d.second == e.second && d.nanosecond == e.nanosecond &&
               d.offset == e.offset && d.zulu == e.zulu && d.unknown_offset == e.unknown_offset;
    }
    case OYLNG_T_BINARY: {
        oylng_str p = oylng_value_bytes(a, v), q = oylng_value_bytes(b, w);
        return eq(p, q.data, q.len);
    }
    default:
        return false;
    }
}

/* Typed document y of stream s written again, under the schema that typed
 * it, block and then flow, and read back with it: NULL if it reads back
 * the same, else why not */
static const char *round_trip(const char *s, size_t n, const oylng_typed *y, char *why, size_t whyn) {
    oylng_str name = oylng_tree_name(oylng_typed_tree(y));
    oylng_schema *sc = NULL;
    if (oylng_typed_how(y) == OYLNG_TYPED_STAMP) {
        sc = oylng_schema_new(s, n, name, 0);
        if (oylng_schema_status(sc) != OYLNG_OK) {
            snprintf(why, whyn, "its schema: %s", oylng_schema_error(sc));
            oylng_schema_free(sc);
            return why;
        }
    } else if (oylng_typed_how(y) != OYLNG_TYPED_NOT) {
        return NULL;   /* a published name, which isn't looked up here */
    }
    const char *res = NULL;
    for (int flow = 0; flow < 2 && !res; flow++) {
        oylng_emitter *e = oylng_emitter_new();
        oylng_emitter_set_flow(e, flow);
        oylng_emitter_set_schema(e, sc);
        oylng_status st = oylng_emit_stream_start(e);
        if (st == OYLNG_OK) st = oylng_emit_doc_start(e, name);
        if (st == OYLNG_OK) st = oylng_emit_value(e, y, oylng_typed_root(y));
        if (st == OYLNG_OK) st = oylng_emit_doc_end(e);
        if (st == OYLNG_OK) st = oylng_emit_stream_end(e);
        oylng_str out = oylng_emitter_output(e);
        if (st != OYLNG_OK) {
            snprintf(why, whyn, "written %s: %s", flow ? "in flow" : "in block", oylng_emitter_error(e));
            res = why;
        } else {
            oylng_parser *p = oylng_parser_new(out.data, out.len);
            oylng_typed *back;
            st = oylng_parse_typed(p, sc, &back);
            if (st != OYLNG_OK || !back) {
                snprintf(why, whyn, "written %s, it doesn't read: %s\n%.*s", flow ? "in flow" : "in block",
                         st ? oylng_parser_error(p) : "no document", (int)(out.len > 300 ? 300 : out.len), out.data);
                res = why;
            } else if (!same_typed(y, oylng_typed_root(y), back, oylng_typed_root(back))) {
                snprintf(why, whyn, "written %s, it reads back otherwise:\n%.*s", flow ? "in flow" : "in block",
                         (int)(out.len > 300 ? 300 : out.len), out.data);
                res = why;
            }
            oylng_typed_free(back);
            oylng_parser_free(p);
        }
        oylng_emitter_free(e);
    }
    oylng_schema_free(sc);
    return res;
}

static int trips;   /* documents written again and read back */

/* ── The draft's examples ────────────────────────────────── */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[n] = '\0';
    *len = (size_t)n;
    return b;
}

/* What the examples leave out: %YAML 2.0, and a plain --- if no marker
 * comes first */
static char *framed(const char *body, size_t n, size_t *out) {
    char *s = malloc(n + 32);
    size_t k = 0;
    if (n < 5 || memcmp(body, "%YAML", 5) != 0) {
        const char *q = body, *e = body + n;
        for (;;) {   /* past blank and comment lines */
            const char *l = q;
            while (l < e && (*l == ' ' || *l == '\t')) l++;
            if (l < e && *l == '#') while (l < e && *l != '\n') l++;
            if (l < e && *l == '\n') { q = l + 1; continue; }
            break;
        }
        memcpy(s, "%YAML 2.0\n", 10);
        k = 10;
        if (q < e && !(e - q >= 3 && (memcmp(q, "---", 3) == 0 || memcmp(q, "...", 3) == 0))) {
            memcpy(s + k, "---\n", 4);
            k += 4;
        }
    }
    memcpy(s + k, body, n);
    k += n;
    s[k] = '\0';
    *out = k;
    return s;
}

/* Whether the stream has a document that isn't a table */
static bool has_docs(const char *s, size_t n) {
    oylng_parser *p = oylng_parser_new(s, n);
    const oylng_event *e;
    bool any = false;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END)
        if (e->type == OYLNG_EVT_DOC_START) any = true;
    oylng_parser_free(p);
    return any;
}

/* Reads the stream as the checker does: typed, or, with only a table, each
 * entry as a root. want: the JSON lines, or NULL. NULL if all is as
 * wanted, else why not. */
static const char *run_example(const char *s, size_t n, const char *want, size_t wn, bool refused, char *why,
                               size_t whyn) {
    if (!has_docs(s, n)) {
        oylng_schema *sc = oylng_schema_new(s, n, (oylng_str){ NULL, 0 }, 0);
        oylng_status st = oylng_schema_status(sc);
        if (st != OYLNG_OK) snprintf(why, whyn, "%s", oylng_schema_error(sc));
        oylng_schema_free(sc);
        if (refused) return st != OYLNG_OK ? NULL : "not refused";
        return st == OYLNG_OK ? NULL : why;
    }
    oylng_parser *p = oylng_parser_new(s, n);
    const char *w = want, *we = want ? want + wn : NULL;
    oylng_typed *y;
    oylng_status st;
    const char *res = NULL;
    while ((st = oylng_parse_typed(p, NULL, &y)) == OYLNG_OK && y) {
        if (want && !res) {
            while (w < we && *w == '\n') w++;
            const char *l = w;
            while (w < we && *w != '\n') w++;
            if (l == w) res = "more documents than JSON lines";
            else {
                size_t jl = (size_t)(w - l);
                char *jt = malloc(jl + 32);
                memcpy(jt, "%YAML 2.0\n---\n", 14);
                memcpy(jt + 14, l, jl);
                jt[14 + jl] = '\n';
                oylng_parser *jp = oylng_parser_new(jt, jl + 15);
                oylng_tree *jtree;
                if (oylng_parse_tree(jp, &jtree) != OYLNG_OK || !jtree) res = "the JSON doesn't read";
                else {
                    if (!same(y, oylng_typed_root(y), jtree, oylng_tree_root(jtree))) {
                        snprintf(why, whyn, "differs from %.*s", (int)(jl > 200 ? 200 : jl), l);
                        res = why;
                    }
                    oylng_tree_free(jtree);
                }
                oylng_parser_free(jp);
                free(jt);
            }
        }
        if (!res && !refused) {
            char tw[600];
            const char *r = round_trip(s, n, y, tw, sizeof tw);
            if (r) {
                snprintf(why, whyn, "%s", r);
                res = why;
            } else trips++;
        }
        oylng_typed_free(y);
    }
    if (st != OYLNG_OK) {
        oylng_mark m = oylng_parser_error_mark(p);
        snprintf(why, whyn, "%zu:%zu: %s", m.line, m.col, oylng_parser_error(p));
    }
    oylng_parser_free(p);
    if (refused) return st != OYLNG_OK ? NULL : "not refused";
    if (st != OYLNG_OK) return why;
    if (!res && want) {
        while (w < we && *w == '\n') w++;
        if (w < we) res = "fewer documents than JSON lines";
    }
    return res;
}

static void examples(const char *path) {
    size_t len;
    char *spec = slurp(path, &len);
    if (!spec) {
        fprintf(stderr, "can't read %s\n", path);
        failed++;
        return;
    }
    int pass = 0, fail = 0;
    char *q = spec;
    struct block { int kind; char *body; size_t n; size_t line; } *b = NULL;
    size_t nb = 0, cap = 0;
    while ((q = strstr(q, "```"))) {
        char *h = q + 3, *nl = strchr(h, '\n');
        if (!nl) break;
        int kind = -1;
        if ((size_t)(nl - h) == 4 && memcmp(h, "yaml", 4) == 0) kind = 0;
        else if ((size_t)(nl - h) == 12 && memcmp(h, "yaml refused", 12) == 0) kind = 1;
        else if ((size_t)(nl - h) == 4 && memcmp(h, "json", 4) == 0) kind = 2;
        char *end = strstr(nl + 1, "```");
        if (!end) break;
        if (kind >= 0) {
            if (nb == cap) b = realloc(b, (cap = cap ? cap * 2 : 64) * sizeof *b);
            size_t line = 1;
            for (char *c = spec; c < q; c++) line += *c == '\n';
            b[nb++] = (struct block){ kind, nl + 1, (size_t)(end - nl - 1), line };
        }
        q = end + 3;
    }
    for (size_t i = 0; i < nb; i++) {
        if (b[i].kind == 2) continue;
        bool refused = b[i].kind == 1;
        const char *want = NULL;
        size_t wn = 0;
        if (!refused && i + 1 < nb && b[i + 1].kind == 2) {
            want = b[i + 1].body;
            wn = b[i + 1].n;
        }
        size_t n;
        char *s = framed(b[i].body, b[i].n, &n);
        char why[600] = "";
        const char *r = run_example(s, n, want, wn, refused, why, sizeof why);
        free(s);
        if (getenv("SCHEMA_VERBOSE") && refused) printf("line %zu refused: %s\n", b[i].line, why);
        if (r) {
            fail++;
            fprintf(stderr, "FAIL the draft's example at line %zu: %s\n", b[i].line, r);
        } else pass++;
    }
    printf("draft: %d examples pass, %d fail; %d documents written again read back the same\n", pass, fail,
           trips);
    failed += fail;
    checks += pass + fail;
    free(b);
    free(spec);
}

/* ── The API ─────────────────────────────────────────────── */

/* The stream's first document, typed: NULL with the error in err */
static oylng_typed *typed(oylng_parser **pp, const char *s, const oylng_schema *app, const char **err) {
    *pp = oylng_parser_new(s, strlen(s));
    oylng_typed *y = NULL;
    *err = NULL;
    if (oylng_parse_typed(*pp, app, &y) != OYLNG_OK) *err = oylng_parser_error(*pp);
    return y;
}

/* ── Published names (section 3.1) ───────────────────────── */

/* An application's folder of published files, found by their hash. Each
 * one it gives is a copy, let go once the parser has returned, so that a
 * parser keeping any of it is caught (under the sanitizers). */
typedef struct {
    const char *files[4];
    bool        any;   /* gives files[0], whatever the hash: a lookup that lies */
    int         calls;
    char       *copy;
    char        addr[200];
} shelf;

static bool shelf_lookup(void *ctx, oylng_str addr, oylng_str sha256, oylng_str *file) {
    shelf *sh = ctx;
    sh->calls++;
    snprintf(sh->addr, sizeof sh->addr, "%.*s", (int)addr.len, addr.data);
    for (int i = 0; i < 4 && sh->files[i]; i++) {
        char h[65];
        size_t n = strlen(sh->files[i]);
        hex_of(sh->files[i], n, h);
        if (!sh->any && (sha256.len != 64 || memcmp(h, sha256.data, 64))) continue;
        free(sh->copy);
        sh->copy = malloc(n + 1);
        memcpy(sh->copy, sh->files[i], n + 1);
        *file = (oylng_str){ sh->copy, n };
        return true;
    }
    return false;
}

/* The documents of stream s, typed with sh as the lookup: how many, the
 * last one's root's type and how it was typed, or 0 and the error */
typedef struct {
    int            docs;
    oylng_type     type;
    oylng_typed_by how;
    oylng_status   st;
    char           err[300];
    oylng_mark     mark;
    char           name[40];
} pubrun;

static pubrun pub_run(const char *s, shelf *sh, const oylng_schema *app, uint64_t limit) {
    pubrun r = { 0 };
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    if (limit) oylng_parser_set_limit(p, limit);
    if (sh) oylng_parser_set_lookup(p, shelf_lookup, sh);
    for (;;) {
        oylng_typed *y;
        r.st = oylng_parse_typed(p, app, &y);
        if (sh) {
            free(sh->copy);   /* the parser has its own of all it needs */
            sh->copy = NULL;
        }
        if (r.st != OYLNG_OK) {
            snprintf(r.err, sizeof r.err, "%s", oylng_parser_error(p));
            r.mark = oylng_parser_error_mark(p);
            break;
        }
        if (!y) break;
        r.docs++;
        oylng_value v = oylng_typed_root(y);
        r.type = oylng_value_type(y, v);
        r.how = oylng_typed_how(y);
        oylng_str nm = oylng_value_name(y, v);
        snprintf(r.name, sizeof r.name, "%.*s", nm.data ? (int)nm.len : 0, nm.data ? nm.data : "");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    return r;
}

static void published_names(void) {
    /* SHA-256: FIPS 180-2's examples, a million a's, and the digests of
       every run of a's to 299, which crosses each way a block can end */
    char h[65];
    hex_of("", 0, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 of nothing: %s", h);
    hex_of("abc", 3, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 of abc: %s", h);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    hex_of(two, strlen(two), h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 of 448 bits: %s", h);
    char *as = malloc(1000000);
    memset(as, 'a', 1000000);
    hex_of(as, 1000000, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 of 10^6 a's: %s", h);
    unsigned char *all = malloc(300 * 32);
    for (int n = 0; n < 300; n++) oylng__sha256(as, (size_t)n, all + 32 * n);
    hex_of(all, 300 * 32, h);
    CHECK(!strcmp(h, "80e953e3b006c4bab43fd5d792f8bd29c122f6cf1497318365fe66bf2d61398d"), "sha256 of 0 to 299 a's: %s", h);
    free(all);
    free(as);

    const char *ab = "%YAML 2.0\n--- &\na: !int\nb: [!bool, ...]\n";
    char hab[65], s[1200], t[600];
    hex_of(ab, strlen(ab), hab);
    shelf sh = { .files = { ab } };
    pubrun r;

    /* looked up once a stream, for each root: the same bytes give a and b */
    snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n--- !mirror.example/a.yes#sha256=%s\n2\n"
             "--- !x.org/v2/b.yes#sha256=%s\n[true]\n", hab, hab, hab);
    r = pub_run(s, &sh, NULL, 0);
    CHECK(r.st == OYLNG_OK && r.docs == 3 && r.type == OYLNG_T_SEQUENCE && r.how == OYLNG_TYPED_PUBLISHED &&
          !strcmp(r.name, "b"), "published, three documents: %s", r.err);
    CHECK(sh.calls == 2, "a name looked up once a stream: %d lookups", sh.calls);
    CHECK(!strcmp(sh.addr, "x.org/v2/b.yes"), "the address, the name before #: %s", sh.addr);

    /* without a lookup, it stamps nothing, and the application's schema
       types it */
    oylng_schema *app = oylng_schema_new("%YAML 2.0\n--- &\nr: !str\n", 24, S("r"), 0);
    snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", hab);
    r = pub_run(s, NULL, app, 0);
    CHECK(r.st == OYLNG_OK && r.type == OYLNG_T_STR && r.how == OYLNG_TYPED_APP, "no lookup: %s", r.err);
    /* with one, it stamps, and the application's schema doesn't replace it */
    sh.calls = 0;
    r = pub_run(s, &sh, app, 0);
    CHECK(r.st == OYLNG_OK && r.type == OYLNG_T_INT && r.how == OYLNG_TYPED_PUBLISHED, "over the app's: %s", r.err);
    oylng_schema_free(app);

    /* names that aren't published: the lookup isn't asked */
    char up[65];
    for (int i = 0; i < 65; i++) up[i] = (char)(hab[i] >= 'a' && hab[i] <= 'f' ? hab[i] - 32 : hab[i]);
    const char *nots[] = { "a.yes#sha256=%s", "x.org/a.yes#sha512=%s", "x.org/a#sha256=%s", "x.org/.yes#sha256=%s",
                           "x.org//a.yes#sha256=%s", "/a.yes#sha256=%s", "x.org/a.yes#sha256=%s0",
                           "x.org/a.yes#sha256=%.63s", "x.org/a.yes#sha256=" };
    for (size_t i = 0; i < sizeof nots / sizeof *nots; i++) {
        int k = snprintf(s, sizeof s, "%%YAML 2.0\n--- !");
        k += snprintf(s + k, sizeof s - k, nots[i], hab);
        snprintf(s + k, sizeof s - k, "\n1\n");
        sh.calls = 0;
        r = pub_run(s, &sh, NULL, 0);
        CHECK(r.st == OYLNG_OK && r.how == OYLNG_TYPED_NOT && sh.calls == 0, "not published, %s: %s", nots[i], r.err);
    }
    snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", up);
    sh.calls = 0;
    r = pub_run(s, &sh, NULL, 0);
    CHECK(r.st == OYLNG_OK && r.how == OYLNG_TYPED_NOT && sh.calls == 0, "upper case hex: %s", r.err);

    /* a file that can't be had, or doesn't hash to the name, refuses the
       document, at the stamp */
    char zero[65];
    memset(zero, '0', 64);
    zero[64] = '\0';
    snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", zero);
    r = pub_run(s, &sh, NULL, 0);
    CHECK(r.st == OYLNG_ERR_SCHEMA && !strncmp(r.err, "/: the application can't get x.org/a.yes", 40) &&
          r.mark.line == 2 && r.mark.col == 6, "can't get it: %zu:%zu: %s", r.mark.line, r.mark.col, r.err);
    /* a lookup that gives other bytes than it was asked for: a space more */
    snprintf(s, sizeof s, "%%YAML 2.0\n---\n0\n--- !x.org/a.yes#sha256=%s\n1\n", hab);
    shelf liar = { .files = { "%YAML 2.0\n--- &\na: !int\nb: [!bool, ...]\n " }, .any = true };
    r = pub_run(s, &liar, NULL, 0);
    CHECK(r.st == OYLNG_ERR_SCHEMA && r.docs == 1 && !strcmp(r.err, "/: x.org/a.yes doesn't hash to its name") &&
          r.mark.line == 4 && r.mark.col == 6, "a lookup that lies: %zu:%zu: %s", r.mark.line, r.mark.col, r.err);

    /* a file that isn't good refuses the schema, naming its address */
    const char *bad[] = {
        "%YAML 2.0\n--- &\na: !int\n...\n",
        "%YAML 2.0\n--- &\na: !int\n...\nafter\n",
        "%YAML 2.0\n--- &\na: !int\n---\n1\n",
        "%YAML 2.0\n--- &\na: !int\n--- &\nb: !int\n",
        "--- &\na: !int\n",
        "%YAML 2.0\n--- &\na: !nothing\n",
        "%YAML 2.0\n--- &\nb: !int\n",
        "",
    };
    const char *why[] = { "one table and nothing else", "one table and nothing else", "one table and nothing else",
                          "one table and nothing else", "starts with the line %YAML 2.0", "nothing", "has no entry a",
                          "starts with the line %YAML 2.0" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        shelf one = { .files = { bad[i] } };
        hex_of(bad[i], strlen(bad[i]), h);
        snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", h);
        r = pub_run(s, &one, NULL, 0);
        CHECK(r.st == OYLNG_ERR_SCHEMA && !strncmp(r.err, "x.org/a.yes", 11) && strstr(r.err, why[i]) &&
              r.mark.line == 2, "bad file %zu: %zu:%zu: %s", i, r.mark.line, r.mark.col, r.err);
    }

    /* the file is a stream of its own, under the parser's limit: a stream
       of 200 holds the stamp, not a file of 300 */
    int k = snprintf(t, sizeof t, "%%YAML 2.0\n--- &\na: !int\n");
    while (k < 300) k += snprintf(t + k, sizeof t - k, "x%d: !str\n", k);
    shelf big = { .files = { t } };
    hex_of(t, strlen(t), h);
    snprintf(s, sizeof s, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", h);
    r = pub_run(s, &big, NULL, 200);
    CHECK(r.st == OYLNG_ERR_SCHEMA && !strncmp(r.err, "x.org/a.yes:", 12), "past the limit: %s", r.err);
    r = pub_run(s, &big, NULL, 400);
    CHECK(r.st == OYLNG_OK && r.type == OYLNG_T_INT, "under the limit: %s", r.err);

    /* the stream's own table is not the file's */
    snprintf(s, sizeof s, "%%YAML 2.0\n--- &\na: !str\nb: !str\n--- !x.org/a.yes#sha256=%s\n1\n--- !a\n1\n", hab);
    r = pub_run(s, &sh, NULL, 0);
    CHECK(r.st == OYLNG_OK && r.docs == 2 && r.type == OYLNG_T_STR && r.how == OYLNG_TYPED_STAMP, "two tables: %s", r.err);
    free(sh.copy);
}

/* Each document of stream s typed with stamps as stamp gives, one letter a
 * document ('0' off, else on, the last letter for the rest), and with
 * lookup sh: how each was typed, a letter each (N, S, A, P), and its root's
 * type, a letter each (a any, m mapping, s string, i int), in hows, then
 * the error, if any */
static oylng_status stamps_run(const char *s, const char *stamp, shelf *sh, const oylng_schema *app,
                               char *hows, char *err, size_t errlen) {
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    if (sh) oylng_parser_set_lookup(p, shelf_lookup, sh);
    size_t o = 0, d = 0, last = strlen(stamp) - 1;
    oylng_status st;
    for (;; d++) {
        oylng_parser_set_stamps(p, stamp[d < last ? d : last] != '0');
        oylng_typed *y;
        st = oylng_parse_typed(p, app, &y);
        if (st != OYLNG_OK || !y) break;
        oylng_typed_by how = oylng_typed_how(y);
        oylng_type ty = oylng_value_type(y, oylng_typed_root(y));
        hows[o++] = "NSAP"[how];
        hows[o++] = ty == OYLNG_T_ANY ? 'a' : ty == OYLNG_T_MAPPING ? 'm' : ty == OYLNG_T_STR ? 's' : ty == OYLNG_T_INT ? 'i' : '?';
        oylng_typed_free(y);
    }
    hows[o] = 0;
    snprintf(err, errlen, "%s", st == OYLNG_OK ? "" : oylng_parser_error(p));
    oylng_parser_free(p);
    return st;
}

/* Section 3: an application may ignore a file's schema, and so hold a file
 * it didn't write to its own */
static void ignored_stamps(void) {
    char hows[64], err[300];
    const char *as = "%YAML 2.0\n--- &\nr: {n?: !str, s?: !str}\n";
    oylng_schema *app = oylng_schema_new(as, strlen(as), S("r"), 0);
    CHECK(oylng_schema_status(app) == OYLNG_OK, "the application's schema");

    /* ignored: a stamp in the table, one that doesn't fit it, one whose
       schema is in error, and one naming nothing; each document is typed
       as one with no stamp is, by the application's schema, or with none,
       not at all */
    const char *s = "%YAML 2.0\n--- &\njob: {n: !int}\nbad: !stirng\n"
                    "--- !job\nn: 3\n--- !job\nn: x\n--- !bad\nn: y\n---\ns: 4\n--- !nope\ns: 5\n";
    oylng_status st = stamps_run(s, "0", NULL, app, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "AmAmAmAmAm"), "off: %s %s", hows, err);
    st = stamps_run(s, "0", NULL, NULL, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "NaNaNaNaNa"), "off, no schema of the application's: %s %s", hows, err);
    st = stamps_run(s, "1", NULL, app, hows, err, sizeof err);
    CHECK(st == OYLNG_ERR_SCHEMA && !strcmp(hows, "Sm") && strstr(err, "/n"), "on: %s %s", hows, err);
    st = stamps_run("%YAML 2.0\n--- &\nbad: !stirng\n--- !bad\nx\n", "1", NULL, NULL, hows, err, sizeof err);
    CHECK(st == OYLNG_ERR_SCHEMA && !*hows, "a schema in error, on: %s", err);

    /* a file's own loose schema doesn't get it past the application's */
    const char *loose = "%YAML 2.0\n--- &\nloose: !any\n--- !loose\nextra: 2\n";
    st = stamps_run(loose, "1", NULL, app, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "Sa"), "on, the file's: %s %s", hows, err);
    st = stamps_run(loose, "0", NULL, app, hows, err, sizeof err);
    CHECK(st == OYLNG_ERR_SCHEMA && !*hows && strstr(err, "extra"), "off, the application's: %s", err);

    /* typed by the application's schema, not the stamp's; and the
       stamp's schema, handed over */
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    oylng_parser_set_stamps(p, false);
    oylng_typed *y;
    CHECK(oylng_parse_typed(p, app, &y) == OYLNG_OK && y, "the first document");
    const oylng_tree *t = oylng_typed_tree(y);
    oylng_value n = oylng_value_get(y, oylng_typed_root(y), S("n"));
    CHECK(eq(oylng_tree_name(t), "job", 3) && oylng_value_type(y, n) == OYLNG_T_STR &&
          eq(oylng_value_text(y, n), "3", 1) && oylng_typed_how(y) == OYLNG_TYPED_APP,
          "n is the application's !str, and the name is still there");
    oylng_tree *tb = (oylng_tree *)1;
    CHECK(oylng_parser_table(p, &tb) == OYLNG_OK && tb, "the table");
    oylng_node job = oylng_node_get(tb, oylng_tree_root(tb), oylng_tree_name(t));
    oylng_node ty = oylng_node_get(tb, job, S("n"));
    CHECK(oylng_node_kind(tb, job) == OYLNG_NODE_MAPPING && eq(oylng_node_text(tb, ty), "!int", 4) && oylng_node_plain(tb, ty),
          "the schema the stamp names: {n: !int}");
    oylng_tree_free(tb);
    oylng_typed_free(y);
    oylng_parser_free(p);

    /* changed between documents: off, on, off */
    s = "%YAML 2.0\n--- &\njob: {n: !int}\n--- !job\nn: 3\n--- !job\nn: 4\n--- !job\nn: 5\n";
    st = stamps_run(s, "010", NULL, app, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "AmSmAm"), "off, on, off: %s %s", hows, err);

    /* an entry a stamp named can be replaced, if the stamp was ignored */
    s = "%YAML 2.0\n--- &\njob: {n: !int}\n--- !job\nn: 3\n--- &\njob: {n: !str}\n---\nx\n";
    st = stamps_run(s, "1", NULL, NULL, hows, err, sizeof err);
    CHECK(st == OYLNG_ERR_SCHEMA && strstr(err, "replaced after a type reached it"), "on: %s", err);
    st = stamps_run(s, "0", NULL, NULL, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "NaNa"), "off: %s %s", hows, err);

    /* a published name: off, nothing is looked up */
    const char *ab = "%YAML 2.0\n--- &\na: !int\n";
    char hab[65], ps[200];
    hex_of(ab, strlen(ab), hab);
    shelf sh = { .files = { ab } };
    oylng_schema *app2 = oylng_schema_new("%YAML 2.0\n--- &\nr: !str\n", 24, S("r"), 0);
    snprintf(ps, sizeof ps, "%%YAML 2.0\n--- !x.org/a.yes#sha256=%s\n1\n", hab);
    st = stamps_run(ps, "0", &sh, app2, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "As") && sh.calls == 0, "off: %s %d calls %s", hows, sh.calls, err);
    st = stamps_run(ps, "0", &sh, NULL, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "Na") && sh.calls == 0, "off, no schema of the application's: %s %d calls %s",
          hows, sh.calls, err);
    st = stamps_run(ps, "1", &sh, app2, hows, err, sizeof err);
    CHECK(st == OYLNG_OK && !strcmp(hows, "Pi") && sh.calls == 1, "on: %s %d calls %s", hows, sh.calls, err);
    free(sh.copy);
    oylng_parser_set_stamps(NULL, false);
    oylng_schema_free(app2);
    oylng_schema_free(app);
}

/* Section 12: an application's code for named types. A binder counts what
 * it makes and lets go, and notes each call's first letter in order. */
typedef struct {
    int  made, dropped, calls;
    char order[64];
} binder;

typedef struct { int64_t x, y; int id; } point;
typedef struct { point *a, *b; } line;

static void note(binder *b, char c) {
    size_t n = strlen(b->order);
    if (n + 1 < sizeof b->order) b->order[n] = c, b->order[n + 1] = 0;
    b->calls++;
}

static const char *bind_point(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    binder *b = ctx;
    note(b, 'p');
    point p = { 0 };
    if (!oylng_value_int(t, oylng_value_get(t, v, S("x")), &p.x) || !oylng_value_int(t, oylng_value_get(t, v, S("y")), &p.y))
        return "a point has two numbers";
    if (p.x < 0) return "x is negative";
    point *o = malloc(sizeof *o);
    *o = p;
    o->id = ++b->made;
    *obj = o;
    return NULL;
}

static const char *bind_line(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    binder *b = ctx;
    note(b, 'l');
    point *a = oylng_value_take(t, oylng_value_get(t, v, S("a"))), *c = oylng_value_take(t, oylng_value_get(t, v, S("b")));
    if (!a || !c) {
        b->dropped += !!a + !!c;
        free(a);
        free(c);
        return "a line needs two points";
    }
    line *l = malloc(sizeof *l);
    *l = (line){ a, c };
    b->made++;
    *obj = l;
    return NULL;
}

static const char *bind_money(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    binder *b = ctx;
    note(b, 'm');
    oylng_str s = oylng_value_text(t, v);
    int64_t *cents = malloc(sizeof *cents);
    *cents = 0;
    for (size_t i = 0; i < s.len; i++)
        if (s.data[i] != '.') *cents = *cents * 10 + (s.data[i] - '0');
    b->made++;
    *obj = cents;
    return NULL;
}

static const char *bind_nothing(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    (void)t, (void)v, (void)obj;
    note(ctx, 'n');
    return NULL;
}

static void drop_obj(void *ctx, void *o) {
    ((binder *)ctx)->dropped++;
    free(o);
}

/* A binding that notes 's' or 'h' and makes an int */
static const char *bind_tag(binder *b, char c, void **obj) {
    note(b, c);
    int *o = malloc(sizeof *o);
    *o = ++b->made;
    *obj = o;
    return NULL;
}
static const char *bind_spot(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    (void)t, (void)v;
    return bind_tag(ctx, 's', obj);
}
static const char *bind_shape(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    (void)t, (void)v;
    return bind_tag(ctx, 'h', obj);
}

static oylng_typed *typed_next(oylng_parser *p, const oylng_schema *app, char *err, size_t errlen);

/* The calls the document of s makes with names bound to code, a
 * space-separated list of name=letter (p point, s spot, h shape, n
 * nothing), and each value's bound name at the paths given, or '-' */
static void chain_calls(const char *s, const char *binds, const char *const *paths, size_t np, char *out, size_t cap) {
    binder b = { 0 };
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    char bs[200];
    snprintf(bs, sizeof bs, "%s", binds);
    for (char *w = strtok(bs, " "); w; w = strtok(NULL, " ")) {
        char *eqs = strchr(w, '=');
        *eqs = 0;
        oylng_bind_fn fn = eqs[1] == 'p' ? bind_point : eqs[1] == 's' ? bind_spot : eqs[1] == 'h' ? bind_shape : bind_nothing;
        oylng_parser_bind(p, (oylng_str){ w, strlen(w) }, fn, eqs[1] == 'n' ? NULL : drop_obj, &b);
    }
    char err[300];
    oylng_typed *y = typed_next(p, NULL, err, sizeof err);
    if (!y) {
        snprintf(out, cap, "refused: %.200s", err);
        oylng_parser_free(p);
        return;
    }
    size_t o = (size_t)snprintf(out, cap, "%s", b.order);
    for (size_t i = 0; i < np; i++) {
        oylng_value v = oylng_typed_root(y);
        char pa[100];
        snprintf(pa, sizeof pa, "%s", paths[i]);
        for (char *w = strtok(pa, "/"); w; w = strtok(NULL, "/"))
            v = isdigit((unsigned char)*w) ? oylng_value_item(y, v, (size_t)atoi(w)) : oylng_value_get(y, v, (oylng_str){ w, strlen(w) });
        oylng_str bn = oylng_value_bound(y, v);
        o += (size_t)snprintf(out + o, cap - o, " %.*s", bn.data ? (int)bn.len : 1, bn.data ? bn.data : "-");
    }
    oylng_typed_free(y);
    oylng_parser_free(p);
}

static void drop_line(void *ctx, void *o) {
    line *l = o;
    ((binder *)ctx)->dropped += 3;
    free(l->a);
    free(l->b);
    free(l);
}

/* The next document of p typed, or NULL and the error in err */
static oylng_typed *typed_next(oylng_parser *p, const oylng_schema *app, char *err, size_t errlen) {
    oylng_typed *y = NULL;
    oylng_status st = oylng_parse_typed(p, app, &y);
    snprintf(err, errlen, "%s", st == OYLNG_OK ? "" : oylng_parser_error(p));
    return y;
}

static void bindings(void) {
    char err[300];
    binder b = { 0 };

    /* section 12's first example: money bound, invoice not */
    const char *s = "%YAML 2.0\n--- &\nmoney: !str(/[0-9]+[.][0-9]{2}/)\ninvoice:\n  total: !money\n"
                    "--- !invoice\ntotal: 19.99\n";
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    CHECK(oylng_parser_bind(p, S("money"), bind_money, drop_obj, &b) == OYLNG_OK, "bound");
    oylng_typed *y = typed_next(p, NULL, err, sizeof err);
    oylng_value r = oylng_typed_root(y), total = oylng_value_get(y, r, S("total"));
    int64_t *cents = oylng_value_object(y, total);
    CHECK(y && cents && *cents == 1999 && !oylng_value_object(y, r) && b.calls == 1, "money is 1999 cents, the invoice nothing: %s", err);
    CHECK(oylng_value_type(y, total) == OYLNG_T_STR && eq(oylng_value_text(y, total), "19.99", 5), "and still the string it was");
    oylng_parser_free(p);
    CHECK(b.dropped == 0, "the document outlives the parser");
    oylng_typed_free(y);
    CHECK(b.made == 1 && b.dropped == 1, "an object not taken is let go with the document");

    /* the second: a union's alternatives bound, innermost first; *o twice
       is two calls and two objects; a line takes its points */
    const char *as = "%YAML 2.0\n--- &\npoint:\n  x: !int\n  y: !int\nsweep:\n  target: !int\nline: {a: !point, b: !point}\n"
                     "shape: {!|: [!point, !sweep, !line]}\nq: {x: !int, y: !int}\n"
                     "r: {shapes: [!shape, ...], same: [!q, ...]}\n";
    oylng_schema *app = oylng_schema_new(as, strlen(as), S("r"), 0);
    CHECK(oylng_schema_status(app) == OYLNG_OK, "the application's schema: %s", oylng_schema_error(app));
    b = (binder){ 0 };
    s = "%YAML 2.0\n--- &\no: {x: 1, y: 2}\n---\nshapes: [*o, {target: 3}, *o, {a: *o, b: {x: 5, y: 6}}]\nsame: [*o, *o]\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    oylng_parser_bind(p, S("sweep"), bind_nothing, NULL, &b);
    oylng_parser_bind(p, S("line"), bind_line, drop_line, &b);
    y = typed_next(p, app, err, sizeof err);
    r = oylng_typed_root(y);
    oylng_value sh = oylng_value_get(y, r, S("shapes")), same = oylng_value_get(y, r, S("same"));
    point *p0 = oylng_value_object(y, oylng_value_item(y, sh, 0)), *p2 = oylng_value_object(y, oylng_value_item(y, sh, 2));
    line *l3 = oylng_value_object(y, oylng_value_item(y, sh, 3));
    CHECK(y && !strcmp(b.order, "pnpppl") && b.made == 5, "the calls, in order: %s, %d made %s", b.order, b.made, err);
    CHECK(p0 && p2 && p0 != p2 && p0->x == 1 && p2->y == 2 && oylng_value_item(y, sh, 0) != oylng_value_item(y, sh, 2),
          "*o twice: two values, two objects");
    CHECK(!oylng_value_object(y, oylng_value_item(y, sh, 1)) && l3 && l3->a->x == 1 && l3->b->y == 6 &&
          !oylng_value_object(y, oylng_value_get(y, oylng_value_item(y, sh, 3), S("a"))), "a line, its points taken");
    CHECK(oylng_value_item(y, same, 0) == oylng_value_item(y, same, 1) && !oylng_value_object(y, oylng_value_item(y, same, 0)),
          "where nothing is bound, *o twice is still one value");
    point *mine = oylng_value_take(y, oylng_value_item(y, sh, 0));
    CHECK(mine == p0 && !oylng_value_object(y, oylng_value_item(y, sh, 0)) && !oylng_value_take(y, oylng_value_item(y, sh, 0)),
          "taken, once");
    oylng_typed_free(y);
    CHECK(b.dropped == 4, "the rest let go: %d", b.dropped);
    free(mine);

    /* refused by a binding: the document, at the value; what was made let
       go; a refusal of the schema's comes first, and calls nothing */
    b = (binder){ 0 };
    CHECK(!typed_next(p, app, err, sizeof err) && !*err, "the end");
    oylng_parser_free(p);
    s = "%YAML 2.0\n---\nshapes: [{x: 1, y: 1}, {target: 2}, {x: -1, y: 0}]\nsame: []\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    CHECK(!typed_next(p, app, err, sizeof err) && !strcmp(err, "/shapes/2: x is negative") &&
          oylng_parser_error_mark(p).line == 3 && oylng_parser_error_mark(p).col == 37,
          "refused: %s at %zu:%zu", err, oylng_parser_error_mark(p).line, oylng_parser_error_mark(p).col);
    CHECK(b.calls == 2 && b.made == 1 && b.dropped == 1, "what was made, let go: %d %d %d", b.calls, b.made, b.dropped);
    oylng_parser_free(p);
    b = (binder){ 0 };
    s = "%YAML 2.0\n---\nshapes: [{x: 1, y: 1}, {x: a, y: 0}]\nsame: []\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    CHECK(!typed_next(p, app, err, sizeof err) && strstr(err, "/shapes/1/x") && b.calls == 0, "the schema first: %s", err);
    oylng_parser_free(p);
    b = (binder){ 0 };
    s = "%YAML 2.0\n---\nshapes: [{a: {x: 1, y: 1}, b: {x: 2, y: 2}}]\nsame: []\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("line"), bind_line, drop_line, &b);
    CHECK(!typed_next(p, app, err, sizeof err) && !strcmp(err, "/shapes/0: a line needs two points") && b.calls == 1,
          "a line with its points unbound: %s", err);
    oylng_parser_free(p);

    /* a root bound, by the stream's own table; bound again, and unbound */
    binder b2 = { 0 };
    b = (binder){ 0 };
    s = "%YAML 2.0\n--- &\npoint: {x: !int, y: !int}\n--- !point\nx: 7\ny: 8\n--- !point\nx: 9\ny: 9\n--- !point\nx: 1\ny: 1\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    y = typed_next(p, NULL, err, sizeof err);
    point *root = oylng_value_take(y, oylng_typed_root(y));
    CHECK(root && root->x == 7 && root->y == 8, "the root's object, by the stamp: %s", err);
    free(root);
    oylng_typed_free(y);
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b2);
    y = typed_next(p, NULL, err, sizeof err);
    CHECK(b.calls == 1 && b2.calls == 1 && oylng_value_object(y, oylng_typed_root(y)), "bound again, replaced");
    oylng_typed_free(y);
    CHECK(oylng_parser_bind(p, S("point"), NULL, NULL, NULL) == OYLNG_OK && oylng_parser_bind(p, S("nope"), NULL, NULL, NULL) == OYLNG_OK,
          "unbound");
    y = typed_next(p, NULL, err, sizeof err);
    CHECK(y && b2.calls == 1 && !oylng_value_object(y, oylng_typed_root(y)), "and nothing called");
    oylng_typed_free(y);
    CHECK(b.dropped == b.made - 1 && b2.dropped == b2.made, "all let go: %d/%d %d/%d", b.dropped, b.made, b2.dropped, b2.made);
    oylng_parser_free(p);

    /* a chain of references: 1024 places, 1024 calls, 1024 objects */
    b = (binder){ 0 };
    char big[2048];
    size_t o = (size_t)snprintf(big, sizeof big, "%%YAML 2.0\n--- &\np: {x: 1, y: 1}\nl0: [*p, *p]\nn0: !point\n"
                                "point: {x: !int, y: !int}\n");
    for (int i = 1; i < 10; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "l%d: [*l%d, *l%d]\nn%d: [!n%d, ...]\n", i, i - 1, i - 1, i, i - 1);
    snprintf(big + o, sizeof big - o, "n10: [!n9, ...]\n--- !n10\n*l9\n");
    p = oylng_parser_new(big, strlen(big));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    y = typed_next(p, NULL, err, sizeof err);
    oylng_value first = oylng_typed_root(y), last = first;
    for (int i = 0; i < 10; i++) {
        first = oylng_value_item(y, first, 0);
        last = oylng_value_item(y, last, 1);
    }
    point *pf = oylng_value_object(y, first), *pl = oylng_value_object(y, last);
    CHECK(y && b.calls == 1024 && b.made == 1024 && pf && pl && pf->id == 1 && pl->id == 1024, "1024 points: %d calls %s",
          b.calls, err);
    oylng_typed_free(y);
    CHECK(b.dropped == 1024, "and let go: %d", b.dropped);
    oylng_parser_free(p);

    /* the one bound value below the second [*o] was seen below the first */
    b = (binder){ 0 };
    s = "%YAML 2.0\n--- &\no: {x: 1, y: 2}\npoint: {x: !int, y: !int}\nr: [[!point, ...], ...]\n--- !r\n- [*o]\n- [*o]\n";
    p = oylng_parser_new(s, strlen(s));
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    y = typed_next(p, NULL, err, sizeof err);
    r = oylng_typed_root(y);
    pf = oylng_value_object(y, oylng_value_item(y, oylng_value_item(y, r, 0), 0));
    pl = oylng_value_object(y, oylng_value_item(y, oylng_value_item(y, r, 1), 0));
    CHECK(y && b.calls == 2 && pf && pl && pf != pl, "found through a value seen before: %d calls %s", b.calls, err);
    oylng_typed_free(y);
    oylng_parser_free(p);

    CHECK(oylng_parser_bind(NULL, S("x"), bind_point, NULL, NULL) == OYLNG_ERR_INPUT &&
          !oylng_value_object(NULL, 0) && !oylng_value_take(NULL, 0) && !oylng_value_bound(NULL, 0).data, "NULLs");
    oylng_schema_free(app);

    /* names in a chain (yaml2 6a116a6): the outermost bound one's code
       runs, and no other's. a goes through spot, then point; b through
       point alone */
    char got[300];
    const char *const ab[] = { "a", "b" };
    const char *chain = "%YAML 2.0\n--- &\npoint: {x: !int, y: !int}\nspot: !point\npair: {a: !spot, b: !point}\n"
                        "--- !pair\na: {x: 1, y: 2}\nb: {x: 3, y: 4}\n";
    chain_calls(chain, "point=p spot=s", ab, 2, got, sizeof got);
    CHECK(!strcmp(got, "sp spot point"), "both bound, spot's runs for a: %s", got);
    chain_calls(chain, "point=p", ab, 2, got, sizeof got);
    CHECK(!strcmp(got, "pp point point"), "point alone bound: %s", got);
    chain_calls(chain, "spot=s", ab, 2, got, sizeof got);
    CHECK(!strcmp(got, "s spot -"), "spot alone bound: %s", got);
    chain_calls(chain, "", ab, 2, got, sizeof got);
    CHECK(!strcmp(got, " - -"), "nothing bound: %s", got);
    {
        oylng_parser *cp = oylng_parser_new(chain, strlen(chain));
        oylng_typed *cy = typed_next(cp, NULL, err, sizeof err);
        oylng_value a = oylng_value_get(cy, oylng_typed_root(cy), S("a"));
        CHECK(cy && eq(oylng_value_name(cy, a), "point", 5) && !oylng_value_bound(cy, a).data,
              "its name is still the nearest, point, and with nothing bound none is bound: %s", err);
        oylng_typed_free(cy);
        oylng_parser_free(cp);
    }

    /* a union's name counts the same way: shape's code takes the place of
       point's and sweep's, and gets what the alternative delivers */
    const char *const sh3[] = { "0", "1", "2" };
    const char *un = "%YAML 2.0\n--- &\npoint: {x: !int, y: !int}\nsweep: {target: !int}\nshape: {!|: [!point, !sweep]}\n"
                     "r: [!shape, ...]\n--- !r\n- {x: 1, y: 2}\n- {target: 3}\n- {x: 4, y: 5}\n";
    chain_calls(un, "point=p sweep=n", sh3, 3, got, sizeof got);
    CHECK(!strcmp(got, "pnp point sweep point"), "the alternatives bound: %s", got);
    chain_calls(un, "point=p sweep=n shape=h", sh3, 3, got, sizeof got);
    CHECK(!strcmp(got, "hhh shape shape shape"), "the union's name bound too: %s", got);

    /* each name counts once in a union, at its first place: p is reached
       through a, not through b, so b's code never runs for it */
    const char *const top[] = { "" };
    const char *twice = "%YAML 2.0\n--- &\np: {x: !int, y: !int}\na: !p\nb: !p\nu: {!|: [!a, !b]}\n--- !u\nx: 1\ny: 2\n";
    chain_calls(twice, "b=s p=p", top, 1, got, sizeof got);
    CHECK(!strcmp(got, "p p"), "through a, then p: %s", got);
    chain_calls(twice, "b=s a=h p=p", top, 1, got, sizeof got);
    CHECK(!strcmp(got, "h a"), "a bound: %s", got);
    chain_calls(twice, "u=s a=h", top, 1, got, sizeof got);
    CHECK(!strcmp(got, "s u"), "u, the outermost: %s", got);

    /* a chain of 200 names: n0 goes through all of them */
    {
        char *big2 = malloc(8000);
        size_t m = (size_t)snprintf(big2, 8000, "%%YAML 2.0\n--- &\npoint: {x: !int, y: !int}\n");
        for (int i = 0; i < 199; i++) m += (size_t)snprintf(big2 + m, 8000 - m, "n%d: !n%d\n", i, i + 1);
        m += (size_t)snprintf(big2 + m, 8000 - m, "n199: !point\nall: {a: !n0, b: !n150, c: !n199}\n"
                                                  "--- !all\na: {x: 1, y: 2}\nb: {x: 1, y: 2}\nc: {x: 1, y: 2}\n");
        const char *const abc[] = { "a", "b", "c" };
        chain_calls(big2, "n100=s point=p n180=h", abc, 3, got, sizeof got);
        CHECK(!strcmp(got, "shp n100 n180 point"), "200 names: %s", got);
        free(big2);
    }
}

/* ── Emitting typed data (section 13) ───────────────────── */

typedef struct { int x, y; const char *fail; int extra; } pt;

static const char *unbind_point(void *ctx, oylng_emitter *e, const void *o) {
    const pt *p = o;
    (void)ctx;
    if (p->fail) return p->fail;
    for (int i = 0; i <= p->extra; i++) {
        oylng_emit_map_start(e, false);
        oylng_emit_scalar(e, S("x"));
        oylng_emit_int(e, p->x);
        oylng_emit_scalar(e, S("y"));
        oylng_emit_int(e, p->y);
        oylng_emit_map_end(e);
    }
    return NULL;
}

/* A point's x as a signed number, in the spelling the code chose: +1 */
static const char *unbind_signed(void *ctx, oylng_emitter *e, const void *o) {
    const pt *p = o;
    char b[24];
    (void)ctx;
    snprintf(b, sizeof b, "%+d", p->x);
    oylng_emit_scalar(e, (oylng_str){ b, strlen(b) });
    return NULL;
}

/* A typed value in short: s3 i5 I5 f0.5 d19.99 btrue n D2001-02-03
 * T(as written) x(hex) A(untyped, as written) {k:v,...} [v,...], an entry's
 * name after an @ */
static void brief(char *o, size_t cap, const oylng_typed *y, oylng_value v) {
    size_t k = strlen(o);
    if (k + 64 >= cap) return;
    oylng_str t = oylng_value_text(y, v);
    int64_t i;
    uint64_t u;
    switch (oylng_value_type(y, v)) {
    case OYLNG_T_ANY: {
        oylng_node n = oylng_value_node(y, v);
        const oylng_tree *tr = oylng_typed_tree(y);
        if (oylng_node_kind(tr, n) == OYLNG_NODE_STRING) {
            oylng_str s = oylng_node_text(tr, n);
            snprintf(o + k, cap - k, "A%.*s", (int)s.len, s.data);
        } else snprintf(o + k, cap - k, "A%s", oylng_node_kind(tr, n) == OYLNG_NODE_MAPPING ? "{}" : "[]");
        break;
    }
    case OYLNG_T_STR: snprintf(o + k, cap - k, "s%.*s", (int)t.len, t.data); break;
    case OYLNG_T_INT:
        if (oylng_value_int(y, v, &i)) snprintf(o + k, cap - k, "i%lld", (long long)i);
        else if (oylng_value_uint(y, v, &u)) snprintf(o + k, cap - k, "i%llu", (unsigned long long)u);
        break;
    case OYLNG_T_BIGINT: snprintf(o + k, cap - k, "I%.*s", (int)t.len, t.data); break;
    case OYLNG_T_FLOAT: snprintf(o + k, cap - k, "f%.17g", oylng_value_float(y, v)); break;
    case OYLNG_T_DECIMAL: snprintf(o + k, cap - k, "d%.*s", (int)t.len, t.data); break;
    case OYLNG_T_BOOL: snprintf(o + k, cap - k, "b%s", oylng_value_bool(y, v) ? "true" : "false"); break;
    case OYLNG_T_NULL: snprintf(o + k, cap - k, "n"); break;
    case OYLNG_T_DATE: snprintf(o + k, cap - k, "D%.*s", (int)t.len, t.data); break;
    case OYLNG_T_DATETIME: snprintf(o + k, cap - k, "T%.*s", (int)t.len, t.data); break;
    case OYLNG_T_BINARY: {
        oylng_str b = oylng_value_bytes(y, v);
        o[k++] = 'x';
        for (size_t j = 0; j < b.len && k + 3 < cap; j++, k += 2) snprintf(o + k, cap - k, "%02x", (unsigned char)b.data[j]);
        o[k] = 0;
        break;
    }
    case OYLNG_T_SEQUENCE:
    case OYLNG_T_MAPPING: {
        bool map = oylng_value_type(y, v) == OYLNG_T_MAPPING;
        snprintf(o + k, cap - k, "%s", map ? "{" : "[");
        for (size_t j = 0; j < oylng_value_count(y, v); j++) {
            k = strlen(o);
            if (j && k + 2 < cap) snprintf(o + k, cap - k, ",");
            if (map) {
                oylng_str key = oylng_value_key(y, v, j);
                k = strlen(o);
                snprintf(o + k, cap - k, "%.*s:", (int)key.len, key.data);
            }
            brief(o, cap, y, map ? oylng_value_at(y, v, j) : oylng_value_item(y, v, j));
        }
        k = strlen(o);
        snprintf(o + k, cap - k, "%s", map ? "}" : "]");
        break;
    }
    default: break;
    }
    oylng_str nm = oylng_value_name(y, v);
    k = strlen(o);
    if (nm.data && k + nm.len + 2 < cap) snprintf(o + k, cap - k, "@%.*s", (int)nm.len, nm.data);
}

/* One document written by a script, under the schema of the table yaml
 * (none if NULL) with root, then read back with it. The script's words: {
 * } [ ] a block collection, {{ [[ a flow one; s:text a string, _ for a
 * space; i: u: f: B: D: (bigint, decimal) a number; b:1 b:0; n null; x:hex
 * bytes; o:name a point object, o!name one that refuses, o2name one that
 * writes two. The output and the error in out and err, what it reads
 * back as in back. */
typedef struct { oylng_status st; char out[600], err[400], back[400]; } emitted;

static emitted emit_doc(const char *yaml, const char *root, const char *script, bool flow) {
    emitted r = { 0 };
    oylng_schema *sc = NULL;
    char tx[600];
    if (yaml) {
        snprintf(tx, sizeof tx, "%%YAML 2.0\n--- &\n%s", yaml);
        sc = oylng_schema_new(tx, strlen(tx), S(root), 0);
        if (oylng_schema_status(sc) != OYLNG_OK) {
            snprintf(r.err, sizeof r.err, "schema: %s", oylng_schema_error(sc));
            r.st = OYLNG_ERR_INPUT;
            oylng_schema_free(sc);
            return r;
        }
    }
    oylng_emitter *e = oylng_emitter_new();
    oylng_emitter_set_flow(e, flow);
    if (sc) oylng_emitter_set_schema(e, sc);
    oylng_emitter_bind(e, S("point"), unbind_point, NULL);
    oylng_emitter_bind(e, S("sweep"), unbind_point, NULL);   /* written as a point */
    oylng_emitter_bind(e, S("signed"), unbind_signed, NULL);
    oylng_status st = oylng_emit_stream_start(e);
    if (st == OYLNG_OK) st = oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    char w[200];
    for (const char *q = script; st == OYLNG_OK && *q;) {
        while (*q == ' ') q++;
        size_t n = strcspn(q, " ");
        if (!n) break;
        snprintf(w, sizeof w, "%.*s", (int)n, q);
        q += n;
        for (char *c = w; *c; c++)
            if (*c == '_') *c = ' ';
        const char *a = w + 2;
        pt p = { 1, 2, NULL, 0 };
        if (!strcmp(w, "{")) st = oylng_emit_map_start(e, false);
        else if (!strcmp(w, "{{")) st = oylng_emit_map_start(e, true);
        else if (!strcmp(w, "[")) st = oylng_emit_seq_start(e, false);
        else if (!strcmp(w, "[[")) st = oylng_emit_seq_start(e, true);
        else if (!strcmp(w, "}")) st = oylng_emit_map_end(e);
        else if (!strcmp(w, "]")) st = oylng_emit_seq_end(e);
        else if (!strcmp(w, "n")) st = oylng_emit_null(e);
        else if (w[0] == 's') st = oylng_emit_scalar(e, S(a));
        else if (w[0] == 'i') st = oylng_emit_int(e, strtoll(a, NULL, 10));
        else if (w[0] == 'u') st = oylng_emit_uint(e, strtoull(a, NULL, 10));
        else if (w[0] == 'f') st = oylng_emit_float(e, strtod(a, NULL));
        else if (w[0] == 'B') st = oylng_emit_number(e, OYLNG_T_BIGINT, S(a));
        else if (w[0] == 'D') st = oylng_emit_number(e, OYLNG_T_DECIMAL, S(a));
        else if (w[0] == 'b') st = oylng_emit_bool(e, *a == '1');
        else if (w[0] == 'x') {
            char by[64];
            size_t m = 0;
            for (; a[0] && a[1] && m < sizeof by; a += 2) {
                unsigned h;
                sscanf(a, "%2x", &h);
                by[m++] = (char)h;
            }
            st = oylng_emit_bytes(e, (oylng_str){ by, m });
        } else if (w[0] == 'o') {
            if (w[1] == '!') p.fail = "it has no x";
            if (w[1] == '2') p.extra = 1;
            st = oylng_emit_object(e, S(w[1] == ':' ? a : w + 2), &p);
        } else st = OYLNG_ERR_INPUT;
    }
    if (st == OYLNG_OK) st = oylng_emit_doc_end(e);
    if (st == OYLNG_OK) st = oylng_emit_stream_end(e);
    r.st = st;
    oylng_str o = oylng_emitter_output(e);
    snprintf(r.out, sizeof r.out, "%.*s", (int)o.len, o.data);
    if (st != OYLNG_OK) snprintf(r.err, sizeof r.err, "%s", oylng_emitter_error(e) ? oylng_emitter_error(e) : "?");
    else {
        oylng_parser *rp = oylng_parser_new(o.data, o.len);
        oylng_typed *y;
        if (oylng_parse_typed(rp, sc, &y) == OYLNG_OK && y) {
            brief(r.back, sizeof r.back, y, oylng_typed_root(y));
            oylng_typed_free(y);
        } else snprintf(r.back, sizeof r.back, "unread: %s", oylng_parser_error(rp));
        oylng_parser_free(rp);
    }
    oylng_emitter_free(e);
    oylng_schema_free(sc);
    return r;
}

/* What a document is written as, and reads back as, or its refusal */
#define WRITES(yaml, root, script, flow, text, back_)                                                        \
    do {                                                                                                     \
        emitted r_ = emit_doc(yaml, root, script, flow);                                                     \
        CHECK(r_.st == OYLNG_OK && strstr(r_.out, text) && !strcmp(r_.back, back_),                          \
              "%s: [%d] %s\n%s\nreads back %s", script, r_.st, r_.err, r_.out, r_.back);                      \
    } while (0)
#define REFUSES(yaml, root, script, flow, msg)                                                               \
    do {                                                                                                     \
        emitted r_ = emit_doc(yaml, root, script, flow);                                                     \
        CHECK(r_.st == OYLNG_ERR_EMIT && strstr(r_.err, msg) && (!(yaml) || !strcmp(r_.out, "%YAML 2.0\n")), \
              "%s: [%d] %s\n%s", script, r_.st, r_.err, r_.out);                                             \
    } while (0)

/* Floats written under !float (section 4.4): each finite double reads
 * back with its bits, in the fewest significant digits that do, as the C
 * library finds them; positional, with a point, for 0 and from 1e-7 to
 * below 1e21, else with an exponent and its sign. Raw bits, decimals of
 * up to 17 digits from 1e-30 to 1e30, and values next to the bounds, the
 * same every run. */
static uint64_t mix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

static void float_spellings(void) {
    const char *yaml = "%YAML 2.0\n--- &\nf: !float\n";
    oylng_schema *sc = oylng_schema_new(yaml, strlen(yaml), S("f"), 0);
    uint64_t x = 1;
    int bad = 0, tried = 0;
    char first[200] = "";
    for (int i = 0; i < 60000; i++) {
        double f;
        uint64_t r = mix(&x);
        if (i % 3 == 0) {
            memcpy(&f, &r, sizeof f);
            if (!isfinite(f)) continue;
        } else if (i % 3 == 1) {
            char d[48];
            snprintf(d, sizeof d, "%s%llue%d", r & 1 ? "-" : "", (unsigned long long)(r >> 8) % 100000000000000000ull,
                     (int)((r >> 1) % 61) - 30);
            f = strtod(d, NULL);
        } else {
            /* a positive double's bits count up with it */
            double b = (r & 1) ? 1e-7 : 1e21;
            uint64_t bits;
            memcpy(&bits, &b, sizeof bits);
            bits += (uint64_t)((int64_t)((r >> 1) % 9) - 4);
            memcpy(&f, &bits, sizeof f);
            if (r & 2) f = -f;
        }
        tried++;
        oylng_emitter *e = oylng_emitter_new();
        oylng_emitter_set_schema(e, sc);
        oylng_status st = oylng_emit_stream_start(e);
        if (st == OYLNG_OK) st = oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
        if (st == OYLNG_OK) st = oylng_emit_float(e, f);
        if (st == OYLNG_OK) st = oylng_emit_doc_end(e);
        oylng_str out = oylng_emitter_output(e);
        char text[80] = "";
        const char *at = out.data ? strstr(out.data, "--- ") : NULL;
        if (at) snprintf(text, sizeof text, "%.*s", (int)strcspn(at + 4, "\n"), at + 4);
        const char *why = NULL;
        double g = NAN;
        if (st != OYLNG_OK) why = oylng_emitter_error(e);
        else {
            oylng_parser *p = oylng_parser_new(out.data, out.len);
            oylng_typed *y = NULL;
            if (oylng_parse_typed(p, sc, &y) == OYLNG_OK && y && oylng_value_type(y, oylng_typed_root(y)) == OYLNG_T_FLOAT)
                g = oylng_value_float(y, oylng_typed_root(y));
            oylng_typed_free(y);
            oylng_parser_free(p);
            const char *ex = strchr(text, 'e');
            bool positional = f == 0 || (fabs(f) >= 1e-7 && fabs(f) < 1e21);
            /* its significant digits, and the fewest the C library needs:
               zeros before the point of a whole number hold places, and its
               .0 marks it a float, but any other zero at the end is a digit */
            char dig[64];
            size_t nd = 0;
            const char *pt = strchr(text, '.');
            bool whole = !ex && pt && !strcmp(pt, ".0");
            for (const char *q = text; *q && q != ex && !(whole && q == pt); q++)
                if (*q >= '0' && *q <= '9' && nd < sizeof dig) dig[nd++] = *q;
            size_t lo = 0;
            while (lo < nd && dig[lo] == '0') lo++;
            if (whole)
                while (nd > lo && dig[nd - 1] == '0') nd--;
            int need = 1;
            for (; f != 0 && need < 17; need++) {
                char c[48];
                snprintf(c, sizeof c, "%.*e", need - 1, f);
                if (strtod(c, NULL) == f) break;
            }
            if (memcmp(&f, &g, sizeof f)) why = "it reads back otherwise";
            else if (positional && (ex || !strchr(text, '.'))) why = "not positional with a point";
            else if (!positional && (!ex || (ex[1] != '+' && ex[1] != '-'))) why = "no signed exponent";
            else if (f != 0 && (int)(nd - lo) > need) why = "more digits than it needs";
        }
        if (why && !bad++) snprintf(first, sizeof first, "%.17g written %s: %s", f, text, why);
        oylng_emitter_free(e);
    }
    CHECK(!bad && tried > 50000, "%d of %d floats: first %s", bad, tried, first);
    oylng_schema_free(sc);
}

static void emitting(void) {
    /* section 13: a string the type would read as something else is
       refused, since quotes don't count (section 4.1, yaml2 961fd4c) */
    const char *u = "n: {!|: [!int, !str]}\n";
    REFUSES(u, "n", "s:3", false, "/: no spelling of the string '3' reads back: written 3, it reads as !int");
    WRITES(u, "n", "i:3", false, "--- 3\n", "i3@n");
    WRITES(u, "n", "s:abc", false, "--- abc\n", "sabc@n");
    REFUSES(u, "n", "f:1.5", false, "/: no spelling of the float 1.5 reads back: written 1.5, it reads as !str");
    /* and one that would refuse the document: 1e999 is a float too large
       (yaml2 8b774fc) */
    REFUSES("n: {!|: [!float, !str]}\n", "n", "s:1e999", false,
            "/: no spelling of the string '1e999' reads back: written 1e999, 1e999 is too large for binary64");
    /* 5 fits {!|: [!signed, !str]} only as the string "5", so it's refused
       (yaml2 340fe27) */
    REFUSES("signed: !int(/[-+][0-9]+/)\nn: {!|: [!signed, !str]}\n", "n", "i:5", false,
            "/: no spelling of the integer 5 reads back: written 5, it reads as !str");

    /* a number in its standard spelling alone, which a pattern must match,
       else refused: no +, no zeros added (section 13, yaml2 54fcd63) */
    const char *sg = "signed: !int(/[-+][0-9]+/)\noffsets: [!signed, ...]\n";
    WRITES(sg, "offsets", "[ i:-3 i:-10 ]", false, "- -3\n- -10\n", "[i-3@signed,i-10@signed]@offsets");
    WRITES(sg, "offsets", "[ i:-3 i:-10 ]", true, "[-3, -10]", "[i-3@signed,i-10@signed]@offsets");
    REFUSES(sg, "offsets", "[ i:-3 i:5 ]", false, "/1: no spelling of the integer 5 reads back: written 5, '5' does not match");
    REFUSES(sg, "offsets", "[ i:0 ]", false, "/0: no spelling of the integer 0 reads back: written 0, '0' does not match");
    REFUSES(sg, "offsets", "[ i:-5 s:5 ]", false, "/1: no spelling of the string '5' reads back: written 5, '5' does not match");
    REFUSES("p: !int(/[0-9]{4}/)\n", "p", "i:7", false, "/: no spelling of the integer 7 reads back: written 7, '7' does not match");
    WRITES("p: !int(/[0-9]{4}/)\n", "p", "i:1234", false, "--- 1234\n", "i1234@p");
    REFUSES("p: !int(/[0-9]{4}/)\n", "p", "i:12345", false, "no spelling of the integer 12345");
    REFUSES("p: !bigint(/[+][0-9]+/)\n", "p", "B:123456789012345678901234567890", false,
            "no spelling of the integer 123456789012345678901234567890 reads back");

    /* a float in the fewest significant digits, positionally from 1e-7 to
       below 1e21 with .0 where it has no fraction, else with an exponent
       and its sign (section 4.4); a decimal as written, so 19.9 isn't
       19.90; and a pattern must match that spelling, or it's refused */
    const char *it = "cents: !decimal(/[0-9]+\\.[0-9]{2}/)\nnone: !null(~|null|)\n"
                     "item:\n  price: !cents\n  ratio: !float(/[0-9]*\\.[0-9]+/)\n  note: {!|: [!none, !str]}\n";
    WRITES(it, "item", "{ s:price D:19.99 s:ratio f:1 s:note n }", false, "price: 19.99\nratio: 1.0\nnote: ~\n",
           "{price:d19.99@cents,ratio:f1,note:n@none}@item");
    WRITES(it, "item", "{ s:price D:0.50 s:ratio f:0.5 s:note s:fine }", true, "{price: 0.50, ratio: 0.5, note: fine}",
           "{price:d0.50@cents,ratio:f0.5,note:sfine}@item");
    REFUSES(it, "item", "{ s:price D:0.50 s:ratio f:0.5 s:note s:null }", false,
            "/note: no spelling of the string 'null' reads back: written null, it reads as !null");
    REFUSES(it, "item", "{ s:price D:19.9 s:ratio f:1 s:note n }", false,
            "/price: no spelling of the decimal 19.9 reads back: written 19.9, '19.9' does not match");
    REFUSES("c: !float(/[0-9]+\\.[0-9]{2}/)\n", "c", "f:19.9", false,
            "/: no spelling of the float 19.9 reads back: written 19.9, '19.9' does not match");
    WRITES("c: !float(/[0-9]+\\.[0-9]{2}/)\n", "c", "f:19.25", false, "--- 19.25\n", "f19.25@c");
    REFUSES("c: !float(/\\.[0-9]+/)\n", "c", "f:0.25", false, "no spelling of the float 0.25 reads back");
    REFUSES("c: !decimal(/[0-9]+\\./)\n", "c", "D:12", false, "no spelling of the decimal 12 reads back");
    WRITES("c: [!float, ...]\n", "c", "[[ f:1e21 f:1e-7 f:1e-8 f:-0 f:123456.789 f:5e-324 f:2 f:1e20 f:-1.5e300 ]", false,
           "[1e+21, 0.0000001, 1e-8, -0.0, 123456.789, 5e-324, 2.0, 100000000000000000000.0, -1.5e+300]",
           "[f1e+21,f9.9999999999999995e-08,f1e-08,f-0,f123456.789,f4.9406564584124654e-324,f2,f1e+20,"
           "f-1.5000000000000001e+300]@c");
    WRITES("c: {!|: [!int, !float]}\n", "c", "f:2", false, "--- 2.0\n", "f2@c");
    REFUSES(NULL, NULL, "f:inf", false, "a float that isn't finite");
    REFUSES(NULL, NULL, "B:007", false, "'007' isn't an integer in the standard spelling");
    REFUSES(NULL, NULL, "D:+1.5", false, "'+1.5' isn't a number in the standard spelling");

    /* an integer reads back as one, !int or !bigint */
    WRITES("k: !bigint\n", "k", "i:-5", false, "--- -5\n", "I-5@k");
    WRITES("k: !int\n", "k", "B:18446744073709551615", false, "--- 18446744073709551615\n", "i18446744073709551615@k");
    WRITES("k: !int\n", "k", "u:18446744073709551615", false, "--- 18446744073709551615\n", "i18446744073709551615@k");
    REFUSES("k: !int\n", "k", "B:18446744073709551616", false, "no spelling of the integer 18446744073709551616");

    /* null is null everywhere, an optional key's value too, or the first
       word of its type, written "" where that is empty; a boolean is its
       first word too (section 13, yaml2 961fd4c) */
    const char *m = "m: {a?: {!|: [!null, !int]}, b?: !str, c: !null, d?: !null(~), e?: [!null(-|), ...], f?: !null(|-)}\n";
    WRITES(m, "m", "{ s:a n s:c n s:d n s:f n }", false, "a: null\nc: null\nd: ~\nf: \"\"\n", "{a:n,c:n,d:n,f:n}@m");
    WRITES(m, "m", "{ s:a n s:c n }", true, "{a: null, c: null}", "{a:n,c:n}@m");
    WRITES(m, "m", "{ s:c n s:e [[ n ] }", false, "e: ['-']", "{c:n,e:[n]}@m");
    WRITES(m, "m", "{ s:c n s:e [ n ] }", false, "e:\n  - '-'\n", "{c:n,e:[n]}@m");
    REFUSES(m, "m", "{ s:b n s:c n }", false, "/b: no spelling of null reads back: written null, it reads as !str");
    WRITES("r: {!|: [!null, !str]}\n", "r", "n", false, "--- null\n", "n@r");
    REFUSES("r: {!|: [!null, !str]}\n", "r", "s:", false, "/: no spelling of the string '' reads back: written \"\", it reads as !null");
    WRITES("r: {!|: [!null(null), !str]}\n", "r", "s:", false, "--- \"\"\n", "s@r");
    const char *sw = "s: {!|: [!true(yes|on), !false(no|off)]}\n";
    WRITES(sw, "s", "b:1", false, "--- yes\n", "btrue@s");
    WRITES(sw, "s", "b:0", false, "--- no\n", "bfalse@s");
    WRITES("s: {!|: [!bool, !true(yes)]}\n", "s", "b:1", false, "--- true\n", "btrue@s");
    WRITES("s: {!|: [!false, !true(#t)]}\n", "s", "b:1", false, "--- '#t'\n", "btrue@s");
    REFUSES("s: !true\n", "s", "b:0", false, "/: no spelling of false reads back: written false, expected !true, got 'false'");

    /* a string where its type is another: refused, quoted or not */
    const char *t = "t: {x: !bool, y: !str, z: !any, w: {!|: [!null, !bool, !int, !str]}, v: {!|: [!date, !str]}}\n";
    WRITES(t, "t", "{ s:x b:1 s:y s:true s:z i:5 s:w s:yes s:v s:x }", false, "x: true\ny: true\nz: 5\nw: yes\nv: x\n",
           "{x:btrue,y:strue,z:A5,w:syes,v:sx}@t");
    REFUSES(t, "t", "{ s:x b:1 s:y s:a s:z n s:w s:true s:v s:x }", false,
            "/w: no spelling of the string 'true' reads back: written true, it reads as !bool");
    REFUSES(t, "t", "{ s:x s:true s:y s:a s:z n s:w n s:v s:a }", false, "/x: no spelling of the string 'true' reads back: written true, it reads as !bool");
    REFUSES(t, "t", "{ s:x b:1 s:y s:a s:z n s:w n s:v s:2001-02-03 }", false,
            "/v: no spelling of the string '2001-02-03' reads back: written 2001-02-03, it reads as !date");
    REFUSES(t, "t", "{ s:x b:1 s:y i:5 s:z n s:w n s:v s:a }", false, "/y: no spelling of the integer 5 reads back");
    WRITES("s: !str\n", "s", "s:a_b:_c", false, "--- 'a b: c'\n", "sa b: c@s");
    REFUSES(t, "t", "{ s:x b:1 }", false, "/: missing y");
    REFUSES(t, "t", "{ s:x b:1 s:y s:a s:z n s:w n s:v s:a s:q i:1 }", false, "/: unknown key q");

    /* bytes in base64, refused where that reads as something else */
    WRITES("x: !binary\n", "x", "x:686921", false, "--- aGkh\n", "x686921@x");
    WRITES("x: !binary\n", "x", "x:", false, "--- \"\"\n", "x@x");
    REFUSES("x: {!|: [!int, !binary]}\n", "x", "x:d76df8", false,
            "/: no spelling of the bytes 1234 reads back: written 1234, it reads as !int");

    /* a key is a string */
    REFUSES(NULL, NULL, "{ i:1 s:a }", false, "a key is a string, and this is !int");
    REFUSES(NULL, NULL, "{ o:point s:a }", false, "a key is a string, and this is a point object");

    /* with no schema, a value is written in its standard spelling */
    {
        emitted r = emit_doc(NULL, NULL, "{ s:a i:5 s:b s:3 s:c n s:d f:0.5 s:e o:point }", false);
        CHECK(r.st == OYLNG_OK && strstr(r.out, "a: 5\nb: 3\nc: null\nd: 0.5\ne:\n  x: 1\n  y: 2\n"), "no schema: %s %s",
              r.err, r.out);
    }

    /* an object (section 12's way back): its value reads back as of its
       name, so a parser's binding of the name gets it */
    const char *sh = "point: {x: !int, y: !int}\nsweep: {target: !int}\nshape: {!|: [!point, !sweep]}\n"
                     "shapes: [!shape, ...]\nw: {a: !any, b?: !point}\nq: {x: !int, y: !int}\n";
    WRITES(sh, "shapes", "[ o:point { s:target i:3 } ]", false, "- x: 1\n  y: 2\n- target: 3\n",
           "[{x:i1,y:i2}@point,{target:i3}@sweep]@shapes");
    REFUSES(sh, "shapes", "[ o:sweep ]", false, "/0: written as a sweep object, it reads as a point");
    /* under !any, an object is written through its code and reads back as
       what that wrote, as 3 reads back as "3" (section 13, yaml2 b40c6f5) */
    WRITES(sh, "w", "{ s:a [ o:point ] }", false, "a:\n  - x: 1\n    y: 2\n", "{a:A[]}@w");
    WRITES(sh, "w", "{ s:a o:point }", true, "{a: {x: 1, y: 2}}", "{a:A{}}@w");
    WRITES(sh, "w", "{ s:a i:1 s:b o:point }", false, "a: 1\nb:\n  x: 1", "{a:A1,b:{x:i1,y:i2}@point}@w");
    REFUSES(sh, "q", "o:point", false, "/: written as a point object, it reads as a q");
    /* names in a chain: with sweep: !point, both bound here, a value where
       a !sweep goes is sweep's, so a point written there reads back as a
       sweep (section 12, yaml2 6a116a6) */
    const char *ch = "point: {x: !int, y: !int}\nsweep: !point\nr: [!sweep, ...]\n";
    WRITES(ch, "r", "[ o:sweep ]", false, "- x: 1\n  y: 2\n", "[{x:i1,y:i2}@point]@r");
    REFUSES(ch, "r", "[ o:point ]", false, "/0: written as a point object, it reads as a sweep");
    REFUSES(sh, "shapes", "[ o!point ]", false, "a point object: it has no x");
    /* an object its code writes as one string is spelled so, and need only
       read back with its name: code bound to signed writes 1 as +1, which
       its standard spelling can't be (section 13) */
    const char *sn = "signed: !int(/[-+][0-9]+/)\nrow: {at: !signed}\nloose: {at: !str}\n";
    WRITES(sn, "row", "{ s:at o:signed }", false, "at: +1\n", "{at:i1@signed}@row");
    WRITES(sn, "row", "{ s:at o:signed }", true, "{at: +1}", "{at:i1@signed}@row");
    REFUSES(sn, "row", "{ s:at i:1 }", false, "/at: no spelling of the integer 1 reads back");
    REFUSES(sn, "loose", "{ s:at o:signed }", false,
            "/at: no spelling of the string '+1' reads back: written +1, written as a signed object, it reads as no named type");
    REFUSES(sh, "shapes", "[ o2point ]", false, "the code bound to point wrote more than one value");
    {
        oylng_emitter *e = oylng_emitter_new();
        pt p = { 1, 2, NULL, 0 };
        oylng_emit_stream_start(e);
        oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
        CHECK(oylng_emit_object(e, S("point"), &p) == OYLNG_ERR_INPUT && !oylng_emitter_error(e), "nothing bound");
        CHECK(oylng_emitter_bind(e, S("point"), unbind_point, NULL) == OYLNG_OK &&
              oylng_emitter_bind(e, S("point"), NULL, NULL) == OYLNG_OK &&
              oylng_emit_object(e, S("point"), &p) == OYLNG_ERR_INPUT, "unbound");
        CHECK(oylng_emitter_bind(e, S("point"), unbind_point, NULL) == OYLNG_OK &&
              oylng_emit_object(e, S("point"), &p) == OYLNG_OK && oylng_emit_doc_end(e) == OYLNG_OK, "bound again");
        oylng_emitter_free(e);
    }
}

static oylng_schema *schema_of(const char *table, const char *root) {
    char tx[400];
    snprintf(tx, sizeof tx, "%%YAML 2.0\n--- &\n%s", table);
    return oylng_schema_new(tx, strlen(tx), S(root), 0);
}

static bool same_time(const oylng_datetime *a, const oylng_datetime *b) {
    return a->year == b->year && a->month == b->month && a->day == b->day && a->hour == b->hour &&
           a->minute == b->minute && a->second == b->second && a->nanosecond == b->nanosecond &&
           a->offset == b->offset && a->zulu == b->zulu && a->unknown_offset == b->unknown_offset;
}

static void emitting_more(void) {
    /* dates and times, written as RFC 3339 writes them */
    oylng_schema *ds = schema_of("d: {a: !date, b: !datetime, c: !datetime, e: {!|: [!int, !datetime]}}\n", "d");
    oylng_emitter *e = oylng_emitter_new();
    CHECK(oylng_emitter_set_schema(e, ds) == OYLNG_OK, "set a schema");
    oylng_datetime a = { 2001, 2, 3, 0, 0, 0, 0, 0, false, false };
    oylng_datetime b = { 2001, 2, 3, 4, 5, 6, 500000000, 330, false, false };
    oylng_datetime c = { 2001, 2, 3, 4, 5, 6, 0, 0, false, true };
    oylng_datetime d = { 2016, 12, 31, 23, 59, 60, 123456789, 0, true, false };
    oylng_emit_stream_start(e);
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    oylng_emit_map_start(e, false);
    oylng_emit_scalar(e, S("a"));
    oylng_emit_datetime(e, OYLNG_T_DATE, &b);   /* the date of it */
    oylng_emit_scalar(e, S("b"));
    oylng_emit_datetime(e, OYLNG_T_DATETIME, &b);
    oylng_emit_scalar(e, S("c"));
    oylng_emit_datetime(e, OYLNG_T_DATETIME, &c);
    oylng_emit_scalar(e, S("e"));
    oylng_emit_datetime(e, OYLNG_T_DATETIME, &d);
    oylng_emit_map_end(e);
    oylng_status st = oylng_emit_doc_end(e);
    oylng_str o = oylng_emitter_output(e);
    CHECK(st == OYLNG_OK && strstr(o.data, "a: 2001-02-03\nb: 2001-02-03T04:05:06.5+05:30\nc: 2001-02-03T04:05:06-00:00\n"
                                           "e: 2016-12-31T23:59:60.123456789Z\n"),
          "dates: %s %.*s", oylng_emitter_error(e) ? oylng_emitter_error(e) : "", (int)o.len, o.data);
    oylng_parser *p = oylng_parser_new(o.data, o.len);
    oylng_typed *y;
    oylng_datetime back[4];
    bool ok = oylng_parse_typed(p, ds, &y) == OYLNG_OK && y;
    for (size_t i = 0; ok && i < 4; i++) ok = oylng_value_datetime(y, oylng_value_at(y, oylng_typed_root(y), i), &back[i]);
    CHECK(ok && same_time(&back[0], &a) && same_time(&back[1], &b) && same_time(&back[2], &c) && same_time(&back[3], &d),
          "dates read back");
    oylng_typed_free(y);
    oylng_parser_free(p);
    oylng_emitter_free(e);
    oylng_datetime bad[] = { { 2001, 13, 3, 0, 0, 0, 0, 0, false, false },
                             { 2001, 2, 3, 4, 5, 6, 0, 60, true, false },
                             { 2001, 2, 3, 4, 5, 6, 1000000000, 0, true, false },
                             { 10000, 1, 1, 0, 0, 0, 0, 0, true, false } };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        e = oylng_emitter_new();
        oylng_emit_stream_start(e);
        oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
        st = oylng_emit_datetime(e, i ? OYLNG_T_DATETIME : OYLNG_T_DATE, &bad[i]);
        CHECK(st == OYLNG_ERR_EMIT && strstr(oylng_emitter_error(e), "doesn't read back"), "bad time %zu: %s", i,
              oylng_emitter_error(e) ? oylng_emitter_error(e) : "taken");
        oylng_emitter_free(e);
    }
    oylng_schema_free(ds);

    /* a document is held till it ends: the output stops short of it, and
       a refused one leaves what came before */
    oylng_schema *ns = schema_of("n: !int\n", "n");
    e = oylng_emitter_new();
    oylng_emitter_set_schema(e, ns);
    oylng_emit_stream_start(e);
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    oylng_emit_int(e, 1);
    oylng_emit_doc_end(e);
    oylng_emit_doc_start(e, S("x"));
    o = oylng_emitter_output(e);
    CHECK(o.len == 16 && !memcmp(o.data, "%YAML 2.0\n--- 1\n", 16), "held: %.*s", (int)o.len, o.data);
    oylng_str taken;
    CHECK(oylng_emitter_take(e, &taken) == OYLNG_OK && taken.len == 16, "taken while held");
    oylng_emit_int(e, 2);
    o = oylng_emitter_output(e);
    CHECK(o.len == 0, "still held: %.*s", (int)o.len, o.data);
    CHECK(oylng_emit_doc_end(e) == OYLNG_OK, "ended");
    o = oylng_emitter_output(e);
    CHECK(o.len == 9 && !memcmp(o.data, "--- !x 2\n", 9), "written at its end: %.*s", (int)o.len, o.data);
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    oylng_emit_scalar(e, S("three"));
    CHECK(oylng_emit_doc_end(e) == OYLNG_ERR_EMIT && strstr(oylng_emitter_error(e), "/: no spelling of the string 'three'"),
          "refused: %s", oylng_emitter_error(e));
    o = oylng_emitter_output(e);
    CHECK(o.len == 9 && !memcmp(o.data, "--- !x 2\n", 9), "what came before: %.*s", (int)o.len, o.data);
    oylng_emitter_free(e);

    /* a literal block reads back as the string it is */
    e = oylng_emitter_new();
    oylng_emitter_set_schema(e, ns);
    oylng_emit_stream_start(e);
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    oylng_emit_int(e, -12);
    CHECK(oylng_emit_doc_end(e) == OYLNG_OK, "an int");
    CHECK(oylng_emitter_set_schema(e, NULL) == OYLNG_OK, "no schema");
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    o = oylng_emitter_output(e);
    CHECK(strstr(o.data, "--- "), "not held: %.*s", (int)o.len, o.data);
    oylng_emitter_free(e);

    /* a schema that types nothing, or is in error, can't be set */
    const char *only = "%YAML 2.0\n--- &\nn: !int\n";
    oylng_schema *chk = oylng_schema_new(only, strlen(only), (oylng_str){ NULL, 0 }, 0);
    oylng_schema *bad_s = schema_of("n: !nope\n", "n");
    e = oylng_emitter_new();
    CHECK(oylng_schema_status(chk) == OYLNG_OK && oylng_emitter_set_schema(e, chk) == OYLNG_ERR_INPUT, "a check's schema");
    CHECK(oylng_schema_status(bad_s) != OYLNG_OK && oylng_emitter_set_schema(e, bad_s) == OYLNG_ERR_INPUT,
          "a schema in error");
    CHECK(oylng_emit_number(e, OYLNG_T_INT, S("5")) == OYLNG_ERR_INPUT &&
              oylng_emit_datetime(e, OYLNG_T_STR, &a) == OYLNG_ERR_INPUT && !oylng_emitter_error(e),
          "not a number's or a date's type");
    oylng_emitter_free(e);
    oylng_schema_free(chk);
    oylng_schema_free(bad_s);
    oylng_schema_free(ns);

    WRITES("s: [!str, ...]\n", "s", "[ s:a\nb\n ]", false, "- |\n  a\n  b\n", "[sa\nb\n]@s");

    /* a typed document written again: where no standard spelling reads
       back, how each value was written does */
    const char *src = "%YAML 2.0\n--- &\nz: !int(/0{25}[0-9]/)\nnl: !null(nil)\n"
                      "r:\n  a: !z\n  b: !nl\n  c: !float(/[0-9]\\.[0-9]{30}/)\n  d: !datetime\n"
                      "--- !r\na: 00000000000000000000000007\nb: nil\nc: 0.100000000000000005551115123125\n"
                      "d: 2001-02-03t04:05:06z\n";
    oylng_schema *rs = oylng_schema_new(src, strlen(src), S("r"), 0);
    p = oylng_parser_new(src, strlen(src));
    y = NULL;
    CHECK(oylng_schema_status(rs) == OYLNG_OK && oylng_parse_typed(p, NULL, &y) == OYLNG_OK && y, "read: %s",
          oylng_parser_error(p));
    e = oylng_emitter_new();
    oylng_emitter_set_schema(e, rs);
    oylng_emit_stream_start(e);
    oylng_emit_doc_start(e, (oylng_str){ NULL, 0 });
    oylng_emit_value(e, y, oylng_typed_root(y));
    st = oylng_emit_doc_end(e);
    o = oylng_emitter_output(e);
    CHECK(st == OYLNG_OK && strstr(o.data, "a: 00000000000000000000000007\nb: nil\nc: 0.100000000000000005551115123125\n"
                                           "d: 2001-02-03T04:05:06Z\n"),
          "as written: %s %.*s", oylng_emitter_error(e) ? oylng_emitter_error(e) : "", (int)o.len, o.data);
    oylng_emitter_free(e);
    oylng_typed_free(y);
    oylng_parser_free(p);
    oylng_schema_free(rs);
}

/* ── Type keys take a scalar type (section 7) ────────────── */

static void scalar_type_keys(void) {
    oylng_parser *p;
    const char *err;
    oylng_typed *y;
    /* refused as the schema is read: through a name, in a union nested in
       a union, among a !**'s rules, and in an application's schema */
    static const char *const refused[] = {
        "%YAML 2.0\n--- &\npoint: {x: !int}\np2: !point\nm: {!p2: !int}\n--- !m\na: 1\n",
        "%YAML 2.0\n--- &\nl: [!int, ...]\nu: {!|: [!str(/x/), {!|: [!int, !l]}]}\nm: {!u: !int}\n--- !m\nx: 1\n",
        "%YAML 2.0\n--- &\nl: [!int, ...]\nm: {a: !int, !**: {!l: !int}}\n--- !m\na: 1\n",
    };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        p = oylng_parser_new(refused[i], strlen(refused[i]));
        y = NULL;
        oylng_status st = oylng_parse_typed(p, NULL, &y);
        err = st == OYLNG_OK ? NULL : oylng_parser_error(p);
        CHECK(!y && st == OYLNG_ERR_SCHEMA && err &&
                  strstr(err, "a type key takes a scalar type, since a key is a scalar"),
              "type key %zu refused: %s", i, err ? err : "accepted");
        oylng_parser_free(p);
    }
    const char *as = "%YAML 2.0\n--- &\npoint: {x: !int}\nr: {!point: !int}\n";
    oylng_schema *app = oylng_schema_new(as, strlen(as), S("r"), 0);
    CHECK(oylng_schema_status(app) == OYLNG_ERR_SCHEMA &&
              strstr(oylng_schema_error(app), "a type key takes a scalar type"),
          "an application's schema: %s", oylng_schema_error(app));
    oylng_schema_free(app);
    /* !any, and a union with it, fit a scalar */
    y = typed(&p, "%YAML 2.0\n--- &\nm: {!any: !int}\n--- !m\na: 1\n", NULL, &err);
    CHECK(y && oylng_value_type(y, oylng_value_get(y, oylng_typed_root(y), S("a"))) == OYLNG_T_INT, "!any as a key: %s",
          err ? err : "");
    oylng_typed_free(y);
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nk: {!|: [!int, !any]}\nm: {!k: !bool}\n--- !m\n7: true\nab: false\n", NULL, &err);
    CHECK(y && oylng_value_count(y, oylng_typed_root(y)) == 2, "a union with !any as a key: %s", err ? err : "");
    oylng_typed_free(y);
    oylng_parser_free(p);
}

/* ── Typing with references kept (section 11) ───────────── */

/* The stream's first document read with references kept and typed, under
 * limit: NULL with the error in err, and the status in *st */
static oylng_typed *kept_typed(oylng_parser **pp, const char *s, uint64_t limit, oylng_status *st, char *err,
                               size_t errlen) {
    *pp = oylng_parser_new(s, strlen(s));
    if (limit) oylng_parser_set_limit(*pp, limit);
    oylng_parser_set_keep(*pp, true);
    oylng_typed *y = NULL;
    *st = oylng_parse_typed(*pp, NULL, &y);
    snprintf(err, errlen, "%s", *st == OYLNG_OK ? "" : oylng_parser_error(*pp));
    return y;
}

static void kept_typing(void) {
    char err[300];
    oylng_parser *p;
    oylng_status st;
    oylng_typed *y;

    /* a string of 1,000 bytes named 1,000 times: full is a million, so the
       normal read refuses it at 10,000; kept, held is some 3,000, and typed
       1,002, as `...` repeats one type and s is typed once */
    size_t cap = 1000 * 6 + 1200, o = 0;
    char *big = malloc(cap);
    o += (size_t)sprintf(big, "%%YAML 2.0\n--- &\ns: ");
    for (int i = 0; i < 1000; i++) big[o++] = 'a' + i % 26;
    o += (size_t)sprintf(big + o, "\none: [!str, ...]\n--- !one\n[");
    for (int i = 0; i < 1000; i++) o += (size_t)sprintf(big + o, "%s*s", i ? ", " : "");
    o += (size_t)sprintf(big + o, "]\n");
    p = oylng_parser_new(big, o);
    oylng_parser_set_limit(p, 10000);
    CHECK(oylng_parse_typed(p, NULL, &y) == OYLNG_ERR_LIMIT && !y, "in full at 10,000: refused for full");
    oylng_parser_free(p);
    y = kept_typed(&p, big, 10000, &st, err, sizeof err);
    oylng_value r = y ? oylng_typed_root(y) : OYLNG_NO_VALUE;
    CHECK(st == OYLNG_OK && y && oylng_value_count(y, r) == 1000 &&
              oylng_value_type(y, oylng_value_item(y, r, 999)) == OYLNG_T_STR &&
              oylng_value_text(y, oylng_value_item(y, r, 999)).len == 1000 &&
              oylng_value_item(y, r, 0) == oylng_value_item(y, r, 999),
          "kept: typed once, one value for all 1,000: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    /* under three types, s is typed three times: typed is 1 + 3 x 1,001,
       3,004, which the limit must reach, though held is some 1,050 */
    char *t3 = malloc(1200);
    size_t n3 = (size_t)sprintf(t3, "%%YAML 2.0\n--- &\ns: ");
    for (int i = 0; i < 1000; i++) t3[n3++] = 'a' + i % 26;
    sprintf(t3 + n3, "\nthree: [!str(1..), !str(2..), !str(3..)]\n--- !three\n[*s, *s, *s]\n");
    y = kept_typed(&p, t3, 3004, &st, err, sizeof err);
    CHECK(st == OYLNG_OK && y, "three types at 3,004: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    y = kept_typed(&p, t3, 3003, &st, err, sizeof err);
    CHECK(st == OYLNG_ERR_LIMIT && !y && strstr(err, "/2: typed passes the size limit of 3003"), "and at 3,003: %s", err);
    oylng_parser_free(p);
    free(t3);
    free(big);

    /* typing spreads a kept mapping's <<: lines and a sequence's -<< items,
       and the typed value is what a full read gives (section 11), whether
       the lines are in the data, behind a reference, or in a type */
    static const char *const alike[] = {
        "%YAML 2.0\n--- &\na: {k: v}\nm: {k: !str, j: !int}\n--- !m\n<<: *a\nj: 1\n",
        "%YAML 2.0\n--- &\ns: [x]\nl: [!str, ...]\n--- !l\n-<< *s\n- y\n",
        "%YAML 2.0\n--- &\na: {k: v}\nb: {<<: *a}\nm: {k: !str}\n--- !m\n*b\n",
        "%YAML 2.0\n--- &\nbase: {x: !int}\njob: {<<: *base, y: !int}\n--- !job\nx: 1\ny: 2\n",
        "%YAML 2.0\n--- &\nhead: [!int]\nrow: [-<< *head, !str]\n--- !row\n[1, a]\n",
        /* the order section 9.1 builds: x and y from the first line; the
           second's y in y's place and z at the end; then p at the end, and
           x in x's place: {x: 6, y: 3, z: 4, p: 5} */
        "%YAML 2.0\n--- &\na: {x: 1, y: 2}\nb: {y: 3, z: 4}\nc: {<<: *a, <<: *b, p: 5, x: 6}\nm: {!*: !int}\n--- !m\n*c\n",
        "%YAML 2.0\n--- &\ne: []\nf: [-<< *e, 1, -<< *e]\ng: [-<< *f, -<< *f, 2]\nm: [!int, ...]\n--- !m\n*g\n",
        "%YAML 2.0\n--- &\na: {k: v}\nm: {k: {!|: [{j: !int}, !any]}}\n--- !m\nk: {<<: *a}\n",
        "%YAML 2.0\n--- &\nb: {x: 1}\nm: {!**: !int}\n--- !m\nq: {<<: *b, y: 2}\n",
        /* in a type, a written key replaces a merged one in its place: the
           type key !str takes a bool, not an int */
        "%YAML 2.0\n--- &\nbase: {!str: !int}\nm: {<<: *base, !str: !bool}\n--- !m\na: true\n",
        /* and its key too: why? written is an optional key, why, where
           "why?" merged is the key why?; as merged keys are kept, so is a
           line's key from a line before it */
        "%YAML 2.0\n--- &\nbase: {\"why?\": !int}\nt: {<<: *base, why?: !str, !*: !int}\n--- !t\n{}\n",
        "%YAML 2.0\n--- &\nbase: {\"why?\": !int}\nt: {<<: *base, why?: !str, !*: !int}\n--- !t\nwhy: a\n",
        "%YAML 2.0\n--- &\na: {\"why?\": !int}\nb: {why?: !str}\nt: {<<: *a, <<: *b}\n--- !t\nwhy?: 5\n",
        "%YAML 2.0\n--- &\na: {\"why?\": !int}\nb: {<<: *a, why?: !str}\nt: {<<: *b}\n--- !t\n{}\n",
        "%YAML 2.0\n--- &\na: {why?: !int}\nb: {<<: *a}\nt: {<<: *b, \"why?\": !str}\n--- !t\nwhy?: x\n",
        /* a value is the last line's to have it, where a line names a
           mapping met before: x 1 in c, 2 in d; and in a type, !str */
        "%YAML 2.0\n--- &\na: {x: 1, y: 0}\nb: {z: 4, x: 2}\nc: {<<: *a, <<: *b, <<: *a}\nm: {!*: !int}\n--- !m\n*c\n",
        ("%YAML 2.0\n--- &\na: {x: 1, y: 0}\nb: {z: 4, x: 2}\nc: {<<: *a, <<: *b, <<: *a}\nd: {<<: *c, <<: *b, q: 3}\n"
         "m: {!*: !int}\n--- !m\n*d\n"),
        "%YAML 2.0\n--- &\na: {x: !int}\nb: {x: !str}\nt: {<<: *a, <<: *b}\n--- !t\nx: abc\n",
        "%YAML 2.0\n--- &\na: {x: !int}\nb: {x: !str}\nc: {<<: *b, <<: *a}\nt: {<<: *a, <<: *c}\n--- !t\nx: 5\n",
    };
    for (size_t i = 0; i < sizeof alike / sizeof *alike; i++) {
        const char *e1 = NULL;
        oylng_parser *q;
        oylng_typed *full = typed(&q, alike[i], NULL, &e1);
        y = kept_typed(&p, alike[i], 0, &st, err, sizeof err);
        CHECK(full && y && same_typed(full, oylng_typed_root(full), y, oylng_typed_root(y)),
              "kept and full alike %zu: %s %s", i, e1 ? e1 : "", err);
        oylng_typed_free(full);
        oylng_parser_free(q);
        oylng_typed_free(y);
        oylng_parser_free(p);
    }
    /* in data too a written key replaces a merged one, node and all: a
       refusal at it is marked where it is written */
    {
        const char *km = "%YAML 2.0\n--- &\nb: {\"x\": 1}\nm: {!bool: !str}\n--- !m\n<<: *b\n\nx: a\n";
        const char *e1 = NULL;
        oylng_parser *q;
        oylng_typed *full = typed(&q, km, NULL, &e1);
        y = kept_typed(&p, km, 0, &st, err, sizeof err);
        oylng_mark a = oylng_parser_error_mark(q), b = oylng_parser_error_mark(p);
        CHECK(!full && !y && a.line == 8 && b.line == 8 && a.col == b.col, "a written key's mark: %zu:%zu, %zu:%zu",
              a.line, a.col, b.line, b.col);
        oylng_parser_free(q);
        oylng_parser_free(p);
        /* and given by two lines, the key is the first's, the value the last's */
        const char *k2 = "%YAML 2.0\n--- &\na: {\"x\": 1}\nb: {x: 2}\nm: {!bool: !str}\n--- !m\n<<: *a\n<<: *b\n";
        full = typed(&q, k2, NULL, &e1);
        y = kept_typed(&p, k2, 0, &st, err, sizeof err);
        a = oylng_parser_error_mark(q), b = oylng_parser_error_mark(p);
        CHECK(!full && !y && a.line == 3 && b.line == 3 && a.col == b.col, "two lines' key's mark: %zu:%zu, %zu:%zu",
              a.line, a.col, b.line, b.col);
        oylng_parser_free(q);
        oylng_parser_free(p);
    }
    /* a type merging a chain of 2^63 - 2 lines, each to {}: within full,
       as {} gives nothing, and merged a mapping at a time, as the full
       read merges it, not a line at a time */
    {
        char *c62 = malloc(4096);
        size_t o = (size_t)sprintf(c62, "%%YAML 2.0\n--- &\ne0: {}\n");
        for (int i = 1; i <= 62; i++) o += (size_t)sprintf(c62 + o, "e%d: {<<: *e%d, <<: *e%d}\n", i, i - 1, i - 1);
        sprintf(c62 + o, "t: {<<: *e62, !*: !int}\n--- !t\na: 1\n");
        y = kept_typed(&p, c62, 0, &st, err, sizeof err);
        int64_t a1 = 0;
        CHECK(st == OYLNG_OK && y &&
                  oylng_value_int(y, oylng_value_get(y, oylng_typed_root(y), S("a")), &a1) && a1 == 1,
              "a type merging 2^63 - 2 lines: %s", err);
        oylng_typed_free(y);
        oylng_parser_free(p);
        free(c62);
    }
    /* the order, as written */
    y = kept_typed(&p, alike[5], 0, &st, err, sizeof err);
    {
        oylng_value r = y ? oylng_typed_root(y) : OYLNG_NO_VALUE;
        int64_t x = 0, p3 = 0;
        CHECK(y && oylng_value_count(y, r) == 4 && eq(oylng_value_key(y, r, 0), "x", 1) &&
                  eq(oylng_value_key(y, r, 2), "z", 1) && eq(oylng_value_key(y, r, 3), "p", 1) &&
                  oylng_value_int(y, oylng_value_at(y, r, 0), &x) && x == 6 &&
                  oylng_value_int(y, oylng_value_at(y, r, 1), &p3) && p3 == 3,
              "section 9.1's order: %s", err);
    }
    oylng_typed_free(y);
    oylng_parser_free(p);
    /* the typed count's exact sums are part 2's suite's: 9JL8 and 3RKM,
       FAC6 and 9WDZ, GF98 and CNZ5 */
    /* lines count, though a line naming {} gives no key: 2^31 - 2 lines
       past the default limit, refused before anything is built; under
       !any alone, nothing spreads */
    char chain[2400];
    size_t co = (size_t)sprintf(chain, "%%YAML 2.0\n--- &\ne0: {}\n");
    for (int i = 1; i <= 30; i++) co += (size_t)sprintf(chain + co, "e%d: {<<: *e%d, <<: *e%d}\n", i, i - 1, i - 1);
    sprintf(chain + co, "m: {!*: !int}\nn: {x: !any}\n--- !m\n<<: *e30\n--- !n\nx: {<<: *e30}\n");
    y = kept_typed(&p, chain, 0, &st, err, sizeof err);
    CHECK(st == OYLNG_ERR_LIMIT && !y && strstr(err, "typed passes the size limit"), "2^31 - 2 lines: %s", err);
    oylng_parser_free(p);
    {
        const char *nn = strstr(chain, "--- !m\n");
        char alone[2400];
        size_t ho = (size_t)(nn - chain);
        memcpy(alone, chain, ho);
        strcpy(alone + ho, "--- !n\nx: {<<: *e30}\n");
        y = kept_typed(&p, alone, 0, &st, err, sizeof err);
        oylng_value xv = y ? oylng_value_get(y, oylng_typed_root(y), S("x")) : OYLNG_NO_VALUE;
        CHECK(st == OYLNG_OK && y && oylng_value_type(y, xv) == OYLNG_T_ANY &&
                  oylng_node_merges(oylng_typed_tree(y), oylng_value_node(y, xv)) == 1,
              "under !any alone, kept: %s", err);
        oylng_typed_free(y);
        oylng_parser_free(p);
        const char *e1 = NULL;
        y = typed(&p, chain, NULL, &e1);
        CHECK(y, "read in full, accepted: %s", e1 ? e1 : "");
        oylng_typed_free(y);
        oylng_parser_free(p);
    }
    /* the entries a schema reaches are held to their full size together,
       before they are compiled: big names s 20 times, so its full is some
       2,000, where held is some 150 */
    char hold[600];
    size_t ho = (size_t)sprintf(hold, "%%YAML 2.0\n--- &\ns: ");
    for (int i = 0; i < 100; i++) hold[ho++] = 'a' + i % 26;
    ho += (size_t)sprintf(hold + ho, "\nbig: [");
    for (int i = 0; i < 20; i++) ho += (size_t)sprintf(hold + ho, "%s*s", i ? ", " : "");
    ho += (size_t)sprintf(hold + ho, "]\nm: {a: !big}\nm2: {a: !int}\n");
    char *h1 = malloc(ho + 40), *h2 = malloc(ho + 40);
    sprintf(h1, "%s--- !m2\na: 1\n", hold);
    sprintf(h2, "%s--- !m\na: *big\n", hold);
    y = kept_typed(&p, h1, 2000, &st, err, sizeof err);
    CHECK(st == OYLNG_OK && y, "an entry no type reaches isn't held: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    y = kept_typed(&p, h2, 2000, &st, err, sizeof err);
    CHECK(st == OYLNG_ERR_SCHEMA && !y && strstr(err, "the entries this schema reaches come to"),
          "the entries reached, past the limit: %s", err);
    oylng_parser_free(p);
    y = kept_typed(&p, h2, 2100, &st, err, sizeof err);
    CHECK(st == OYLNG_OK && y, "within it, typed: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    free(h1);
    free(h2);
    /* a type is compiled only where a document reaches it, so an entry
       with <<: no type names costs nothing; under !any nothing is typed,
       so a <<: there is delivered as kept, as in an untyped document */
    const char *nr = "%YAML 2.0\n--- &\na: {k: v}\nb: {<<: *a}\nm: {k: !str}\n--- !m\nk: x\n";
    y = kept_typed(&p, nr, 0, &st, err, sizeof err);
    CHECK(st == OYLNG_OK && y, "an entry with <<: not reached: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    const char *ua = "%YAML 2.0\n--- &\na: {k: v}\nm: {k: !any}\n--- !m\nk: {<<: *a}\n";
    y = kept_typed(&p, ua, 0, &st, err, sizeof err);
    oylng_value kv = y ? oylng_value_get(y, oylng_typed_root(y), S("k")) : OYLNG_NO_VALUE;
    CHECK(st == OYLNG_OK && y && oylng_value_type(y, kv) == OYLNG_T_ANY &&
              oylng_node_merges(oylng_typed_tree(y), oylng_value_node(y, kv)) == 1,
          "<<: under !any, as kept: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    const char *un = "%YAML 2.0\n--- &\na: {k: v}\n---\n<<: *a\nj: 1\n";
    y = kept_typed(&p, un, 0, &st, err, sizeof err);
    CHECK(st == OYLNG_OK && y && oylng_typed_how(y) == OYLNG_TYPED_NOT, "untyped, kept as read: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);

    /* code runs once per use, kept or not: *o twice is two points */
    const char *pt = "%YAML 2.0\n--- &\npoint: {x: !int, y: !int}\nps: [!point, ...]\no: {x: 1, y: 2}\n--- !ps\n[*o, *o]\n";
    binder b = { 0 };
    p = oylng_parser_new(pt, strlen(pt));
    oylng_parser_set_keep(p, true);
    oylng_parser_bind(p, S("point"), bind_point, drop_obj, &b);
    y = typed_next(p, NULL, err, sizeof err);
    r = y ? oylng_typed_root(y) : OYLNG_NO_VALUE;
    point *q0 = y ? oylng_value_object(y, oylng_value_item(y, r, 0)) : NULL;
    point *q1 = y ? oylng_value_object(y, oylng_value_item(y, r, 1)) : NULL;
    CHECK(y && b.calls == 2 && q0 && q1 && q0 != q1 && q1->y == 2, "kept, *o twice: two calls, two points: %s", err);
    oylng_typed_free(y);
    oylng_parser_free(p);
    CHECK(b.dropped == 2, "both let go: %d", b.dropped);
}

static void api(void) {
    oylng_parser *p;
    const char *err;
    oylng_typed *y;

    /* plain, on events and trees */
    const char *styles = "%YAML 2.0\n---\n- a\n- 'b'\n- \"c\"\n- |\n  d\n- >\n  e\n-\n- [f, \"g\"]\n";
    p = oylng_parser_new(styles, strlen(styles));
    const oylng_event *e;
    char got[16] = "";
    size_t k = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END)
        if (e->type == OYLNG_EVT_SCALAR && k < sizeof got - 1) got[k++] = e->plain ? 'p' : 'q';
    CHECK(strcmp(got, "pqqqqppq") == 0, "plain on events: %s", got);
    oylng_parser_free(p);

    /* numbers */
    y = typed(&p,
              "%YAML 2.0\n--- &\nn: [!int, ...]\n--- !n\n[-9223372036854775808, 18446744073709551615, -0, 0]\n",
              NULL, &err);
    CHECK(y && oylng_typed_how(y) == OYLNG_TYPED_STAMP, "stamped: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y);
        int64_t i;
        uint64_t u;
        CHECK(oylng_value_int(y, oylng_value_item(y, r, 0), &i) && i == INT64_MIN, "int64's least");
        CHECK(!oylng_value_uint(y, oylng_value_item(y, r, 0), &u), "no uint below 0");
        CHECK(oylng_value_uint(y, oylng_value_item(y, r, 1), &u) && u == UINT64_MAX, "uint64's most");
        CHECK(!oylng_value_int(y, oylng_value_item(y, r, 1), &i), "past int64");
        CHECK(oylng_value_int(y, oylng_value_item(y, r, 2), &i) && i == 0 &&
              oylng_value_uint(y, oylng_value_item(y, r, 2), &u), "-0 is 0");
        CHECK(oylng_value_name(y, r).data && eq(oylng_value_name(y, r), "n", 1), "the root's name");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nn: {!|: [!int, !str]}\n--- !n\n18446744073709551616\n", NULL, &err);
    CHECK(!y && err && strstr(err, "outside !int"), "past !int refuses: %s", err ? err : "accepted");
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nf: [!float, ...]\n--- !f\n[1e-999, -1e-999, 9007199254740993, "
                  "1.79769313486231570815e308]\n", NULL, &err);
    CHECK(y != NULL, "floats: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y);
        double a = oylng_value_float(y, oylng_value_item(y, r, 0)), b = oylng_value_float(y, oylng_value_item(y, r, 1));
        CHECK(a == 0 && !signbit(a) && b == 0 && signbit(b), "too small is 0, signed");
        CHECK(oylng_value_float(y, oylng_value_item(y, r, 2)) == 9007199254740992.0, "the nearest, even");
        CHECK(oylng_value_float(y, oylng_value_item(y, r, 3)) == 1.7976931348623157e308, "binary64's most");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nf: {!|: [!float(0..), !str]}\n--- !f\n1.8e308\n", NULL, &err);
    CHECK(!y && err && strstr(err, "binary64"), "too large refuses: %s", err ? err : "accepted");
    oylng_parser_free(p);

    /* precision, as section 4.6 has it */
    const char *prec[][3] = { { "5:2", "999.99", "y" }, { "5:2", "1000.00", "n" }, { "3:1", "1.50", "y" },
                              { "3:0", "1e3", "n" }, { "4:0", "1e3", "y" }, { "5:4", "0.0015", "y" },
                              { "5:3", "0.0015", "n" }, { "1:0", "0e999999999999999999999", "y" } };
    for (size_t i = 0; i < sizeof prec / sizeof *prec; i++) {
        char s[200];
        snprintf(s, sizeof s, "%%YAML 2.0\n--- &\nd: {!|: [!decimal(%s), !str]}\n--- !d\n%s\n", prec[i][0], prec[i][1]);
        y = typed(&p, s, NULL, &err);
        CHECK(y && oylng_value_type(y, oylng_typed_root(y)) == (prec[i][2][0] == 'y' ? OYLNG_T_DECIMAL : OYLNG_T_STR),
              "%s under %s", prec[i][1], prec[i][0]);
        oylng_typed_free(y);
        oylng_parser_free(p);
    }
    /* limits on numbers as written, exponents of any size */
    const char *lim[][3] = { { "..1e30", "999999999999999999999999999999", "y" },
                             { "..1e30", "1000000000000000000000000000001", "n" },
                             { "1e99999999999999999999..", "1e100000000000000000000", "y" },
                             { "..1e99999999999999999999", "1e100000000000000000000", "n" },
                             { "..1e99999999999999999999", "10e99999999999999999998", "y" },
                             { "-1..", "-1.0000000000000000000000001", "n" } };
    for (size_t i = 0; i < sizeof lim / sizeof *lim; i++) {
        char s[300];
        snprintf(s, sizeof s, "%%YAML 2.0\n--- &\nd: {!|: [!decimal(%s), !str]}\n--- !d\n%s\n", lim[i][0], lim[i][1]);
        y = typed(&p, s, NULL, &err);
        CHECK(y && oylng_value_type(y, oylng_typed_root(y)) == (lim[i][2][0] == 'y' ? OYLNG_T_DECIMAL : OYLNG_T_STR),
              "%s in %s: %s", lim[i][1], lim[i][0], err ? err : "");
        oylng_typed_free(y);
        oylng_parser_free(p);
    }

    /* dates, binary */
    y = typed(&p, "%YAML 2.0\n--- &\nt: [!datetime, !date, !binary]\n--- !t\n[1990-12-31T18:59:60-05:00, "
                  "2000-02-29, \"aGk=\"]\n", NULL, &err);
    CHECK(y != NULL, "a leap second, a leap day, binary: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y);
        oylng_datetime d;
        CHECK(oylng_value_datetime(y, oylng_value_item(y, r, 0), &d) && d.second == 60 && d.offset == -300,
              "the leap second's fields");
        oylng_str b = oylng_value_bytes(y, oylng_value_item(y, r, 2));
        CHECK(eq(b, "hi", 2), "binary's bytes");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nt: !datetime\n--- !t\n2001-02-03T04:05:06.1234567890Z\n", NULL, &err);
    CHECK(!y && err && strstr(err, "can't hold"), "10 fraction digits refuse: %s", err ? err : "accepted");
    oylng_parser_free(p);
    const char *cut = "%YAML 2.0\n--- &\nt: !datetime\n--- !t\n2001-02-03T04:05:06.1234567899Z\n";
    p = oylng_parser_new(cut, strlen(cut));
    oylng_parser_set_cut(p, true);
    y = NULL;
    oylng_parse_typed(p, NULL, &y);
    oylng_datetime dt;
    CHECK(y && oylng_value_datetime(y, oylng_typed_root(y), &dt) && dt.nanosecond == 123456789,
          "the cut drops digits, never rounding up");
    oylng_typed_free(y);
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nb: !binary\n--- !b\naGl=\n", NULL, &err);
    CHECK(!y, "pad bits that aren't zero");
    oylng_parser_free(p);

    /* an application's schema, and a check of a schema file */
    const char *app = "%YAML 2.0\n--- &\nport: !int(1..65535)\nsvc:\n  name: !str\n  port?: !port\n";
    oylng_schema *sc = oylng_schema_new(app, strlen(app), S("svc"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_OK, "an app's schema: %s", oylng_schema_error(sc));
    y = typed(&p, "%YAML 2.0\n---\nname: web\nport: 8080\n", sc, &err);
    CHECK(y && oylng_typed_how(y) == OYLNG_TYPED_APP, "typed by the app's schema: %s", err ? err : "");
    if (y) {
        oylng_value port = oylng_value_get(y, oylng_typed_root(y), S("port"));
        uint64_t u;
        CHECK(oylng_value_uint(y, port, &u) && u == 8080 && eq(oylng_value_name(y, port), "port", 4),
              "its port, named");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n---\nname: web\nport: 0\n", sc, &err);
    CHECK(!y && err && strstr(err, "/port"), "the path to a value refused: %s", err ? err : "accepted");
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nsvc: !str\n--- !svc\nname: web\n", sc, &err);
    CHECK(!y && err, "a stamp wins over the app's schema");
    oylng_parser_free(p);
    oylng_schema_free(sc);
    sc = oylng_schema_new(app, strlen(app), S("nope"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA, "no such root");
    oylng_schema_free(sc);
    const char *badschema = "%YAML 2.0\n--- &\na: !int\nb:\n  c: !nosuch\n";
    sc = oylng_schema_new(badschema, strlen(badschema), (oylng_str){ NULL, 0 }, 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && oylng_schema_error_mark(sc).line == 5,
          "a schema file checked, its error's line: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);

    /* a schema's error and a document's refusal: a column counts
       characters, and the offset bytes (part 1, section 2) */
    const char *cjk = "%YAML 2.0\n--- &\n名前: !int\nb:\n  東京: !nosuch\n";
    sc = oylng_schema_new(cjk, strlen(cjk), (oylng_str){ NULL, 0 }, 0);
    oylng_mark wm = oylng_schema_error_mark(sc);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && wm.line == 5 && wm.col == 7 && wm.offset == 42,
          "a schema's error, in characters: %zu:%zu (%zu): %s", wm.line, wm.col, wm.offset, oylng_schema_error(sc));
    oylng_schema_free(sc);
    y = typed(&p, "%YAML 2.0\n--- &\nv:\n  名前: !str\n  n: !int\n--- !v\n{名前: 東京, n: x}\n", NULL, &err);
    wm = oylng_parser_error_mark(p);
    CHECK(!y && err && wm.line == 7 && wm.col == 13 && wm.offset == 71, "a refusal, in characters: %zu:%zu (%zu): %s",
          wm.line, wm.col, wm.offset, err ? err : "");
    oylng_parser_free(p);

    /* what nothing reaches isn't a type; what something reached can't be replaced */
    y = typed(&p, "%YAML 2.0\n--- &\nmotd: !important\nn: !int\n--- !n\n3\n", NULL, &err);
    CHECK(y != NULL, "an entry nothing reaches: %s", err ? err : "");
    oylng_typed_free(y);
    oylng_parser_free(p);

    /* typed counts each time a shared node is typed (section 11) */
    const char *three = "%YAML 2.0\n--- &\ns: abcdefgh\nthree: [!str, !str(1..), !str(2..)]\n--- !three\n"
                        "[*s, *s, *s]\n";
    p = oylng_parser_new(three, strlen(three));
    oylng_parser_set_limit(p, 28 + 60);
    y = NULL;
    CHECK(oylng_parse_typed(p, NULL, &y) == OYLNG_OK, "typed 28, under the limit: %s", oylng_parser_error(p));
    oylng_typed_free(y);
    oylng_parser_free(p);

    /* patterns (sections 4.3 and 4.5): a number a pattern matched, in the
       standard spelling */
    y = typed(&p, "%YAML 2.0\n--- &\nb: !bigint(/[-+][0-9]+/)\nd: !decimal(/[-+]?[0-9]*\\.?[0-9]*/)\n"
                  "i: !int(/[-+][0-9]+/)\nu: !str(/\\p{Lu}\\P{L}*/)\nr: [!b, !b, !d, !d, !d, !i, !u]\n--- !r\n"
                  "[+0012, -000, +.5, -00.50, 7., -0, \"A1 \"]\n", NULL, &err);
    CHECK(y != NULL, "patterns: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y);
        static const char *const want[] = { "12", "0", "0.5", "-0.50", "7" };
        for (size_t i = 0; i < 5; i++) {
            oylng_str t = oylng_value_text(y, oylng_value_item(y, r, i));
            CHECK(eq(t, want[i], strlen(want[i])), "the standard spelling of item %zu: %.*s", i, (int)t.len,
                  t.data);
        }
        int64_t i64;
        CHECK(oylng_value_int(y, oylng_value_item(y, r, 5), &i64) && i64 == 0, "-0 by a pattern is 0");
        CHECK(oylng_value_type(y, oylng_value_item(y, r, 6)) == OYLNG_T_STR, "a quoted string matches");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nn:\n  !|:\n    - !int(/[0-9_]+/)\n    - !str\n--- !n\n1_000\n", NULL, &err);
    CHECK(!y && err && strstr(err, "matched, and is not an integer"), "matched and unread refuses: %s",
          err ? err : "accepted");
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nm:\n  !str(/x-.*/): !any\n  name: !str\n--- !m\nname: a\nx-b: [1]\n"
                  "x-: c\n", NULL, &err);
    CHECK(y != NULL, "a pattern as a type key: %s", err ? err : "");
    oylng_typed_free(y);
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nm:\n  !str(/x-.*/): !any\n--- !m\ny-b: 1\n", NULL, &err);
    CHECK(!y && err && strstr(err, "unknown key y-b"), "a key no pattern takes: %s", err ? err : "accepted");
    oylng_parser_free(p);
    /* a pattern counts its positions in the ceiling and the full size: a group
       1 more than it holds, and every part at least 1 (section 9) */
    const char *wide = "%YAML 2.0\n--- &\na: !str(/(a|b|c){256}/)\n";
    sc = oylng_schema_new(wide, strlen(wide), (oylng_str){ NULL, 0 }, 0);
    CHECK(oylng_schema_status(sc) == OYLNG_OK, "1,024 positions load: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    sc = oylng_schema_new(wide, strlen(wide), (oylng_str){ NULL, 0 }, 1023);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "full size 1024"),
          "a pattern's full size: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    const char *wider = "%YAML 2.0\n--- &\na: !str(/(a|b|c){256}c/)\n";
    sc = oylng_schema_new(wider, strlen(wider), (oylng_str){ NULL, 0 }, 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "1025 positions"),
          "1,025 don't: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    /* refused part way through a union or a sequence (ASan sees a leak here) */
    const char *unclosed = "%YAML 2.0\n--- &\na: !int | !int(1\nb: [!int, !str | !str(/x/, !int]\n";
    sc = oylng_schema_new(unclosed, strlen(unclosed), S("a"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "unclosed parentheses"),
          "unclosed in a union: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    sc = oylng_schema_new(unclosed, strlen(unclosed), S("b"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "unclosed pattern"),
          "unclosed in a sequence: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    const char *empties = "%YAML 2.0\n--- &\na: !str(/(|||){205}/)\nb: !str(/(x{0}){1024}/)\n"
                          "c: !str(/(|||){204}/)\n";
    sc = oylng_schema_new(empties, strlen(empties), S("a"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "1025 positions"),
          "an empty branch counts 1, a group 1 more: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    sc = oylng_schema_new(empties, strlen(empties), S("c"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_OK, "(|||){204} counts 1,020: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);
    sc = oylng_schema_new(empties, strlen(empties), S("b"), 0);
    CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "2048 positions"),
          "a part repeated 0 times counts 1: %s", oylng_schema_error(sc));
    oylng_schema_free(sc);

    /* !** (section 10): what it walks is typed, a scalar no rule picks out
       a string, and what it reaches has no name */
    y = typed(&p, "%YAML 2.0\n--- &\ncfg:\n  name: !str\n  !**:\n    port: !int\n--- !cfg\nname: web\n"
                  "x: {port: 80, host: h}\nl: [1, {port: 81}]\n", NULL, &err);
    CHECK(y != NULL, "!**: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y), x = oylng_value_get(y, r, S("x")), l = oylng_value_get(y, r, S("l"));
        uint64_t u;
        CHECK(oylng_value_type(y, x) == OYLNG_T_MAPPING && !oylng_value_name(y, x).data, "a mapping it walks");
        CHECK(oylng_value_uint(y, oylng_value_get(y, x, S("port")), &u) && u == 80, "a rule at depth");
        CHECK(oylng_value_type(y, oylng_value_get(y, x, S("host"))) == OYLNG_T_STR, "a key no rule takes");
        CHECK(oylng_value_type(y, l) == OYLNG_T_SEQUENCE && oylng_value_count(y, l) == 2, "a sequence it walks");
        CHECK(oylng_value_type(y, oylng_value_item(y, l, 0)) == OYLNG_T_STR, "an item is no key");
        CHECK(oylng_value_uint(y, oylng_value_get(y, oylng_value_item(y, l, 1), S("port")), &u) && u == 81,
              "a rule in a sequence's mapping");
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    static const char *const deep_bad[][2] = {
        { "a:\n  !**:\n    !**: !int\n", "holds no `!**`" },
        { "a:\n  !**: !b\nb:\n  x: !int\n", "takes a scalar type" },
        { "a:\n  !**:\n    !*: [!int, ...]\n", "takes a scalar type" },
        { "a:\n  !**:\n    !str(/a{400}/): !int\n    !str(/b{400}/): !int\n    !str(/c{400}/): !int\n",
          "1200 positions" },
    };
    for (size_t i = 0; i < sizeof deep_bad / sizeof *deep_bad; i++) {
        char text[200];
        snprintf(text, sizeof text, "%%YAML 2.0\n--- &\n%s", deep_bad[i][0]);
        sc = oylng_schema_new(text, strlen(text), S("a"), 0);
        CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), deep_bad[i][1]),
              "!** refused for %s: %s", deep_bad[i][1], oylng_schema_error(sc) ? oylng_schema_error(sc) : "loaded");
        oylng_schema_free(sc);
    }
    /* a union is a mapping with one key, `!|`, and a sequence (section 9) */
    static const char *const union_bad[][2] = {
        { "a: !int | !str\n", "not with ` | `" },
        { "a: {!|: [!int], x: 1}\n", "no other key" },
        { "a: {!|: []}\n", "one or more" },
        { "a: {!|: !int}\n", "one or more" },
        { "a: {!|: [!int, ...]}\n", "repeats an item" },
        { "u: {!|: [!int]}\na: {<<: *u, x: !int}\n", "no other key" },
        { "a: {!|: [{!|: [!a]}, !int]}\n", "only names itself" },
        { "a:\n  !**: {!|: [!int, {x: !int}]}\n", "takes a scalar type" },
    };
    for (size_t i = 0; i < sizeof union_bad / sizeof *union_bad; i++) {
        char text[200];
        snprintf(text, sizeof text, "%%YAML 2.0\n--- &\n%s", union_bad[i][0]);
        sc = oylng_schema_new(text, strlen(text), S("a"), 0);
        CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), union_bad[i][1]),
              "a union refused, %s: %s", union_bad[i][1], oylng_schema_error(sc) ? oylng_schema_error(sc) : "loaded");
        oylng_schema_free(sc);
    }
    /* a union pasted into itself, level on level: literals pasted again
       change nothing, and types add up to the ceiling */
    for (int lits = 1; lits >= 0; lits--) {
        char text[2000];
        int k = snprintf(text, sizeof text, "%%YAML 2.0\n--- &\nu0: {!|: [%s]}\n", lits ? "x, y" : "!int, !str");
        int levels = lits ? 20 : 11;
        for (int l = 1; l <= levels; l++) k += snprintf(text + k, sizeof text - k, "u%d: {!|: [*u%d, *u%d]}\n", l, l - 1, l - 1);
        snprintf(text + k, sizeof text - k, "a: !u%d\n", levels);
        sc = oylng_schema_new(text, strlen(text), S("a"), 0);
        if (lits) CHECK(oylng_schema_status(sc) == OYLNG_OK, "2^21 literals pasted: %s", oylng_schema_error(sc));
        else
            CHECK(oylng_schema_status(sc) == OYLNG_ERR_SCHEMA && strstr(oylng_schema_error(sc), "past the ceiling"),
                  "4,096 types pasted: %s", oylng_schema_error(sc) ? oylng_schema_error(sc) : "loaded");
        oylng_schema_free(sc);
    }
    y = typed(&p, "%YAML 2.0\n--- &\nu: {!|: [!int, !str]}\na: {<<: *u}\n--- !a\nx\n", NULL, &err);
    CHECK(y && oylng_value_type(y, oylng_typed_root(y)) == OYLNG_T_STR, "a union merged alone: %s", err ? err : "");
    oylng_typed_free(y);
    oylng_parser_free(p);

    /* a node shared under two `!**` is typed under each (section 11) */
    y = typed(&p, "%YAML 2.0\n--- &\nv: {x: 1}\nleaf: {}\nroot:\n  a:\n    child: !leaf\n    !**: !int\n"
                  "  b:\n    child: !leaf\n    !**: !str\n--- !root\na:\n  child: *v\nb:\n  child: *v\n", NULL, &err);
    CHECK(y != NULL, "shared under two !**: %s", err ? err : "");
    if (y) {
        oylng_value r = oylng_typed_root(y);
        oylng_value ax = oylng_value_get(y, oylng_value_get(y, oylng_value_get(y, r, S("a")), S("child")), S("x"));
        oylng_value bx = oylng_value_get(y, oylng_value_get(y, oylng_value_get(y, r, S("b")), S("child")), S("x"));
        CHECK(oylng_value_type(y, ax) == OYLNG_T_INT && oylng_value_type(y, bx) == OYLNG_T_STR,
              "each under its own !**: %d, %d", (int)oylng_value_type(y, ax), (int)oylng_value_type(y, bx));
        oylng_typed_free(y);
    }
    oylng_parser_free(p);
    /* a `!**` with a key required takes only a mapping; in a union, any */
    y = typed(&p, "%YAML 2.0\n--- &\na:\n  x: !str\n  !**: !int\n--- !a\n5\n", NULL, &err);
    CHECK(!y && err && strstr(err, "expected a mapping"), "a key required: %s", err ? err : "accepted");
    oylng_parser_free(p);
    y = typed(&p, "%YAML 2.0\n--- &\nj:\n  !**: !int\nu: {!|: [!j, !bool]}\n--- !u\n[1]\n", NULL, &err);
    CHECK(!y && err && strstr(err, "sequence is not allowed"), "in a union: %s", err ? err : "accepted");
    oylng_parser_free(p);

    /* an untyped document */
    y = typed(&p, "%YAML 2.0\n--- !elsewhere.com/x.yes\na: 1\n", NULL, &err);
    CHECK(y && oylng_typed_how(y) == OYLNG_TYPED_NOT && oylng_value_type(y, oylng_typed_root(y)) == OYLNG_T_ANY,
          "a global name stamps nothing");
    oylng_typed_free(y);
    oylng_parser_free(p);

    published_names();
    ignored_stamps();
    bindings();
    emitting();
    float_spellings();
    emitting_more();
    kept_typing();
    scalar_type_keys();
}

int main(int argc, char **argv) {
    api();
    printf("schema: %d API checks, %d failed\n", checks, failed);
    if (argc > 1) examples(argv[1]);
    return failed ? 1 : 0;
}
