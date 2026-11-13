/*
 * oylng_tree.c — a document as a tree of nodes
 *
 * The parser gives a document's events in tree mode, where a node of the
 * table comes as one event naming it. Each such node is copied from the
 * table once, the first time it is named, and then shared: the tree is a
 * graph without cycles, as large as what is written.
 *
 * A node is 16 bytes. A collection's children are in one array, kids, a
 * mapping's alternating key and value; a mapping of more than SMALL pairs
 * has a hash table after them. A string's text is in the input where it
 * was there, else in the tree's pool. Offsets are 32 bits, so a tree's
 * input is less than 4 GiB.
 *
 * Nothing recurses: a collection's children are gathered on a stack until
 * it closes, and a node of the table is copied through a list of the
 * collections still to fill.
 */

#include "oylng_internal.h"
#include <stdlib.h>
#include <string.h>

#define SMALL 8          /* up to this many pairs, keys are compared one by one */
#define F_POOLED 1u      /* a string's text is in the pool */
#define F_PLAIN  2u      /* a string written plain, or an empty value */
#define F_TABLE  4u      /* copied from the table, so it may be shared */

typedef struct {
    uint32_t a;          /* STR: where its text starts. SEQ, MAP: where its kids start */
    uint32_t n;          /* STR: its length. SEQ: its items. MAP: its pairs */
    uint32_t pos;        /* where it is written in the input */
    uint8_t  kind;       /* oylng_kind */
    uint8_t  flags;
    uint16_t unused;
} tnode;

struct oylng_tree {
    const char    *in;
    size_t         inlen;
    tnode         *nodes;
    size_t         nn, capn;
    uint32_t      *kids;
    size_t         nk, capk;
    char          *pool;
    size_t         npool, cappool;
    oylng_node     root;
    bool           has_name;
    uint8_t        name_flags;
    uint32_t       name_a, name_n;
    bool           keyed;
    oylng_hash_key key;
};

/* What is needed only while the tree is read */
typedef struct {
    oylng_tree   *T;
    oylng_parser *P;
    oylng_status  st;
    const char   *msg;
    uint32_t     *stk;            /* the children of the collections open */
    size_t        ns, caps;
    struct open { oylng_node id; size_t s0; } *open;
    size_t        no, capo;
    uint64_t     *memo;           /* the parser's: see oylng__tree_memo */
    uint32_t      gen;
    struct todo { uint32_t from; oylng_node id; } *todo;   /* to fill */
    size_t        nt, capt;
    void         *grown;          /* what room_for gives */
} builder;

static void oops(builder *B, oylng_status st, const char *msg) {
    if (B->st != OYLNG_OK) return;
    B->st = st;
    B->msg = msg;
}

/* Array p grown to room for need elements, in B->grown: false if memory
 * ran out, and p is as it was */
static bool room_for(builder *B, void *p, size_t *cap, size_t need, size_t elem) {
    size_t c = *cap ? *cap : 16;
    while (c < need) {
        if (c > SIZE_MAX / 2 / elem) { oops(B, OYLNG_ERR_MEMORY, "out of memory"); return false; }
        c *= 2;
    }
    void *np = realloc(p, c * elem);
    if (!np) { oops(B, OYLNG_ERR_MEMORY, "out of memory"); return false; }
    B->grown = np;
    *cap = c;
    return true;
}

#define ROOM(B, p, cap, need) \
    ((need) <= (cap) || (room_for((B), (p), &(cap), (need), sizeof *(p)) && ((p) = (B)->grown, true)))

static oylng_node new_node(builder *B, uint8_t kind, size_t pos) {
    oylng_tree *T = B->T;
    if (T->nn >= UINT32_MAX - 1) { oops(B, OYLNG_ERR_LIMIT, "a tree of 4G nodes or more"); return OYLNG_NO_NODE; }
    if (!ROOM(B, T->nodes, T->capn, T->nn + 1)) return OYLNG_NO_NODE;
    tnode *nd = &T->nodes[T->nn];
    *nd = (tnode){ 0, 0, (uint32_t)pos, kind, 0, 0 };
    return (oylng_node)T->nn++;
}

