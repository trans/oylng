/*
 * test_api.c — what the suite doesn't check: errors' lines and columns,
 * the offset after a stop, limits, levels, CR LF, the API's edges, what
 * trees share, and trees that keep references
 */

#include "oylng/oylng.h"
#include "oylng/schema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Reads the whole stream: OYLNG_OK, or the error */
static oylng_status read_all(oylng_parser *p, int *events) {
    const oylng_event *e;
    int n = 0;
    for (;;) {
        oylng_status st = oylng_parse_next(p, &e);
        if (st != OYLNG_OK) { if (events) *events = n; return st; }
        n++;
        if (e->type == OYLNG_EVT_STREAM_END) { if (events) *events = n; return OYLNG_OK; }
    }
}

static oylng_status parse(const char *s, uint64_t limit, oylng_mark *m) {
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    if (limit) oylng_parser_set_limit(p, limit);
    oylng_status st = read_all(p, NULL);
    if (m) *m = oylng_parser_error_mark(p);
    oylng_parser_free(p);
    return st;
}

static void test_error_marks(void) {
    oylng_mark m;
    CHECK(parse("%YAML 2.0\n---\na: 1\na: 2\n", 0, &m) == OYLNG_ERR_PARSE, "repeated key");
    CHECK(m.line == 4 && m.col == 1 && m.offset == 19, "repeated key at %zu:%zu (%zu)", m.line, m.col, m.offset);
    CHECK(parse("%YAML 2.0\n---\nkey: \"\\x41\"\n", 0, &m) == OYLNG_ERR_PARSE, "bad escape");
    CHECK(m.line == 3 && m.col == 7, "bad escape at %zu:%zu", m.line, m.col);
    CHECK(parse("%YAML 2.0\n---\na:\n  b: !x\n", 0, &m) == OYLNG_ERR_PARSE, "tag");
    CHECK(m.line == 4 && m.col == 6, "tag at %zu:%zu", m.line, m.col);
    CHECK(parse("%YAML 2.0\r\n---\r\na: *nope\r\n", 0, &m) == OYLNG_ERR_PARSE, "missing entry");
    CHECK(m.line == 3 && m.col == 4, "missing entry at %zu:%zu", m.line, m.col);
    CHECK(parse("%YAML 1.2\n---\na: 1\n", 0, &m) == OYLNG_ERR_PARSE, "1.2 stream");
    CHECK(m.line == 1 && m.col == 1, "1.2 stream at %zu:%zu", m.line, m.col);
}

static void test_stop(void) {
    const char *s = "%YAML 2.0\n---\ntitle: Hello\n...\n# Heading\n\nBody.\n";
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    CHECK(read_all(p, NULL) == OYLNG_OK, "front matter");
    size_t off = 0;
    CHECK(oylng_parser_stopped(p, &off), "stopped");
    CHECK(strcmp(s + off, "# Heading\n\nBody.\n") == 0, "rest is \"%s\"", s + off);
    oylng_parser_free(p);

    const char *t = "%YAML 2.0\n---\na: 1\n...\n\n";
    p = oylng_parser_new(t, strlen(t));
    CHECK(read_all(p, NULL) == OYLNG_OK && !oylng_parser_stopped(p, NULL), "... then blank lines: no stop");
    oylng_parser_free(p);
}

static void test_limits(void) {
    /* section 10's first example: full 41, held 29 */
    const char *ex = "%YAML 2.0\n--- &\na: [x, yy]\nm: {k: v}\n---\nb: *a\nc:\n  -<< *a\n  - z\nd:\n  <<: *m\n";
    CHECK(parse(ex, 41, NULL) == OYLNG_OK, "41 allows it");
    CHECK(parse(ex, 40, NULL) == OYLNG_ERR_LIMIT, "40 refuses it");

    /* the replacement in section 10: full never reaches 18, held is 14 */
    const char *peak = "%YAML 2.0\n--- &\na: [x, yy]\n--- &\na:\n  -<< *a\n  - z\n";
    CHECK(parse(peak, 14, NULL) == OYLNG_OK, "replacement at 14");
    CHECK(parse(peak, 13, NULL) == OYLNG_ERR_LIMIT, "replacement at 13");

    /* replacing an entry takes it off full before the new key is checked:
       the first table is full 26 (x 13, k 2 + 11) and held 16; then k: v
       leaves full 17 and held 20, but 26 plus the new key would be 28 */
    const char *rep = "%YAML 2.0\n--- &\nx: [aaaa, bbbb]\nk: *x\n--- &\nk: v\n";
    CHECK(parse(rep, 26, NULL) == OYLNG_OK, "replacement at the limit");
    CHECK(parse(rep, 25, NULL) == OYLNG_ERR_LIMIT, "replacement past it");

    /* a chain that doubles 62 times passes 2^64 and must not wrap */
    char *chain = malloc(8192);
    strcpy(chain, "%YAML 2.0\n--- &\na0: [x, x]\n");
    for (int i = 1; i < 63; i++)
        sprintf(chain + strlen(chain), "a%d:\n  -<< *a%d\n  -<< *a%d\n", i, i - 1, i - 1);
    strcat(chain, "---\nb: *a62\n");
    CHECK(parse(chain, 0, NULL) == OYLNG_ERR_LIMIT, "doubling chain at the default");
    CHECK(parse(chain, UINT64_MAX - 1, NULL) == OYLNG_ERR_LIMIT, "doubling chain saturates, not wraps");
    free(chain);
}

static void test_levels(void) {
    char *s = malloc(8192);
    if (!s) return;
    strcpy(s, "%YAML 2.0\n---\n");
    size_t n = strlen(s);
    for (int i = 0; i < 256; i++) s[n++] = '[';
    for (int i = 0; i < 256; i++) s[n++] = ']';
    s[n++] = '\n'; s[n] = 0;
    CHECK(parse(s, 0, NULL) == OYLNG_OK, "256 levels");
    strcpy(s, "%YAML 2.0\n---\n");
    n = strlen(s);
    for (int i = 0; i < 257; i++) s[n++] = '[';
    for (int i = 0; i < 257; i++) s[n++] = ']';
    s[n++] = '\n'; s[n] = 0;
    CHECK(parse(s, 0, NULL) == OYLNG_ERR_LIMIT, "257 levels");
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    oylng_parser_set_levels(p, 300);
    CHECK(read_all(p, NULL) == OYLNG_OK, "257 levels when set to 300");
    oylng_parser_free(p);

    /* levels count through references */
    strcpy(s, "%YAML 2.0\n--- &\ne0: x\n");
    for (int i = 1; i <= 255; i++) sprintf(s + strlen(s), "e%d: [*e%d]\n", i, i - 1);
    char *deep = malloc(strlen(s) + 64);
    if (!deep) { free(s); return; }
    sprintf(deep, "%s---\ny: *e255\n", s);
    CHECK(parse(deep, 0, NULL) == OYLNG_OK, "256 levels through references");
    sprintf(deep, "%s---\ny: [*e255]\n", s);
    CHECK(parse(deep, 0, NULL) == OYLNG_ERR_LIMIT, "257 levels through references");
    free(deep);

    /* a -<< puts the items, not the sequence, at its place, and a <<: the
       values: [-<< *s] with s: [x] is one level, and {<<: *m} with m: {a: [x]}
       is two */
    const char *table[] = { "s: [x]", "m: {a: [x]}" };
    const char *inner[] = { "[-<< *s]", "{<<: *m}" };
    int below[] = { 255, 254 };
    for (int t = 0; t < 2; t++)
        for (int extra = 0; extra < 2; extra++) {
            n = (size_t)sprintf(s, "%%YAML 2.0\n--- &\n%s\n---\n", table[t]);
            for (int i = 0; i < below[t] + extra; i++) s[n++] = '[';
            n += (size_t)sprintf(s + n, "%s", inner[t]);
            for (int i = 0; i < below[t] + extra; i++) s[n++] = ']';
            s[n++] = '\n'; s[n] = 0;
            oylng_status st = parse(s, 0, NULL);
            CHECK(st == (extra ? OYLNG_ERR_LIMIT : OYLNG_OK), "%s at %d levels: %d", inner[t], 256 + extra, st);
        }
    free(s);
}

