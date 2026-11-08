/*
 * oylng_arena.c — Bump allocator with block chaining (from Oyl's oyl_arena.c)
 *
 * All allocations during a parse go through here.
 * One free at the end. No per-object bookkeeping.
 */

#include "oylng_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Block ───────────────────────────────────────────────── */

typedef struct oylng_block {
    struct oylng_block *next;
    size_t            cap;
    size_t            used;
    /* data follows immediately */
} oylng_block;

#define BLOCK_DATA(b) ((char *)(b) + sizeof(oylng_block))

struct oylng_arena {
    oylng_block *head;     /* current block (allocates from here) */
    oylng_block *blocks;   /* all blocks (for freeing) */
    size_t     default_cap;
};

/* ── Internal ────────────────────────────────────────────── */

static oylng_block *block_new(size_t cap) {
    if (cap > SIZE_MAX - sizeof(oylng_block)) return NULL;
    oylng_block *b = (oylng_block *)malloc(sizeof(oylng_block) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

/* ── Public API ──────────────────────────────────────────── */

oylng_arena *oylng_arena_new(size_t initial_cap) {
    if (initial_cap < 4096) initial_cap = 4096;

    oylng_arena *a = (oylng_arena *)malloc(sizeof(oylng_arena));
    if (!a) return NULL;

    a->head = block_new(initial_cap);
    if (!a->head) { free(a); return NULL; }

    a->blocks      = a->head;
    a->default_cap = initial_cap;
    return a;
}

/* A test build (OYLNG_ARENA_EACH) gives every request a block of its own:
 * each is then a malloc, which the allocation failure test can fail. */
#ifdef OYLNG_ARENA_EACH
#define ARENA_EACH 1
#else
#define ARENA_EACH 0
#endif

/* Offset within block b at or after `from` whose address is aligned. */
static size_t aligned_offset(const oylng_block *b, size_t from, size_t align) {
    uintptr_t addr = (uintptr_t)BLOCK_DATA(b) + from;
    return from + (size_t)((align - (addr & (align - 1))) & (align - 1));
}

void *oylng_arena_alloc(oylng_arena *a, size_t size, size_t align) {
    if (align == 0) align = 1;
    if (align & (align - 1)) return NULL;            /* not a power of two */
    if (size > SIZE_MAX - align) return NULL;        /* size + padding overflows */

    oylng_block *b = a->head;
    size_t aligned = aligned_offset(b, b->used, align);

    if (ARENA_EACH || aligned > b->cap || size > b->cap - aligned) {
        /* need a new block — at least double or fit the request */
        size_t cap = a->default_cap;
        if (cap < size + align) cap = size + align;
        if (b->cap <= SIZE_MAX / 2 && cap < b->cap * 2) cap = b->cap * 2;
        if (ARENA_EACH) cap = size + align;

        oylng_block *nb = block_new(cap);
        if (!nb) return NULL;

        nb->next  = a->blocks;
        a->blocks = nb;
        a->head   = nb;
        b = nb;
        aligned = aligned_offset(b, 0, align);
    }

    void *ptr = BLOCK_DATA(b) + aligned;
    b->used = aligned + size;
    return ptr;
}

void oylng_arena_free(oylng_arena *a) {
    if (!a) return;
    oylng_block *b = a->blocks;
    while (b) {
        oylng_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}
