/*
 * oylng — read a YAML 2.0 stream and show what it delivers
 *
 *     oylng [--events | --json | --emit [--flow] [--indent N]] [--limit N] [FILE]
 *     oylng --typed [--schema SCHEMA ROOT] [--cut] [--limit N] [FILE]
 *
 * --events (the default) writes the events in the suite's notation;
 * --json writes one line of JSON per document; --emit writes the stream
 * again as YAML 2.0, its references resolved, in block style or with
 * --flow one line per document. --typed types each document (part 2, YAML
 * Easy Schema) by its stamp, or else by the entry ROOT of the file SCHEMA,
 * and writes one line of JSON per document, each value [type, value]; a
 * stream with a table and no documents has every entry checked as a root.
 * --cut cuts a time's fraction to the digits the engine holds;
 * --typed-limit N, for tests, sets the size limit for part 2's counts
 * alone (a schema's full size, and typed), as yaml2's checker has them,
 * where --limit sets it for the stream too. With no
 * FILE, or with -, reads standard input. On an error, writes it to
 * standard error with its line and column, and exits with 1.
 */

#include "oylng/oylng.h"
#include "oylng/schema.h"
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

/* The library's own, for --typed-limit */
void oylng__parser_set_counts_limit(oylng_parser *p, uint64_t limit);
oylng_schema *oylng__schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit,
                                uint64_t counts_limit);

/* A tree's node as JSON: strings, arrays and objects */
static void tree_json(strbuf *b, const oylng_tree *t, oylng_node n) {
    size_t c = oylng_node_count(t, n);
    oylng_str s;
    switch (oylng_node_kind(t, n)) {
    case OYLNG_NODE_STRING:
        s = oylng_node_text(t, n);
        json_str(b, s.data, s.len);
        break;
    case OYLNG_NODE_SEQUENCE:
        sb_str(b, "[");
        for (size_t i = 0; i < c; i++) {
            if (i) sb_str(b, ", ");
            tree_json(b, t, oylng_node_item(t, n, i));
        }
        sb_str(b, "]");
        break;
    case OYLNG_NODE_MAPPING:
        sb_str(b, "{");
        for (size_t i = 0; i < c; i++) {
            if (i) sb_str(b, ", ");
            s = oylng_node_text(t, oylng_node_key(t, n, i));
            json_str(b, s.data, s.len);
            sb_str(b, ": ");
            tree_json(b, t, oylng_node_value(t, n, i));
        }
        sb_str(b, "}");
        break;
    default: sb_str(b, "null");
    }
}

/* A typed value as JSON: [type, value], the value a string except for
 * booleans, null, and collections */
static void typed_json(strbuf *b, const oylng_typed *t, oylng_value v) {
    static const char *const tag[] = {
        [OYLNG_T_ANY] = "any", [OYLNG_T_STR] = "str", [OYLNG_T_INT] = "int",
        [OYLNG_T_BIGINT] = "bigint", [OYLNG_T_FLOAT] = "float",
        [OYLNG_T_DECIMAL] = "decimal", [OYLNG_T_BOOL] = "bool", [OYLNG_T_NULL] = "null",
        [OYLNG_T_DATE] = "date", [OYLNG_T_DATETIME] = "datetime",
        [OYLNG_T_BINARY] = "binary", [OYLNG_T_SEQUENCE] = "seq", [OYLNG_T_MAPPING] = "map",
    };
    oylng_type ty = oylng_value_type(t, v);
    if (ty == OYLNG_T_NONE || ty > OYLNG_T_MAPPING) { sb_str(b, "null"); return; }
    sb_str(b, "[\"");
    sb_str(b, tag[ty]);
    sb_str(b, "\", ");
    char num[40];
    oylng_str s = oylng_value_text(t, v);
    size_t c = oylng_value_count(t, v);
    switch (ty) {
    case OYLNG_T_ANY: {
        const oylng_tree *tr = oylng_typed_tree(t);
        tree_json(b, tr, oylng_value_node(t, v));
        break;
    }
    case OYLNG_T_INT: {
        int64_t i;
        uint64_t u;
        if (oylng_value_int(t, v, &i)) snprintf(num, sizeof num, "%lld", (long long)i);
        else if (oylng_value_uint(t, v, &u)) snprintf(num, sizeof num, "%llu", (unsigned long long)u);
        else snprintf(num, sizeof num, "?");
        json_str(b, num, strlen(num));
        break;
    }
    case OYLNG_T_FLOAT:
        snprintf(num, sizeof num, "%.17g", oylng_value_float(t, v));
        json_str(b, num, strlen(num));
        break;
    case OYLNG_T_BOOL: sb_str(b, oylng_value_bool(t, v) ? "true" : "false"); break;
    case OYLNG_T_NULL: sb_str(b, "null"); break;
    case OYLNG_T_BINARY: {
        /* its text, then its bytes in hex */
        json_str(b, s.data, s.len);
        oylng_str by = oylng_value_bytes(t, v);
        sb_str(b, ", \"");
        for (size_t i = 0; i < by.len; i++) {
            snprintf(num, sizeof num, "%02x", (unsigned char)by.data[i]);
            sb_str(b, num);
        }
        sb_str(b, "\"");
        break;
    }
    case OYLNG_T_SEQUENCE:
        sb_str(b, "[");
        for (size_t i = 0; i < c; i++) {
            if (i) sb_str(b, ", ");
            typed_json(b, t, oylng_value_item(t, v, i));
        }
        sb_str(b, "]");
        break;
    case OYLNG_T_MAPPING:
        sb_str(b, "{");
        for (size_t i = 0; i < c; i++) {
            if (i) sb_str(b, ", ");
            oylng_str k = oylng_value_key(t, v, i);
            json_str(b, k.data, k.len);
            sb_str(b, ": ");
            typed_json(b, t, oylng_value_at(t, v, i));
        }
        sb_str(b, "}");
        break;
    default:   /* str, bigint, decimal, date, datetime: the text */
        json_str(b, s.data, s.len);
    }
    sb_str(b, "]");
}

