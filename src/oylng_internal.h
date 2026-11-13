/*
 * oylng_internal.h — what the library's files share, and nothing public
 */

#ifndef OYLNG_INTERNAL_H
#define OYLNG_INTERNAL_H

#include "oylng/oylng.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Characters (section 3), for the parser and the emitter ─ */

#define OYLNG_BAD_CP UINT32_MAX

/* The code point at q, and its length; OYLNG_BAD_CP if it isn't UTF-8 */
static inline uint32_t oylng_utf8(const unsigned char *q, const unsigned char *e, size_t *len) {
    unsigned c = q[0];
    if (c < 0x80) { *len = 1; return c; }
    uint32_t cp;
    size_t n;
    if (c < 0xC2) return OYLNG_BAD_CP;
    else if (c < 0xE0) { n = 2; cp = c & 0x1F; }
    else if (c < 0xF0) { n = 3; cp = c & 0x0F; }
    else if (c < 0xF5) { n = 4; cp = c & 0x07; }
    else return OYLNG_BAD_CP;
    if ((size_t)(e - q) < n) return OYLNG_BAD_CP;
    for (size_t i = 1; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) return OYLNG_BAD_CP;
        cp = cp << 6 | (q[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return OYLNG_BAD_CP;
    if (cp >= 0xD800 && cp < 0xE000) return OYLNG_BAD_CP;
    *len = n;
    return cp;
}

/* Whether a character may appear (dq: inside double quotes). Line feeds
 * and carriage returns are breaks, and their callers deal with them. */
static inline bool oylng_allowed(uint32_t c, bool dq) {
    if (c < 0x80) return c == 0x09 || (c >= 0x20 && c < 0x7F) || (dq && c == 0x7F);
    if (c < 0xA0) return dq;
    if (c == 0x2028 || c == 0x2029 || c == 0xFEFF || c == 0xFFFE || c == 0xFFFF) return dq;
    return true;
}

/* Whether a byte may be in a name's fragment, after its # (section 4.3):
 * ASCII letters, digits and = */
static inline bool oylng_frag_char(unsigned c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '=';
}

/* Whether a character may be in a document's name before any fragment
 * (section 4.3): letters and digits, . _ - /, and beyond ASCII the ranges
 * XML 1.0 allows */
static inline bool oylng_name_cp(uint32_t u) {
    if (u < 0x80)
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
               u == '.' || u == '_' || u == '/' || u == '-';
    return u == 0xB7 || (u >= 0xC0 && u <= 0xD6) || (u >= 0xD8 && u <= 0xF6) ||
           (u >= 0xF8 && u <= 0x37D) || (u >= 0x37F && u <= 0x1FFF) ||
           (u >= 0x200C && u <= 0x200D) || (u >= 0x203F && u <= 0x2040) ||
           (u >= 0x2070 && u <= 0x218F) || (u >= 0x2C00 && u <= 0x2FEF) ||
           (u >= 0x3001 && u <= 0xD7FF) || (u >= 0xF900 && u <= 0xFDCF) ||
           (u >= 0xFDF0 && u <= 0xFEFE) || (u >= 0xFF00 && u <= 0xFFFD) ||
           (u >= 0x10000 && u <= 0xEFFFF);
}

/* ── Arena (oylng_arena.c, from Oyl's oyl_arena.c) ──────────
 * A bump allocator with no per-object bookkeeping. A mark releases
 * everything allocated after it; marks are released last taken first. */

typedef struct oylng_arena oylng_arena;

typedef struct {
    void  *block;
    size_t used;
} oylng_arena_mark;

oylng_arena     *oylng_arena_new(size_t initial_cap);
void            *oylng_arena_alloc(oylng_arena *a, size_t size, size_t align);
oylng_arena_mark oylng_arena_get_mark(const oylng_arena *a);
void             oylng_arena_release(oylng_arena *a, oylng_arena_mark m);
void             oylng_arena_reset(oylng_arena *a);   /* releases everything */
void             oylng_arena_free(oylng_arena *a);

/* ── Hashing (oylng_hash.c, from Oyl's oyl_hash.c) ─────────
 * The key index hashes with SipHash-1-3, the variant Python's and Rust's
 * tables use, under a secret key each parser takes from the system's
 * randomness. Without the key no one can tell where an entry goes, so no
 * stream can be written whose keys all collide (section 7.4). A table only
 * finds things, never orders them, so the key changes nothing a caller
 * sees. */

typedef struct {
    uint64_t k0, k1;
} oylng_hash_key;

/* A new secret key */
void oylng_hash_key_new(oylng_hash_key *key);

/* A hash being computed, fed in pieces */
typedef struct {
    uint64_t v0, v1, v2, v3;
    uint64_t buf, len;   /* the bytes not yet in a block, and how many in all */
} oylng_hasher;

#define OYLNG_ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

static inline void oylng_sip_round(oylng_hasher *h) {
    h->v0 += h->v1; h->v1 = OYLNG_ROTL64(h->v1, 13); h->v1 ^= h->v0; h->v0 = OYLNG_ROTL64(h->v0, 32);
    h->v2 += h->v3; h->v3 = OYLNG_ROTL64(h->v3, 16); h->v3 ^= h->v2;
    h->v0 += h->v3; h->v3 = OYLNG_ROTL64(h->v3, 21); h->v3 ^= h->v0;
    h->v2 += h->v1; h->v1 = OYLNG_ROTL64(h->v1, 17); h->v1 ^= h->v2; h->v2 = OYLNG_ROTL64(h->v2, 32);
}

static inline void oylng_sip_block(oylng_hasher *h, uint64_t m) {
    h->v3 ^= m;
    oylng_sip_round(h);
    h->v0 ^= m;
}

static inline void oylng_hash_start(oylng_hasher *h, const oylng_hash_key *key) {
    h->v0 = 0x736f6d6570736575u ^ key->k0;
    h->v1 = 0x646f72616e646f6du ^ key->k1;
    h->v2 = 0x6c7967656e657261u ^ key->k0;
    h->v3 = 0x7465646279746573u ^ key->k1;
    h->buf = h->len = 0;
}

/* SipHash reads its blocks little-endian, on every machine; compilers make
 * this one load where the machine is little-endian too */
static inline uint64_t oylng_load64le(const unsigned char *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
           (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static inline void oylng_hash_bytes(oylng_hasher *h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (; n && (h->len & 7); n--) {
        h->buf |= (uint64_t)*p++ << (8 * (h->len++ & 7));
        if (!(h->len & 7)) {
            oylng_sip_block(h, h->buf);
            h->buf = 0;
        }
    }
    for (; n >= 8; n -= 8, p += 8, h->len += 8) oylng_sip_block(h, oylng_load64le(p));
    for (; n; n--) h->buf |= (uint64_t)*p++ << (8 * (h->len++ & 7));
}

static inline uint64_t oylng_hash_end(oylng_hasher *h) {
    uint64_t b = h->len << 56 | h->buf;
    oylng_sip_block(h, b);
    h->v2 ^= 0xff;
    oylng_sip_round(h);
    oylng_sip_round(h);
    oylng_sip_round(h);
    return h->v0 ^ h->v1 ^ h->v2 ^ h->v3;
}

static inline uint64_t oylng_hash(const oylng_hash_key *key, const void *data, size_t n) {
    oylng_hasher h;
    oylng_hash_start(&h, key);
    oylng_hash_bytes(&h, data, n);
    return oylng_hash_end(&h);
}

/* ── Between the parser and the tree (oylng_parser.c) ────────
 * oylng_parse_tree reads a document's events with the parser in tree mode,
 * in which a node of the table comes as one event of its own, OYLNG__EVT_REF,
 * naming the node (value.len) where the table holds it (offset), instead of
 * as its events. The tree copies each node of the table it is given once,
 * through oylng__table_node and oylng__table_children, and shares the copy
 * at every place the node appears. */

#define OYLNG__EVT_REF 64

enum { OYLNG__T_STR, OYLNG__T_SEQ, OYLNG__T_MAP };

typedef struct {
    int         kind;   /* OYLNG__T_ */
    const char *data;   /* a string's text, valid while the parser's table is */
    size_t      len;
    size_t      pos;    /* where the table holds it */
    bool        plain;  /* a string written plain, or an empty value */
} oylng__tnode;

void         oylng__parser_tree_mode(oylng_parser *p, bool on);
void         oylng__parser_input(const oylng_parser *p, const char **in, size_t *len);
/* Whether the next event is outside a document: STREAM_START, DOC_START,
 * STREAM_END, or an error */
bool         oylng__parser_between(const oylng_parser *p);
/* Ends the stream with an error the parser then reports */
oylng_status oylng__parser_fail(oylng_parser *p, oylng_status st, const char *msg);
/* An entry for each node of the table, for a tree to note what it copied
 * where: (gen << 32) | its node in the tree, gen being the tree's own
 * number, given here, so no tree's entries are taken for another's. Get
 * it once a document's table is read; NULL if memory runs out. */
uint64_t    *oylng__tree_memo(oylng_parser *p, uint32_t *gen);
void         oylng__table_node(const oylng_parser *p, uint32_t i, oylng__tnode *out);
/* Calls fn for each child of the table's collection i, references followed:
 * a sequence's items, each -<< spread; a mapping's keys and values,
 * alternating, its merges applied. False if memory runs out. */
bool         oylng__table_children(const oylng_parser *p, uint32_t i,
                                   void (*fn)(void *, uint32_t), void *ctx);

/* ── Between the parser and the typed reader (part 2) ────────
 * A schema is read from the table's nodes. A type that reaches an entry
 * marks it, so that the parser refuses to replace it (part 2, section 8).
 * The typed reader keeps the table's compiled schemas with the parser,
 * which lets them go when it lets the table go. */

/* The node of the table's entry named k, references followed, and in *e
 * the entry's number; UINT32_MAX if the table has none */
uint32_t     oylng__table_entry(oylng_parser *p, const char *k, size_t kl, uint32_t *e);
void         oylng__entry_reach(oylng_parser *p, uint32_t e);
/* The table's entries, numbered in the order first defined: how many, and
 * entry e's name */
uint32_t     oylng__table_count(const oylng_parser *p);
void         oylng__table_entry_at(const oylng_parser *p, uint32_t e, const char **k, size_t *kl);
uint64_t     oylng__parser_limit(const oylng_parser *p);
/* The limit for part 2's counts, a schema's full size and typed: the
 * parser's limit, unless a test sets one for them alone (0 to unset) */
uint64_t     oylng__parser_counts_limit(const oylng_parser *p);
void         oylng__parser_set_counts_limit(oylng_parser *p, uint64_t limit);
/* oylng_schema_new, with counts_limit as above */
struct oylng_schema *oylng__schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit,
                                       uint64_t counts_limit);
bool         oylng__parser_cut(const oylng_parser *p);
void        *oylng__parser_ctx(const oylng_parser *p);
void         oylng__parser_set_ctx(oylng_parser *p, void *ctx, void (*free_fn)(void *));
/* As oylng__parser_fail, at an offset in the input */
oylng_status oylng__parser_fail_at(oylng_parser *p, oylng_status st, size_t offset, const char *msg);

/* The tree's side (oylng_tree.c). oylng__tree_read is oylng_parse_tree,
 * calling at_start with the document's name before reading the rest of
 * it: an error it gives ends the read. */
oylng_status oylng__tree_read(oylng_parser *p, oylng_tree **out,
                              oylng_status (*at_start)(void *ctx, oylng_str name), void *ctx);
/* Whether node n was copied from the table, so that it may be shared */
bool         oylng__node_shared(const oylng_tree *t, oylng_node n);
/* The number of the pair whose key is key in mapping map, or UINT32_MAX */
uint32_t     oylng__node_find(const oylng_tree *t, oylng_node map, oylng_str key);

/* Fails the emitter, as its own errors do (oylng_emitter.c) */
oylng_status oylng__emitter_fail(oylng_emitter *e, oylng_status st, const char *msg);

#endif /* OYLNG_INTERNAL_H */