static void test_api_edges(void) {
    const char *s = "%YAML 2.0\n--- !job\n- \"\\u00e9\"\n- 'it''s'\n- plain\n";
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    const oylng_event *e;
    oylng_parse_next(p, &e);
    CHECK(e->type == OYLNG_EVT_STREAM_START, "stream start");
    oylng_parse_next(p, &e);
    CHECK(e->type == OYLNG_EVT_DOC_START && e->value.len == 3 && !memcmp(e->value.data, "job", 3), "name");
    oylng_parse_next(p, &e);
    oylng_parse_next(p, &e);
    CHECK(e->type == OYLNG_EVT_SCALAR && e->value.len == 2 && !memcmp(e->value.data, "\xC3\xA9", 2), "escape");
    oylng_parse_next(p, &e);
    CHECK(e->value.len == 4 && !memcmp(e->value.data, "it's", 4), "single quotes");
    oylng_parse_next(p, &e);
    CHECK(e->value.data == s + strlen(s) - 6, "a plain scalar points into the input");
    CHECK(read_all(p, NULL) == OYLNG_OK, "rest");
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_STREAM_END, "STREAM_END again");
    oylng_parser_free(p);

    p = oylng_parser_new("%YAML 2.0\n---\n[\n", 16);
    CHECK(read_all(p, NULL) == OYLNG_ERR_PARSE, "unclosed");
    CHECK(oylng_parse_next(p, &e) == OYLNG_ERR_PARSE && e == NULL, "the error again");
    CHECK(oylng_parser_error(p) != NULL, "a message");
    oylng_parser_free(p);

    CHECK(oylng_parse_next(NULL, &e) == OYLNG_ERR_INPUT, "NULL parser");
    p = oylng_parser_new(NULL, 0);
    CHECK(read_all(p, NULL) == OYLNG_ERR_PARSE, "an empty input isn't a stream");
    oylng_parser_free(p);

    /* a document is delivered as it is read, and one in error stops
       before its end (section 2) */
    const char *bad = "%YAML 2.0\n---\na: 1\n---\nb: 1\nb: 2\n";
    p = oylng_parser_new(bad, strlen(bad));
    int n = 0;
    oylng_status st = read_all(p, &n);
    /* +STR +DOC +MAP a 1 -MAP -DOC, then +DOC +MAP b 1 */
    CHECK(st == OYLNG_ERR_PARSE && n == 11, "events before the error: %d", n);
    oylng_parser_free(p);
}

/* The stream's scalars, space-separated, with { } [ ] for collections; or
 * ERR line:col */
static const char *brief(const char *s) {
    static char out[1 << 16];
    size_t o = 0;
    out[0] = 0;
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    const oylng_event *e;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) {
            oylng_mark m = oylng_parser_error_mark(p);
            snprintf(out, sizeof out, "ERR %zu:%zu", m.line, m.col);
            break;
        }
        if (e->type == OYLNG_EVT_STREAM_END) break;
        const char *t = e->type == OYLNG_EVT_MAP_START ? "{" : e->type == OYLNG_EVT_MAP_END ? "}"
                      : e->type == OYLNG_EVT_SEQ_START ? "[" : e->type == OYLNG_EVT_SEQ_END ? "]" : NULL;
        if (e->type == OYLNG_EVT_SCALAR)
            o += (size_t)snprintf(out + o, sizeof out - o, "%s%.*s", o ? " " : "", (int)e->value.len, e->value.data);
        else if (t) o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", o ? " " : "", t);
        if (o >= sizeof out) { o = sizeof out - 1; break; }
    }
    oylng_parser_free(p);
    return out;
}

/* Block mapping lines "k<from>: <pre>v<from>" up to k<to - 1>, indented
 * by ind */
static size_t keys(char *b, size_t o, int ind, const char *pre, int from, int to) {
    for (int i = from; i < to; i++) o += (size_t)sprintf(b + o, "%*sk%d: %sv%d\n", ind, "", i, pre, i);
    return o;
}

/* Past 8 pairs, a mapping's keys are found by hash: repeats, merges, paths */
static void test_big_maps(void) {
    static char b[1 << 15];
    size_t o;

    /* a repeat at each size: compared one by one, hashed, after growing */
    int at[] = { 2, 8, 9, 10, 17, 33, 100 };
    for (size_t t = 0; t < sizeof at / sizeof *at; t++) {
        o = (size_t)sprintf(b, "%%YAML 2.0\n---\n");
        o = keys(b, o, 0, "", 0, at[t]);
        o += (size_t)sprintf(b + o, "k%d: again\n", at[t] / 2);
        char want[32];
        snprintf(want, sizeof want, "ERR %d:1", at[t] + 3);
        CHECK(!strcmp(brief(b), want), "repeat after %d keys: %s", at[t], brief(b));
        o = (size_t)sprintf(b, "%%YAML 2.0\n---\n");
        keys(b, o, 0, "", 0, at[t] + 1);
        CHECK(strncmp(brief(b), "{ k0 v0", 7) == 0, "%d keys", at[t] + 1);
    }

    /* flow, past 8 */
    {
        const char *f = "%YAML 2.0\n---\n{a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10, c: 11}\n";
        char want[32];
        snprintf(want, sizeof want, "ERR 3:%d", (int)(strstr(f, ", c: 11") + 2 - strchr(f, '{')) + 1);
        CHECK(!strcmp(brief(f), want), "flow repeat: %s, not %s", brief(f), want);
    }

    /* nested: the inner mapping's table sits above the outer's, then the
       outer grows; a key of the outer may be a key of the inner */
    o = (size_t)sprintf(b, "%%YAML 2.0\n---\n");
    o = keys(b, o, 0, "", 0, 12);
    o += (size_t)sprintf(b + o, "inner:\n");
    o = keys(b, o, 2, "", 0, 40);
    o = keys(b, o, 0, "", 12, 30);
    CHECK(strstr(brief(b), "inner { k0 v0") && !strstr(brief(b), "ERR"), "nested: %.40s", brief(b));
    o += (size_t)sprintf(b + o, "k3: again\n");
    CHECK(!strcmp(brief(b), "ERR 74:1"), "repeat in the outer after the inner: %s", brief(b));

    /* the table: paths into a mapping of 20 keys, and one of 3 */
    o = (size_t)sprintf(b, "%%YAML 2.0\n--- &\nbig:\n");
    o = keys(b, o, 2, "", 0, 20);
    o += (size_t)sprintf(b + o, "small: {a: 1, b: 2, c: 3}\n");
    size_t table = o;
    sprintf(b + o, "---\n- *big:k0\n- *big:k19\n- *big:k10\n- *small:c\n- *small:a\n");
    CHECK(!strcmp(brief(b), "[ v0 v19 v10 3 1 ]"), "paths: %s", brief(b));
    sprintf(b + table, "---\n- *big:k20\n");
    CHECK(!strcmp(brief(b), "ERR 26:8"), "no such key: %s", brief(b));

    /* merged from a big mapping: replaced in place, added at the end, then
       paths into the merged mapping, which is big too */
    o = table;
    o += (size_t)sprintf(b + o, "m:\n  <<: *big\n  k5: new\n  extra: x\n");
    size_t merged = o;
    sprintf(b + o, "---\n- *m:k5\n- *m:k19\n- *m:extra\n- *m:k0\n");
    CHECK(!strcmp(brief(b), "[ new v19 x v0 ]"), "paths through a merge: %s", brief(b));
    sprintf(b + merged, "---\n<<: *big\nk5: w\nk19: z\n");
    {
        const char *r = brief(b);
        CHECK(strstr(r, "k4 v4 k5 w k6 v6") && strstr(r, "k19 z }"), "merged in place: %.60s", r);
    }
    sprintf(b + merged, "---\n<<: *big\nk5: w\nk5: again\n");
    CHECK(!strcmp(brief(b), "ERR 32:1"), "written twice after a merge: %s", brief(b));

    /* two merges, the second over the first; then a small merged mapping */
    o = (size_t)sprintf(b, "%%YAML 2.0\n--- &\nbig:\n");
    o = keys(b, o, 2, "", 0, 20);
    o += (size_t)sprintf(b + o, "big2:\n");
    o = keys(b, o, 2, "w", 10, 30);
    o += (size_t)sprintf(b + o, "s: {a: 1, b: 2}\nms:\n  <<: *s\n  b: 3\n");
    sprintf(b + o, "---\n- *ms:b\n- *ms:a\n- {<<: *big, <<: *big2}\n");
    {
        const char *r = brief(b);
        CHECK(!strncmp(r, "[ 3 1 { k0 v0", 13) && strstr(r, "k9 v9 k10 wv10")
              && strstr(r, "k19 wv19 k20 wv20") && strstr(r, "k29 wv29 } ]"),
              "two merges: %.80s", r);
    }
}

