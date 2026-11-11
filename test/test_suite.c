/*
 * test_suite.c — runs the YAML 2.0 suite
 *
 *     test_suite DATA [-v]
 *
 * DATA is the suite written out one directory per test, by the suite's
 * bin/suite-to-data (trans/yaml2, suite/README.md): in.yaml, and test.event
 * or error, with rest, limit, keep and together where a test has them.
 * Each test's events must match test.event, a test with error must be
 * refused, and the text after a stopping ... must be rest. Tests marked
 * keep or together are for interfaces this parser doesn't offer yet, and
 * are skipped. Each test that passes is then written again by the emitter,
 * four ways, and each must read back as the same events; written again
 * from what it wrote, block style must give the same text. Each test is
 * also read as trees, which must give the same events, offsets and
 * errors, find each key's value, and write the same text as the events.
 * Exits with 1 if any test fails.
 */

#define _POSIX_C_SOURCE 200809L
#include "oylng/oylng.h"
#include "events.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passed, failed, skipped, verbose, trips, trees;

static char *slurp(const char *dir, const char *name, size_t *len) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    strbuf b = { 0 };
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sb_add(&b, chunk, n);
    fclose(f);
    if (!b.data) sb_add(&b, "", 0);
    if (len) *len = b.len;
    return b.data;
}

static int has(const char *dir, const char *name) {
    char path[4096];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

/* CR LF read as LF, as the suite compares rest */
static void unix_breaks(char *s, size_t *len) {
    size_t o = 0;
    for (size_t i = 0; i < *len; i++)
        if (!(s[i] == '\r' && i + 1 < *len && s[i + 1] == '\n')) s[o++] = s[i];
    *len = o;
    s[o] = '\0';
}

/* The events of text as test.event has them, or NULL and *err if it is
 * refused */
static char *events_of(const char *text, size_t len, const char *limit, const char **err) {
    oylng_parser *p = oylng_parser_new(text, len);
    if (limit) oylng_parser_set_limit(p, strtoull(limit, NULL, 10));
    strbuf got = { 0 };
    const oylng_event *e;
    *err = NULL;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) {
            static char msg[512];
            oylng_mark m = oylng_parser_error_mark(p);
            snprintf(msg, sizeof msg, "%zu:%zu: %s", m.line, m.col, oylng_parser_error(p));
            *err = msg;
            free(got.data);
            oylng_parser_free(p);
            return NULL;
        }
        ev_line(&got, e);
        if (e->type == OYLNG_EVT_STREAM_END) break;
    }
    oylng_parser_free(p);
    if (!got.data) sb_add(&got, "", 0);
    return got.data;
}

/* The stream read and written again, or NULL and *err */
static char *emitted(const char *text, size_t len, const char *limit, bool flow, unsigned indent,
                     size_t *olen, const char **err) {
    oylng_parser *p = oylng_parser_new(text, len);
    if (limit) oylng_parser_set_limit(p, strtoull(limit, NULL, 10));
    oylng_emitter *em = oylng_emitter_new();
    oylng_emitter_set_flow(em, flow);
    oylng_emitter_set_indent(em, indent);
    strbuf out = { 0 };
    const oylng_event *e;
    *err = NULL;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) { *err = oylng_parser_error(p); break; }
        if (oylng_emit(em, e) != OYLNG_OK) { *err = oylng_emitter_error(em); break; }
        oylng_str piece;
        oylng_emitter_take(em, &piece);
        sb_add(&out, piece.data, piece.len);
        if (e->type == OYLNG_EVT_STREAM_END) break;
    }
    if (*err) {
        static char msg[512];
        snprintf(msg, sizeof msg, "%s", *err);
        *err = msg;
    }
    oylng_emitter_free(em);
    oylng_parser_free(p);
    if (*err) { free(out.data); return NULL; }
    if (!out.data) sb_add(&out, "", 0);
    *olen = out.len;
    return out.data;
}

/* Why the stream, written again four ways, doesn't read back as events
 * want; NULL if it does */
