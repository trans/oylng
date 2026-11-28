/*
 * events.h — events written as the YAML 2.0 suite writes them
 *
 * The suite (trans/yaml2, suite/README.md) follows yaml-test-suite's
 * notation with fewer events: +STR, +DOC (and +DOC !name), +MAP, +SEQ,
 * =VAL :text, or =VAL "text for a value that isn't plain in a stream
 * marked quotes or json, and their ends. Shared by the oylng tool and the
 * harness.
 */

#ifndef OYLNG_EVENTS_H
#define OYLNG_EVENTS_H

#include "oylng/oylng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *data;
    size_t len, cap;
} strbuf;

static inline void sb_add(strbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->len + n + 1) c *= 2;
        char *d = realloc(b->data, c);
        if (!d) { fputs("out of memory\n", stderr); exit(2); }
        b->data = d;
        b->cap = c;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static inline void sb_str(strbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

/* The code point at s, and its length (the parser has checked it's UTF-8) */
static inline unsigned ev_cp(const unsigned char *s, size_t n, size_t *len) {
    if (s[0] < 0x80 || n < 2) { *len = 1; return s[0]; }
    if (s[0] < 0xE0) { *len = 2; return (s[0] & 0x1Fu) << 6 | (s[1] & 0x3Fu); }
    if (s[0] < 0xF0 || n < 4) {
        *len = 3;
        return (s[0] & 0x0Fu) << 12 | (s[1] & 0x3Fu) << 6 | (s[2] & 0x3Fu);
    }
    *len = 4;
    return (s[0] & 0x07u) << 18 | (s[1] & 0x3Fu) << 12 | (s[2] & 0x3Fu) << 6 | (s[3] & 0x3Fu);
}

/* The characters the suite writes as \u and four hex digits */
static inline int ev_escaped(unsigned c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xA0 || c == 0xAD || c == 0x1680 ||
           c == 0x180E || (c >= 0x2000 && c <= 0x200F) || (c >= 0x2028 && c <= 0x202F) ||
           (c >= 0x205F && c <= 0x206F) || c == 0x3000 || c == 0xFEFF ||
           (c >= 0xFFF9 && c <= 0xFFFB) || c == 0xFFFE || c == 0xFFFF ||
           /* its seven marks: ␣ » — ← ⇔ ↵ ∎ */
           c == 0x2423 || c == 0xBB || c == 0x2014 || c == 0x2190 || c == 0x21D4 ||
           c == 0x21B5 || c == 0x220E;
}

static inline void ev_esc(strbuf *b, const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        size_t len;
        unsigned c = ev_cp(u + i, n - i, &len);
        const char *named = NULL;
        switch (c) {
        case '\\': named = "\\\\"; break;
        case 0:    named = "\\0"; break;
        case '\b': named = "\\b"; break;
        case '\t': named = "\\t"; break;
        case '\n': named = "\\n"; break;
        case '\r': named = "\\r"; break;
        }
        if (named) sb_str(b, named);
        else if (ev_escaped(c)) {
            char hex[8];
            snprintf(hex, sizeof hex, "\\u%04X", c);
            sb_str(b, hex);
        } else sb_add(b, s + i, len);
        i += len;
    }
}

/* Where the events written are: whether the document's stream is marked
 * quotes or json, and, for each collection open, whether it is a mapping
 * and how many nodes it has had, so that a mapping's key is known. The
 * events go through ev_line in order, from a document's start. */
static struct {
    bool      marked;
    uint64_t *n;      /* each one's nodes, times 2, plus 1 for a mapping */
    size_t    depth, cap;
} ev_at;

static inline void ev_node(void) {
    if (ev_at.depth) ev_at.n[ev_at.depth - 1] += 2;
}

static inline void ev_open(bool map) {
    ev_node();
    if (ev_at.depth == ev_at.cap) {
        size_t c = ev_at.cap ? ev_at.cap * 2 : 64;
        uint64_t *n = realloc(ev_at.n, c * sizeof *n);
        if (!n) { fputs("out of memory\n", stderr); exit(2); }
        ev_at.n = n;
        ev_at.cap = c;
    }
    ev_at.n[ev_at.depth++] = map;
}

/* One event, as a line of the suite's test.event: in a stream marked
 * quotes or json, a value that isn't plain is =VAL " (suite/README.md) */
static inline void ev_line(strbuf *b, const oylng_event *e) {
    switch (e->type) {
    case OYLNG_EVT_STREAM_START: sb_str(b, "+STR\n"); break;
    case OYLNG_EVT_STREAM_END:   sb_str(b, "-STR\n"); break;
    case OYLNG_EVT_DOC_START:
        ev_at.marked = e->word != OYLNG_WORD_NONE;
        ev_at.depth = 0;
        sb_str(b, "+DOC");
        if (e->value.data) { sb_str(b, " !"); ev_esc(b, e->value.data, e->value.len); }
        sb_str(b, "\n");
        break;
    case OYLNG_EVT_DOC_END:   sb_str(b, "-DOC\n"); break;
    case OYLNG_EVT_MAP_START: ev_open(true); sb_str(b, "+MAP\n"); break;
    case OYLNG_EVT_SEQ_START: ev_open(false); sb_str(b, "+SEQ\n"); break;
    case OYLNG_EVT_MAP_END:
    case OYLNG_EVT_SEQ_END:
        if (ev_at.depth) ev_at.depth--;
        sb_str(b, e->type == OYLNG_EVT_MAP_END ? "-MAP\n" : "-SEQ\n");
        break;
    case OYLNG_EVT_SCALAR: {
        uint64_t n = ev_at.depth ? ev_at.n[ev_at.depth - 1] : 0;
        bool key = (n & 1) && (n >> 1) % 2 == 0;
        ev_node();
        sb_str(b, ev_at.marked && !key && !e->plain ? "=VAL \"" : "=VAL :");
        ev_esc(b, e->value.data, e->value.len);
        sb_str(b, "\n");
        break;
    }
    default: break;
    }
}

#endif
