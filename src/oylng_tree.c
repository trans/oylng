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
 *
 * A tree that keeps references (oylng_parser_set_keep) is the same, but
 * nothing in it is spread: a -<< is an item of its own, a SPLICE node, and
 * a <<: line a MERGE node, each naming the node it names, shared. A
 * mapping's MERGE nodes go in kids just before its pairs, then how many
 * there are, so that its pairs start where they always do.
 */

#include "oylng_internal.h"
#include <stdlib.h>
#include <string.h>

#define SMALL 8          /* up to this many pairs, keys are compared one by one */
#define F_POOLED 1u      /* a string's text is in the pool */
#define F_PLAIN  2u      /* a string written plain, or an empty value */
#define F_TABLE  4u      /* copied from the table, so it may be shared */
#define F_MERGES 8u      /* a kept tree's mapping with <<: lines, before its pairs */
#define F_SPLICES 16u    /* a kept tree's sequence with -<< items */

typedef struct {
    uint32_t a;          /* STR: where its text starts. SEQ, MAP: where its kids start.
                            SPLICE, MERGE: the node it names */
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
    bool           kept;   /* references kept: SPLICE and MERGE nodes */
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

/* A kept tree's -<< or <<: line, naming node t */
static oylng_node link_node(builder *B, bool splice, oylng_node t, size_t pos, uint8_t flags) {
    oylng_node id = new_node(B, splice ? OYLNG_NODE_SPLICE : OYLNG_NODE_MERGE, pos);
    if (id == OYLNG_NO_NODE) return id;
    B->T->nodes[id].a = t;
    B->T->nodes[id].flags = flags;
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
static void close_merged(builder *B, oylng_node id, size_t s0);

static void close_coll(builder *B, oylng_node id, size_t s0) {
    oylng_tree *T = B->T;
    size_t nkids = B->ns - s0;
    bool map = T->nodes[id].kind == OYLNG_NODE_MAPPING;
    if (T->kept && map && nkids && T->nodes[B->stk[s0]].kind == OYLNG_NODE_MERGE) {
        close_merged(B, id, s0);
        return;
    }
    size_t pairs = map ? nkids / 2 : nkids;
    if (T->kept && !map)
        for (size_t i = s0; i < B->ns; i++)
            if (T->nodes[B->stk[i]].kind == OYLNG_NODE_SPLICE) { T->nodes[id].flags |= F_SPLICES; break; }
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

/* A kept mapping with <<: lines, which come first: they go in kids before
 * its pairs, then their count, and the pairs are closed as any mapping's */
static void close_merged(builder *B, oylng_node id, size_t s0) {
    oylng_tree *T = B->T;
    size_t nkids = B->ns - s0, nm = 0;
    while (nm < nkids && T->nodes[B->stk[s0 + nm]].kind == OYLNG_NODE_MERGE) nm++;
    if (nm + 1 > UINT32_MAX - T->nk) { oops(B, OYLNG_ERR_LIMIT, "a tree of 4G children or more"); return; }
    if (!ROOM(B, T->kids, T->capk, T->nk + nm + 1)) return;
    memcpy(T->kids + T->nk, B->stk + s0, nm * sizeof *T->kids);
    T->kids[T->nk + nm] = (uint32_t)nm;
    T->nk += nm + 1;
    /* the pairs, moved down to where the lines were */
    memmove(B->stk + s0, B->stk + s0 + nm, (nkids - nm) * sizeof *B->stk);
    B->ns -= nm;
    close_coll(B, id, s0);   /* its first child now a key, never a line */
    T->nodes[id].flags |= F_MERGES;
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
    else if (tn.kind == OYLNG__T_SPLICE || tn.kind == OYLNG__T_MERGE) {
        /* what it names is made now and filled with the rest */
        oylng_node t = table_node(B, tn.link);
        id = t == OYLNG_NO_NODE ? t : link_node(B, tn.kind == OYLNG__T_SPLICE, t, tn.pos, F_TABLE);
    } else {
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
        case OYLNG__EVT_SPLICE:
        case OYLNG__EVT_MERGE:
            id = from_table(B, (uint32_t)e->value.len);
            if (id != OYLNG_NO_NODE) id = link_node(B, e->type == OYLNG__EVT_SPLICE, id, e->offset, 0);
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
        B.T->kept = oylng__parser_keep(P);
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

oylng_status oylng_parser_table(oylng_parser *P, oylng_tree **out) {
    if (!out) return OYLNG_ERR_INPUT;
    *out = NULL;
    if (!P) return OYLNG_ERR_INPUT;
    oylng_status st = oylng__parser_status(P);
    if (st != OYLNG_OK) return st;
    uint32_t n = oylng__table_count(P);
    if (!n) return OYLNG_OK;
    const char *in;
    size_t len;
    oylng__parser_input(P, &in, &len);
    if (len >= UINT32_MAX) return OYLNG_ERR_LIMIT;
    builder B = { .P = P, .st = OYLNG_OK };
    B.T = calloc(1, sizeof *B.T);
    if (!B.T) return OYLNG_ERR_MEMORY;
    B.T->in = in;
    B.T->inlen = len;
    B.T->kept = oylng__parser_keep(P);
    /* a mapping of the entries, where the first one's key is */
    uint32_t k, v;
    oylng__tnode kn;
    oylng__table_entry_nodes(P, 0, &k, &v);
    oylng__table_node(P, k, &kn);
    oylng_node root = new_node(&B, OYLNG_NODE_MAPPING, kn.pos);
    B.T->root = root;
    for (uint32_t e = 0; e < n && B.st == OYLNG_OK; e++) {
        oylng__table_entry_nodes(P, e, &k, &v);
        oylng_node kid = from_table(&B, k);
        if (kid != OYLNG_NO_NODE) push_child(&B, kid);
        oylng_node vid = B.st == OYLNG_OK ? from_table(&B, v) : OYLNG_NO_NODE;
        if (vid != OYLNG_NO_NODE) push_child(&B, vid);
    }
    if (B.st == OYLNG_OK) close_coll(&B, root, 0);
    free(B.stk);
    free(B.open);
    free(B.todo);
    if (B.st != OYLNG_OK) {
        oylng_tree_free(B.T);
        return B.st;   /* the parser's own state is untouched */
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

oylng_node oylng_node_target(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    return nd && (nd->kind == OYLNG_NODE_SPLICE || nd->kind == OYLNG_NODE_MERGE) ? nd->a : OYLNG_NO_NODE;
}

size_t oylng_node_merges(const oylng_tree *T, oylng_node map) {
    const tnode *nd = get(T, map);
    return nd && nd->kind == OYLNG_NODE_MAPPING && (nd->flags & F_MERGES) ? T->kids[nd->a - 1] : 0;
}

oylng_node oylng_node_merge(const oylng_tree *T, oylng_node map, size_t i) {
    size_t nm = oylng_node_merges(T, map);
    return i < nm ? T->kids[T->nodes[map].a - 1 - nm + i] : OYLNG_NO_NODE;
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

size_t oylng__node_lines(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    if (!nd) return 0;
    if (nd->kind == OYLNG_NODE_MAPPING) return oylng_node_merges(T, n);
    if (nd->kind != OYLNG_NODE_SEQUENCE || !(nd->flags & F_SPLICES)) return 0;
    size_t c = 0;
    for (uint32_t i = 0; i < nd->n; i++) c += T->nodes[T->kids[nd->a + i]].kind == OYLNG_NODE_SPLICE;
    return c;
}

/* A step of spreading from the budget: past it, refused */
#define STEP(B, budget) (*(budget) ? (--*(budget), true) : (oops((B), OYLNG_ERR_LIMIT, "spreading passes its budget"), false))

/* What spreading walks: a mapping or a sequence, and its next line or
 * item */
struct sframe { oylng_node n; uint32_t next; };

/* Whether set s, of nodes plus 1, has n; with add, adding it */
static bool seen_add(builder *B, uint32_t **s, size_t *cap, size_t *count, oylng_node n, bool add) {
    if (add && 2 * (*count + 1) > *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        uint32_t *nw = calloc(nc, sizeof *nw);
        if (!nw) { oops(B, OYLNG_ERR_MEMORY, "out of memory"); return false; }
        for (size_t j = 0; j < *cap; j++)
            if ((*s)[j]) {
                size_t h = (((*s)[j] - 1) * 0x9E3779B1u) & (nc - 1);
                while (nw[h]) h = (h + 1) & (nc - 1);
                nw[h] = (*s)[j];
            }
        free(*s);
        *s = nw;
        *cap = nc;
    }
    if (!*cap) return false;
    size_t h = (n * 0x9E3779B1u) & (*cap - 1);
    while ((*s)[h] && (*s)[h] != n + 1) h = (h + 1) & (*cap - 1);
    if ((*s)[h]) return true;
    if (add) {
        (*s)[h] = n + 1;
        ++*count;
    }
    return false;
}

/* The pairs of kept mapping m as a full read builds them (part 1, section
 * 9.1), onto the stack, in time linear in the mappings its lines reach:
 * - their order: each line's, the first first, the mapping it names built
 *   the same way, then m's own, each added where it isn't yet: a walk
 *   forward that adds a mapping's keys once its lines are done;
 * - a key's node: m's own, else the first line's to have the key, the same
 *   way down, as a full read's written key replaces a merged one: the
 *   first found walking forward, own keys before lines;
 * - a value: m's own, else the last line's to have it: the first found
 *   walking own keys first, then the lines from the last, as a lookup does.
 * Each walk visits a mapping once, as a second visit finds no key not
 * already there. Each line followed and each key looked at takes a step
 * from the budget, in each walk. */
static void spread_pairs(builder *B, oylng_node m, struct sframe **fs, size_t *capf, uint64_t budget) {
    oylng_tree *T = B->T;
    if (!T->keyed) {
        oylng_hash_key_new(&T->key);
        T->keyed = true;
    }
    struct kslot { oylng_node key, val; } *sl = NULL;
    uint32_t *ix = NULL, *order = NULL, *seen = NULL;   /* ix: a slot's number plus 1 */
    size_t cap = 0, ns = 0, capsl = 0, no = 0, capo = 0, capseen = 0, nseen = 0;
    const oylng_node UNSET = OYLNG_NO_NODE, PLACED = OYLNG_NO_NODE - 1;
    for (int pass = 0; pass < 2 && B->st == OYLNG_OK; pass++) {
        /* pass 0 walks forward, giving the order and the key nodes; pass 1
           walks the lines from the last, giving the values */
        uint64_t steps = budget;
        if (seen) memset(seen, 0, capseen * sizeof *seen);
        nseen = 0;
        size_t nf = 0;
        if (!ROOM(B, *fs, *capf, 1)) break;
        (*fs)[nf++] = (struct sframe){ m, UINT32_MAX };
        while (nf && B->st == OYLNG_OK) {
            struct sframe *f = &(*fs)[nf - 1];
            oylng_node x = f->n;
            const tnode *nd = &T->nodes[x];
            uint32_t lines = (uint32_t)oylng_node_merges(T, x);
            if (f->next == UINT32_MAX) {   /* come to x: marked seen, its own keys looked at */
                seen_add(B, &seen, &capseen, &nseen, x, true);
                for (uint32_t i = 0; i < nd->n && B->st == OYLNG_OK; i++) {
                    oylng_node k = T->kids[nd->a + 2 * i];
                    oylng_str ks = text_of(T, &T->nodes[k]);
                    if (!STEP(B, &steps)) break;
                    if (2 * (ns + 1) > cap) {   /* a bigger index, the slots hashed again */
                        size_t nc = cap ? cap * 2 : 16;
                        uint32_t *ni = calloc(nc, sizeof *ni);
                        if (!ni) { oops(B, OYLNG_ERR_MEMORY, "out of memory"); break; }
                        for (size_t j = 0; j < ns; j++) {
                            size_t h = (size_t)key_hash(T, text_of(T, &T->nodes[sl[j].key])) & (nc - 1);
                            while (ni[h]) h = (h + 1) & (nc - 1);
                            ni[h] = (uint32_t)j + 1;
                        }
                        free(ix);
                        ix = ni;
                        cap = nc;
                    }
                    size_t h = (size_t)key_hash(T, ks) & (cap - 1);
                    while (ix[h] && !same(text_of(T, &T->nodes[sl[ix[h] - 1].key]), ks)) h = (h + 1) & (cap - 1);
                    if (pass == 0 && !ix[h]) {
                        if (!ROOM(B, sl, capsl, ns + 1)) break;
                        sl[ns] = (struct kslot){ k, UNSET };
                        ix[h] = (uint32_t)++ns;
                    } else if (pass == 1 && ix[h] && sl[ix[h] - 1].val == UNSET)
                        sl[ix[h] - 1].val = T->kids[nd->a + 2 * i + 1];
                }
                f->next = pass == 0 ? 0 : lines;
                continue;
            }
            if (pass == 0 ? f->next < lines : f->next > 0) {
                uint32_t i = pass == 0 ? f->next++ : --f->next;
                oylng_node t = T->nodes[oylng_node_merge(T, x, i)].a;
                if (!STEP(B, &steps)) break;
                if (seen_add(B, &seen, &capseen, &nseen, t, false)) continue;
                if (!ROOM(B, *fs, *capf, nf + 1)) break;
                (*fs)[nf++] = (struct sframe){ t, UINT32_MAX };
                continue;
            }
            nf--;
            if (pass == 0)   /* x's lines done: its keys not yet placed, at the end */
                for (uint32_t i = 0; i < nd->n; i++) {
                    oylng_str ks = text_of(T, &T->nodes[T->kids[nd->a + 2 * i]]);
                    size_t h = (size_t)key_hash(T, ks) & (cap - 1);
                    while (!same(text_of(T, &T->nodes[sl[ix[h] - 1].key]), ks)) h = (h + 1) & (cap - 1);
                    struct kslot *e = &sl[ix[h] - 1];
                    if (e->val == UNSET) {
                        if (!ROOM(B, order, capo, no + 1)) break;
                        order[no++] = ix[h] - 1;
                        e->val = PLACED;
                    }
                }
        }
        if (pass == 0)
            for (size_t j = 0; j < ns; j++) sl[j].val = UNSET;
    }
    for (size_t j = 0; j < no && B->st == OYLNG_OK; j++) {
        if (!ROOM(B, B->stk, B->caps, B->ns + 2)) break;
        B->stk[B->ns++] = sl[order[j]].key;
        B->stk[B->ns++] = sl[order[j]].val;
    }
    free(sl);
    free(ix);
    free(order);
    free(seen);
}

/* The items of kept sequence s, onto the stack, each -<< replaced by the
 * items of the sequence it names, spread the same way; each item and
 * each -<< taking a step from the budget */
static void spread_items(builder *B, oylng_node s, struct sframe **fs, size_t *capf, uint64_t budget) {
    oylng_tree *T = B->T;
    size_t nf = 0;
    if (!ROOM(B, *fs, *capf, 1)) return;
    (*fs)[nf++] = (struct sframe){ s, 0 };
    while (nf && B->st == OYLNG_OK) {
        struct sframe *f = &(*fs)[nf - 1];
        const tnode *nd = &T->nodes[f->n];
        if (f->next >= nd->n) { nf--; continue; }
        oylng_node it = T->kids[nd->a + f->next++];
        if (!STEP(B, &budget)) break;
        if (T->nodes[it].kind == OYLNG_NODE_SPLICE) {
            oylng_node t = T->nodes[it].a;
            if (!ROOM(B, *fs, *capf, nf + 1)) break;
            (*fs)[nf++] = (struct sframe){ t, 0 };
        } else push_child(B, it);
    }
}

oylng_status oylng__tree_spread(oylng_tree *T, oylng_node n, uint64_t budget, oylng_node *out) {
    *out = OYLNG_NO_NODE;
    const tnode *nd = get(T, n);
    if (!nd || (nd->kind != OYLNG_NODE_MAPPING && nd->kind != OYLNG_NODE_SEQUENCE)) return OYLNG_ERR_INPUT;
    builder B = { .T = T, .st = OYLNG_OK };
    struct sframe *fs = NULL;
    size_t capf = 0;
    uint8_t kind = nd->kind, flags = nd->flags & F_TABLE;
    oylng_node id = new_node(&B, kind, nd->pos);
    if (id != OYLNG_NO_NODE) {
        if (kind == OYLNG_NODE_MAPPING) spread_pairs(&B, n, &fs, &capf, budget);
        else spread_items(&B, n, &fs, &capf, budget);
    }
    free(fs);
    if (B.st == OYLNG_OK) {
        bool kept = T->kept;
        T->kept = false;   /* closed as a full read's: no lines among its children */
        close_coll(&B, id, 0);
        T->kept = kept;
    }
    free(B.stk);
    if (B.st != OYLNG_OK) return B.st;
    T->nodes[id].flags = flags;
    *out = id;
    return OYLNG_OK;
}

oylng_mark oylng_node_mark(const oylng_tree *T, oylng_node n) {
    const tnode *nd = get(T, n);
    if (!nd) return (oylng_mark){ 0, 0, 0 };
    return oylng__mark_at(T->in, nd->pos);
}

/* ── A tree the emitter builds (part 2, section 13) ──────── */

oylng_status oylng__tree_make(const oylng__tnew *nd, size_t n, const char *text, oylng_tree **out) {
    *out = NULL;
    builder B = { .st = OYLNG_OK };
    B.T = calloc(1, sizeof *B.T);
    if (!B.T) return OYLNG_ERR_MEMORY;
    B.T->in = "";
    B.T->root = OYLNG_NO_NODE;
    for (size_t i = 0; i < n && B.st == OYLNG_OK; i++) {
        bool str = nd[i].kind == OYLNG_NODE_STRING;
        oylng_node id = str ? new_str(&B, text + nd[i].off, nd[i].len, 0, nd[i].plain ? F_PLAIN : 0)
                            : new_node(&B, nd[i].kind, 0);
        if (id == OYLNG_NO_NODE) break;
        if (B.no) push_child(&B, id);
        else B.T->root = id;
        if (!str && ROOM(&B, B.open, B.capo, B.no + 1)) B.open[B.no++] = (struct open){ id, B.ns };
        /* a node is its record's number, so each collection open knows how
           many children it takes */
        while (B.st == OYLNG_OK && B.no && B.ns - B.open[B.no - 1].s0 == nd[B.open[B.no - 1].id].n) {
            struct open o = B.open[--B.no];
            close_coll(&B, o.id, o.s0);
        }
    }
    free(B.stk);
    free(B.open);
    if (B.st == OYLNG_OK && (B.no || B.T->nn != n)) B.st = OYLNG_ERR_INPUT;
    if (B.st != OYLNG_OK) {
        oylng_tree_free(B.T);
        return B.st;
    }
    *out = B.T;
    return OYLNG_OK;
}

bool oylng__tree_set_str(oylng_tree *T, oylng_node n, const char *s, size_t len, bool plain) {
    tnode *nd = &T->nodes[n];
    oylng_str now = text_of(T, nd);
    if (!same(now, (oylng_str){ s, len })) {
        if ((nd->flags & F_POOLED) && nd->a + (size_t)nd->n == T->npool) T->npool = nd->a;   /* the last tried */
        builder B = { .T = T, .st = OYLNG_OK };
        uint32_t a;
        uint8_t fl;
        if (!place_text(&B, s, len, &a, &fl)) return false;
        nd = &T->nodes[n];
        nd->a = a;
        nd->n = (uint32_t)len;
        nd->flags = fl;
    }
    nd->flags = (uint8_t)((nd->flags & ~F_PLAIN) | (plain ? F_PLAIN : 0));
    return true;
}

/* ── Writing a tree ──────────────────────────────────────── */

oylng_status oylng_emit_tree(oylng_emitter *E, const oylng_tree *T) {
    if (!E || !T || T->root == OYLNG_NO_NODE) return OYLNG_ERR_INPUT;
    /* written in full, its references could be any size */
    if (T->kept) return OYLNG_ERR_INPUT;
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