static const char *round_trip(const char *in, size_t len, const char *limit, const char *want,
                              char *detail, size_t dn) {
    static const struct { bool flow; unsigned indent; const char *name; } ways[] = {
        { false, 2, "block" }, { true, 2, "flow" }, { false, 1, "indent 1" }, { false, 5, "indent 5" },
    };
    for (size_t w = 0; w < sizeof ways / sizeof *ways; w++) {
        size_t olen;
        const char *err;
        char *out = emitted(in, len, limit, ways[w].flow, ways[w].indent, &olen, &err);
        if (!out) {
            snprintf(detail, dn, "%s: %s", ways[w].name, err);
            return "the emitter refused it";
        }
        char *back = events_of(out, olen, limit, &err);
        const char *why = NULL;
        if (!back) {
            snprintf(detail, dn, "%s: %s\n%s", ways[w].name, err, out);
            why = "what the emitter wrote is refused";
        } else if (strcmp(back, want) != 0) {
            snprintf(detail, dn, "%s:\n%s", ways[w].name, out);
            why = "what the emitter wrote reads otherwise";
        } else if (w == 0) {
            size_t olen2;
            char *again = emitted(out, olen, limit, false, 2, &olen2, &err);
            if (!again || olen2 != olen || memcmp(again, out, olen) != 0) {
                snprintf(detail, dn, "block:\n%s", out);
                why = "written again, it changes";
            }
            free(again);
        }
        free(back);
        free(out);
        if (why) return why;
        trips++;
    }
    return NULL;
}

/* A tree's node as events, and the offsets of the nodes' starts; false if
 * a lookup of a key doesn't give its value */
static bool tree_events(const oylng_tree *t, oylng_node n, strbuf *ev, strbuf *offs) {
    oylng_kind k = oylng_node_kind(t, n);
    oylng_event e = { OYLNG_EVT_SCALAR, oylng_node_text(t, n), oylng_node_mark(t, n).offset };
    char o[32];
    snprintf(o, sizeof o, "%zu\n", e.offset);
    sb_str(offs, o);
    if (k == OYLNG_NODE_STRING) {
        ev_line(ev, &e);
        return true;
    }
    bool map = k == OYLNG_NODE_MAPPING;
    e.type = map ? OYLNG_EVT_MAP_START : OYLNG_EVT_SEQ_START;
    ev_line(ev, &e);
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++) {
        if (map) {
            oylng_node key = oylng_node_key(t, n, i), v = oylng_node_value(t, n, i);
            if (oylng_node_kind(t, key) != OYLNG_NODE_STRING) return false;
            if (oylng_node_get(t, n, oylng_node_text(t, key)) != v) return false;
            if (!tree_events(t, key, ev, offs) || !tree_events(t, v, ev, offs)) return false;
        } else if (!tree_events(t, oylng_node_item(t, n, i), ev, offs)) return false;
    }
    e.type = map ? OYLNG_EVT_MAP_END : OYLNG_EVT_SEQ_END;
    ev_line(ev, &e);
    return true;
}

/* Why the stream read as trees doesn't give the events want, the offsets
 * offs, the error the events gave (want NULL) and the text the emitter
 * writes from events; NULL if it does */
static const char *tree_check(const char *in, size_t len, const char *limit, const char *want,
                              const char *offs, const char *werr, oylng_mark wm,
                              char *detail, size_t dn) {
    oylng_parser *p = oylng_parser_new(in, len);
    if (limit) oylng_parser_set_limit(p, strtoull(limit, NULL, 10));
    oylng_emitter *em = oylng_emitter_new();
    oylng_emit_stream_start(em);
    strbuf ev = { 0 }, of = { 0 };
    sb_str(&ev, "+STR\n");
    const char *why = NULL;
    oylng_tree *t;
    oylng_status st;
    while ((st = oylng_parse_tree(p, &t)) == OYLNG_OK && t) {
        oylng_event d = { OYLNG_EVT_DOC_START, oylng_tree_name(t), 0 };
        ev_line(&ev, &d);
        if (!tree_events(t, oylng_tree_root(t), &ev, &of)) why = "a key's lookup is wrong";
        d.type = OYLNG_EVT_DOC_END;
        ev_line(&ev, &d);
        if (want && oylng_emit_tree(em, t) != OYLNG_OK) {
            snprintf(detail, dn, "%s", oylng_emitter_error(em));
            why = "the emitter refused the tree";
        }
        oylng_tree_free(t);
        if (why) break;
    }
    sb_str(&ev, "-STR\n");
    if (why) ;
    else if (!want) {
        oylng_mark m = oylng_parser_error_mark(p);
        const char *e = oylng_parser_error(p);
        if (st == OYLNG_OK) why = "a tree wasn't refused";
        else if (!e || strcmp(e, werr) != 0 || m.offset != wm.offset) {
            snprintf(detail, dn, "%zu:%zu: %s", m.line, m.col, e ? e : "(no message)");
            why = "a tree was refused otherwise";
        }
    } else if (st != OYLNG_OK) {
        oylng_mark m = oylng_parser_error_mark(p);
        snprintf(detail, dn, "%zu:%zu: %s", m.line, m.col, oylng_parser_error(p));
        why = "a tree was refused";
    } else if (strcmp(ev.data, want) != 0) {
        snprintf(detail, dn, "\n%s", ev.data);
        why = "trees give other events";
    } else if (strcmp(of.data ? of.data : "", offs) != 0) {
        why = "trees give other offsets";
    } else {
        oylng_emit_stream_end(em);
        size_t olen;
        const char *err;
        char *out = emitted(in, len, limit, false, 2, &olen, &err);
        oylng_str got = oylng_emitter_output(em);
        if (!out || got.len != olen || memcmp(got.data, out, olen) != 0) {
            snprintf(detail, dn, "\n%.*s", (int)got.len, got.data);
            why = "a tree is written otherwise";
        }
        free(out);
    }
    if (!why) trees++;
    free(ev.data);
    free(of.data);
    oylng_emitter_free(em);
    oylng_parser_free(p);
    return why;
}