/* Where a text is: in the input if it is there, else copied to the pool */
static bool place_text(builder *B, const char *data, size_t len, uint32_t *a, uint8_t *flags) {
    oylng_tree *T = B->T;
    uintptr_t d = (uintptr_t)data, s = (uintptr_t)T->in;
    *flags = 0;
    if (!len) { *a = 0; return true; }
    if (d >= s && d - s <= T->inlen && len <= T->inlen - (d - s)) {
        *a = (uint32_t)(d - s);
        return true;
    }
    if (len > UINT32_MAX - T->npool) { oops(B, OYLNG_ERR_LIMIT, "a tree of 4 GiB of text or more"); return false; }
    if (!ROOM(B, T->pool, T->cappool, T->npool + len)) return false;
    memcpy(T->pool + T->npool, data, len);
    *a = (uint32_t)T->npool;
    *flags = F_POOLED;
    T->npool += len;
    return true;
}

static oylng_node new_str(builder *B, const char *data, size_t len, size_t pos, uint8_t flags) {
    if (len > UINT32_MAX) { oops(B, OYLNG_ERR_LIMIT, "a string of 4 GiB or more"); return OYLNG_NO_NODE; }
    oylng_node id = new_node(B, OYLNG_NODE_STRING, pos);
    if (id == OYLNG_NO_NODE) return id;
    uint32_t a;
    uint8_t fl;
    if (!place_text(B, data, len, &a, &fl)) return OYLNG_NO_NODE;
    tnode *nd = &B->T->nodes[id];
    nd->a = a;
    nd->n = (uint32_t)len;
    nd->flags = fl | flags;
    return id;
}

static void push_child(builder *B, oylng_node id) {
    if (!ROOM(B, B->stk, B->caps, B->ns + 1)) return;
    B->stk[B->ns++] = id;
}

static inline oylng_str text_of(const oylng_tree *T, const tnode *nd) {
    return (oylng_str){ (nd->flags & F_POOLED ? T->pool : T->in) + nd->a, nd->n };
}

static size_t index_cap(size_t pairs) {
    size_t c = 16;
    while (c < 2 * pairs) c *= 2;
    return c;
}

static uint64_t key_hash(const oylng_tree *T, oylng_str k) {
    return oylng_hash(&T->key, k.data, k.len);
}

/* Collection id closes with the children on the stack from s0 */
static void close_coll(builder *B, oylng_node id, size_t s0) {
    oylng_tree *T = B->T;
    size_t nkids = B->ns - s0;
    bool map = T->nodes[id].kind == OYLNG_NODE_MAPPING;
    size_t pairs = map ? nkids / 2 : nkids;
    size_t room = nkids + (map && pairs > SMALL ? index_cap(pairs) : 0);
    if (room > UINT32_MAX - T->nk) { oops(B, OYLNG_ERR_LIMIT, "a tree of 4G children or more"); return; }
    tnode *nd = &T->nodes[id];
    nd->a = (uint32_t)T->nk;
    nd->n = (uint32_t)pairs;
    B->ns = s0;
    if (!nkids) return;   /* and kids may be NULL */
    if (!ROOM(B, T->kids, T->capk, T->nk + room)) return;
    uint32_t *k = T->kids + T->nk;
    memcpy(k, B->stk + s0, nkids * sizeof *k);
    T->nk += room;
    if (room == nkids) return;
    /* the index: each slot is a pair's number plus 1, or 0 */
    if (!T->keyed) {
        oylng_hash_key_new(&T->key);
        T->keyed = true;
    }
    size_t cap = room - nkids;
    uint32_t *slot = k + nkids;
    memset(slot, 0, cap * sizeof *slot);
    for (size_t i = 0; i < pairs; i++) {
        size_t h = (size_t)key_hash(T, text_of(T, &T->nodes[k[2 * i]])) & (cap - 1);
        while (slot[h]) h = (h + 1) & (cap - 1);
        slot[h] = (uint32_t)i + 1;
    }
}

/* The tree's node for node i of the table: made now, if it is new, and a
 * collection put on the list to fill */
