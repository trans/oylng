/*
 * bench_typed.c — what typing costs (part 2): speed and memory
 *
 *     bench_typed [MB] [REPS]       speed: each input of bench_gen.h at MB
 *                                   megabytes (default 8), REPS times (20)
 *     bench_typed_mem [MB ...]      memory: peak heap at each size (default
 *                                   0.001, 1 and 8), built with -DMEM
 *
 * Each input has a schema an application would give for it, written below,
 * and is read five ways, in turn in each round, so that the ratios come
 * from the same moments, each timed right after a run of its own: as events, as trees, typed with no schema (a
 * document that nothing types stays strings: the typed layer alone), typed
 * by the schema, and typed by the schema with code bound to its item type,
 * code that makes nothing (section 12: what running code at each value
 * costs the parser, not what the code does). Run it pinned to one core:
 * taskset -c N. Times are medians; only the ratios are worth quoting, as a
 * busy machine moves every time alike. Freed memory stays in the heap
 * (glibc's mallopt), so that no read pays for paging in what another gave
 * back, which moved times by a fifth.
 *
 * The memory build counts every allocation, at its usable size plus the 8
 * bytes glibc keeps in front of it, and prints the peak above what was
 * held before the read, the input not counted, in bytes and per input byte.
 */

#define _GNU_SOURCE
#include "oylng/oylng.h"
#include "oylng/schema.h"
#include "bench_gen.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef MEM
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void  __libc_free(void *);
static size_t cur, peak;
static void add(void *p) { if (p) { cur += malloc_usable_size(p) + 8; if (cur > peak) peak = cur; } }
static void sub(void *p) { if (p) cur -= malloc_usable_size(p) + 8; }
void *malloc(size_t n) { void *p = __libc_malloc(n); add(p); return p; }
void *calloc(size_t a, size_t b) { void *p = __libc_calloc(a, b); add(p); return p; }
void *realloc(void *q, size_t n) {
    size_t old = q ? malloc_usable_size(q) + 8 : 0;
    void *p = __libc_realloc(q, n);
    if (p) { cur -= old; add(p); } else if (!n) cur -= old;
    return p;
}
void free(void *p) { sub(p); __libc_free(p); }
#endif

/* ── The inputs and their schemas ────────────────────────── */

typedef struct {
    const char *name;
    int         input;    /* 0 mixed, 1 block, 2 json, 3 config, 4 config_table */
    const char *schema;   /* the table of an application's schema */
    const char *root;
    const char *item;     /* the entry bound, for the fifth way */
} shape;

#define NSHAPES 7

static const char config_schema[] =
      "%YAML 2.0\n--- &\n"
      "deployment:\n"
      "  apiVersion: !str\n"
      "  kind: !str\n"
      "  metadata: {name: !str, labels: !labels}\n"
      "  spec:\n"
      "    replicas: !int\n"
      "    selector: {matchLabels: !labels}\n"
      "    template: {spec: {containers: [!container, ...]}}\n"
      "labels: {app: !str, tier: !str}\n"
      "container:\n"
      "  name: !str\n"
      "  image: !str\n"
      "  args: [!str, ...]\n"
      "  env: [{name: !str, value: !str}, ...]\n"
      "  command: [!str, ...]\n"
      "  description: !str\n";

static const shape shapes[NSHAPES] = {
    { "mixed", 0,
      "%YAML 2.0\n--- &\n"
      "doc:\n"
      "  metadata: {name: !str, version: !str, generated: !bool}\n"
      "  entries: [!entry, ...]\n"
      "entry:\n"
      "  id: !int\n"
      "  name: !str\n"
      "  value: !float\n"
      "  tags: [!str, ...]\n"
      "  nested: {x: !int, y: !int, label: !str}\n",
      "doc", "entry" },
    { "block", 1,
      "%YAML 2.0\n--- &\n"
      "doc: [!item, ...]\n"
      "item: {id: !int, name: !str, value: !int, nested: {x: !int, y: !int}}\n",
      "doc", "item" },
    { "json", 2,
      "%YAML 2.0\n--- &\n"
      "doc: [!item, ...]\n"
      "item: {id: !int, name: !str, value: !int, nested: {x: !int, y: !int}}\n",
      "doc", "item" },
    { "config", 3, config_schema, "deployment", "container" },
    { "config_table", 4, config_schema, "deployment", "container" },
    /* block again, its values held to ranges and a pattern (section 4) */
    { "block, rules", 1,
      "%YAML 2.0\n--- &\n"
      "doc: [!item, ...]\n"
      "item:\n"
      "  id: !int(0..)\n"
      "  name: !str(/item-[0-9]+/)\n"
      "  value: !int(0..999)\n"
      "  nested: {x: !int(0..499), y: !int(0..499)}\n",
      "doc", "item" },
    /* and each item a union of three mappings, the last the one that fits,
       chosen by its keys (section 9) */
    { "block, union", 1,
      "%YAML 2.0\n--- &\n"
      "doc: [!item, ...]\n"
      "item: {!|: [!label, !size, !full]}\n"
      "label: {id: !int, label: !str}\n"
      "size: {id: !int, size: !int, nested: {x: !int, y: !int}}\n"
      "full: {id: !int, name: !str, value: !int, nested: {x: !int, y: !int}}\n",
      "doc", "item" },
};

