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
    printf("draft: %d examples pass, %d fail\n", pass, fail);
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
}

int main(int argc, char **argv) {
    api();
    printf("schema: %d API checks, %d failed\n", checks, failed);
    if (argc > 1) examples(argv[1]);
    return failed ? 1 : 0;
}
