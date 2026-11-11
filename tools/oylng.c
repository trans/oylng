/*
 * oylng — read a YAML 2.0 stream and show what it delivers
 *
 *     oylng [--events | --json | --emit [--flow] [--indent N]] [--limit N] [FILE]
 *
 * --events (the default) writes the events in the suite's notation;
 * --json writes one line of JSON per document; --emit writes the stream
 * again as YAML 2.0, its references resolved, in block style or with
 * --flow one line per document. With no FILE, or with -,
 * reads standard input. On an error, writes it to standard error with its
 * line and column, and exits with 1.
 */

#include "oylng/oylng.h"
#include "events.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *f, size_t *len) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    if (ferror(f)) { free(buf); return NULL; }
    *len = n;
    return buf;
}

/* A JSON string */
static void json_str(strbuf *b, const char *s, size_t n) {
    sb_str(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        switch (c) {
        case '"':  sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        default:
            if (c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); sb_str(b, esc); }
            else sb_add(b, s + i, 1);
        }
    }
    sb_str(b, "\"");
}

int main(int argc, char **argv) {
    int json = 0, emit = 0, flow = 0;
    unsigned indent = 2;
    unsigned long long limit = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1, emit = 0;
        else if (!strcmp(argv[i], "--events")) json = 0, emit = 0;
        else if (!strcmp(argv[i], "--emit")) emit = 1, json = 0;
        else if (!strcmp(argv[i], "--flow")) flow = 1;
        else if (!strcmp(argv[i], "--indent") && i + 1 < argc) indent = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: oylng [--events | --json | --emit [--flow] [--indent N]] [--limit N] [FILE]");
            return 0;
        } else if (!path) path = argv[i];
        else { fprintf(stderr, "oylng: one file at a time\n"); return 2; }
    }

    FILE *f = (!path || !strcmp(path, "-")) ? stdin : fopen(path, "rb");
    if (!f) { fprintf(stderr, "oylng: %s: %s\n", path, strerror(errno)); return 2; }
    size_t len = 0;
    char *in = read_all(f, &len);
    if (f != stdin) fclose(f);
    if (!in) { fprintf(stderr, "oylng: can't read %s\n", path ? path : "standard input"); return 2; }

    oylng_parser *p = oylng_parser_new(in, len);
    if (!p) { fputs("oylng: out of memory\n", stderr); return 2; }
    if (limit) oylng_parser_set_limit(p, limit);

    oylng_emitter *em = NULL;
    if (emit) {
        em = oylng_emitter_new();
        if (!em) { fputs("oylng: out of memory\n", stderr); return 2; }
        oylng_emitter_set_indent(em, indent);
        oylng_emitter_set_flow(em, flow);
    }

    strbuf out = { 0 };
    /* for JSON: in each open collection, whether a comma is due, and for a
       mapping whether its key is out and its value next */
    unsigned char comma[OYLNG_MAX_LEVELS * 2 + 2], ismap[OYLNG_MAX_LEVELS * 2 + 2], key[OYLNG_MAX_LEVELS * 2 + 2];
    int d = 0, rc = 0;
    const oylng_event *e;
    for (;;) {
        oylng_status st = oylng_parse_next(p, &e);
        if (st != OYLNG_OK) {
            fwrite(out.data, 1, out.len, stdout);
            oylng_mark m = oylng_parser_error_mark(p);
            fprintf(stderr, "%s:%zu:%zu: %s\n", path ? path : "<stdin>", m.line, m.col, oylng_parser_error(p));
            rc = 1;
            break;
        }
        if (em) {
            if (oylng_emit(em, e) != OYLNG_OK) {
                fprintf(stderr, "oylng: can't write it again: %s\n", oylng_emitter_error(em));
                rc = 1;
                break;
            }
            oylng_str piece;
            oylng_emitter_take(em, &piece);
            sb_add(&out, piece.data, piece.len);
        } else if (!json) ev_line(&out, e);
        else {
            int closing = e->type == OYLNG_EVT_MAP_END || e->type == OYLNG_EVT_SEQ_END;
            int node = e->type == OYLNG_EVT_SCALAR || e->type == OYLNG_EVT_MAP_START ||
                       e->type == OYLNG_EVT_SEQ_START;
            if (node && d > 0) {
                if (ismap[d] && key[d]) sb_str(&out, ": ");
                else if (comma[d]) sb_str(&out, ", ");
                if (ismap[d]) key[d] = !key[d];
                comma[d] = 1;
            }
            switch (e->type) {
            case OYLNG_EVT_SCALAR: json_str(&out, e->value.data, e->value.len); break;
            case OYLNG_EVT_MAP_START: sb_str(&out, "{"); d++; ismap[d] = 1; key[d] = 0; comma[d] = 0; break;
            case OYLNG_EVT_SEQ_START: sb_str(&out, "["); d++; ismap[d] = 0; comma[d] = 0; break;
            case OYLNG_EVT_DOC_END: sb_str(&out, "\n"); break;
            default: break;
            }
            if (closing) { sb_str(&out, e->type == OYLNG_EVT_MAP_END ? "}" : "]"); d--; }
        }
        if (e->type == OYLNG_EVT_STREAM_END) {
            fwrite(out.data, 1, out.len, stdout);
            size_t off;
            if (oylng_parser_stopped(p, &off))
                fprintf(stderr, "%s: stopped at ...: the rest starts at byte %zu\n", path ? path : "<stdin>", off);
            break;
        }
        if (out.len > 1 << 16) { fwrite(out.data, 1, out.len, stdout); out.len = 0; }
    }
    free(out.data);
    oylng_emitter_free(em);
    oylng_parser_free(p);
    free(in);
    return rc;
}
