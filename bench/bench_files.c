/*
 * bench_files.c — how fast the parser reads real files
 *
 *     bench_files [-r ROUNDS] DIR...
 *
 * Each DIR is a group, named by its last part: every .yaml and .yml file
 * under it. A file is read as YAML 2.0 with "%YAML 2.0" in front, and "---"
 * too where its first line that isn't blank or a comment doesn't start a
 * document; a byte order mark is dropped. A file this parser refuses is
 * counted, by its message, and left out: such files need converting to
 * 2.0. Built with HAVE_LIBYAML, libyaml reads each file as written, and a
 * file it refuses is left out too.
 *
 * Each file is read on its own, with a new parser each time, as a program
 * reading it would, as events and as trees, and by libyaml as events and
 * as documents (yaml_parser_load). Each read is repeated for about 200 µs
 * a batch, the reads of a file taking turns, in ROUNDS rounds (default 7),
 * and a file's time is its median. A group's times are its files' sums.
 * Run it pinned to one core: taskset -c N.
 */

#define _XOPEN_SOURCE 700
#include "oylng/oylng.h"
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_LIBYAML
#include <yaml.h>
#endif

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
    int    group;
    char  *path;
    char  *in;     /* as written */
    size_t len;
    char  *in2;    /* as 2.0 */
    size_t len2;
} file;

static file  *files;
static size_t nfiles, capfiles;
static int    cur_group;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (cap - n < 65536) {
            char *nb = realloc(b, cap = cap * 2 + 65536);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb;
        }
        got = fread(b + n, 1, cap - n, f);
        n += got;
    } while (got);
    fclose(f);
    *len = n;
    return b;
}

/* Whether the first line that isn't blank or a comment starts a document */
static bool starts_doc(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j == n) return false;
        if (s[j] == '\n' || s[j] == '\r' || s[j] == '#') {
            while (j < n && s[j] != '\n') j++;
            i = j + 1;
            continue;
        }
        return j == i && n - i >= 3 && !memcmp(s + i, "---", 3) &&
               (n - i == 3 || s[i + 3] == ' ' || s[i + 3] == '\t' || s[i + 3] == '\n' || s[i + 3] == '\r');
    }
    return false;
}

static int add_file(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st, (void)ftw;
    size_t pl = strlen(path);
    if (type != FTW_F || !((pl > 5 && !strcmp(path + pl - 5, ".yaml")) || (pl > 4 && !strcmp(path + pl - 4, ".yml"))))
        return 0;
    file f = { cur_group, strdup(path), NULL, 0, NULL, 0 };
    if (!f.path || !(f.in = slurp(path, &f.len))) { free(f.path); return 0; }
    const char *s = f.in;
    size_t n = f.len;
    if (n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3, n -= 3;
    const char *head = starts_doc(s, n) ? "%YAML 2.0\n" : "%YAML 2.0\n---\n";
    size_t h = strlen(head);
    if (!(f.in2 = malloc(h + n + 1))) { free(f.in); free(f.path); return 0; }
    memcpy(f.in2, head, h);
    memcpy(f.in2 + h, s, n);
    f.len2 = h + n;
    if (nfiles == capfiles) {
        file *nf = realloc(files, (capfiles = capfiles * 2 + 256) * sizeof *files);
        if (!nf) { perror("bench_files"); exit(1); }
        files = nf;
    }
    files[nfiles++] = f;
    return 0;
}

static int by_path(const void *a, const void *b) {
    const file *x = a, *y = b;
    return x->group != y->group ? x->group - y->group : strcmp(x->path, y->path);
}

/* ── The reads: each gives what it read (events or nodes), or -1 ── */

static long ng_events(const file *f) {
    oylng_parser *p = oylng_parser_new(f->in2, f->len2);
    if (!p) return -1;
    const oylng_event *e;
    long n = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) n++;
    if (oylng_parser_error(p)) n = -1;
    oylng_parser_free(p);
    return n;
}

