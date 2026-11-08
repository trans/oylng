/*
 * bench_parser.c — how fast the parser delivers events
 *
 *     bench_parser [MB] [REPS]
 *
 * Generates each input of bench_gen.h at MB megabytes (default 8), reads
 * it REPS times (default 20), and prints the best and median time and
 * throughput. Built with HAVE_LIBYAML, it reads each input with libyaml as
 * well, without its %YAML 2.0 line, interleaved, and prints libyaml's
 * median and how many times faster this parser is. Run it pinned to one
 * core: taskset -c N.
 */

#define _POSIX_C_SOURCE 200809L
#include "oylng/oylng.h"
#include "bench_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef HAVE_LIBYAML
#include <yaml.h>
#endif

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double run(const char *in, size_t len, long *events) {
    double t0 = now();
    oylng_parser *p = oylng_parser_new(in, len);
    oylng_parser_set_limit(p, UINT64_MAX);
    const oylng_event *e;
    long n = 0;
    while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END) n++;
    if (oylng_parser_error(p)) {
        oylng_mark m = oylng_parser_error_mark(p);
        fprintf(stderr, "error %zu:%zu: %s\n", m.line, m.col, oylng_parser_error(p));
        exit(1);
    }
    oylng_parser_free(p);
    *events = n;
    return now() - t0;
}

#ifdef HAVE_LIBYAML
static double run_libyaml(const char *in, size_t len, long *events) {
    double t0 = now();
    yaml_parser_t p;
    yaml_parser_initialize(&p);
    yaml_parser_set_input_string(&p, (const unsigned char *)in, len);
    yaml_event_t e;
    long n = 0;
    for (;;) {
        if (!yaml_parser_parse(&p, &e)) {
            fprintf(stderr, "libyaml: %s at %zu:%zu\n", p.problem, p.problem_mark.line + 1, p.problem_mark.column + 1);
            exit(1);
        }
        int end = e.type == YAML_STREAM_END_EVENT;
        yaml_event_delete(&e);
        if (end) break;
        n++;
    }
    yaml_parser_delete(&p);
    *events = n;
    return now() - t0;
}
#endif

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 8;
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    if (reps < 1 || reps > 1000) reps = 20;
    struct { const char *name; char *(*gen)(size_t, size_t *); } inputs[] = {
        { "mixed", gen_mixed }, { "block", gen_block }, { "json", gen_json },
    };
    printf("%-14s %8s %10s %10s %10s %12s", "input", "MB", "best ms", "median ms", "MB/s", "events");
#ifdef HAVE_LIBYAML
    printf(" %12s %8s", "libyaml ms", "faster");
#endif
    printf("\n");
    for (int k = 0; k < 5; k++) {
        size_t len;
        char *in;
        const char *name;
        if (k < 3) { in = inputs[k].gen(mb << 20, &len); name = inputs[k].name; }
        else { in = gen_config_as(mb << 20, &len, k == 4); name = k == 4 ? "config_table" : "config"; }
        static double t[1000];
        long events = 0;
        run(in, len, &events);
#ifdef HAVE_LIBYAML
        /* config_table has entries and references, which 1.2 can't read */
        int other = k != 4;
        static double u[1000];
        long uev = 0;
#endif
        for (int r = 0; r < reps; r++) {
            t[r] = run(in, len, &events);
#ifdef HAVE_LIBYAML
            if (other) u[r] = run_libyaml(in + 10, len - 10, &uev);
#endif
        }
        qsort(t, (size_t)reps, sizeof *t, cmp);
        printf("%-14s %8.2f %10.2f %10.2f %10.1f %12ld", name, len / 1048576.0, t[0] * 1e3,
               t[reps / 2] * 1e3, len / 1048576.0 / t[0], events);
#ifdef HAVE_LIBYAML
        if (other) {
            qsort(u, (size_t)reps, sizeof *u, cmp);
            printf(" %12.2f %7.2fx", u[reps / 2] * 1e3, u[reps / 2] / t[reps / 2]);
            if (uev != events) printf("  (libyaml gave %ld events)", uev);
        }
#endif
        printf("\n");
        free(in);
    }
    return 0;
}
