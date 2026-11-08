/*
 * fuzz_parser.c — libFuzzer harness for the parser
 *
 * The first byte sets options: its low bits a size limit (from a few bytes
 * up to the default), and its high bit 300 levels instead of 256. The rest
 * is the stream. Every event sequence must be well formed: the stream and
 * each document, mapping and sequence close, in order, and a mapping holds
 * pairs.
 *
 *     clang -fsanitize=fuzzer,address,undefined -Iinclude src/oylng_*.c fuzz/fuzz_parser.c
 */

#include "oylng/oylng.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    unsigned opt = data[0];
    data++, size--;

    /* a copy of its own, so reading past the end is caught */
    char *in = malloc(size ? size : 1);
    if (!in) return 0;
    memcpy(in, data, size);

    oylng_parser *p = oylng_parser_new(in, size);
    if (!p) { free(in); return 0; }
    static const uint64_t limits[8] = { 0, 4, 16, 64, 256, 4096, 1 << 16, 0 };
    if (limits[opt & 7]) oylng_parser_set_limit(p, limits[opt & 7]);
    if (opt & 0x80) oylng_parser_set_levels(p, 300);

    /* for each open collection, whether it's a mapping and how many nodes
       it has had */
    static unsigned char ismap[OYLNG_MAX_LEVELS + 8];
    static size_t nodes[OYLNG_MAX_LEVELS + 8];
    int depth = 0, in_doc = 0, started = 0;
    const oylng_event *e;
    for (;;) {
        if (oylng_parse_next(p, &e) != OYLNG_OK) {
            if (!oylng_parser_error(p)) abort();
            break;
        }
        switch (e->type) {
        case OYLNG_EVT_STREAM_START:
            if (started) abort();
            started = 1;
            break;
        case OYLNG_EVT_DOC_START:
            if (in_doc || depth) abort();
            in_doc = 1;
            break;
        case OYLNG_EVT_DOC_END:
            if (!in_doc || depth) abort();
            in_doc = 0;
            break;
        case OYLNG_EVT_MAP_START:
        case OYLNG_EVT_SEQ_START:
            if (!in_doc || depth >= OYLNG_MAX_LEVELS) abort();
            if (depth) nodes[depth]++;
            depth++;
            ismap[depth] = e->type == OYLNG_EVT_MAP_START;
            nodes[depth] = 0;
            break;
        case OYLNG_EVT_MAP_END:
        case OYLNG_EVT_SEQ_END:
            if (!depth || ismap[depth] != (e->type == OYLNG_EVT_MAP_END)) abort();
            if (ismap[depth] && nodes[depth] % 2) abort();
            depth--;
            break;
        case OYLNG_EVT_SCALAR:
            if (!in_doc) abort();
            if (depth) nodes[depth]++;
            if (e->value.len && !e->value.data) abort();
            if (e->value.len) { volatile char c = e->value.data[e->value.len - 1]; (void)c; }
            break;
        case OYLNG_EVT_STREAM_END:
            if (in_doc || depth) abort();
            break;
        default:
            abort();
        }
        if (e->type == OYLNG_EVT_STREAM_END) break;
    }
    oylng_parser_free(p);
    free(in);
    return 0;
}
