/*
 * test_emit.c — the emitter: what it writes, what it refuses, and that
 * every string it is given reads back as itself
 *
 *     test_emit [LENGTH]
 *
 * The last test writes every string of up to LENGTH characters (3 by
 * default) from an alphabet of the characters that matter to YAML, and
 * random longer ones, in each place a scalar can go, and reads each back.
 */

#include "oylng/oylng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static oylng_str S(const char *s) { return (oylng_str){ s, strlen(s) }; }

/* The stream read and written again; "ERR: message" if either refuses */
static const char *rewrite(const char *in, bool flow, unsigned indent) {
    static char out[1 << 16];
    oylng_parser *p = oylng_parser_new(in, strlen(in));
    oylng_emitter *em = oylng_emitter_new();
    oylng_emitter_set_flow(em, flow);
    oylng_emitter_set_indent(em, indent);
    const oylng_event *e;
    out[0] = 0;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) {
            snprintf(out, sizeof out, "ERR: %s", oylng_parser_error(p));
            break;
        }
        if (oylng_emit(em, e) != OYLNG_OK) {
            snprintf(out, sizeof out, "ERR: %s", oylng_emitter_error(em));
            break;
        }
        if (e->type == OYLNG_EVT_STREAM_END) {
            oylng_str o = oylng_emitter_output(em);
            snprintf(out, sizeof out, "%.*s", (int)o.len, o.data);
            break;
        }
    }
    oylng_emitter_free(em);
    oylng_parser_free(p);
    return out;
}

#define SAME(in, want) do { const char *got_ = rewrite(in, false, 2); \
    CHECK(!strcmp(got_, want), "rewrite of:\n%s\ngot:\n%s\nwant:\n%s", in, got_, want); } while (0)
#define SAME_FLOW(in, want) do { const char *got_ = rewrite(in, true, 2); \
    CHECK(!strcmp(got_, want), "flow rewrite of:\n%s\ngot:\n%s\nwant:\n%s", in, got_, want); } while (0)