static oylng_node table_node(builder *B, uint32_t i) {
    if (!B->memo && !(B->memo = oylng__tree_memo(B->P, &B->gen))) {
        oops(B, OYLNG_ERR_MEMORY, "out of memory");
        return OYLNG_NO_NODE;
    }
    uint64_t m = B->memo[i];
    if ((uint32_t)(m >> 32) == B->gen) return (oylng_node)m;
    oylng__tnode tn;
    oylng__table_node(B->P, i, &tn);
    oylng_node id;
    if (tn.kind == OYLNG__T_STR) id = new_str(B, tn.data, tn.len, tn.pos, F_TABLE | (tn.plain ? F_PLAIN : 0));
    else {
        id = new_node(B, tn.kind == OYLNG__T_SEQ ? OYLNG_NODE_SEQUENCE : OYLNG_NODE_MAPPING, tn.pos);
        if (id != OYLNG_NO_NODE) B->T->nodes[id].flags = F_TABLE;
        if (id != OYLNG_NO_NODE && ROOM(B, B->todo, B->capt, B->nt + 1))
            B->todo[B->nt++] = (struct todo){ i, id };
    }
    if (id == OYLNG_NO_NODE) return id;
    B->memo[i] = (uint64_t)B->gen << 32 | id;
    return id;
}

static void table_child(void *ctx, uint32_t i) {
    builder *B = ctx;
    if (B->st != OYLNG_OK) return;
    oylng_node id = table_node(B, i);
    if (id != OYLNG_NO_NODE) push_child(B, id);
}

/* The tree's node for node i of the table, filled */
static oylng_node from_table(builder *B, uint32_t i) {
    oylng_node id = table_node(B, i);
    while (B->nt && B->st == OYLNG_OK) {
        struct todo t = B->todo[--B->nt];
        size_t s0 = B->ns;
        if (!oylng__table_children(B->P, t.from, table_child, B)) oops(B, OYLNG_ERR_MEMORY, "out of memory");
        if (B->st == OYLNG_OK) close_coll(B, t.id, s0);
    }
    return id;
}

/* The document's events, after its DOC_START */
static void read_doc(builder *B) {
    oylng_tree *T = B->T;
    const oylng_event *e;
    for (;;) {
        oylng_status st = oylng_parse_next(B->P, &e);
        if (st != OYLNG_OK) { B->st = st; return; }
        oylng_node id;
        switch ((int)e->type) {
        case OYLNG_EVT_SCALAR:
            id = new_str(B, e->value.data, e->value.len, e->offset, e->plain ? F_PLAIN : 0);
            break;
        case OYLNG__EVT_REF:
            id = from_table(B, (uint32_t)e->value.len);
            break;
        case OYLNG_EVT_MAP_START:
        case OYLNG_EVT_SEQ_START:
            id = new_node(B, e->type == OYLNG_EVT_MAP_START ? OYLNG_NODE_MAPPING : OYLNG_NODE_SEQUENCE,
                          e->offset);
            if (id == OYLNG_NO_NODE) return;
            if (B->no) push_child(B, id);
            else T->root = id;
            if (!ROOM(B, B->open, B->capo, B->no + 1)) return;
            B->open[B->no++] = (struct open){ id, B->ns };
            if (B->st != OYLNG_OK) return;
            continue;
        case OYLNG_EVT_MAP_END:
        case OYLNG_EVT_SEQ_END: {
            struct open o = B->open[--B->no];
            close_coll(B, o.id, o.s0);
            if (B->st != OYLNG_OK) return;
            continue;
        }
        case OYLNG_EVT_DOC_END:
            return;
        default:
            oops(B, OYLNG_ERR_INPUT, "the parser gave an event a document can't have");
            return;
        }
        if (id == OYLNG_NO_NODE) return;
        if (B->no) push_child(B, id);
        else T->root = id;
        if (B->st != OYLNG_OK) return;
    }
}

/* ── Public API ──────────────────────────────────────────── */

void oylng_tree_free(oylng_tree *T) {
    if (!T) return;
    free(T->nodes);
    free(T->kids);
    free(T->pool);
    free(T);
}

oylng_status oylng_parse_tree(oylng_parser *P, oylng_tree **out) {
    return oylng__tree_read(P, out, NULL, NULL);
}