static long ng_tree(const file *f) {
    oylng_parser *p = oylng_parser_new(f->in2, f->len2);
    if (!p) return -1;
    oylng_tree *t;
    oylng_status st;
    long n = 0;
    while ((st = oylng_parse_tree(p, &t)) == OYLNG_OK && t) {
        n += (long)oylng_tree_nodes(t);
        oylng_tree_free(t);
    }
    oylng_parser_free(p);
    return st == OYLNG_OK ? n : -1;
}

#ifdef HAVE_LIBYAML
static long ly_events(const file *f) {
    yaml_parser_t p;
    yaml_event_t e;
    long n = 0;
    if (!yaml_parser_initialize(&p)) return -1;
    yaml_parser_set_input_string(&p, (const unsigned char *)f->in, f->len);
    for (;;) {
        if (!yaml_parser_parse(&p, &e)) { n = -1; break; }
        int done = e.type == YAML_STREAM_END_EVENT;
        yaml_event_delete(&e);
        if (done) break;
        n++;
    }
    yaml_parser_delete(&p);
    return n;
}

static long ly_tree(const file *f) {
    yaml_parser_t p;
    yaml_document_t d;
    long n = 0;
    if (!yaml_parser_initialize(&p)) return -1;
    yaml_parser_set_input_string(&p, (const unsigned char *)f->in, f->len);
    for (;;) {
        if (!yaml_parser_load(&p, &d)) { n = -1; break; }
        if (!yaml_document_get_root_node(&d)) { yaml_document_delete(&d); break; }
        n += d.nodes.top - d.nodes.start;
        yaml_document_delete(&d);
    }
    yaml_parser_delete(&p);
    return n;
}
#  define NREADS 4
static long (*const reads[NREADS])(const file *) = { ng_events, ng_tree, ly_events, ly_tree };
#else
#  define NREADS 2
static long (*const reads[NREADS])(const file *) = { ng_events, ng_tree };
#endif

/* The engine's refusals, by message */
typedef struct { char *msg; int n; } reason;
static reason *reasons;
static int nreasons;

static void refused(const file *f) {
    oylng_parser *p = oylng_parser_new(f->in2, f->len2);
    const oylng_event *e;
    while (p && oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) {}
    const char *m = p && oylng_parser_error(p) ? oylng_parser_error(p) : "out of memory";
    /* a key's text varies; the rest of the message doesn't */
    char buf[256];
    snprintf(buf, sizeof buf, "%s", m);
    char *q = strchr(buf, '"'), *r = q ? strrchr(buf, '"') : NULL;
    if (q && r > q) {
        char tail[256];
        snprintf(tail, sizeof tail, "%s", r + 1);
        snprintf(q, sizeof buf - (size_t)(q - buf), "\"...\"%s", tail);
    }
    oylng_parser_free(p);
    for (int i = 0; i < nreasons; i++)
        if (!strcmp(reasons[i].msg, buf)) { reasons[i].n++; return; }
    reason *nr = realloc(reasons, (size_t)(nreasons + 1) * sizeof *reasons);
    if (!nr) return;
    reasons = nr;
    reasons[nreasons++] = (reason){ strdup(buf), 1 };
}