static void test_shapes(void) {
    SAME("%YAML 2.0\n", "%YAML 2.0\n");
    SAME("%YAML 2.0\n---\n", "%YAML 2.0\n--- \"\"\n");
    SAME("%YAML 2.0\n--- a\n--- !job b\n", "%YAML 2.0\n--- a\n--- !job b\n");
    SAME("%YAML 2.0\n---\na: 1\nb:\n  c: [x, y]\n  d: {}\n  e: []\n",
         "%YAML 2.0\n---\na: 1\nb:\n  c:\n    - x\n    - y\n  d: {}\n  e: []\n");
    SAME("%YAML 2.0\n---\n- a: 1\n  b: 2\n- - x\n  - - y\n- {}\n- \n",
         "%YAML 2.0\n---\n- a: 1\n  b: 2\n- - x\n  - - y\n- {}\n- \"\"\n");
    SAME("%YAML 2.0\n--- !n\n- x\n", "%YAML 2.0\n--- !n\n- x\n");
    SAME("%YAML 2.0\n--- []\n--- {}\n", "%YAML 2.0\n--- []\n--- {}\n");
    /* references and merges in full, the table gone */
    SAME("%YAML 2.0\n--- &\nd: {pool: 5}\ns: [a, b]\n---\nx:\n  <<: *d\n  k: v\ny: *s\nz:\n  -<< *s\n  - c\n",
         "%YAML 2.0\n---\nx:\n  pool: 5\n  k: v\ny:\n  - a\n  - b\nz:\n  - a\n  - b\n  - c\n");
    SAME_FLOW("%YAML 2.0\n---\na: 1\nb:\n  c: [x, y]\n  d: {}\n",
              "%YAML 2.0\n--- {a: 1, b: {c: [x, y], d: {}}}\n");
    SAME_FLOW("%YAML 2.0\n--- !n\n- x\n- [y, {z: \"\"}]\n", "%YAML 2.0\n--- !n [x, [y, {z: \"\"}]]\n");

    /* plain where it reads back the same */
    SAME("%YAML 2.0\n---\n- hello world\n- a:b\n- x#y\n- -x\n- 1.5\n- http://x.y/z?q=1#f\n- ...\n- \"%x\"\n"
         "- '@h'\n- \"`c`\"\n- 'a, b'\n- \"[x\"\n- \"x]\"\n- <<\n- 'é ü 端口'\n",
         "%YAML 2.0\n---\n- hello world\n- a:b\n- x#y\n- -x\n- 1.5\n- http://x.y/z?q=1#f\n- ...\n- %x\n"
         "- @h\n- `c`\n- a, b\n- '[x'\n- x]\n- <<\n- é ü 端口\n");
    /* quoted where it wouldn't */
    SAME("%YAML 2.0\n---\n- 'a: b'\n- 'x #y'\n- ' x'\n- 'x '\n- '-'\n- '- x'\n- '? x'\n- 'x:'\n- '#'\n"
         "- '&a'\n- '*a'\n- '!t'\n- '|'\n- '>'\n- \"'\"\n- '\"'\n- \"it's\"\n- '-<< *a'\n- \"\\t\"\n",
         "%YAML 2.0\n---\n- 'a: b'\n- 'x #y'\n- ' x'\n- 'x '\n- '-'\n- '- x'\n- '? x'\n- 'x:'\n- '#'\n"
         "- '&a'\n- '*a'\n- '!t'\n- '|'\n- '>'\n- \"'\"\n- '\"'\n- it's\n- '-<< *a'\n- \"\\t\"\n");
    /* a tab: \t on one line, as it is in a block scalar */
    SAME("%YAML 2.0\n---\n- \"a\\tb\"\n- \"\\tx\\n\\ty\\n\"\n- \"'\\t\"\n",
         "%YAML 2.0\n---\n- \"a\\tb\"\n- |\n  \tx\n  \ty\n- \"'\\t\"\n");
    /* in a flow collection, flow indicators quoted too */
    SAME_FLOW("%YAML 2.0\n---\n- 'a, b'\n- x]\n- 'a:[b'\n- a:b\n", "%YAML 2.0\n--- ['a, b', 'x]', 'a:[b', a:b]\n");
    /* keys: <<, %, --- and ... at column 0, and the empty key */
    SAME("%YAML 2.0\n---\n\"<<\": 1\n'%k': 2\n'---': 3\n'...': 4\n\"\": 5\nx:\n  '%k': 6\n  ---: 7\n",
         "%YAML 2.0\n---\n'<<': 1\n'%k': 2\n'---': 3\n'...': 4\n\"\": 5\nx:\n  %k: 6\n  ---: 7\n");
    /* escapes: controls, CR, and characters only double quotes hold */
    SAME("%YAML 2.0\n---\n- \"a\\u0000b\"\n- \"\\r\"\n- \"\\u0085\"\n- \"\\u2028\"\n- \"\\ufeff\"\n- \"\\\\\"\n- \"\\u007f\"\n",
         "%YAML 2.0\n---\n- \"a\\u0000b\"\n- \"\\r\"\n- \"\\u0085\"\n- \"\\u2028\"\n- \"\\uFEFF\"\n- \\\n- \"\\u007F\"\n");

    /* lines: a literal block scalar where it reads back exactly */
    SAME("%YAML 2.0\n---\na: |\n  one\n    two\n\n  three\nb: |-\n  x\n  y\nc: \"\\n\\nx\\n\"\n",
         "%YAML 2.0\n---\na: |\n  one\n    two\n\n  three\nb: |-\n  x\n  y\nc: |\n\n\n  x\n");
    SAME("%YAML 2.0\n---\n- \"x\\n\\n\"\n- \"\\n\"\n- \" x\\ny\"\n- \"x\\r\\ny\"\n- \"\\n  \\nx\"\n",
         "%YAML 2.0\n---\n- \"x\\n\\n\"\n- \"\\n\"\n- \" x\\ny\"\n- \"x\\r\\ny\"\n- \"\\n  \\nx\"\n");
    SAME("%YAML 2.0\n--- \"a\\nb\"\n", "%YAML 2.0\n--- |-\n  a\n  b\n");
    SAME("%YAML 2.0\n---\n- - \"a\\nb\\n\"\n- k: \"a\\n\"\n", "%YAML 2.0\n---\n- - |\n    a\n    b\n- k: |\n    a\n");
    SAME_FLOW("%YAML 2.0\n---\n- \"a\\nb\"\n", "%YAML 2.0\n--- [\"a\\nb\"]\n");
    /* keys are one line */
    SAME("%YAML 2.0\n---\n\"a\\nb\": x\n", "%YAML 2.0\n---\n\"a\\nb\": x\n");

    /* indent */
    const char *in = "%YAML 2.0\n---\na:\n  b: [x]\n  c: \"l\\n\"\n";
    const char *got = rewrite(in, false, 4);
    CHECK(!strcmp(got, "%YAML 2.0\n---\na:\n    b:\n        - x\n    c: |\n        l\n"), "indent 4: %s", got);
    got = rewrite(in, false, 1);
    CHECK(!strcmp(got, "%YAML 2.0\n---\na:\n b:\n  - x\n c: |\n  l\n"), "indent 1: %s", got);
}

