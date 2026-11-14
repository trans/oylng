/*
 * test_yes.c — runs part 2's suite (YAML Easy Schema)
 *
 *     test_yes DATA [-v]
 *
 * DATA is trans/yaml2's suite/yes written out one directory per test by
 * suite/bin/suite-to-data (suite/yes/README.md): in.yaml; typed.event, or
 * error with refused and at; and limit, cut, schema.yaml and root, holds,
 * leap-second, year-zero, keep and store where a test has them. Each test
 * is typed (oylng_parse_typed), by an application's schema where it has
 * one, and must give typed.event, a !float compared by its bits; or be
 * refused, as refused says: the schema, or a document, whose path is
 * compared with at (a different one is counted, not failed: the suite's is
 * the first fault its checker meets). The limit applies to the stream, and
 * to an application's schema for part 2's counts (its full size), not to
 * the schema's text as a stream, as the suite has it.
 *
 * Skipped, and counted: keep and together, interfaces this parser doesn't
 * offer; store, since it looks up no published name; holds other than its
 * own (9 fraction digits, a second 60, the year 0000), and a test without
 * holds that it refuses for a time it can't hold; and what it refuses
 * because it doesn't do it yet (!**); a limit of 0, which the library takes
 * as the default. Exits with 1 if any test fails.
 */

#define _POSIX_C_SOURCE 200809L
#include "oylng/oylng.h"
#include "oylng/schema.h"
#include "events.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passed, failed, verbose, paths;
static int skip_keep, skip_store, skip_holds, skip_yet, skip_zero;

/* The library's own: a schema with the limit for part 2's counts alone */
oylng_schema *oylng__schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit,
                                uint64_t counts_limit);

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

/* A one-line file's line, without its break; NULL if there's none */
static char *line(const char *dir, const char *name) {
    char *s = slurp(dir, name, NULL);
    if (s) s[strcspn(s, "\n")] = '\0';
    return s;
}

static void ev(strbuf *b, const char *head, const char *s, size_t n) {
    sb_str(b, head);
    ev_esc(b, s, n);
    sb_str(b, "\n");
}

/* An untyped node, as part 1's events have it */
static void any_events(strbuf *b, const oylng_tree *t, oylng_node n) {
    oylng_kind k = oylng_node_kind(t, n);
    if (k == OYLNG_NODE_STRING) {
        oylng_str s = oylng_node_text(t, n);
        ev(b, "=VAL :", s.data, s.len);
        return;
    }
    bool map = k == OYLNG_NODE_MAPPING;
    sb_str(b, map ? "+MAP\n" : "+SEQ\n");
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++) {
        if (map) {
            any_events(b, t, oylng_node_key(t, n, i));
            any_events(b, t, oylng_node_value(t, n, i));
        } else any_events(b, t, oylng_node_item(t, n, i));
    }
    sb_str(b, map ? "-MAP\n" : "-SEQ\n");
}

/* A time's text, its fraction cut to the 9 digits held, as the cut
 * delivers it */
static void cut_time(strbuf *b, oylng_str s) {
    const char *dot = memchr(s.data, '.', s.len);
    if (!dot) { ev(b, "=DATETIME :", s.data, s.len); return; }
    size_t i = (size_t)(dot - s.data) + 1, e = i;
    while (e < s.len && s.data[e] >= '0' && s.data[e] <= '9') e++;
    if (e - i <= 9) { ev(b, "=DATETIME :", s.data, s.len); return; }
    sb_str(b, "=DATETIME :");
    ev_esc(b, s.data, i + 9);
    ev_esc(b, s.data + e, s.len - e);
    sb_str(b, "\n");
}