static char *gen(int k, size_t target, size_t *len) {
    int i = shapes[k].input;
    char *in = i == 0 ? gen_mixed(target, len) : i == 1 ? gen_block(target, len) :
               i == 2 ? gen_json(target, len) : gen_config_as(target, len, i == 4);
    if (!in) { fprintf(stderr, "out of memory\n"); exit(1); }
    return in;
}

/* ── The five ways ───────────────────────────────────────── */

static void die(const char *how, oylng_parser *p) {
    oylng_mark m = oylng_parser_error_mark(p);
    fprintf(stderr, "%s: %zu:%zu: %s\n", how, m.line, m.col, oylng_parser_error(p));
    exit(1);
}

static oylng_parser *parser(const char *in, size_t len) {
    oylng_parser *p = oylng_parser_new(in, len);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    oylng_parser_set_limit(p, UINT64_MAX);
    return p;
}

static size_t events(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    (void)sc, (void)item;
    oylng_parser *p = parser(in, len);
    const oylng_event *e;
    size_t n = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) n++;
    if (oylng_parser_error(p)) die("events", p);
    oylng_parser_free(p);
    return n;
}

static size_t trees(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    (void)sc, (void)item;
    oylng_parser *p = parser(in, len);
    oylng_tree *t;
    size_t n = 0;
    while (oylng_parse_tree(p, &t) == OYLNG_OK && t) {
        n += oylng_tree_nodes(t);
        oylng_tree_free(t);
    }
    if (oylng_parser_error(p)) die("trees", p);
    oylng_parser_free(p);
    return n;
}

static int made;   /* what the bound code is given: one byte for each object */

static const char *make_nothing(void *ctx, oylng_typed *t, oylng_value v, void **object) {
    (void)ctx, (void)t, (void)v;
    *object = &made;
    return NULL;
}

/* typed by sc, or by nothing; with item bound if it isn't NULL. Returns the
 * documents, each checked to be typed as it should be */
static size_t typed(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    oylng_parser *p = parser(in, len);
    if (item && oylng_parser_bind(p, (oylng_str){ item, strlen(item) }, make_nothing, NULL, NULL) != OYLNG_OK)
        die("bind", p);
    oylng_typed *y;
    size_t n = 0;
    oylng_typed_by want = sc ? OYLNG_TYPED_APP : OYLNG_TYPED_NOT;
    while (oylng_parse_typed(p, sc, &y) == OYLNG_OK && y) {
        if (oylng_typed_how(y) != want) { fprintf(stderr, "typed %d, not %d\n", oylng_typed_how(y), want); exit(1); }
        n++;
        oylng_typed_free(y);
    }
    if (oylng_parser_error(p)) die("typed", p);
    oylng_parser_free(p);
    return n;
}

static size_t typed_none(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    (void)sc, (void)item;
    return typed(in, len, NULL, NULL);
}

static size_t typed_app(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    (void)item;
    return typed(in, len, sc, NULL);
}

static size_t typed_bound(const char *in, size_t len, const oylng_schema *sc, const char *item) {
    return typed(in, len, sc, item);
}

typedef size_t (*way)(const char *in, size_t len, const oylng_schema *sc, const char *item);

static const struct { const char *name; way run; } ways[5] = {
    { "events", events }, { "trees", trees }, { "typed, none", typed_none },
    { "typed", typed_app }, { "typed, bound", typed_bound },
};