/* Each call must give st, and then the emitter is failed */
static oylng_emitter *started(void) {
    oylng_emitter *e = oylng_emitter_new();
    oylng_emit_stream_start(e);
    return e;
}

static void test_errors(void) {
    oylng_emitter *e;
    const oylng_str none = { NULL, 0 };

    e = oylng_emitter_new();
    CHECK(oylng_emit_scalar(e, S("x")) == OYLNG_ERR_EMIT, "a scalar before the stream");
    CHECK(oylng_emitter_error(e) && strstr(oylng_emitter_error(e), "hasn't started"), "%s", oylng_emitter_error(e));
    CHECK(oylng_emit_stream_start(e) == OYLNG_ERR_EMIT, "an error is final");
    oylng_emitter_free(e);

    e = started();
    CHECK(oylng_emitter_error(e) == NULL, "no error yet");
    CHECK(oylng_emit_scalar(e, S("x")) == OYLNG_ERR_EMIT, "a scalar between documents");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    CHECK(oylng_emit_doc_end(e) == OYLNG_ERR_EMIT, "a document with no value");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_scalar(e, S("a"));
    CHECK(oylng_emit_scalar(e, S("b")) == OYLNG_ERR_EMIT, "two roots");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_map_start(e, false);
    oylng_emit_scalar(e, S("k"));
    CHECK(oylng_emit_map_end(e) == OYLNG_ERR_EMIT, "a key with no value");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_map_start(e, false);
    CHECK(oylng_emit_seq_start(e, false) == OYLNG_ERR_EMIT, "a collection as a key");
    CHECK(strstr(oylng_emitter_error(e), "can't be a key") != NULL, "%s", oylng_emitter_error(e));
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_map_start(e, false);
    CHECK(oylng_emit_seq_end(e) == OYLNG_ERR_EMIT, "a sequence's end in a mapping");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_seq_start(e, false);
    CHECK(oylng_emit_doc_end(e) == OYLNG_ERR_EMIT, "a document's end with a collection open");
    oylng_emitter_free(e);

    e = started();
    oylng_emit_doc_start(e, none);
    CHECK(oylng_emit_stream_end(e) == OYLNG_ERR_EMIT, "the stream's end in a document");
    oylng_emitter_free(e);

    /* names */
    const char *bad_names[] = { "a:b", "a b", "%41", "a\xC3", "x\xE2\x80\xA8",
                                "a#", "a#b#c", "a#b-c", "a#\xC3\xA9", "#b", "a#b c" };
    for (size_t i = 0; i < sizeof bad_names / sizeof *bad_names; i++) {
        e = started();
        CHECK(oylng_emit_doc_start(e, S(bad_names[i])) == OYLNG_ERR_EMIT, "the name %s", bad_names[i]);
        oylng_emitter_free(e);
    }
    e = started();
    CHECK(oylng_emit_doc_start(e, (oylng_str){ "", 0 }) == OYLNG_ERR_EMIT, "an empty name");
    oylng_emitter_free(e);
    e = started();
    CHECK(oylng_emit_doc_start(e, S("github.com/a/b-1_\xE9\x85\x8D\xE7\xBD\xAE")) == OYLNG_OK, "a good name");
    oylng_emitter_free(e);
    const char *frag_names[] = { "a#b", "a.yes#sha256=0f9A", "\xE9\x85\x8D#x=" };
    for (size_t i = 0; i < sizeof frag_names / sizeof *frag_names; i++) {
        e = started();
        CHECK(oylng_emit_doc_start(e, S(frag_names[i])) == OYLNG_OK, "the name %s, with a fragment", frag_names[i]);
        oylng_emitter_free(e);
    }

    /* invalid UTF-8 */
    const char *bad_text[] = { "\xC3", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "a\xFF" };
    for (size_t i = 0; i < sizeof bad_text / sizeof *bad_text; i++) {
        e = started();
        oylng_emit_doc_start(e, none);
        CHECK(oylng_emit_scalar(e, S(bad_text[i])) == OYLNG_ERR_EMIT, "invalid UTF-8 %zu", i);
        oylng_emitter_free(e);
    }

    /* repeated keys, few and many */
    for (int many = 0; many < 2; many++) {
        e = started();
        oylng_emit_doc_start(e, none);
        oylng_emit_map_start(e, many);
        int nk = many ? 300 : 5;
        char k[16];
        oylng_status st = OYLNG_OK;
        for (int i = 0; i < nk && st == OYLNG_OK; i++) {
            snprintf(k, sizeof k, "k%d", i);
            st = oylng_emit_scalar(e, S(k));
            if (st == OYLNG_OK) st = oylng_emit_scalar(e, S("v"));
        }
        CHECK(st == OYLNG_OK, "distinct keys: %s", oylng_emitter_error(e));
        snprintf(k, sizeof k, "k%d", nk - 2);
        CHECK(oylng_emit_scalar(e, S(k)) == OYLNG_ERR_EMIT, "a repeated key among %d", nk);
        CHECK(strstr(oylng_emitter_error(e), k) != NULL, "%s", oylng_emitter_error(e));
        oylng_emitter_free(e);
    }
    /* the same key in two mappings, and after a nested one closes */
    e = started();
    oylng_emit_doc_start(e, none);
    oylng_emit_map_start(e, false);
    for (int i = 0; i < 20; i++) {
        char k[16];
        snprintf(k, sizeof k, "o%d", i);
        oylng_emit_scalar(e, S(k));
        oylng_emit_map_start(e, false);
        oylng_emit_scalar(e, S(k));
        oylng_emit_scalar(e, S("x"));
        oylng_emit_map_end(e);
    }
    CHECK(oylng_emit_map_end(e) == OYLNG_OK, "keys repeated only across mappings: %s", oylng_emitter_error(e));
    oylng_emitter_free(e);

    /* levels */
    e = started();
    oylng_emit_doc_start(e, none);
    oylng_status st = OYLNG_OK;
    int depth = 0;
    while (st == OYLNG_OK && depth <= OYLNG_MAX_LEVELS) { st = oylng_emit_seq_start(e, false); depth++; }
    CHECK(st == OYLNG_ERR_EMIT && depth == OYLNG_MAX_LEVELS + 1, "levels: %d", depth);
    oylng_emitter_free(e);

    /* arguments */
    CHECK(oylng_emit_scalar(NULL, S("x")) == OYLNG_ERR_INPUT, "NULL emitter");
    e = started();
    oylng_emit_doc_start(e, none);
    CHECK(oylng_emit_scalar(e, (oylng_str){ NULL, 3 }) == OYLNG_ERR_INPUT, "NULL data");
    CHECK(oylng_emit_scalar(e, (oylng_str){ NULL, 0 }) == OYLNG_OK, "NULL data, empty");
    oylng_emitter_free(e);
}