/* What streaming holds: merged mappings until they close, keys until
 * theirs does, and nothing of a delivered value past the next call */
static void test_stream(void) {
    const char *t = "%YAML 2.0\n--- &\nb: {x: 1, y: 2}\nm: {<<: *b, y: 3}\ne: []\n---\n";
    char s[4096];
    /* a merged mapping among others, with a mapping, a reference and a
       built value inside, then more */
    sprintf(s, "%s- a\n- <<: *b\n  y: 'it''s'\n  z: {p: *m}\n- {<<: *m, w: \"t\\u0041\"}\n- c\n", t);
    CHECK(!strcmp(brief(s), "[ a { x 1 y it's z { p { x 1 y 3 } } } { x 1 y 3 w tA } c ]"),
          "merged among others: %s", brief(s));
    /* one inside another, and the root */
    sprintf(s, "%s<<: *m\nn:\n  <<: *b\n  k: [-<< *e, q]\n", t);
    CHECK(!strcmp(brief(s), "{ x 1 y 3 n { x 1 y 2 k [ q ] } }"), "nested merges: %s", brief(s));
    sprintf(s, "%s--- {<<: *b, x: 0}\n", t);
    CHECK(!strcmp(brief(s), "{ x 0 y 2 }"), "a flow root: %s", brief(s));
    sprintf(s, "%s[-<< *e, -<< *e]\n", t);
    CHECK(!strcmp(brief(s), "[ ]"), "empty splices: %s", brief(s));
    /* {} built before any pair, and a path into a merge of {}: there are
       no pairs to point at yet, which the sanitizers watch for */
    const char *none = "%YAML 2.0\n--- &\ne: {}\n---\nx: *e\n";
    CHECK(!strcmp(brief(none), "{ x { } }"), "{} built first: %s", brief(none));
    none = "%YAML 2.0\n--- &\ne: {}\nm: {<<: *e}\n---\n- *m:k\n";
    CHECK(!strcmp(brief(none), "ERR 6:6"), "a path into a merge of {}: %s", brief(none));

    /* built keys are kept while values come and go: a repeat of the
       first, after values built from lines */
    size_t n = (size_t)sprintf(s, "%%YAML 2.0\n---\n\"k\\u0041\": 1\n");
    for (int i = 0; i < 40; i++) n += (size_t)sprintf(s + n, "\"v%d\\t\": |\n  line %d\n", i, i);
    sprintf(s + n, "kA: 2\n");
    CHECK(!strcmp(brief(s), "ERR 84:1"), "a built key's repeat: %s", brief(s));

    /* a built value lasts until the next call */
    const char *v = "%YAML 2.0\n---\n- \"a\\tb\"\n- \"c\\td\"\n";
    oylng_parser *p = oylng_parser_new(v, strlen(v));
    const oylng_event *e;
    for (int i = 0; i < 4; i++) oylng_parse_next(p, &e);
    CHECK(e->type == OYLNG_EVT_SCALAR && e->value.len == 3 && !memcmp(e->value.data, "a\tb", 3), "first value");
    oylng_parse_next(p, &e);
    CHECK(e->type == OYLNG_EVT_SCALAR && e->value.len == 3 && !memcmp(e->value.data, "c\td", 3), "second value");
    oylng_parser_free(p);
}

/* What the fast paths read, at their edges: test-plain runs these without
 * them, and the results must be the same */
static void test_fast(void) {
    oylng_mark m;
    int n;
    /* the size limit passed inside a flow collection on one line: 1 + 1 +
       (2 + 3) * 2 */
    const char *f = "%YAML 2.0\n---\n- {a: bb, c: dd}\n";
    CHECK(parse(f, 12, NULL) == OYLNG_OK, "a flow line at the limit");
    CHECK(parse(f, 11, &m) == OYLNG_ERR_LIMIT && m.line == 3 && m.col == 14, "past it, at dd: %zu:%zu",
          m.line, m.col);
    oylng_parser *p = oylng_parser_new(f, strlen(f));
    oylng_parser_set_limit(p, 11);
    CHECK(read_all(p, &n) == OYLNG_ERR_LIMIT && n == 7, "the events before it: %d", n);
    oylng_parser_free(p);
    CHECK(!strcmp(brief("%YAML 2.0\n---\n{a: 1, b: 2, a: 3}\n"), "ERR 3:14"), "a repeat in a flow line");

    /* more events on one line than the queue has room for */
    char s[4096];
    size_t o = (size_t)sprintf(s, "%%YAML 2.0\n---\n[");
    for (int i = 0; i < 300; i++) o += (size_t)sprintf(s + o, "%s%d", i ? ", " : "", i);
    sprintf(s + o, "]\n");
    const char *b = brief(s);
    CHECK(!strncmp(b, "[ 0 1 2", 7) && strstr(b, " 298 299 ]") && strlen(strstr(b, "299")) == 5,
          "a long flow line: %.40s", b);
    o = (size_t)sprintf(s, "%%YAML 2.0\n---\n{");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(s + o, "k%d: %d, ", i, i);
    sprintf(s + o, "k0: x}\n");
    CHECK(!strcmp(brief(s), "ERR 3:882"), "a repeat in a long flow line: %s", brief(s));

    /* block mapping lines, then a repeat, or the limit */
    const char *r = "%YAML 2.0\n---\na: 1\nb: 2\nc: 3\na: 4\n";
    p = oylng_parser_new(r, strlen(r));
    CHECK(read_all(p, &n) == OYLNG_ERR_PARSE && n == 9, "a repeat after lines: %d events", n);
    m = oylng_parser_error_mark(p);
    CHECK(m.line == 6 && m.col == 1, "at the repeat: %zu:%zu", m.line, m.col);
    oylng_parser_free(p);
    const char *l = "%YAML 2.0\n---\nab: cd\nef: gh\n";
    CHECK(parse(l, 13, NULL) == OYLNG_OK, "lines at the limit");
    CHECK(parse(l, 12, &m) == OYLNG_ERR_LIMIT && m.line == 4 && m.col == 5, "past it, at gh: %zu:%zu",
          m.line, m.col);

    /* what a line's plain value holds, and what may follow it */
    CHECK(!strcmp(brief("%YAML 2.0\r\n---\r\na: 1 # c\r\nb: 'x' # d\r\nc: \"y\"\r\n"), "{ a 1 b x c y }"),
          "breaks and comments: %s", brief("%YAML 2.0\r\n---\r\na: 1 # c\r\nb: 'x' # d\r\nc: \"y\"\r\n"));
    CHECK(!strcmp(brief("%YAML 2.0\n---\na: b#c\nd: e:f  \ng: h i\n"), "{ a b#c d e:f g h i }"),
          "# and : in values: %s", brief("%YAML 2.0\n---\na: b#c\nd: e:f  \ng: h i\n"));
    CHECK(!strcmp(brief("%YAML 2.0\n---\na: \"x\"# c\n"), "ERR 3:7"), "a # right after a quote");
    CHECK(!strcmp(brief("%YAML 2.0\n---\na: b: c\n"), "ERR 3:5"), "a second : in a line");
}