static void typed_events(strbuf *b, const oylng_typed *y, oylng_value v, bool cut) {
    char num[48];
    oylng_str s = oylng_value_text(y, v);
    size_t c = oylng_value_count(y, v);
    switch (oylng_value_type(y, v)) {
    case OYLNG_T_ANY: any_events(b, oylng_typed_tree(y), oylng_value_node(y, v)); break;
    case OYLNG_T_STR: ev(b, "=VAL :", s.data, s.len); break;
    case OYLNG_T_INT: {
        int64_t i;
        uint64_t u;
        if (oylng_value_int(y, v, &i)) snprintf(num, sizeof num, "%lld", (long long)i);
        else if (oylng_value_uint(y, v, &u)) snprintf(num, sizeof num, "%llu", (unsigned long long)u);
        else snprintf(num, sizeof num, "?");
        ev(b, "=INT :", num, strlen(num));
        break;
    }
    case OYLNG_T_BIGINT: ev(b, "=BIG :", s.data, s.len); break;
    case OYLNG_T_FLOAT:
        snprintf(num, sizeof num, "%.17g", oylng_value_float(y, v));
        ev(b, "=FLT :", num, strlen(num));
        break;
    case OYLNG_T_DECIMAL: ev(b, "=DEC :", s.data, s.len); break;
    case OYLNG_T_BOOL: sb_str(b, oylng_value_bool(y, v) ? "=BOOL :true\n" : "=BOOL :false\n"); break;
    case OYLNG_T_NULL: sb_str(b, "=NULL\n"); break;
    case OYLNG_T_DATE: ev(b, "=DATE :", s.data, s.len); break;
    case OYLNG_T_DATETIME:
        if (cut) cut_time(b, s);
        else ev(b, "=DATETIME :", s.data, s.len);
        break;
    case OYLNG_T_BINARY: {
        oylng_str by = oylng_value_bytes(y, v);
        sb_str(b, "=BIN :");
        for (size_t i = 0; i < by.len; i++) {
            snprintf(num, sizeof num, "%02x", (unsigned char)by.data[i]);
            sb_str(b, num);
        }
        sb_str(b, "\n");
        break;
    }
    case OYLNG_T_SEQUENCE:
        sb_str(b, "+SEQ\n");
        for (size_t i = 0; i < c; i++) typed_events(b, y, oylng_value_item(y, v, i), cut);
        sb_str(b, "-SEQ\n");
        break;
    case OYLNG_T_MAPPING:
        sb_str(b, "+MAP\n");
        for (size_t i = 0; i < c; i++) {
            oylng_str k = oylng_value_key(y, v, i);
            ev(b, "=VAL :", k.data, k.len);
            typed_events(b, y, oylng_value_at(y, v, i), cut);
        }
        sb_str(b, "-MAP\n");
        break;
    default: sb_str(b, "?\n");
    }
}

/* got against want, line by line, a =FLT by the bits of the binary64 its
 * text reads as */
static bool same_events(const char *got, const char *want) {
    for (;;) {
        size_t gl = strcspn(got, "\n"), wl = strcspn(want, "\n");
        if (gl > 6 && wl > 6 && !memcmp(got, "=FLT :", 6) && !memcmp(want, "=FLT :", 6)) {
            double g = strtod(got + 6, NULL), w = strtod(want + 6, NULL);
            uint64_t gb, wb;
            memcpy(&gb, &g, 8);
            memcpy(&wb, &w, 8);
            if (gb != wb) return false;
        } else if (gl != wl || memcmp(got, want, gl)) return false;
        got += gl;
        want += wl;
        if (!*got || !*want) return !*got && !*want;
        got++;
        want++;
    }
}