/* Taken in pieces, the output is the same */
static void test_take(void) {
    oylng_emitter *a = oylng_emitter_new(), *b = oylng_emitter_new();
    char joined[8192];
    size_t jn = 0;
    oylng_emit_stream_start(a);
    oylng_emit_stream_start(b);
    for (int d = 0; d < 3; d++) {
        oylng_emit_doc_start(a, (oylng_str){ NULL, 0 });
        oylng_emit_doc_start(b, (oylng_str){ NULL, 0 });
        oylng_emit_map_start(a, false);
        oylng_emit_map_start(b, false);
        for (int i = 0; i < 30; i++) {
            char k[16];
            snprintf(k, sizeof k, "key%d", i);
            oylng_emit_scalar(a, S(k)); oylng_emit_scalar(b, S(k));
            oylng_emit_seq_start(a, false); oylng_emit_seq_start(b, false);
            oylng_emit_scalar(a, S("x")); oylng_emit_scalar(b, S("x"));
            oylng_emit_seq_end(a); oylng_emit_seq_end(b);
            oylng_str piece;
            CHECK(oylng_emitter_take(b, &piece) == OYLNG_OK, "take");
            memcpy(joined + jn, piece.data, piece.len);
            jn += piece.len;
        }
        oylng_emit_map_end(a); oylng_emit_map_end(b);
        oylng_emit_doc_end(a); oylng_emit_doc_end(b);
    }
    oylng_emit_stream_end(a); oylng_emit_stream_end(b);
    oylng_str piece;
    oylng_emitter_take(b, &piece);
    memcpy(joined + jn, piece.data, piece.len);
    jn += piece.len;
    oylng_str whole = oylng_emitter_output(a);
    CHECK(whole.len == jn && !memcmp(whole.data, joined, jn), "pieces joined: %zu against %zu", jn, whole.len);
    oylng_emitter_free(a);
    oylng_emitter_free(b);
}