/* The text of a tree's string, as a C string, for checks */
static const char *txt(const oylng_tree *t, oylng_node n) {
    static char b[4][64];
    static int k;
    oylng_str s = oylng_node_text(t, n);
    char *o = b[k++ & 3];
    snprintf(o, 64, "%.*s", (int)s.len, s.data ? s.data : "(null)");
    return o;
}

static oylng_node at(const oylng_tree *t, oylng_node map, const char *key) {
    return oylng_node_get(t, map, (oylng_str){ key, strlen(key) });
}

static oylng_tree *tree_of(const char *s, oylng_parser **pp) {
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    oylng_parser_set_limit(p, UINT64_MAX);
    oylng_tree *t = NULL;
    oylng_status st = oylng_parse_tree(p, &t);
    if (st != OYLNG_OK) printf("  (%s)\n", oylng_parser_error(p));
    if (pp) *pp = p;
    else oylng_parser_free(p);
    return t;
}

static void test_tree(void) {
    /* a node of the table is one node, wherever it is named */
    oylng_tree *t = tree_of("%YAML 2.0\n--- &\na: {p: 1, q: [x, y]}\n---\n[*a, *a, {r: *a}]\n", NULL);
    CHECK(t, "a tree");
    oylng_node r = oylng_tree_root(t);
    CHECK(oylng_node_kind(t, r) == OYLNG_NODE_SEQUENCE && oylng_node_count(t, r) == 3, "the root");
    oylng_node a = oylng_node_item(t, r, 0);
    CHECK(a != OYLNG_NO_NODE && oylng_node_item(t, r, 1) == a, "*a twice is one node");
    CHECK(at(t, oylng_node_item(t, r, 2), "r") == a, "and a third time, inside");
    CHECK(oylng_tree_nodes(t) == 10, "10 nodes, not %zu", oylng_tree_nodes(t));
    CHECK(!strcmp(txt(t, at(t, a, "p")), "1") && oylng_node_count(t, at(t, a, "q")) == 2, "a's contents");
    CHECK(oylng_node_item(t, r, 3) == OYLNG_NO_NODE && oylng_node_key(t, r, 0) == OYLNG_NO_NODE &&
          oylng_node_kind(t, OYLNG_NO_NODE) == OYLNG_NODE_NONE && oylng_node_count(t, 999) == 0 &&
          oylng_node_text(t, r).data == NULL, "what isn't there");
    oylng_tree_free(t);

    /* references of references: ten to the seventh strings, in 22 nodes (each x
     * written is a node) */
    char big[2048];
    size_t o = (size_t)snprintf(big, sizeof big, "%%YAML 2.0\n--- &\nl0: [x, x, x, x, x, x, x, x, x, x]\n");
    for (int i = 1; i < 7; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "l%d: [*l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d]\n",
                              i, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1);
    snprintf(big + o, sizeof big - o, "---\n{all: *l6, s: [-<< *l0, -<< *l0, y]}\n");
    t = tree_of(big, NULL);
    CHECK(t && oylng_tree_nodes(t) == 22, "%zu nodes", oylng_tree_nodes(t));
    oylng_node all = at(t, oylng_tree_root(t), "all"), sp = at(t, oylng_tree_root(t), "s");
    for (int i = 0; i < 6; i++) all = oylng_node_item(t, all, 9);
    CHECK(oylng_node_count(t, all) == 10 && !strcmp(txt(t, oylng_node_item(t, all, 9)), "x"), "down to l0");
    CHECK(oylng_node_count(t, sp) == 21 && oylng_node_item(t, sp, 0) == oylng_node_item(t, sp, 10) &&
          oylng_node_item(t, sp, 3) == oylng_node_item(t, all, 3) && !strcmp(txt(t, oylng_node_item(t, sp, 20)), "y"),
          "splices spread, their items shared");
    oylng_tree_free(t);

    /* merges: a key's value is the node it came from */
    t = tree_of("%YAML 2.0\n--- &\nbase: {a: 1, b: [2]}\nm: {<<: *base, c: 3}\n---\n"
                "x: *base\ny: *m\nz:\n  <<: *base\n  b: 4\n", NULL);
    r = oylng_tree_root(t);
    oylng_node x = at(t, r, "x"), y = at(t, r, "y"), z = at(t, r, "z");
    CHECK(oylng_node_count(t, y) == 3 && at(t, y, "a") == at(t, x, "a") && at(t, y, "b") == at(t, x, "b") &&
          !strcmp(txt(t, at(t, y, "c")), "3"), "a merge in the table");
    CHECK(oylng_node_count(t, z) == 2 && at(t, z, "a") == at(t, x, "a") && !strcmp(txt(t, at(t, z, "b")), "4"),
          "a merge in the document");
    CHECK(at(t, z, "c") == OYLNG_NO_NODE && at(t, r, "") == OYLNG_NO_NODE, "keys that aren't there");
    oylng_tree_free(t);

    /* a mapping's index, and texts that outlive the parser */
    char m[8192];
    o = (size_t)snprintf(m, sizeof m, "%%YAML 2.0\n--- !doc/one\n");
    for (int i = 0; i < 300; i++) o += (size_t)snprintf(m + o, sizeof m - o, "k%d: \"v\\u00e9%d\"\n", i, i);
    oylng_parser *p;
    t = tree_of(m, &p);
    oylng_parser_free(p);
    r = oylng_tree_root(t);
    CHECK(!strcmp(oylng_tree_name(t).data ? "named" : "", "named") && oylng_tree_name(t).len == 7 &&
          !memcmp(oylng_tree_name(t).data, "doc/one", 7), "the name");
    int found = 0;
    for (int i = 0; i < 300; i++) {
        char k[16], v[16];
        snprintf(k, sizeof k, "k%d", i);
        snprintf(v, sizeof v, "v\xc3\xa9%d", i);
        oylng_node n = at(t, r, k);
        found += n == oylng_node_value(t, r, (size_t)i) && !strcmp(txt(t, n), v);
    }
    CHECK(found == 300, "300 keys found by the index: %d", found);
    CHECK(at(t, r, "k300") == OYLNG_NO_NODE && at(t, r, "k") == OYLNG_NO_NODE, "and two not");
    oylng_mark mk = oylng_node_mark(t, oylng_node_value(t, r, 2));
    CHECK(mk.line == 5 && mk.col == 5, "a value's mark: %zu:%zu", mk.line, mk.col);
    oylng_tree_free(t);

    /* many small trees naming the end of a large table: each tree's notes
     * of what it copied are the parser's, never cleared */
    size_t cap = 4u << 20;
    char *many = malloc(cap);
    o = (size_t)snprintf(many, cap, "%%YAML 2.0\n--- &\n");
    for (int i = 0; i < 100000; i++) o += (size_t)snprintf(many + o, cap - o, "e%d: [%d]\n", i, i);
    for (int i = 0; i < 20000; i++) o += (size_t)snprintf(many + o, cap - o, "---\n[*e99999, *e%d]\n", i);
    p = oylng_parser_new(many, o);
    int good = 0;
    while (oylng_parse_tree(p, &t) == OYLNG_OK && t) {
        r = oylng_tree_root(t);
        good += oylng_tree_nodes(t) == 5 && !strcmp(txt(t, oylng_node_item(t, oylng_node_item(t, r, 0), 0)), "99999");
        oylng_tree_free(t);
    }
    CHECK(good == 20000 && !oylng_parser_error(p), "20,000 trees: %d", good);
    oylng_parser_free(p);
    free(many);

    /* trees and events in turn; a tree inside a document is refused */
    const char *three = "%YAML 2.0\n--- &\ns: [1, 2]\n---\n*s\n---\na: *s\n---\n[*s]\n";
    p = oylng_parser_new(three, strlen(three));
    const oylng_event *e;
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_STREAM_START, "events first");
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t && oylng_node_count(t, oylng_tree_root(t)) == 2, "a tree");
    oylng_tree_free(t);
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_DOC_START, "events again");
    CHECK(oylng_parse_tree(p, &t) == OYLNG_ERR_INPUT && !t && !oylng_parser_error(p), "not inside");
    int n = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_DOC_END) n++;
    CHECK(n == 7, "the document's events, references followed: %d", n);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "the third");
    oylng_emitter *em = oylng_emitter_new();
    oylng_emit_stream_start(em);
    CHECK(oylng_emit_tree(em, t) == OYLNG_OK && oylng_emit_tree(em, t) == OYLNG_OK, "written twice");
    oylng_emit_stream_end(em);
    oylng_str out = oylng_emitter_output(em);
    const char *want = "%YAML 2.0\n---\n- - 1\n  - 2\n---\n- - 1\n  - 2\n";
    CHECK(out.len == strlen(want) && !memcmp(out.data, want, out.len), "as %.*s", (int)out.len, out.data);
    oylng_emitter_free(em);
    oylng_tree_free(t);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && !t, "the end");
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && !t, "and again");
    oylng_parser_free(p);

    /* an error is the parser's */
    const char *bad = "%YAML 2.0\n---\na: 1\n---\nb: 2\nb: 3\n";
    p = oylng_parser_new(bad, strlen(bad));
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "the first is good");
    oylng_tree_free(t);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_ERR_PARSE && !t, "the second isn't");
    mk = oylng_parser_error_mark(p);
    CHECK(oylng_parser_error(p) && mk.line == 6 && mk.col == 1, "at %zu:%zu", mk.line, mk.col);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_ERR_PARSE && !t, "and stays so");
    oylng_parser_free(p);
    CHECK(oylng_parse_tree(NULL, &t) == OYLNG_ERR_INPUT && oylng_parse_tree(NULL, NULL) == OYLNG_ERR_INPUT &&
          oylng_tree_root(NULL) == OYLNG_NO_NODE && oylng_tree_nodes(NULL) == 0 &&
          oylng_emit_tree(NULL, NULL) == OYLNG_ERR_INPUT, "NULLs");
    oylng_tree_free(NULL);
}

