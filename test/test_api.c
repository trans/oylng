/*
 * test_api.c — what the suite doesn't check: errors' lines and columns,
 * the offset after a stop, limits, levels, CR LF, and the API's edges
 */

#include "oylng/oylng.h"
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

    /* a document is checked whole before its first event */
    const char *bad = "%YAML 2.0\n---\na: 1\n---\nb: 1\nb: 2\n";
    p = oylng_parser_new(bad, strlen(bad));
    int n = 0;
    oylng_status st = read_all(p, &n);
    CHECK(st == OYLNG_ERR_PARSE && n == 7, "events before the bad document: %d", n);   /* +STR +DOC +MAP a 1 -MAP -DOC */
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

int main(void) {
    test_error_marks();
    test_stop();
    test_limits();
    test_levels();
    test_api_edges();
    test_big_maps();
    printf("api: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