/* ── Every string reads back ─────────────────────────────── */

/* The characters that matter: indicators, blanks, breaks, characters only
 * double quotes hold, and some beyond ASCII */
static const char *alphabet[] = {
    " ", "\t", "\n", "\r", "-", "?", ":", "#", ",", "[", "]", "{", "}", "&", "*", "!", "|", ">",
    "'", "\"", "%", "@", "`", "<", ".", "a", "\\", "/", "\x01", "\x7F", "\xC3\xA9",
    "\xC2\x85", "\xE2\x80\xA8", "\xEF\xBB\xBF", "\xF0\x9F\x98\x80",
};
#define NALPHA (sizeof alphabet / sizeof *alphabet)

/* Writes s in every place a scalar goes, one document each, and reads the
 * stream back: every scalar must be s */
static int places;

static bool trip(const char *s, size_t n) {
    oylng_emitter *e = oylng_emitter_new();
    oylng_str v = { s, n };
    const oylng_str none = { NULL, 0 };
    const char *k2 = n == 1 && s[0] == 'k' ? "j" : "k";   /* a second key, not s */
    oylng_emit_stream_start(e);
    /* the root */
    oylng_emit_doc_start(e, none); oylng_emit_scalar(e, v); oylng_emit_doc_end(e);
    /* a key and a value at column 0, and in a mapping inside it */
    for (int flow = 0; flow < 2; flow++) {
        oylng_emit_doc_start(e, none);
        oylng_emit_map_start(e, flow);
        oylng_emit_scalar(e, v); oylng_emit_scalar(e, v);
        oylng_emit_scalar(e, S(k2));
        oylng_emit_map_start(e, false);
        oylng_emit_scalar(e, v); oylng_emit_scalar(e, v);
        oylng_emit_map_end(e);
        oylng_emit_map_end(e);
        oylng_emit_doc_end(e);
    }
    /* items: in a sequence, a sequence in one, a mapping in one */
    for (int flow = 0; flow < 2; flow++) {
        oylng_emit_doc_start(e, none);
        oylng_emit_seq_start(e, flow);
        oylng_emit_scalar(e, v);
        oylng_emit_seq_start(e, false);
        oylng_emit_scalar(e, v); oylng_emit_scalar(e, v);
        oylng_emit_seq_end(e);
        oylng_emit_map_start(e, false);
        oylng_emit_scalar(e, v); oylng_emit_scalar(e, v);
        oylng_emit_map_end(e);
        oylng_emit_scalar(e, v);
        oylng_emit_seq_end(e);
        oylng_emit_doc_end(e);
    }
    oylng_status st = oylng_emit_stream_end(e);
    bool ok = st == OYLNG_OK;
    if (!ok) printf("FAIL: the emitter refused %.*s: %s\n", (int)n, s, oylng_emitter_error(e));
    oylng_str out = oylng_emitter_output(e);
    oylng_parser *p = ok ? oylng_parser_new(out.data, out.len) : NULL;
    const oylng_event *ev;
    int scalars = 0;
    while (ok) {
        if (oylng_parse_next(p, &ev) != OYLNG_OK) {
            oylng_mark m = oylng_parser_error_mark(p);
            printf("FAIL: refused at %zu:%zu: %s\n%.*s", m.line, m.col, oylng_parser_error(p), (int)out.len, out.data);
            ok = false;
            break;
        }
        if (ev->type == OYLNG_EVT_STREAM_END) break;
        if (ev->type != OYLNG_EVT_SCALAR) continue;
        scalars++;
        bool is_k = ev->value.len == 1 && ev->value.data[0] == k2[0];
        if (!is_k && (ev->value.len != n || memcmp(ev->value.data, s, n) != 0)) {
            printf("FAIL: read back otherwise, scalar %d:\n%.*s", scalars, (int)out.len, out.data);
            ok = false;
        }
    }
    oylng_parser_free(p);
    oylng_emitter_free(e);
    places += scalars;
    return ok;
}