/* The table of the stream at its point p has reached, as a tree: NULL and
 * a note if it isn't OYLNG_OK */
static oylng_tree *table_of(oylng_parser *p) {
    oylng_tree *t = (oylng_tree *)1;
    oylng_status st = oylng_parser_table(p, &t);
    if (st != OYLNG_OK) printf("  (table: %d, %s)\n", (int)st, oylng_parser_error(p));
    return st == OYLNG_OK ? t : NULL;
}

/* Whether table t's entries are the keys keys, in order, separated by spaces */
static bool keys_are(const oylng_tree *t, const char *keys) {
    char b[200] = "";
    size_t o = 0;
    oylng_node r = oylng_tree_root(t);
    for (size_t i = 0; i < oylng_node_count(t, r); i++)
        o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", i ? " " : "", txt(t, oylng_node_key(t, r, i)));
    if (strcmp(b, keys)) printf("  (keys: %s)\n", b);
    return t && oylng_node_kind(t, r) == OYLNG_NODE_MAPPING && !strcmp(b, keys);
}

static void test_table(void) {
    /* the table, its entries in order, a node named twice shared; read
     * with the document after it, and the same once that has ended */
    const char *s = "%YAML 2.0\n--- &\na: [x, yy]\nm: {k: v}\nb: *a\nbase: {p: 1}\nmm: {<<: *base, q: 2}\n"
                    "---\nd: *m\n";
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    oylng_tree *t = table_of(p);
    CHECK(!t, "no table before the stream is read");
    const oylng_event *e;
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_STREAM_START && !table_of(p),
          "nor at its start: a table is read with the document after it");
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_DOC_START, "the document");
    t = table_of(p);
    CHECK(keys_are(t, "a m b base mm"), "inside it, its table");
    oylng_node r = oylng_tree_root(t), a = at(t, r, "a"), mm = at(t, r, "mm");
    CHECK(oylng_node_count(t, a) == 2 && !strcmp(txt(t, oylng_node_item(t, a, 1)), "yy") && at(t, r, "b") == a,
          "a, and b the same node");
    CHECK(oylng_node_count(t, mm) == 2 && at(t, mm, "p") == at(t, at(t, r, "base"), "p") &&
          !strcmp(txt(t, at(t, mm, "q")), "2"), "a merge applied, its value shared");
    CHECK(oylng_tree_nodes(t) == 18, "18 nodes, as written: %zu", oylng_tree_nodes(t));
    oylng_mark mk = oylng_node_mark(t, r);
    CHECK(mk.line == 3 && mk.col == 1 && !oylng_tree_name(t).data, "where the first entry is, and no name: %zu:%zu",
          mk.line, mk.col);
    oylng_tree_free(t);
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_DOC_END) {}
    t = table_of(p);
    CHECK(keys_are(t, "a m b base mm"), "after the document's end");
    oylng_tree_free(t);
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && e->type == OYLNG_EVT_STREAM_END, "the stream's end");
    t = table_of(p);
    CHECK(keys_are(t, "a m b base mm"), "and after it");
    oylng_tree_free(t);
    oylng_parser_free(p);

    /* a second --- & adds entries and replaces them, each in its place;
     * ... takes the table away; a stream of tables alone has one too */
    s = "%YAML 2.0\n--- &\na: 1\nb: 2\n---\nx\n--- &\na: 3\nc: 4\n---\ny\n...\n---\nz\n...\n--- &\nq: '!str'\nr: !str\n";
    p = oylng_parser_new(s, strlen(s));
    const char *want[] = { "a b", "a b c", "" };
    int good = 0, docs = 0;
    while (oylng_parse_tree(p, &t) == OYLNG_OK && t) {
        oylng_tree_free(t);
        t = table_of(p);
        good += docs < 3 && (*want[docs] ? keys_are(t, want[docs]) : !t);
        if (docs == 1) good += !strcmp(txt(t, at(t, oylng_tree_root(t), "a")), "3");
        oylng_tree_free(t);
        docs++;
    }
    CHECK(docs == 3 && good == 4 && !oylng_parser_error(p), "tables in turn: %d of 4", good);
    t = table_of(p);
    r = oylng_tree_root(t);
    CHECK(keys_are(t, "q r") && !oylng_node_plain(t, at(t, r, "q")) && oylng_node_plain(t, at(t, r, "r")) &&
          !strcmp(txt(t, at(t, r, "q")), "!str"), "the last table, and how its scalars were written");
    oylng_tree_free(t);
    oylng_parser_free(p);

    /* many entries, found by the index; texts that outlive the parser;
     * a chain of references as large as written */
    char big[4096];
    size_t o = (size_t)snprintf(big, sizeof big, "%%YAML 2.0\n--- &\n");
    for (int i = 0; i < 20; i++) o += (size_t)snprintf(big + o, sizeof big - o, "e%d: \"\\u00e9%d\"\n", i, i);
    o += (size_t)snprintf(big + o, sizeof big - o, "l0: [x, x, x, x, x, x, x, x, x, x]\n");
    for (int i = 1; i < 7; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "l%d: [*l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d, *l%d]\n",
                              i, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1);
    snprintf(big + o, sizeof big - o, "---\n*l6\n");
    p = oylng_parser_new(big, strlen(big));
    oylng_parser_set_limit(p, UINT64_MAX);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "the document");
    oylng_tree_free(t);
    t = table_of(p);
    oylng_parser_free(p);
    r = oylng_tree_root(t);
    CHECK(t && oylng_node_count(t, r) == 27 && !strcmp(txt(t, at(t, r, "e17")), "\xc3\xa9" "17") &&
          at(t, r, "e20") == OYLNG_NO_NODE, "20 strings found by the index, the parser gone");
    CHECK(oylng_tree_nodes(t) == 1 + 27 + 20 + 11 + 6, "the table as written: %zu nodes", oylng_tree_nodes(t));
    oylng_tree_free(t);

    /* an error is given back; nothing else changes the parser */
    s = "%YAML 2.0\n--- &\na: 1\na: 2\n---\nx\n";
    p = oylng_parser_new(s, strlen(s));
    CHECK(oylng_parse_tree(p, &t) == OYLNG_ERR_PARSE && !t, "a key twice in the table");
    t = (oylng_tree *)1;
    CHECK(oylng_parser_table(p, &t) == OYLNG_ERR_PARSE && !t, "the parser's error, and no table");
    oylng_parser_free(p);
    s = "%YAML 2.0\n--- &\na: 1\n---\n*a\n---\n[*a]\n";
    p = oylng_parser_new(s, strlen(s));
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK && oylng_parse_next(p, &e) == OYLNG_OK && oylng_parse_next(p, &e) == OYLNG_OK &&
          e->type == OYLNG_EVT_SCALAR, "into the first document");
    t = table_of(p);
    oylng_tree_free(t);
    t = table_of(p);
    oylng_tree_free(t);
    int n = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) n++;
    CHECK(n == 6 && !oylng_parser_error(p), "the rest of the stream, as it would be: %d events", n);
    oylng_parser_free(p);
    CHECK(oylng_parser_table(NULL, &t) == OYLNG_ERR_INPUT && !t && oylng_parser_table(p = oylng_parser_new("", 0), NULL) == OYLNG_ERR_INPUT,
          "NULLs");
    oylng_parser_free(p);
}

