/*
 * test_batch.c — reads many streams from standard input, for diffcheck.py
 *
 * Each input is a 4-byte little-endian length, then that many bytes, then
 * a 4-byte level limit (0 for the default). For each, writes either
 *
 *     OK <offset after a stopping ..., or ->\n<events in the suite's notation>
 *
 * or ERR <line>:<col> <message>\n, then a NUL byte.
 */

#include "oylng/oylng.h"
#include "events.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_u32(uint32_t *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, stdin) != 4) return 0;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

int main(void) {
    uint32_t len, levels;
    while (read_u32(&len)) {
        char *in = malloc(len ? len : 1);
        if (!in || fread(in, 1, len, stdin) != len || !read_u32(&levels)) return 2;
        oylng_parser *p = oylng_parser_new(in, len);
        if (levels) oylng_parser_set_levels(p, levels);
        strbuf out = { 0 };
        const oylng_event *e;
        int ok = 1;
        for (;;) {
            if (oylng_parse_next(p, &e) != OYLNG_OK) { ok = 0; break; }
            ev_line(&out, e);
            if (e->type == OYLNG_EVT_STREAM_END) break;
        }
        if (ok) {
            size_t off;
            if (oylng_parser_stopped(p, &off)) printf("OK %zu\n", off);
            else printf("OK -\n");
            fwrite(out.data, 1, out.len, stdout);
        } else {
            oylng_mark m = oylng_parser_error_mark(p);
            printf("ERR %zu:%zu %s\n", m.line, m.col, oylng_parser_error(p));
        }
        fputc('\0', stdout);
        fflush(stdout);
        free(out.data);
        oylng_parser_free(p);
        free(in);
    }
    return 0;
}