static void test_strings(int maxlen) {
    char s[64];
    int bad = 0;
    size_t strings = 0;
    /* every string of up to maxlen characters */
    size_t idx[16] = { 0 };
    for (int len = 0; len <= maxlen && bad < 5; len++) {
        memset(idx, 0, sizeof idx);
        for (;;) {
            size_t n = 0;
            for (int i = 0; i < len; i++) {
                size_t m = strlen(alphabet[idx[i]]);
                memcpy(s + n, alphabet[idx[i]], m);
                n += m;
            }
            strings++;
            if (!trip(s, n) && ++bad >= 5) break;
            int i = 0;
            while (i < len && ++idx[i] == NALPHA) idx[i++] = 0;
            if (i == len) break;
        }
    }
    /* and random longer ones, the same every run */
    unsigned long long r = 88172645463325252ull;
    for (int t = 0; t < 100000 && bad < 5; t++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        int len = (int)(4 + r % 9);
        size_t n = 0;
        for (int i = 0; i < len; i++) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            /* letters more often, as text has them */
            const char *c = r % 3 == 0 ? "a" : alphabet[(r >> 8) % NALPHA];
            size_t m = strlen(c);
            memcpy(s + n, c, m);
            n += m;
        }
        strings++;
        if (!trip(s, n)) bad++;
    }
    checks++;
    if (bad) failures++;
    printf("strings: %zu, each written in every place a scalar goes: %d scalars read back\n", strings, places);
}

int main(int argc, char **argv) {
    int maxlen = argc > 1 ? atoi(argv[1]) : 3;
    test_shapes();
    test_errors();
    test_take();
    test_strings(maxlen);
    printf("emit: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
