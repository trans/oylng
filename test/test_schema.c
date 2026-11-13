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
 * number, true, false and null are plain and a string is quoted. Examples
 * with what the engine doesn't do yet, patterns and !**, are skipped.
 */

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

static bool not_yet(const char *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if ((s[i] == '(' || s[i] == '/') && s[i + 1] == (s[i] == '(' ? '/' : ')')) return true;
        if (i + 2 < n && s[i] == '!' && s[i + 1] == '*' && s[i + 2] == '*') return true;
    }
    return false;
}

static void examples(const char *path) {
    size_t len;
    char *spec = slurp(path, &len);
    if (!spec) {
        fprintf(stderr, "can't read %s\n", path);
        failed++;
        return;
    }
    int pass = 0, fail = 0, skip = 0;
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
        if (not_yet(b[i].body, b[i].n)) {
            skip++;
            continue;
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
    printf("draft: %d examples pass, %d fail, %d skipped (patterns or !**)\n", pass, fail, skip);
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
    y = typed(&p, "%YAML 2.0\n--- &\nn: !int | !str\n--- !n\n18446744073709551616\n", NULL, &err);
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
    y = typed(&p, "%YAML 2.0\n--- &\nf: !float(0..) | !str\n--- !f\n1.8e308\n", NULL, &err);
    CHECK(!y && err && strstr(err, "binary64"), "too large refuses: %s", err ? err : "accepted");
    oylng_parser_free(p);

    /* precision, as section 4.6 has it */
    const char *prec[][3] = { { "5:2", "999.99", "y" }, { "5:2", "1000.00", "n" }, { "3:1", "1.50", "y" },
                              { "3:0", "1e3", "n" }, { "4:0", "1e3", "y" }, { "5:4", "0.0015", "y" },
                              { "5:3", "0.0015", "n" }, { "1:0", "0e999999999999999999999", "y" } };
    for (size_t i = 0; i < sizeof prec / sizeof *prec; i++) {
        char s[200];
        snprintf(s, sizeof s, "%%YAML 2.0\n--- &\nd: !decimal(%s) | !str\n--- !d\n%s\n", prec[i][0], prec[i][1]);
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
        snprintf(s, sizeof s, "%%YAML 2.0\n--- &\nd: !decimal(%s) | !str\n--- !d\n%s\n", lim[i][0], lim[i][1]);
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

    /* an untyped document */
    y = typed(&p, "%YAML 2.0\n--- !elsewhere.com/x.yes\na: 1\n", NULL, &err);
    CHECK(y && oylng_typed_how(y) == OYLNG_TYPED_NOT && oylng_value_type(y, oylng_typed_root(y)) == OYLNG_T_ANY,
          "a global name stamps nothing");
    oylng_typed_free(y);
    oylng_parser_free(p);
}

int main(int argc, char **argv) {
    api();
    printf("schema: %d API checks, %d failed\n", checks, failed);
    if (argc > 1) examples(argv[1]);
    return failed ? 1 : 0;
}