static oylng_schema *schema_for(int k) {
    const char *tx = shapes[k].schema;
    oylng_schema *sc = oylng_schema_new(tx, strlen(tx), (oylng_str){ shapes[k].root, strlen(shapes[k].root) }, 0);
    if (!sc || oylng_schema_status(sc) != OYLNG_OK) {
        fprintf(stderr, "%s's schema: %s\n", shapes[k].name, sc ? oylng_schema_error(sc) : "out of memory");
        exit(1);
    }
    return sc;
}

/* ── Speed ───────────────────────────────────────────────── */

#ifndef MEM
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
#ifdef __GLIBC__
    /* what a read frees stays in the heap, paged in, for the next: what
       paging in costs follows the peak heap, which the memory build gives */
    mallopt(M_MMAP_MAX, 0);
    mallopt(M_TRIM_THRESHOLD, -1);
#endif
    size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 8;
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    if (reps < 1 || reps > 1000) reps = 20;
    printf("medians of %d rounds, ms; each way's time over the trees' and over the typed time\n\n", reps);
    printf("%-13s %6s %8s %8s %8s %8s %8s   %6s %6s %6s %6s\n", "input", "MB", "events", "trees", "none",
           "typed", "bound", "ev/tr", "none", "typed", "bd/ty");
    for (int k = 0; k < NSHAPES; k++) {
        size_t len = 0;
        char *in = gen(k, mb << 20, &len);
        oylng_schema *sc = schema_for(k);
        static double t[5][1000];
        for (int w = 0; w < 5; w++) ways[w].run(in, len, sc, shapes[k].item);   /* warm, and checked */
        /* each way timed right after a run of its own, untimed, so that each
           finds the heap as it left it: what one way frees, the next may
           find paged in or given back, which moves its time by a fifth */
        for (int r = 0; r < reps; r++)
            for (int w = 0; w < 5; w++) {
                ways[w].run(in, len, sc, shapes[k].item);
                double t0 = now();
                ways[w].run(in, len, sc, shapes[k].item);
                t[w][r] = now() - t0;
            }
        double med[5];
        for (int w = 0; w < 5; w++) {
            qsort(t[w], (size_t)reps, sizeof t[w][0], cmp);
            med[w] = t[w][reps / 2];
        }
        printf("%-13s %6.2f %8.2f %8.2f %8.2f %8.2f %8.2f   %6.2f %6.2f %6.2f %6.2f\n", shapes[k].name,
               len / 1048576.0, med[0] * 1e3, med[1] * 1e3, med[2] * 1e3, med[3] * 1e3, med[4] * 1e3,
               med[0] / med[1], med[2] / med[1], med[3] / med[1], med[4] / med[3]);
        oylng_schema_free(sc);
        free(in);
    }
    printf("\nnone: typed with no schema; typed: by the schema; bound: and code bound to the item type\n");
    return 0;
}

/* ── Memory ──────────────────────────────────────────────── */

#else
int main(int argc, char **argv) {
    static const double dflt[] = { 0.001, 1, 8 };
    int nsizes = argc > 1 ? argc - 1 : 3;
    printf("peak heap above what was held before, in KB, and per input byte; the input not counted\n");
    for (int s = 0; s < nsizes; s++) {
        double mb = argc > 1 ? atof(argv[s + 1]) : dflt[s];
        printf("\n%-13s %9s %9s %9s %9s %9s %9s %9s\n", "input", "KB in", "schema", "events", "trees", "none",
               "typed", "bound");
        for (int k = 0; k < NSHAPES; k++) {
            size_t len = 0;
            char *in = gen(k, (size_t)(mb * 1048576.0), &len);
            size_t before = cur;
            peak = cur;
            oylng_schema *sc = schema_for(k);
            size_t held[6] = { cur - before };   /* the schema, as it is held */
            for (int w = 0; w < 5; w++) {
                size_t base = cur;
                peak = cur;
                ways[w].run(in, len, sc, shapes[k].item);
                held[w + 1] = peak - base;
                if (cur != base) { fprintf(stderr, "%s, %s: %zu bytes not let go\n", shapes[k].name, ways[w].name, cur - base); exit(1); }
            }
            printf("%-13s %9.1f", shapes[k].name, len / 1024.0);
            for (int w = 0; w < 6; w++) printf(" %9.1f", held[w] / 1024.0);
            printf("\n%-13s %9s %9s", "", "per byte", "");
            for (int w = 1; w < 6; w++) printf(" %9.2f", (double)held[w] / (double)len);
            printf("\n");
            oylng_schema_free(sc);
            free(in);
        }
    }
    printf("\nnone: typed with no schema; typed: by the schema; bound: and code bound to the item type\n");
    return 0;
}
#endif