oylng_status oylng__tree_read(oylng_parser *P, oylng_tree **out,
                              oylng_status (*at_start)(void *ctx, oylng_str name), void *ctx) {
    if (!out) return OYLNG_ERR_INPUT;
    *out = NULL;
    if (!P || !oylng__parser_between(P)) return OYLNG_ERR_INPUT;
    builder B = { .P = P, .st = OYLNG_OK };
    const char *in;
    size_t len;
    oylng__parser_input(P, &in, &len);
    if (len >= UINT32_MAX) return oylng__parser_fail(P, OYLNG_ERR_LIMIT, "a tree's input is less than 4 GiB");
    oylng__parser_tree_mode(P, true);
    const oylng_event *e;
    oylng_status st = oylng_parse_next(P, &e);
    if (st == OYLNG_OK && e->type == OYLNG_EVT_STREAM_START) st = oylng_parse_next(P, &e);
    if (st != OYLNG_OK || e->type == OYLNG_EVT_STREAM_END) {
        oylng__parser_tree_mode(P, false);
        return st;
    }
    B.T = calloc(1, sizeof *B.T);
    if (!B.T) oops(&B, OYLNG_ERR_MEMORY, "out of memory");
    else {
        B.T->in = in;
        B.T->inlen = len;
        B.T->root = OYLNG_NO_NODE;
        if (e->value.data) {
            B.T->has_name = true;
            B.T->name_n = (uint32_t)e->value.len;
            place_text(&B, e->value.data, e->value.len, &B.T->name_a, &B.T->name_flags);
        }
        if (B.st == OYLNG_OK && at_start) {
            st = at_start(ctx, e->value);
            if (st != OYLNG_OK) B.st = st;
        }
        if (B.st == OYLNG_OK) read_doc(&B);
    }
    oylng__parser_tree_mode(P, false);
    free(B.stk);
    free(B.open);
    free(B.todo);
    if (B.st != OYLNG_OK) {
        oylng_tree_free(B.T);
        /* an error of the parser's is already its error; the tree's
         * becomes the parser's */
        return B.msg ? oylng__parser_fail(P, B.st, B.msg) : B.st;
    }
    *out = B.T;
    return OYLNG_OK;
}

oylng_node oylng_tree_root(const oylng_tree *T) { return T ? T->root : OYLNG_NO_NODE; }

oylng_str oylng_tree_name(const oylng_tree *T) {
    if (!T || !T->has_name) return (oylng_str){ NULL, 0 };
    return (oylng_str){ (T->name_flags & F_POOLED ? T->pool : T->in) + T->name_a, T->name_n };
}

size_t oylng_tree_nodes(const oylng_tree *T) { return T ? T->nn : 0; }

static const tnode *get(const oylng_tree *T, oylng_node n) {
    return T && n < T->nn ? &T->nodes[n] : NULL;
}

oylng_kind oylng_node_kind(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    return nd ? (oylng_kind)nd->kind : OYLNG_NODE_NONE;
}

oylng_str oylng_node_text(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    if (!nd || nd->kind != OYLNG_NODE_STRING) return (oylng_str){ NULL, 0 };
    return text_of(T, nd);
}

bool oylng_node_plain(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    return nd && nd->kind == OYLNG_NODE_STRING && (nd->flags & F_PLAIN);
}

bool oylng__node_shared(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    return nd && (nd->flags & F_TABLE);
}

size_t oylng_node_count(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    return nd && nd->kind != OYLNG_NODE_STRING ? nd->n : 0;
}

oylng_node oylng_node_item(const oylng_tree *T, oylng_node seq, size_t i) {
    const tnode *nd = get(T, seq);
    if (!nd || nd->kind != OYLNG_NODE_SEQUENCE || i >= nd->n) return OYLNG_NO_NODE;
    return T->kids[nd->a + i];
}

oylng_node oylng_node_key(const oylng_tree *T, oylng_node map, size_t i) {
    const tnode *nd = get(T, map);
    if (!nd || nd->kind != OYLNG_NODE_MAPPING || i >= nd->n) return OYLNG_NO_NODE;
    return T->kids[nd->a + 2 * i];
}

oylng_node oylng_node_value(const oylng_tree *T, oylng_node map, size_t i) {
    const tnode *nd = get(T, map);
    if (!nd || nd->kind != OYLNG_NODE_MAPPING || i >= nd->n) return OYLNG_NO_NODE;
    return T->kids[nd->a + 2 * i + 1];
}