static void run(const char *dir) {
    if (has(dir, "keep") || has(dir, "together")) { skip_keep++; return; }
    if (has(dir, "store")) { skip_store++; return; }
    char *holds = line(dir, "holds");
    bool ours = !holds || (!strcmp(holds, "9") && has(dir, "leap-second") && has(dir, "year-zero"));
    if (!ours) { skip_holds++; free(holds); return; }

    size_t len, slen = 0;
    char *in = slurp(dir, "in.yaml", &len);
    char *name = line(dir, "===");
    char *limit = line(dir, "limit");
    char *schema = slurp(dir, "schema.yaml", &slen);
    char *root = line(dir, "root");
    char *refused = line(dir, "refused");
    char *at = line(dir, "at");
    bool want_error = has(dir, "error"), cut = has(dir, "cut");
    char *want = want_error ? NULL : slurp(dir, "typed.event", NULL);
    uint64_t lim = limit ? strtoull(limit, NULL, 10) : 0;

    const char *err = NULL, *why = NULL;
    bool schema_err = false;
    char detail[8192] = "";
    oylng_mark m = { 0, 0, 0 };
    strbuf got = { 0 };
    oylng_parser *p = NULL;
    oylng_schema *app = NULL;
    if (limit && !lim) { skip_zero++; goto done; }   /* the library's 0 is the default */
    if (schema) {
        /* the suite applies the limit to the schema's counts, not to its text as a stream */
        app = oylng__schema_new(schema, slen, root ? (oylng_str){ root, strlen(root) } : (oylng_str){ NULL, 0 }, 0,
                                lim);
        if (oylng_schema_status(app) != OYLNG_OK) {
            err = oylng_schema_error(app);
            m = oylng_schema_error_mark(app);
            schema_err = oylng_schema_status(app) == OYLNG_ERR_SCHEMA;
        }
    }
    if (!err) {
        p = oylng_parser_new(in, len);
        if (lim) oylng_parser_set_limit(p, lim);
        oylng_parser_set_cut(p, cut);
        sb_str(&got, "+STR\n");
        for (;;) {
            oylng_typed *y;
            oylng_status st = oylng_parse_typed(p, app, &y);
            if (st != OYLNG_OK) {
                err = oylng_parser_error(p);
                m = oylng_parser_error_mark(p);
                schema_err = st == OYLNG_ERR_SCHEMA;
                break;
            }
            if (!y) break;
            oylng_str dn = oylng_tree_name(oylng_typed_tree(y));
            if (dn.data) ev(&got, "+DOC !", dn.data, dn.len);
            else sb_str(&got, "+DOC\n");
            typed_events(&got, y, oylng_typed_root(y), cut);
            sb_str(&got, "-DOC\n");
            oylng_typed_free(y);
        }
        sb_str(&got, "-STR\n");
    }

    /* the engine's path, where it refused a document: its error up to ": " */
    bool doc_err = schema_err && err && err[0] == '/';
    size_t plen = doc_err ? strcspn(err, ":") : 0;
    if (err && strstr(err, "supported yet")) { skip_yet++; goto done; }
    if (err && !holds && strstr(err, "can't hold")) { skip_holds++; goto done; }
    if (want_error) {
        if (!err) why = "not refused";
        else if (refused && !strcmp(refused, "schema") && (!schema_err || doc_err)) why = "refused, not for the schema";
        else if (refused && !strcmp(refused, "document") && !doc_err) why = "refused, not for a document";
        else if (doc_err && at && (strlen(at) != plen || memcmp(at, err, plen))) {
            paths++;
            if (verbose) printf("PATH %s (%s): the suite's %s, the engine's %s\n", dir, name ? name : "?", at, err);
        }
        if (why && err) snprintf(detail, sizeof detail, "%zu:%zu: %s", m.line, m.col, err);
    } else if (err) {
        why = "refused";
        snprintf(detail, sizeof detail, "%zu:%zu: %s", m.line, m.col, err);
    } else if (!want || !same_events(got.data, want)) why = "wrong events";

    if (why) {
        failed++;
        printf("FAIL %s (%s): %s %s\n", dir, name ? name : "?", why, detail);
        if (verbose && !want_error)
            printf("--- in.yaml\n%s\n--- want\n%s--- got\n%s", in, want ? want : "(none)\n", got.data ? got.data : "");
    } else {
        passed++;
        if (verbose > 1) printf("ok   %s (%s)%s%s\n", dir, name ? name : "?", err ? ": " : "", err ? err : "");
    }
done:
    oylng_parser_free(p);
    oylng_schema_free(app);
    free(got.data);
    free(in);
    free(name);
    free(limit);
    free(schema);
    free(root);
    free(refused);
    free(at);
    free(want);
    free(holds);
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
    if (!data) { fprintf(stderr, "usage: test_yes DATA [-v]\n"); return 2; }
    walk(data);
    printf("yes: %d passed, %d failed; skipped %d with keep, %d with a store, %d holding more, %d not built yet (!**),"
           " %d with a limit of 0; %d refused at another path\n",
           passed, failed, skip_keep, skip_store, skip_holds, skip_yet, skip_zero, paths);
    return failed ? 1 : 0;
}