/* --typed */
static int run_typed(oylng_parser *p, const char *in, size_t len, const char *path,
                     const oylng_schema *app, uint64_t limit, uint64_t counts_limit) {
    strbuf out = { 0 };
    oylng_typed *t;
    size_t docs = 0;
    int rc = 0;
    for (;;) {
        oylng_status st = oylng_parse_typed(p, app, &t);
        if (st != OYLNG_OK) {
            fwrite(out.data, 1, out.len, stdout);
            out.len = 0;
            oylng_mark m = oylng_parser_error_mark(p);
            fprintf(stderr, "%s:%zu:%zu: %s\n", path ? path : "<stdin>", m.line, m.col, oylng_parser_error(p));
            rc = 1;
            break;
        }
        if (!t) break;
        docs++;
        typed_json(&out, t, oylng_typed_root(t));
        sb_str(&out, "\n");
        oylng_typed_free(t);
        if (out.len > 1 << 16) { fwrite(out.data, 1, out.len, stdout); out.len = 0; }
    }
    fwrite(out.data, 1, out.len, stdout);
    free(out.data);
    if (!rc && !docs) {
        /* a schema file: every entry as a root */
        oylng_schema *all = oylng__schema_new(in, len, (oylng_str){ NULL, 0 }, limit, counts_limit);
        if (!all) { fputs("oylng: out of memory\n", stderr); return 2; }
        if (oylng_schema_status(all) != OYLNG_OK) {
            oylng_mark m = oylng_schema_error_mark(all);
            fprintf(stderr, "%s:%zu:%zu: %s\n", path ? path : "<stdin>", m.line, m.col, oylng_schema_error(all));
            rc = 1;
        }
        oylng_schema_free(all);
    }
    return rc;
}

int main(int argc, char **argv) {
    int json = 0, emit = 0, flow = 0, typed = 0, cut = 0;
    const char *schema_path = NULL, *schema_root = NULL;
    unsigned indent = 2;
    unsigned long long limit = 0, counts_limit = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1, emit = 0;
        else if (!strcmp(argv[i], "--events")) json = 0, emit = 0;
        else if (!strcmp(argv[i], "--emit")) emit = 1, json = 0;
        else if (!strcmp(argv[i], "--flow")) flow = 1;
        else if (!strcmp(argv[i], "--typed")) typed = 1, json = 0, emit = 0;
        else if (!strcmp(argv[i], "--schema") && i + 2 < argc) schema_path = argv[++i], schema_root = argv[++i];
        else if (!strcmp(argv[i], "--cut")) cut = 1;
        else if (!strcmp(argv[i], "--typed-limit") && i + 1 < argc) counts_limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--indent") && i + 1 < argc) indent = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: oylng [--events | --json | --emit [--flow] [--indent N]] [--limit N] [FILE]\n"
                 "       oylng --typed [--schema SCHEMA ROOT] [--cut] [--limit N] [FILE]");
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

    if (typed) {
        oylng_schema *app = NULL;
        if (schema_path) {
            FILE *sf = fopen(schema_path, "rb");
            if (!sf) { fprintf(stderr, "oylng: %s: %s\n", schema_path, strerror(errno)); return 2; }
            size_t slen = 0;
            char *stext = read_all(sf, &slen);
            fclose(sf);
            if (!stext) { fprintf(stderr, "oylng: can't read %s\n", schema_path); return 2; }
            app = oylng__schema_new(stext, slen, (oylng_str){ schema_root, strlen(schema_root) }, limit, counts_limit);
            free(stext);
            if (!app) { fputs("oylng: out of memory\n", stderr); return 2; }
            if (oylng_schema_status(app) != OYLNG_OK) {
                oylng_mark m = oylng_schema_error_mark(app);
                fprintf(stderr, "%s:%zu:%zu: %s\n", schema_path, m.line, m.col, oylng_schema_error(app));
                oylng_schema_free(app);
                oylng_parser_free(p);
                free(in);
                return 1;
            }
        }
        oylng_parser_set_cut(p, cut);
        oylng__parser_set_counts_limit(p, counts_limit);
        int rc = run_typed(p, in, len, path, app, limit, counts_limit);
        oylng_schema_free(app);
        oylng_parser_free(p);
        free(in);
        return rc;
    }

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