static bool same(oylng_str a, oylng_str b) {
    return a.len == b.len && (!a.len || memcmp(a.data, b.data, a.len) == 0);
}

uint32_t oylng__node_find(const oylng_tree *T, oylng_node map, oylng_str key) {
    const tnode *nd = get(T, map);
    if (!nd || nd->kind != OYLNG_NODE_MAPPING || !nd->n || (key.len && !key.data)) return UINT32_MAX;
    const uint32_t *k = T->kids + nd->a;
    if (nd->n <= SMALL) {
        for (uint32_t i = 0; i < nd->n; i++)
            if (same(text_of(T, &T->nodes[k[2 * i]]), key)) return i;
        return UINT32_MAX;
    }
    size_t cap = index_cap(nd->n);
    const uint32_t *slot = k + 2 * (size_t)nd->n;
    for (size_t h = (size_t)key_hash(T, key) & (cap - 1); slot[h]; h = (h + 1) & (cap - 1)) {
        uint32_t i = slot[h] - 1;
        if (same(text_of(T, &T->nodes[k[2 * i]]), key)) return i;
    }
    return UINT32_MAX;
}

oylng_node oylng_node_get(const oylng_tree *T, oylng_node map, oylng_str key) {
    uint32_t i = oylng__node_find(T, map, key);
    return i == UINT32_MAX ? OYLNG_NO_NODE : T->kids[T->nodes[map].a + 2 * (size_t)i + 1];
}

oylng_mark oylng_node_mark(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    if (!nd) return (oylng_mark){ 0, 0, 0 };
    oylng_mark m = { nd->pos, 1, 1 };
    const char *q = T->in, *at = T->in + nd->pos, *line = q;
    while ((q = memchr(q, '\n', (size_t)(at - q)))) {
        m.line++;
        line = ++q;
    }
    m.col = (size_t)(at - line) + 1;
    return m;
}

/* ── Writing a tree ──────────────────────────────────────── */

oylng_status oylng_emit_tree(oylng_emitter *E, const oylng_tree *T) {
    if (!E || !T || T->root == OYLNG_NO_NODE) return OYLNG_ERR_INPUT;
    oylng_status st = oylng_emit_doc_start(E, oylng_tree_name(T));
    if (st != OYLNG_OK) return st;
    /* the collections open: each one's node and its next child */
    struct walk { oylng_node id; uint32_t next; } local[64], *stk = local;
    size_t n = 0, cap = 64;
    oylng_node id = T->root;
    for (;;) {
        const tnode *nd = &T->nodes[id];
        if (nd->kind == OYLNG_NODE_STRING) st = oylng_emit_scalar(E, text_of(T, nd));
        else {
            bool map = nd->kind == OYLNG_NODE_MAPPING;
            st = map ? oylng_emit_map_start(E, false) : oylng_emit_seq_start(E, false);
            if (st == OYLNG_OK && n == cap) {
                struct walk *ns = malloc(2 * cap * sizeof *ns);
                if (!ns) st = oylng__emitter_fail(E, OYLNG_ERR_MEMORY, "out of memory");
                else {
                    memcpy(ns, stk, n * sizeof *ns);
                    if (stk != local) free(stk);
                    stk = ns;
                    cap *= 2;
                }
            }
            if (st == OYLNG_OK) stk[n++] = (struct walk){ id, 0 };
        }
        /* the next node: a child of the innermost collection with one left */
        while (st == OYLNG_OK && n) {
            struct walk *w = &stk[n - 1];
            const tnode *c = &T->nodes[w->id];
            uint32_t total = c->kind == OYLNG_NODE_MAPPING ? 2 * c->n : c->n;
            if (w->next < total) break;
            st = c->kind == OYLNG_NODE_MAPPING ? oylng_emit_map_end(E) : oylng_emit_seq_end(E);
            n--;
        }
        if (st != OYLNG_OK || !n) break;
        struct walk *w = &stk[n - 1];
        id = T->kids[T->nodes[w->id].a + w->next++];
    }
    if (stk != local) free(stk);
    return st != OYLNG_OK ? st : oylng_emit_doc_end(E);
}
