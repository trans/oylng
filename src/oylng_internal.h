/*
 * oylng_internal.h — what the library's files share, and nothing public
 */

#ifndef OYLNG_INTERNAL_H
#define OYLNG_INTERNAL_H

#include "oylng/oylng.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

#endif /* OYLNG_INTERNAL_H */