static int by_count(const void *a, const void *b) { return ((const reason *)b)->n - ((const reason *)a)->n; }

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    int rounds = 7, first = 1;
    if (argc > 2 && !strcmp(argv[1], "-r")) { rounds = atoi(argv[2]); first = 3; }
    if (rounds < 1 || rounds > 1000 || first >= argc) {
        fprintf(stderr, "usage: bench_files [-r ROUNDS] DIR...\n");
        return 2;
    }
    int ngroups = argc - first;
    for (int g = 0; g < ngroups; g++) {
        cur_group = g;
        if (nftw(argv[first + g], add_file, 16, FTW_PHYS) != 0) { perror(argv[first + g]); return 1; }
    }
    qsort(files, nfiles, sizeof *files, by_path);

    /* which files every read takes, and how many reads make a batch */
    enum { OK, NG_REFUSED, LY_REFUSED };
    int *state = calloc(nfiles, sizeof *state);
    long *batch = calloc(nfiles, sizeof *batch);
    double *t = calloc(nfiles * NREADS * (size_t)rounds, sizeof *t);
    if (!state || !batch || !t) { perror("bench_files"); return 1; }
    for (size_t i = 0; i < nfiles; i++) {
        file *f = &files[i];
#ifdef HAVE_LIBYAML
        if (ly_events(f) < 0 || ly_tree(f) < 0) { state[i] = LY_REFUSED; continue; }
#endif
        if (ng_events(f) < 0 || ng_tree(f) < 0) { state[i] = NG_REFUSED; refused(f); continue; }
        double t0 = now();
        long c = 0;
        do { ng_events(f); c++; } while (now() - t0 < 2e-4);
        batch[i] = c;
    }
    for (int r = 0; r < rounds; r++)
        for (size_t i = 0; i < nfiles; i++) {
            if (state[i] != OK) continue;
            for (int j = 0; j < NREADS; j++) {
                int w = (int)((j + r + i) % NREADS);   /* a different order each time */
                double t0 = now();
                for (long c = 0; c < batch[i]; c++) reads[w](&files[i]);
                t[(i * NREADS + w) * rounds + r] = (now() - t0) / batch[i];
            }
        }

    /* each file's median, for each read */
    double *med = calloc(nfiles * NREADS, sizeof *med);
    if (!med) { perror("bench_files"); return 1; }
    for (size_t i = 0; i < nfiles; i++)
        for (int j = 0; j < NREADS && state[i] == OK; j++) {
            double *v = &t[(i * NREADS + j) * rounds];
            qsort(v, (size_t)rounds, sizeof *v, cmp);
            med[i * NREADS + j] = v[rounds / 2];
        }

    printf("%-14s %6s %6s %8s", "group", "files", "read", "refused");
#ifdef HAVE_LIBYAML
    printf(" %8s", "libyaml");
#endif
    printf(" %8s %8s %8s", "KB read", "MB/s", "tree/ev");
#ifdef HAVE_LIBYAML
    printf(" %8s %8s", "events x", "trees x");
#endif
    printf("\n");
    /* a row for each group, then one for all of them if there are more */
    for (int g = 0; g < ngroups + (ngroups > 1); g++) {
        size_t n = 0, ok = 0, ng = 0, ly = 0, bytes = 0;
        double sum[NREADS] = { 0 };
        for (size_t i = 0; i < nfiles; i++) {
            if (g < ngroups && files[i].group != g) continue;
            n++;
            if (state[i] == NG_REFUSED) ng++;
            else if (state[i] == LY_REFUSED) ly++;
            else {
                ok++;
                bytes += files[i].len2;
                for (int j = 0; j < NREADS; j++) sum[j] += med[i * NREADS + j];
            }
        }
        const char *name = "all";
        if (g < ngroups) {
            const char *d = argv[first + g], *slash = strrchr(d, '/');
            name = slash && slash[1] ? slash + 1 : d;
        }
        printf("%-14s %6zu %6zu %8zu", name, n, ok, ng);
#ifdef HAVE_LIBYAML
        printf(" %8zu", ly);
#else
        (void)ly;
#endif
        if (ok) {
            printf(" %8zu %8.1f %7.2fx", bytes / 1024, bytes / 1e6 / sum[0], sum[1] / sum[0]);
#ifdef HAVE_LIBYAML
            printf(" %7.2fx %7.2fx", sum[2] / sum[0], sum[3] / sum[1]);
#endif
        }
        printf("\n");
    }
    printf("\nread: read as YAML 2.0 by this parser");
#ifdef HAVE_LIBYAML
    printf(" and as written by libyaml");
#endif
    printf("; MB/s: its events; tree/ev: its trees' time against its events'");
#ifdef HAVE_LIBYAML
    printf(";\nevents x: how many times as fast as libyaml, for events; trees x: for trees, against its documents");
#endif
    printf("\n");
    if (nreasons) {
        qsort(reasons, (size_t)nreasons, sizeof *reasons, by_count);
        printf("\nRefused by this parser, which files need converting for:\n");
        for (int i = 0; i < nreasons && i < 12; i++) printf("%6d  %s\n", reasons[i].n, reasons[i].msg);
        if (nreasons > 12) printf("        and %d other messages\n", nreasons - 12);
    }
    return 0;
}
