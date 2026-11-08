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
 * are skipped. Exits with 1 if any test fails.
 */

#define _POSIX_C_SOURCE 200809L
#include "oylng/oylng.h"
#include "events.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passed, failed, skipped, verbose;

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
    strbuf got = { 0 };
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
        if (e->type == OYLNG_EVT_STREAM_END) break;
    }

    const char *why = NULL;
    char detail[512] = "";
    if (want_error) {
        if (!err) why = "not refused";
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
    return failed ? 1 : 0;
}