static void run(const char *dir) {
    if (has(dir, "keep") || has(dir, "together")) { skipped++; return; }
    size_t len;
    char *in = slurp(dir, "in.yaml", &len);
    char *name = slurp(dir, "===", NULL);
    if (name) name[strcspn(name, "\n")] = '\0';
    int want_error = has(dir, "error");
    char *want = want_error ? NULL : slurp(dir, "test.event", NULL);
    char *limit = slurp(dir, "limit", NULL);

    oylng_parser *p = oylng_parser_new(in, len);
    if (limit) oylng_parser_set_limit(p, strtoull(limit, NULL, 10));
    strbuf got = { 0 }, offs = { 0 };
    sb_add(&offs, "", 0);
    const char *err = NULL;
    oylng_mark m = { 0, 0, 0 };
    const oylng_event *e;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) {
            err = oylng_parser_error(p);
            m = oylng_parser_error_mark(p);
            break;
        }
        ev_line(&got, e);
        if (e->type == OYLNG_EVT_SCALAR || e->type == OYLNG_EVT_MAP_START || e->type == OYLNG_EVT_SEQ_START) {
            char o[32];
            snprintf(o, sizeof o, "%zu\n", e->offset);
            sb_str(&offs, o);
        }
        if (e->type == OYLNG_EVT_STREAM_END) break;
    }

    const char *why = NULL;
    char detail[8192] = "";
    if (want_error) {
        if (!err) why = "not refused";
        else why = tree_check(in, len, limit, NULL, NULL, err, m, detail, sizeof detail);
    } else if (err) {
        why = "refused";
        snprintf(detail, sizeof detail, "%zu:%zu: %s", m.line, m.col, err);
    } else if (!want || strcmp(got.data, want) != 0) {
        why = "wrong events";
    } else {
        size_t off, rlen = 0;
        char *rest = slurp(dir, "rest", &rlen);
        bool stopped = oylng_parser_stopped(p, &off);
        if (rest && !stopped) why = "didn't stop at ...";
        else if (!rest && stopped) why = "stopped at ...";
        else if (rest) {
            size_t glen = len - off;
            char *g = malloc(glen + 1);
            memcpy(g, in + off, glen);
            unix_breaks(g, &glen);
            unix_breaks(rest, &rlen);
            if (glen != rlen || memcmp(g, rest, glen) != 0) why = "wrong rest after ...";
            free(g);
        }
        free(rest);
        if (!why) why = round_trip(in, len, limit, want, detail, sizeof detail);
        if (!why) why = tree_check(in, len, limit, want, offs.data, NULL, m, detail, sizeof detail);
    }

    if (why) {
        failed++;
        printf("FAIL %s (%s): %s %s\n", dir, name ? name : "?", why, detail);
        if (verbose && !want_error) {
            printf("--- in.yaml\n%s\n--- want\n%s--- got\n%s", in, want ? want : "(none)\n",
                   got.data ? got.data : "");
            if (err) printf("error %zu:%zu: %s\n", m.line, m.col, err);
        }
    } else {
        passed++;
        if (verbose > 1) printf("ok   %s (%s)%s%s\n", dir, name ? name : "?", err ? ": " : "", err ? err : "");
    }
    oylng_parser_free(p);
    free(got.data);
    free(offs.data);
    free(in);
    free(name);
    free(want);
    free(limit);
}

static int cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every directory under dir holding an in.yaml, in order */
static void walk(const char *dir) {
    if (has(dir, "in.yaml")) { run(dir); return; }
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) names = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof *names);
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp);
    for (size_t i = 0; i < n; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) walk(path);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv) {
    const char *data = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "-vv")) verbose += 2;
        else data = argv[i];
    }
    if (!data) { fprintf(stderr, "usage: test_suite DATA [-v]\n"); return 2; }
    walk(data);
    printf("suite: %d passed, %d failed, %d skipped (keep or together)\n", passed, failed, skipped);
    printf("emitter: %d streams written again and read back the same\n", trips);
    printf("tree: %d streams read as trees, the same as their events\n", trees);
    return failed ? 1 : 0;
}