/* A stream read as kept trees: OYLNG_OK or the error, and the first
 * document's tree in *first if asked for */
static oylng_status kept(const char *s, uint64_t limit, bool together, oylng_tree **first) {
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    if (limit) oylng_parser_set_limit(p, limit);
    oylng_parser_set_together(p, together);
    oylng_parser_set_keep(p, true);
    if (first) *first = NULL;
    oylng_tree *t;
    oylng_status st;
    while ((st = oylng_parse_tree(p, &t)) == OYLNG_OK && t) {
        if (first && !*first) *first = t;
        else oylng_tree_free(t);
    }
    oylng_parser_free(p);
    return st;
}

static oylng_status together_events(const char *s, uint64_t limit) {
    oylng_parser *p = oylng_parser_new(s, strlen(s));
    oylng_parser_set_limit(p, limit);
    oylng_parser_set_together(p, true);
    oylng_status st = read_all(p, NULL);
    oylng_parser_free(p);
    return st;
}

static bool text_is(const oylng_tree *t, oylng_node n, const char *s) {
    oylng_str x = oylng_node_text(t, n);
    return x.data && x.len == strlen(s) && memcmp(x.data, s, x.len) == 0;
}

static void test_keep(void) {
    /* set before the first read, or not at all; events then refused, and
       the parser still reads trees */
    const char *one = "%YAML 2.0\n---\na: 1\n";
    oylng_parser *p = oylng_parser_new(one, strlen(one));
    const oylng_event *e;
    CHECK(oylng_parse_next(p, &e) == OYLNG_OK, "stream start");
    oylng_parser_set_keep(p, true);
    oylng_parser_set_together(p, true);
    CHECK(read_all(p, NULL) == OYLNG_OK, "set too late, keep changes nothing");
    oylng_parser_free(p);
    p = oylng_parser_new(one, strlen(one));
    oylng_parser_set_keep(p, true);
    CHECK(oylng_parse_next(p, &e) == OYLNG_ERR_INPUT && !e, "no events with references kept");
    oylng_tree *t;
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t && text_is(t, at(t, oylng_tree_root(t), "a"), "1"),
          "a tree after that");
    oylng_emitter *em = oylng_emitter_new();
    oylng_emit_stream_start(em);
    CHECK(t && oylng_emit_tree(em, t) == OYLNG_ERR_INPUT, "a kept tree isn't written");
    oylng_emitter_free(em);
    oylng_tree_free(t);
    oylng_parser_free(p);
    p = oylng_parser_new(one, strlen(one));
    oylng_parser_set_keep(p, true);
    oylng_typed *y;
    CHECK(oylng_parse_typed(p, NULL, &y) == OYLNG_ERR_INPUT && !y, "nor typed, yet");
    oylng_parser_free(p);

    /* a chain doubling 62 times through -<< reads as 2^63 items, and as a
       tree of 4 nodes a link */
    char *chain = malloc(8192);
    strcpy(chain, "%YAML 2.0\n--- &\na0: [x, x]\n");
    for (int i = 1; i < 63; i++)
        sprintf(chain + strlen(chain), "a%d:\n  -<< *a%d\n  -<< *a%d\n", i, i - 1, i - 1);
    strcat(chain, "---\nb: *a62\n");
    CHECK(kept(chain, 0, false, &t) == OYLNG_OK && t, "the doubling chain, kept");
    if (t) {
        oylng_node s = at(t, oylng_tree_root(t), "b"), sp = oylng_node_item(t, s, 0);
        CHECK(oylng_node_kind(t, s) == OYLNG_NODE_SEQUENCE && oylng_node_count(t, s) == 2 &&
              oylng_node_kind(t, sp) == OYLNG_NODE_SPLICE && oylng_node_count(t, sp) == 0,
              "a62 is two splices");
        oylng_node a61 = oylng_node_target(t, sp);
        CHECK(oylng_node_kind(t, a61) == OYLNG_NODE_SEQUENCE && oylng_node_target(t, oylng_node_item(t, s, 1)) == a61,
              "both naming a61, shared");
        CHECK(oylng_node_mark(t, sp).line == 5 + 3 * 61, "where the -<< is written: line %zu",
              oylng_node_mark(t, sp).line);
        CHECK(oylng_tree_nodes(t) < 4 * 64, "a small tree: %zu nodes", oylng_tree_nodes(t));
        CHECK(oylng_node_target(t, s) == OYLNG_NO_NODE && oylng_node_merges(t, s) == 0 &&
              oylng_node_text(t, sp).data == NULL, "a sequence names nothing");
        oylng_tree_free(t);
    }
    free(chain);

    /* held alone: section 10's first example is held 29 */
    const char *ex = "%YAML 2.0\n--- &\na: [x, yy]\nm: {k: v}\n---\nb: *a\nc:\n  -<< *a\n  - z\nd:\n  <<: *m\n";
    CHECK(kept(ex, 29, false, NULL) == OYLNG_OK, "29 allows it, kept");
    CHECK(kept(ex, 28, false, NULL) == OYLNG_ERR_LIMIT, "28 refuses it, kept");
    CHECK(parse(ex, 29, NULL) == OYLNG_ERR_LIMIT, "and 29 refuses it in full");

    /* <<: lines as written, before the pairs; the pairs only those written */
    const char *mg = "%YAML 2.0\n--- &\nbase: {x: 1, y: 2}\na: {p: 1, q: 2}\nb: {q: 3, r: 4}\n---\n"
                     "m:\n  <<: *base\n  name: a\n  y: 3\nn:\n  <<: *a\n  <<: *b\n";
    CHECK(kept(mg, 0, false, &t) == OYLNG_OK && t, "merges, kept");
    if (t) {
        oylng_node r = oylng_tree_root(t), m = at(t, r, "m"), n = at(t, r, "n");
        oylng_node g = oylng_node_merge(t, m, 0), base = oylng_node_target(t, g);
        CHECK(oylng_node_merges(t, m) == 1 && oylng_node_kind(t, g) == OYLNG_NODE_MERGE &&
              oylng_node_merge(t, m, 1) == OYLNG_NO_NODE, "m has one <<: line");
        CHECK(oylng_node_kind(t, base) == OYLNG_NODE_MAPPING && text_is(t, at(t, base, "x"), "1") &&
              oylng_node_mark(t, base).line == 3, "naming base, where the table has it");
        CHECK(oylng_node_mark(t, g).line == 8 && oylng_node_mark(t, g).col == 3, "the line where it is written");
        CHECK(oylng_node_count(t, m) == 2 && text_is(t, oylng_node_key(t, m, 0), "name") &&
              text_is(t, at(t, m, "y"), "3") && at(t, m, "x") == OYLNG_NO_NODE, "m's written pairs alone");
        CHECK(oylng_node_merges(t, n) == 2 && oylng_node_count(t, n) == 0 &&
              text_is(t, at(t, oylng_node_target(t, oylng_node_merge(t, n, 1)), "r"), "4"),
              "n has two <<: lines, in order, and no pairs");
        CHECK(oylng_node_merges(t, at(t, base, "x")) == 0 && oylng_node_merge(t, r, 0) == OYLNG_NO_NODE,
              "nothing else has <<: lines");
        oylng_tree_free(t);
    }

    /* a path through kept <<: lines: the written key first, then the last
       line's mapping first, down to the end; as in full */
    const char *pt = "%YAML 2.0\n--- &\na: {p: 1}\nb:\n  <<: *a\n  q: 2\nc:\n  <<: *b\ne: {p: 9}\n"
                     "d: {<<: *a, <<: *e}\nf: {<<: *e, p: 7}\ns: [x]\nu:\n  -<< *s\n  - y\n---\n"
                     "x: *c:p\ny: *c:q\nz: *u\nv: *d:p\nw: *f:p\n";
    const char *pv[][2] = { { "x", "1" }, { "y", "2" }, { "v", "9" }, { "w", "7" } };
    for (int full = 0; full < 2; full++) {
        if (full) {
            p = oylng_parser_new(pt, strlen(pt));
            CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "the paths, in full");
            oylng_parser_free(p);
        } else CHECK(kept(pt, 0, false, &t) == OYLNG_OK && t, "the paths, kept");
        if (!t) continue;
        oylng_node r = oylng_tree_root(t);
        for (int i = 0; i < 4; i++)
            CHECK(text_is(t, at(t, r, pv[i][0]), pv[i][1]), "%s is %s (%s)", pv[i][0], pv[i][1], full ? "full" : "kept");
        oylng_node z = at(t, r, "z");
        if (full) CHECK(oylng_node_count(t, z) == 2 && text_is(t, oylng_node_item(t, z, 0), "x"), "z spread");
        else
            CHECK(oylng_node_count(t, z) == 2 && oylng_node_kind(t, oylng_node_item(t, z, 0)) == OYLNG_NODE_SPLICE &&
                  text_is(t, oylng_node_item(t, oylng_node_target(t, oylng_node_item(t, z, 0)), 0), "x") &&
                  text_is(t, oylng_node_item(t, z, 1), "y"), "z kept");
        oylng_tree_free(t);
    }
    const char *bad[] = {
        "%YAML 2.0\n--- &\na: {p: 1}\nb: {<<: *a}\n---\nx: *b:nope\n",
        "%YAML 2.0\n--- &\na: {p: 1}\n---\nm:\n  <<: *a\n  k: 1\n  k: 2\n",
        "%YAML 2.0\n--- &\na: {p: 1}\nm:\n  <<: *a\n  k: 1\n  k: 2\n",
        "%YAML 2.0\n--- &\na: {p: 1}\n---\nm:\n  k: 1\n  <<: *a\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        oylng_mark m1, m2 = { 0 };
        oylng_status s1 = parse(bad[i], 0, &m1);
        p = oylng_parser_new(bad[i], strlen(bad[i]));
        oylng_parser_set_keep(p, true);
        oylng_status s2;
        while ((s2 = oylng_parse_tree(p, &t)) == OYLNG_OK && t) oylng_tree_free(t);
        m2 = oylng_parser_error_mark(p);
        oylng_parser_free(p);
        CHECK(s1 == OYLNG_ERR_PARSE && s2 == s1 && m1.offset == m2.offset, "refused alike, kept or not: %zu", i);
    }
    CHECK(kept("%YAML 2.0\n--- &\na: {p: 1}\n---\nm: {<<: *a, p: 5}\n", 0, false, NULL) == OYLNG_OK,
          "a written key may still be a merged one");

    /* mappings each merging the last twice: a lookup looks in each once,
       not 2^30 times */
    char *dag = malloc(4096);
    strcpy(dag, "%YAML 2.0\n--- &\nm0: {k: v}\n");
    for (int i = 1; i <= 30; i++) sprintf(dag + strlen(dag), "m%d: {<<: *m%d, <<: *m%d}\n", i, i - 1, i - 1);
    strcat(dag, "---\na: *m30:k\n");
    CHECK(kept(dag, 0, false, &t) == OYLNG_OK && t && text_is(t, at(t, oylng_tree_root(t), "a"), "v"),
          "found through 30 levels of merges");
    oylng_tree_free(t);
    strcat(dag, "b: *m30:nope\n");
    CHECK(kept(dag, 0, false, NULL) == OYLNG_ERR_PARSE, "and not found, soon");

    /* past the limit, paths find what they find within it: with the
       mappings' keys gathered within held, and, a's full past held too,
       looking through them */
    for (int deep = 0; deep < 2; deep++) {
        char *pd = malloc(8192);
        strcpy(pd, "%YAML 2.0\n--- &\nm0: {k: v}\n");
        for (int i = 1; i <= 30; i++) sprintf(pd + strlen(pd), "m%d: {<<: *m%d, <<: *m%d}\n", i, i - 1, i - 1);
        strcat(pd, deep ? "a: {<<: *m30, p: 1}\n" : "a: {p: 1}\n");
        strcat(pd, "b:\n  <<: *a\n  q: 2\nc:\n  <<: *b\ne: {p: 9}\nd: {<<: *a, <<: *e}\nf: {<<: *e, p: 7}\n---\n"
                   "x: *c:p\ny: *c:q\nv: *d:p\nw: *f:p\n");
        if (deep) strcat(pd, "k: *c:k\n");
        CHECK(kept(pd, 0, false, &t) == OYLNG_OK && t, "the paths past the limit (%s)", deep ? "looked through" : "gathered");
        if (t) {
            oylng_node r = oylng_tree_root(t);
            for (int i = 0; i < 4; i++)
                CHECK(text_is(t, at(t, r, pv[i][0]), pv[i][1]), "%s is %s (%s)", pv[i][0], pv[i][1],
                      deep ? "looked through" : "gathered");
            if (deep) CHECK(text_is(t, at(t, r, "k"), "v"), "k from m0, below the chain");
            oylng_tree_free(t);
        }
        free(pd);
    }

    /* past the limit, the doubling mappings' keys are gathered within
       held, for as many paths as are written */
    char *pl = malloc(1 << 16);
    strcpy(pl, dag);
    *strstr(pl, "---\n") = 0;
    strcat(pl, "---\n");
    for (int i = 0; i < 100; i++) sprintf(pl + strlen(pl), "a%d: *m30:k\n", i);
    CHECK(kept(pl, 1000, false, NULL) == OYLNG_OK, "100 paths through 31 doubling mappings, gathered, not 3,100 steps looked through");
    /* 100 mappings each naming a, of 200 keys, on 19 lines, and b on the
       first: plans and walks for them take more steps than the table
       holds, so after some 17 are walked, paths look through their lines,
       20 steps for p, in b; the stream may take as many steps as the
       limit: 300 paths, not 1000 */
    for (int n = 300; n <= 1000; n += 700) {
        strcpy(pl, "%YAML 2.0\n--- &\na: {");
        for (int i = 0; i < 200; i++) sprintf(pl + strlen(pl), "%sk%d: 1", i ? ", " : "", i);
        strcat(pl, "}\nb: {p: 2}\n");
        for (int j = 0; j < 100; j++) {
            sprintf(pl + strlen(pl), "t%d:\n  <<: *b\n", j);
            for (int i = 0; i < 19; i++) strcat(pl, "  <<: *a\n");
        }
        strcat(pl, "---\n");
        for (int i = 0; i < n; i++) sprintf(pl + strlen(pl), "- *t%d:p\n", i % 100);
        oylng_parser *lp = oylng_parser_new(pl, strlen(pl));
        oylng_parser_set_limit(lp, 10000);
        oylng_parser_set_keep(lp, true);
        oylng_status ls;
        oylng_tree *first = NULL;
        while ((ls = oylng_parse_tree(lp, &t)) == OYLNG_OK && t) {
            if (!first) first = t;
            else oylng_tree_free(t);
        }
        const char *le = oylng_parser_error(lp);
        if (n == 300) {
            CHECK(ls == OYLNG_OK, "300 paths looked through: %s", le ? le : "");
            bool all = first != NULL;
            for (uint32_t i = 0; all && i < 300; i++) all = text_is(first, oylng_node_item(first, oylng_tree_root(first), i), "2");
            CHECK(all, "each p from b");
        } else
            CHECK(ls == OYLNG_ERR_LIMIT && le && strstr(le, "looking through <<: lines past the size limit of 10000"),
                  "1000 paths: %s", le ? le : "");
        if (first) oylng_tree_free(first);
        oylng_parser_free(lp);
    }
    free(pl);
    free(dag);

    /* the table, kept: its <<: lines too */
    p = oylng_parser_new(mg, strlen(mg));
    oylng_parser_set_keep(p, true);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "a kept document, for its table");
    oylng_tree_free(t);
    oylng_tree *tb;
    CHECK(oylng_parser_table(p, &tb) == OYLNG_OK && tb && oylng_node_count(tb, oylng_tree_root(tb)) == 3,
          "its table");
    oylng_tree_free(tb);
    oylng_parser_free(p);
    p = oylng_parser_new(pt, strlen(pt));
    oylng_parser_set_keep(p, true);
    CHECK(oylng_parse_tree(p, &t) == OYLNG_OK && t, "another");
    oylng_tree_free(t);
    CHECK(oylng_parser_table(p, &tb) == OYLNG_OK && tb, "its table");
    if (tb) {
        oylng_node c = at(tb, oylng_tree_root(tb), "c");
        CHECK(oylng_node_merges(tb, c) == 1 && oylng_node_count(tb, c) == 0 &&
              oylng_node_merges(tb, oylng_node_target(tb, oylng_node_merge(tb, c, 0))) == 1,
              "c's <<: line, naming b's");
    }
    oylng_tree_free(tb);
    oylng_parser_free(p);

    /* together, the documents' sizes add up: 5 each */
    const char *three = "%YAML 2.0\n---\na: 1\n---\nb: 2\n---\nc: 3\n";
    CHECK(together_events(three, 15) == OYLNG_OK && together_events(three, 14) == OYLNG_ERR_LIMIT,
          "15 allows three together, 14 doesn't");
    CHECK(parse(three, 5, NULL) == OYLNG_OK, "one at a time, 5 does");
    CHECK(kept(three, 15, true, NULL) == OYLNG_OK && kept(three, 14, true, NULL) == OYLNG_ERR_LIMIT,
          "and as kept trees");
    /* ... takes the table off, unless references are kept too: the first
       table is 8, the documents 4 held and 9 and 6 full, the second table 5 */
    const char *dots = "%YAML 2.0\n--- &\na: [x, yy]\n---\nb: *a\n...\n--- &\nc: [z]\n---\nd: *c\n";
    CHECK(together_events(dots, 20) == OYLNG_OK && together_events(dots, 19) == OYLNG_ERR_LIMIT,
          "together, full is 20 at most");
    CHECK(kept(dots, 21, true, NULL) == OYLNG_OK && kept(dots, 20, true, NULL) == OYLNG_ERR_LIMIT,
          "together and kept, held is 21");
    CHECK(kept(dots, 12, false, NULL) == OYLNG_OK && kept(dots, 11, false, NULL) == OYLNG_ERR_LIMIT,
          "kept alone, 12");
}

int main(void) {
    test_error_marks();
    test_stop();
    test_limits();
    test_levels();
    test_api_edges();
    test_big_maps();
    test_stream();
    test_fast();
    test_tree();
    test_table();
    test_keep();
    printf("api: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
