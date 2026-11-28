/*
 * oylng_emitter.c — YAML 2.0 written from events (section 11)
 *
 * It writes what the events say: %YAML 2.0 first, a marker for each
 * document, keys in the order given, every node in full. A scalar is
 * plain where the parser reads that back as the same string, else in
 * single quotes, else in double quotes; text with line breaks is a literal
 * block scalar where that reads back exactly. The tests for which a plain
 * scalar reads back follow the parser's own (plain_start_bad, plain_end
 * and key in oylng_parser.c), and the round-trip tests hold them to it.
 *
 * Output is written as it goes. A line break is written only when the
 * next line starts, and a space only when an inline node follows it, so a
 * block collection's start writes nothing until its first child shows
 * whether it goes on the next line, or it ends empty as {} or [].
 *
 * With a schema (part 2, section 13), a document is written as it goes
 * too, so that every call is checked where it is made, and its nodes are
 * kept as records. At its end the records make a tree, which the typer
 * types as a parser would, trying each scalar's spellings in turn until
 * one reads back as given; then the document is written again, over what
 * was written of it, each scalar as the typer chose.
 */

#include "oylng_yes.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_KEYS 8          /* keys a mapping compares one by one, before it hashes them */
#define EMPTY_SLOT UINT32_MAX

enum { E_BEFORE, E_STREAM, E_ROOT, E_DONE, E_END, E_FAILED };

/* How a scalar is written: as it reads back best, or plain, which the
 * caller has found reads back, or not plain, or as an empty value */
enum { F_AUTO, F_PLAIN, F_QUOTED, F_EMPTY };

/* An open collection */
typedef struct {
    bool     map;        /* else a sequence */
    bool     flow;
    bool     compact;    /* a block collection that is a sequence's item: its first
                            child goes on the item's - line */
    uint32_t col;        /* a block collection's column: its keys', or its -s' */
    uint64_t n;          /* the nodes in it so far; a mapping's alternate key, value */
    uint32_t k0;         /* its keys, in keys[] from here */
    size_t   b0;         /* and their text, in kbuf from here */
    uint32_t t0, tcap;   /* its hash table in lix, once it has more than SMALL_KEYS */
} eframe;

typedef struct {
    size_t   off, len;   /* in kbuf */
    uint32_t hash;       /* once its mapping has a table */
} ekey;

struct oylng_emitter {
    char  *out;
    size_t len, cap;
    bool   bol;          /* the output ends a line, or is empty */
    bool   space;        /* a node written inline here needs a space before it */
    bool   oom;
    int    state;
    oylng_status status;
    char   msg[448];

    unsigned indent;
    bool     flow_all;

    eframe  *fr;
    uint32_t sp, capfr;

    /* the keys of the open mappings, innermost last, kept to refuse a
       repeat: a key's text needn't outlive the call that gave it */
    ekey     *keys;
    uint32_t  nkeys, capkeys;
    char     *kbuf;
    size_t    klen, kcap;
    uint32_t *lix;
    uint32_t  nlix, caplix;
    oylng_hash_key hkey;
    bool      hkey_set;

    /* typed data (part 2, section 13): a document held till its end, as
       records of its nodes, then typed and written again */
    const oylng_schema *schema;   /* types each document from the next on */
    const oylng_schema *held;     /* types the document open */
    bool      replay;             /* it is being written again */
    int       force;              /* how replay writes the next scalar */
    size_t    mark;               /* the output before it */
    bool      mbol, mspace;
    struct erec *rec;
    oylng__tnew *tn;              /* the same nodes, for the tree */
    size_t    nrec, caprec, captn;
    uint32_t *opn;                /* the records of its collections open */
    uint32_t  nopn, capopn;
    char     *tb;                 /* its text */
    size_t    tlen, tcap;
    size_t    dname, dnlen;       /* its name in tb, or SIZE_MAX */
    char     *cb;                 /* a spelling */
    size_t    ccap;
    oylng_str hint;               /* how the next typed value was written, if it was read */
    bool      hplain;
    uint32_t  cnode;              /* the node being spelled */
    char      why[160];           /* why its spelling doesn't read back */
    unsigned  craw;               /* and its next spelling to look at */
    struct ebind { char *name; size_t len; oylng_unbind_fn fn; void *ctx; } *binds;
    size_t    nbinds, capbinds;
};

/* Where the next node goes */
typedef struct {
    eframe *parent;      /* NULL: it is the root */
    bool    flow;        /* in a flow collection */
    bool    key;         /* it is a mapping's key */
    bool    bol;         /* it starts a line at column 0 */
    long    lit;         /* the column a literal block scalar's text would take, or -1
                            where one can't go */
} place;

static oylng_status fail(oylng_emitter *E, oylng_status st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E->msg, sizeof E->msg, fmt, ap);
    va_end(ap);
    E->state = E_FAILED;
    E->status = st;
    return st;
}

/* After a call's writing: out of memory is found here, once */
static oylng_status done(oylng_emitter *E) {
    if (E->oom) return fail(E, OYLNG_ERR_MEMORY, "out of memory");
    return OYLNG_OK;
}

static bool grow(oylng_emitter *E, void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 16;
    while (c < need) {
        if (c > SIZE_MAX / 2 / elem) { E->oom = true; return false; }
        c *= 2;
    }
    void *np = realloc(*p, c * elem);
    if (!np) { E->oom = true; return false; }
    *p = np;
    *cap = c;
    return true;
}

static bool grow32(oylng_emitter *E, void **p, uint32_t *cap, uint64_t need, size_t elem) {
    if (need > UINT32_MAX) { E->oom = true; return false; }
    size_t c = *cap;
    if (!grow(E, p, &c, (size_t)need, elem)) return false;
    *cap = c > UINT32_MAX ? UINT32_MAX : (uint32_t)c;
    return true;
}

/* ── Output ──────────────────────────────────────────────── */

/* Room for n more bytes: false if memory runs out */
static bool out_grow(oylng_emitter *E, size_t n) {
    if (E->oom) return false;
    size_t c = E->cap ? E->cap : 4096;
    while (c < E->len + n) {
        if (c > SIZE_MAX / 2) { E->oom = true; return false; }
        c *= 2;
    }
    char *nb = realloc(E->out, c);
    if (!nb) { E->oom = true; return false; }
    E->out = nb;
    E->cap = c;
    return true;
}

/* Text with no line break in it; a line break is out_nl's */
static inline void out_add(oylng_emitter *E, const char *s, size_t n) {
    if (!n || (E->len + n > E->cap && !out_grow(E, n))) return;
    memcpy(E->out + E->len, s, n);
    E->len += n;
    E->bol = false;
}

static inline void out_nl(oylng_emitter *E) {
    if (E->len + 1 > E->cap && !out_grow(E, 1)) return;
    E->out[E->len++] = '\n';
    E->bol = true;
}

#define OUT(E, lit) out_add(E, lit, sizeof lit - 1)

static void out_spaces(oylng_emitter *E, size_t k) {
    static const char sp[] = "                                ";
    while (k) {
        size_t m = k < 32 ? k : 32;
        out_add(E, sp, m);
        k -= m;
    }
}

static void inline_space(oylng_emitter *E) {
    if (E->space) OUT(E, " ");
    E->space = false;
}

static void new_line(oylng_emitter *E, size_t col) {
    if (!E->bol) out_nl(E);
    out_spaces(E, col);
    E->space = false;
}

/* ── Scalars (section 6) ─────────────────────────────────── */

static inline bool is_flowi(int c) {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

/* Whether s may start a plain scalar here, and be one as a whole: its
 * first character, and what s is as a key or at column 0 (section 6.1) */
static bool first_ok(const char *s, size_t n, const place *pl) {
    unsigned c = (unsigned char)s[0];
    switch (c) {
    case ' ': case '\t': case ',': case '[': case ']': case '{': case '}': case '#':
    case '&': case '*': case '|': case '>': case '\'': case '"': case '!':
        return false;
    case '%':
        if (pl->bol) return false;
        break;
    case '-': case '?': case ':':
        if (n == 1 || s[1] == ' ' || s[1] == '\t' || (pl->flow && is_flowi(s[1]))) return false;
        break;
    }
    /* -<< and a space: a splice, where it's an item */
    if (n > 3 && !memcmp(s, "-<<", 3) && (s[3] == ' ' || s[3] == '\t')) return false;
    /* a plain << key merges; and at column 0, a line of --- or ... is a
       marker or an end: such a key reads back, but would only mislead */
    if (pl->key && n == 2 && s[0] == '<' && s[1] == '<') return false;
    if (pl->bol && n >= 3 && (!memcmp(s, "---", 3) || !memcmp(s, "...", 3))) return false;
    return true;
}

/* Whether s, one line of characters allowed outside double quotes, reads
 * back as itself written plain here (section 6.1) */
static bool plain_ok(const char *s, size_t n, const place *pl) {
    bool flow = pl->flow;
    if (!first_ok(s, n, pl)) return false;
    unsigned last = (unsigned char)s[n - 1];
    if (last == ' ' || last == '\t' || last == ':') return false;
    for (size_t i = 1; i < n; i++) {
        unsigned d = (unsigned char)s[i];
        if (d == '#' && (s[i - 1] == ' ' || s[i - 1] == '\t')) return false;
        if (d == ':' && (s[i + 1] == ' ' || s[i + 1] == '\t' || (flow && is_flowi(s[i + 1])))) return false;
        if (flow && is_flowi(d)) return false;
    }
    return true;
}

/* Whether s, with a line break in it and only characters allowed outside
 * double quotes, reads back as itself as a literal block scalar (section
 * 6.4): | keeps one final line break and |- none, so s may end with one
 * at most; and the first line with text sets the indent, so it can't
 * start with a space, nor can a line of spaces come before it */
static bool literal_ok(const char *s, size_t n) {
    size_t bn = s[n - 1] == '\n' ? n - 1 : n;
    if (bn == 0 || s[bn - 1] == '\n') return false;
    for (size_t i = 0; i < bn; i++) {
        if (s[i] == '\n') continue;          /* an empty line before the text */
        return s[i] != ' ';
    }
    return false;
}

static void literal_out(oylng_emitter *E, const char *s, size_t n, size_t col) {
    bool clip = s[n - 1] == '\n';
    size_t bn = clip ? n - 1 : n;
    if (clip) OUT(E, "|");
    else OUT(E, "|-");
    out_nl(E);
    for (size_t i = 0;;) {
        const char *nl = memchr(s + i, '\n', bn - i);
        size_t j = nl ? (size_t)(nl - s) : bn;
        if (j > i) {
            out_spaces(E, col);
            out_add(E, s + i, j - i);
        }
        out_nl(E);
        if (!nl) break;
        i = j + 1;
    }
}

/* JSON's escapes, and \u for the characters that would otherwise be
 * there unseen: the controls, U+2028, U+2029, U+FEFF, U+FFFE, U+FFFF */
static void dquote_out(oylng_emitter *E, const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    OUT(E, "\"");
    size_t st = 0;
    for (size_t i = 0; i < n;) {
        unsigned c = u[i];
        size_t len = 1;
        const char *esc = NULL;
        char hex[8];
        if (c < 0x80) {
            switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    snprintf(hex, sizeof hex, "\\u%04X", c);
                    esc = hex;
                }
            }
        } else {
            uint32_t cp = oylng_utf8(u + i, u + n, &len);   /* checked: it is UTF-8 */
            if (cp < 0xA0 || cp == 0x2028 || cp == 0x2029 || cp == 0xFEFF || cp == 0xFFFE || cp == 0xFFFF) {
                snprintf(hex, sizeof hex, "\\u%04X", (unsigned)cp);
                esc = hex;
            }
        }
        if (esc) {
            out_add(E, s + st, i - st);
            out_add(E, esc, strlen(esc));
            st = i + len;
        }
        i += len;
    }
    out_add(E, s + st, n - st);
    OUT(E, "\"");
}

/* Bytes a plain scalar may hold anywhere after its first, with no second
 * look: printable ASCII but the tab, # : ' and the flow indicators. A
 * space is among them: what makes it matter, # or : beside it, or being
 * last, is looked at anyway. */
#define B(c) ((uint64_t)1 << ((c) & 63))
static const uint64_t body_lo = (0xFFFFFFFF00000000u) & ~B('#') & ~B(':') & ~B('\'') & ~B(',');
static const uint64_t body_hi = (0x7FFFFFFFFFFFFFFFu) & ~B('[') & ~B(']') & ~B('{') & ~B('}');
#undef B

static inline bool body_byte(unsigned c) {
    return c < 64 ? (body_lo >> c) & 1 : c < 128 && (body_hi >> (c - 64)) & 1;
}

static oylng_status scalar_out(oylng_emitter *E, const char *s, size_t n, const place *pl, int style) {
    const unsigned char *u = (const unsigned char *)s;
    if (style == F_EMPTY) return OYLNG_OK;
    if (style == F_PLAIN) {
        inline_space(E);
        out_add(E, s, n);
        return OYLNG_OK;
    }
    /* the common string, one pass: such bytes throughout, written plain
       unless its first or last byte, or what it is as a key, says not */
    if (n && style == F_AUTO) {
        size_t i = 1;
        while (i < n && body_byte(u[i])) i++;
        if (i == n && (body_byte(u[0]) || u[0] == ':') && u[n - 1] != ' ' && first_ok(s, n, pl)) {
            inline_space(E);
            out_add(E, s, n);
            return OYLNG_OK;
        }
    }
    bool lf = false, dq = false, sq = false, tab = false;
    for (size_t i = 0; i < n;) {
        unsigned c = u[i];
        if (c < 0x80) {
            if (c == '\n') lf = true;
            else if (c == '\'') sq = true;
            else if (c == '\t') tab = true;
            else if (c < 0x20 || c == 0x7F) dq = true;   /* a CR too */
            i++;
            continue;
        }
        size_t len;
        uint32_t cp = oylng_utf8(u + i, u + n, &len);
        if (cp == OYLNG_BAD_CP)
            return fail(E, OYLNG_ERR_EMIT, "a string that isn't valid UTF-8, at its byte %zu", i);
        if (!oylng_allowed(cp, false)) dq = true;
        i += len;
    }
    /* on one line, a tab is written \t, where it can be seen; in a block
       scalar's lines it stays, as text such as a Makefile has it */
    inline_space(E);
    if (n == 0) OUT(E, "\"\"");
    else if (style == F_AUTO && !lf && !dq && !tab && plain_ok(s, n, pl)) out_add(E, s, n);
    else if (lf && !dq && pl->lit >= 0 && literal_ok(s, n)) literal_out(E, s, n, (size_t)pl->lit);
    else if (!lf && !dq && !tab && !sq) {
        OUT(E, "'");
        out_add(E, s, n);
        OUT(E, "'");
    } else dquote_out(E, s, n);
    return OYLNG_OK;
}

/* ── Keys (section 7.4) ──────────────────────────────────── */

static uint32_t key_hash(oylng_emitter *E, const char *s, size_t n) {
    if (!E->hkey_set) {
        oylng_hash_key_new(&E->hkey);
        E->hkey_set = true;
    }
    return (uint32_t)oylng_hash(&E->hkey, n ? s : "", n);
}

static inline bool key_is(const oylng_emitter *E, const ekey *k, const char *s, size_t n) {
    return k->len == n && (n == 0 || memcmp(E->kbuf + k->off, s, n) == 0);
}

static void table_put(oylng_emitter *E, const eframe *f, uint32_t i) {
    uint32_t mask = f->tcap - 1, *t = E->lix + f->t0;
    uint32_t j = E->keys[f->k0 + i].hash & mask;
    while (t[j] != EMPTY_SLOT) j = (j + 1) & mask;
    t[j] = i;
}

/* A table for mapping f's keys, or a bigger one; f is the innermost
 * open collection, so its table is the last in lix */
static void table_build(oylng_emitter *E, eframe *f, uint32_t cnt) {
    ekey *k = E->keys + f->k0;
    if (!f->tcap)
        for (uint32_t i = 0; i < cnt; i++) k[i].hash = key_hash(E, E->kbuf + k[i].off, k[i].len);
    uint64_t cap = f->tcap ? (uint64_t)f->tcap * 2 : 32;
    while (cap < (uint64_t)cnt * 2) cap *= 2;
    if (!grow32(E, (void **)&E->lix, &E->caplix, f->t0 + cap, sizeof *E->lix)) return;
    E->nlix = (uint32_t)(f->t0 + cap);
    f->tcap = (uint32_t)cap;
    memset(E->lix + f->t0, 0xFF, (size_t)cap * sizeof *E->lix);
    for (uint32_t i = 0; i < cnt; i++) table_put(E, f, i);
}

/* Adds key s to mapping f, the innermost open collection: false if it has
 * it already */
static bool key_add(oylng_emitter *E, eframe *f, const char *s, size_t n) {
    uint32_t cnt = E->nkeys - f->k0, h = 0;
    if (f->tcap) {
        h = key_hash(E, s, n);
        uint32_t mask = f->tcap - 1;
        const uint32_t *t = E->lix + f->t0;
        for (uint32_t j = h & mask; t[j] != EMPTY_SLOT; j = (j + 1) & mask) {
            const ekey *k = &E->keys[f->k0 + t[j]];
            if (k->hash == h && key_is(E, k, s, n)) return false;
        }
    } else
        for (uint32_t i = 0; i < cnt; i++)
            if (key_is(E, &E->keys[f->k0 + i], s, n)) return false;

    if (!grow(E, (void **)&E->kbuf, &E->kcap, E->klen + n, 1)) return true;
    if (!grow32(E, (void **)&E->keys, &E->capkeys, (uint64_t)E->nkeys + 1, sizeof *E->keys)) return true;
    if (n) memcpy(E->kbuf + E->klen, s, n);
    E->keys[E->nkeys++] = (ekey){ E->klen, n, h };
    E->klen += n;
    cnt++;
    if (cnt > SMALL_KEYS) {
        if (!f->tcap || (uint64_t)cnt * 2 > f->tcap) table_build(E, f, cnt);
        else table_put(E, f, cnt - 1);
    }
    return true;
}

/* ── Placing nodes ───────────────────────────────────────── */

/* Writes what comes before the next node: the line it starts and its
 * indent, a sequence's -, or a flow collection's comma; and says where it
 * goes */
static oylng_status where(oylng_emitter *E, place *pl, bool coll) {
    pl->parent = NULL;
    if (E->sp == 0) {
        if (E->state == E_ROOT) {
            pl->flow = pl->key = pl->bol = false;
            pl->lit = (long)E->indent;
            return OYLNG_OK;
        }
        return fail(E, OYLNG_ERR_EMIT, "%s",
                    E->state == E_DONE   ? "a document has one root node: end the document first" :
                    E->state == E_STREAM ? "a node goes in a document: start one first" :
                    E->state == E_BEFORE ? "the stream hasn't started" : "the stream has ended");
    }
    eframe *f = &E->fr[E->sp - 1];
    bool key = f->map && f->n % 2 == 0;
    if (key && coll) return fail(E, OYLNG_ERR_EMIT, "a key is a string: a collection can't be a key");
    pl->parent = f;
    pl->key = key;
    if (f->flow) {
        if (f->n && (!f->map || key)) {
            OUT(E, ",");
            E->space = true;
        }
        pl->flow = true;
        pl->bol = false;
        pl->lit = -1;
        return OYLNG_OK;
    }
    if (!f->map || key) {
        if (f->n == 0 && f->compact) inline_space(E);
        else new_line(E, f->col);
        if (!f->map) {
            OUT(E, "-");
            E->space = true;
        }
    }
    pl->flow = false;
    pl->bol = key && f->col == 0;
    pl->lit = key ? -1 : (long)f->col + (long)E->indent;
    return OYLNG_OK;
}

/* A node is written: its mapping's next node is the key's value */
static void node_done(oylng_emitter *E) {
    if (E->sp == 0) {
        E->state = E_DONE;
        return;
    }
    eframe *f = &E->fr[E->sp - 1];
    bool key = f->map && f->n % 2 == 0;
    f->n++;
    if (key) {
        OUT(E, ":");
        E->space = true;
    }
}

static bool live(const oylng_emitter *E) { return E && E->state != E_FAILED; }

/* ── Typed data (part 2, section 13) ─────────────────────── */

/* A node of the held document. Its text, and whether it is first written
 * plain, are in tn. */
typedef struct erec {
    uint8_t  want;          /* a value's type, OYLNG_T_STR for a string; OYLNG_T_NONE for a key
                               or a collection */
    bool     flow;          /* a value in a flow collection; a collection in flow style */
    bool     empty;         /* a value that reads back here written empty */
    uint32_t end;           /* the record after its last descendant */
    size_t   name, nlen;    /* in tb, an object's named type (section 12), or SIZE_MAX */
    size_t   b, blen;       /* in tb, OYLNG_T_BINARY's bytes */
    size_t   hint, hlen;    /* in tb, how it was written where it was read, or SIZE_MAX */
    bool     hplain;
    union {
        double         f;
        oylng_datetime dt;
    } x;
} erec;

static const char *const type_names[] = {
    "nothing", "untyped", "!str", "!int", "!bigint", "!float", "!decimal", "!bool", "!null",
    "!date", "!datetime", "!binary", "a sequence", "a mapping",
};

/* Text added to the held document's: where it is, or SIZE_MAX if memory
 * runs out */
static size_t text_add(oylng_emitter *E, const void *s, size_t n) {
    if (!grow(E, (void **)&E->tb, &E->tcap, E->tlen + n + 1, 1)) return SIZE_MAX;
    size_t at = E->tlen;
    if (n) memcpy(E->tb + at, s, n);
    E->tlen += n;
    return at;
}

/* The next node of the held document; NULL if it can't be */
static erec *rec_add(oylng_emitter *E, uint8_t kind, const char *s, size_t n, bool plain) {
    if (E->nrec >= UINT32_MAX - 1) {
        fail(E, OYLNG_ERR_LIMIT, "a typed document of 4G nodes or more");
        return NULL;
    }
    if (!grow(E, (void **)&E->rec, &E->caprec, E->nrec + 1, sizeof *E->rec) ||
        !grow(E, (void **)&E->tn, &E->captn, E->nrec + 1, sizeof *E->tn))
        return NULL;
    size_t off = 0;
    if (kind == OYLNG_NODE_STRING && (off = text_add(E, s, n)) == SIZE_MAX) return NULL;
    E->tn[E->nrec] = (oylng__tnew){ kind, plain, 0, off, n };
    erec *r = &E->rec[E->nrec++];
    *r = (erec){ .name = SIZE_MAX, .hint = SIZE_MAX };
    r->end = (uint32_t)E->nrec;
    return r;
}

static bool holding(const oylng_emitter *E) { return E->held && !E->replay; }

/* Whether an empty value reads back as one here: the root, a mapping's
 * value, or a block sequence's item */
static bool empty_ok(const place *pl) {
    return !pl->parent || (!pl->key && (pl->parent->map || !pl->flow));
}

/* Whether s, as scalar_out sees it, reads back written plain here */
static bool can_plain(const char *s, size_t n, const place *pl) {
    if (!n) return false;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        if (u[i] < 0x80) {
            if (u[i] < 0x20 || u[i] == 0x7F) return false;
            i++;
            continue;
        }
        size_t len;
        uint32_t cp = oylng_utf8(u + i, u + n, &len);
        if (cp == OYLNG_BAD_CP || !oylng_allowed(cp, false)) return false;
        i += len;
    }
    return plain_ok(s, n, pl);
}

/* Spelling i of a value of type want whose standard spelling is t, n
 * long: 1, with it in *out; 0 past the last; -1 if spelling i is none. A
 * string, a date, a time or bytes is plain, then quoted. Anything else
 * has its standard spelling alone (section 13), so null is null. */
static int raw_spelling(uint8_t want, const char *t, size_t n, unsigned i, oylng_str *out, bool *plain) {
    *plain = true;
    switch (want) {
    case OYLNG_T_STR:
    case OYLNG_T_DATE:
    case OYLNG_T_DATETIME:
    case OYLNG_T_BINARY:
        if (i > 1) return 0;
        if (!n && !i) return -1;   /* an empty string is "", not an empty value */
        *out = (oylng_str){ t, n };
        *plain = i == 0;
        return 1;
    default:
        if (i) return 0;
        *out = (oylng_str){ t, n };
        return 1;
    }
}

static bool str_is(oylng_str a, const char *s, size_t n) {
    return a.data && a.len == n && (!n || memcmp(a.data, s, n) == 0);
}

static bool writable(oylng_str s, bool plain, const place *pl, bool empty) {
    return !plain || (s.len ? can_plain(s.data, s.len, pl) : empty);
}

/* The first spelling of a value that reads back written here */
static void first_spelling(uint8_t want, const char *t, size_t n, const place *pl, bool empty, oylng_str *out,
                           bool *plain) {
    for (unsigned i = 0;; i++) {
        int got = raw_spelling(want, t, n, i, out, plain);
        if (got > 0 && writable(*out, *plain, pl, empty)) return;
        if (!got) break;
    }
    *out = (oylng_str){ t, n };   /* none: quoted, which always reads back as written */
    *plain = false;
}

/* Spelling i of record r: the standard one; then the text it was read
 * from, where it was read and the application kept that, by writing a
 * typed document again (section 13); then, for a string, a date, a time
 * or bytes, quoted */
static int spelling(const oylng_emitter *E, const erec *r, const oylng__tnew *t, unsigned i, oylng_str *out,
                    bool *plain) {
    const char *std = E->tb + t->off;
    if (r->hint != SIZE_MAX && i) {
        if (i > 1) return raw_spelling(r->want, std, t->len, i - 1, out, plain);
        *out = (oylng_str){ E->tb + r->hint, r->hlen };
        *plain = r->hplain;
        oylng_str first;
        bool fp;
        if (raw_spelling(r->want, std, t->len, 0, &first, &fp) > 0 && fp == *plain &&
            str_is(first, out->data, out->len))
            return -1;   /* the same */
        return 1;
    }
    return raw_spelling(r->want, std, t->len, i, out, plain);
}

/* Spelling i of record r where a boolean or null is written as one of
 * the nw words w: those for its value, in order, each plain and then
 * quoted, an empty one as ""; then its own (section 13) */
static int place_spelling(const oylng_emitter *E, const erec *r, const oylng__tnew *t, unsigned i,
                          const oylng__word *w, uint32_t nw, oylng_str *out, bool *plain) {
    if (r->want == OYLNG_T_BOOL || r->want == OYLNG_T_NULL) {
        if (i < 2 * (size_t)nw) {
            const oylng__word *x = &w[i / 2];
            uint8_t what = r->want == OYLNG_T_NULL ? OYLNG__NULL : E->tb[t->off] == 't' ? OYLNG__TRUE : OYLNG__FALSE;
            if (x->what != what || (i % 2 == 0 && !x->text.len)) return -1;
            *out = x->text;
            *plain = i % 2 == 0;
            return 1;
        }
        i -= 2 * nw;
    }
    return spelling(E, r, t, i, out, plain);
}

/* For the typer (oylng__speller) */
static bool next_spelling(void *ctx, uint32_t node, unsigned k, const oylng__word *w, uint32_t nw, oylng_str *out,
                          bool *plain) {
    oylng_emitter *E = ctx;
    if (!k || node != E->cnode) {
        E->cnode = node;
        E->craw = 0;
    }
    const erec *r = &E->rec[node];
    const oylng__tnew *t = &E->tn[node];
    place pl = { NULL, r->flow, false, false, -1 };
    for (;;) {
        int got = place_spelling(E, r, t, E->craw++, w, nw, out, plain);
        if (!got) return false;
        if (got > 0 && writable(*out, *plain, &pl, r->empty)) return true;
    }
}

/* An integer's standard spelling, of a value OYLNG_T_INT or _BIGINT */
static oylng_str int_text(const oylng_typed *Y, oylng_value v, char *buf, size_t cap) {
    if (oylng_value_type(Y, v) == OYLNG_T_BIGINT) return oylng_value_text(Y, v);
    int64_t i;
    uint64_t u;
    int w = oylng_value_int(Y, v, &i) ? snprintf(buf, cap, "%lld", (long long)i)
          : oylng_value_uint(Y, v, &u) ? snprintf(buf, cap, "%llu", (unsigned long long)u) : 0;
    return (oylng_str){ buf, w > 0 ? (size_t)w : 0 };
}

static const char *fits(void *ctx, uint32_t node, const oylng_typed *Y, uint32_t v) {
    oylng_emitter *E = ctx;
    const erec *r = &E->rec[node];
    const oylng__tnew *t = &E->tn[node];
    const char *tx = E->tb + t->off;
    oylng_type ty = oylng_value_type(Y, v);
    char *why = E->why;   /* read before the next call */
    bool ok = true;
    /* under !any nothing is typed, so whatever was written reads back as
       plain data: 3 as "3", an object as what its code wrote (section 13) */
    if (ty == OYLNG_T_ANY) return NULL;
    /* an object its code wrote as one string: that text is how it's
       spelled, so it need only read back with the object's bound name, as
       code bound to signed may write a 5 as +5 (section 13) */
    bool spelled = r->name != SIZE_MAX && r->want == OYLNG_T_STR;
    if (t->kind == OYLNG_NODE_STRING && !spelled) {
        bool ints = (r->want == OYLNG_T_INT || r->want == OYLNG_T_BIGINT) &&
                    (ty == OYLNG_T_INT || ty == OYLNG_T_BIGINT);
        if (ty != r->want && !ints) ok = false;
        else if (ints) {
            char buf[24];
            ok = str_is(int_text(Y, v, buf, sizeof buf), tx, t->len);
        } else
            switch (ty) {
            case OYLNG_T_STR:
            case OYLNG_T_DECIMAL:
                ok = str_is(oylng_value_text(Y, v), tx, t->len);
                break;
            case OYLNG_T_FLOAT: {
                double f = oylng_value_float(Y, v);
                ok = memcmp(&f, &r->x.f, sizeof f) == 0;
                break;
            }
            case OYLNG_T_BOOL:
                ok = oylng_value_bool(Y, v) == (tx[0] == 't');
                break;
            case OYLNG_T_DATE:
            case OYLNG_T_DATETIME: {
                oylng_datetime d;
                const oylng_datetime *w = &r->x.dt;
                ok = oylng_value_datetime(Y, v, &d) && d.year == w->year && d.month == w->month && d.day == w->day;
                if (ok && ty == OYLNG_T_DATETIME)
                    ok = d.hour == w->hour && d.minute == w->minute && d.second == w->second &&
                         d.nanosecond == w->nanosecond && d.offset == w->offset && d.zulu == w->zulu &&
                         d.unknown_offset == w->unknown_offset;
                break;
            }
            case OYLNG_T_BINARY:
                ok = str_is(oylng_value_bytes(Y, v), E->tb + r->b, r->blen);
                break;
            default:
                break;
            }
        if (!ok) {
            if (ty == r->want || ints) snprintf(why, sizeof E->why, "it reads as another %s", type_names[ty]);
            else snprintf(why, sizeof E->why, "it reads as %s", type_names[ty]);
            return why;
        }
    }
    if (r->name != SIZE_MAX) {
        /* its object comes from the outermost bound name's code; if none
           is bound, its nearest name says what it reads as */
        oylng_str nm = oylng_value_bound(Y, v);
        bool ok = str_is(nm, E->tb + r->name, r->nlen);
        if (!nm.data) nm = oylng_value_name(Y, v);
        if (!ok) {
            int a = (int)(r->nlen > 40 ? 40 : r->nlen), b = (int)(nm.len > 40 ? 40 : nm.len);
            if (nm.data)
                snprintf(why, sizeof E->why, "written as a %.*s object, it reads as a %.*s", a, E->tb + r->name, b, nm.data);
            else snprintf(why, sizeof E->why, "written as a %.*s object, it reads as no named type", a, E->tb + r->name);
            return why;
        }
    }
    return NULL;
}

static void describe(void *ctx, uint32_t node, char *buf, size_t cap) {
    const oylng_emitter *E = ctx;
    const erec *r = &E->rec[node];
    const oylng__tnew *t = &E->tn[node];
    int n = (int)(t->len > 40 ? 40 : t->len);
    const char *s = E->tb + t->off, *more = (size_t)n < t->len ? "..." : "";
    switch (r->want) {
    case OYLNG_T_STR:      snprintf(buf, cap, "the string '%.*s%s'", n, s, more); break;
    case OYLNG_T_INT:
    case OYLNG_T_BIGINT:   snprintf(buf, cap, "the integer %.*s%s", n, s, more); break;
    case OYLNG_T_FLOAT:    snprintf(buf, cap, "the float %.*s%s", n, s, more); break;
    case OYLNG_T_DECIMAL:  snprintf(buf, cap, "the decimal %.*s%s", n, s, more); break;
    case OYLNG_T_DATE:     snprintf(buf, cap, "the date %.*s", n, s); break;
    case OYLNG_T_DATETIME: snprintf(buf, cap, "the time %.*s", n, s); break;
    case OYLNG_T_BINARY:   snprintf(buf, cap, "the bytes %.*s%s", n, s, more); break;
    default:               snprintf(buf, cap, "%.*s", n, s); break;
    }
}

/* A value of a standard type, its standard spelling s */
static oylng_status typed_out(oylng_emitter *E, uint8_t want, const char *s, size_t n, const erec *extra) {
    place pl;
    oylng_status st = where(E, &pl, false);
    if (st != OYLNG_OK) return st;
    if (pl.key) return fail(E, OYLNG_ERR_EMIT, "a key is a string, and this is %s", type_names[want]);
    bool empty = empty_ok(&pl);
    if (holding(E)) {
        erec *r = rec_add(E, OYLNG_NODE_STRING, s, n, false);
        if (!r) return done(E);
        if (extra) *r = *extra;
        r->name = r->hint = SIZE_MAX;
        r->end = (uint32_t)E->nrec;
        r->want = want;
        r->flow = pl.flow;
        r->empty = empty;
        if (E->hint.data) {
            size_t at = text_add(E, E->hint.data, E->hint.len);
            if (at == SIZE_MAX) return done(E);
            r = &E->rec[E->nrec - 1];
            r->hint = at;
            r->hlen = E->hint.len;
            r->hplain = E->hplain;
        }
        oylng__tnew *t = &E->tn[E->nrec - 1];
        s = E->tb + t->off;   /* the record's, which lasts */
    }
    oylng_str c;
    bool plain;
    first_spelling(want, s, n, &pl, empty, &c, &plain);
    if (holding(E)) E->tn[E->nrec - 1].plain = plain;
    st = scalar_out(E, c.data, c.len, &pl, plain ? (c.len ? F_PLAIN : F_EMPTY) : F_QUOTED);
    if (st != OYLNG_OK) return st;
    node_done(E);
    return done(E);
}

/* ── The held document's end ─────────────────────────────── */

/* Typed, then written again as typed */
static oylng_status held_end(oylng_emitter *E) {
    oylng_tree *T;
    oylng_status st = oylng__tree_make(E->tn, E->nrec, E->tb, &T);
    if (st != OYLNG_OK)
        return fail(E, st, "%s", st == OYLNG_ERR_MEMORY ? "out of memory" : "a typed document too large to hold");
    oylng__speller sp = { E, next_spelling, fits, describe };
    oylng_typed *Y;
    char msg[400];
    /* the names bound here, as a parser reading it back would bind them:
       a value's code is the outermost bound name's (section 12) */
    oylng_str *bn = E->nbinds ? malloc(E->nbinds * sizeof *bn) : NULL;
    if (E->nbinds && !bn) {
        oylng_tree_free(T);
        E->len = E->mark;
        return fail(E, OYLNG_ERR_MEMORY, "out of memory");
    }
    for (size_t i = 0; i < E->nbinds; i++) bn[i] = (oylng_str){ E->binds[i].name, E->binds[i].len };
    st = oylng__type_emit(E->held, T, &sp, bn, (uint32_t)E->nbinds, &Y, msg, sizeof msg);
    free(bn);
    if (st != OYLNG_OK) {
        E->len = E->mark;
        if (E->oom) return done(E);   /* a spelling had no room */
        return fail(E, st == OYLNG_ERR_MEMORY ? st : OYLNG_ERR_EMIT, "%s", msg);
    }
    T = (oylng_tree *)oylng_typed_tree(Y);
    E->len = E->mark;
    E->bol = E->mbol;
    E->space = E->mspace;
    E->state = E_STREAM;
    E->replay = true;
    st = oylng_emit_doc_start(E, E->dname == SIZE_MAX ? (oylng_str){ NULL, 0 }
                                                      : (oylng_str){ E->tb + E->dname, E->dnlen });
    /* opn holds what each collection open still takes */
    E->nopn = 0;
    for (size_t i = 0; i < E->nrec && st == OYLNG_OK; i++) {
        const oylng__tnew *t = &E->tn[i];
        bool whole = true;
        if (t->kind == OYLNG_NODE_STRING) {
            oylng_str s = { E->tb + t->off, t->len };
            E->force = F_AUTO;
            if (E->rec[i].want != OYLNG_T_NONE) {
                s = oylng_node_text(T, (oylng_node)i);
                E->force = !oylng_node_plain(T, (oylng_node)i) ? F_QUOTED : s.len ? F_PLAIN : F_EMPTY;
            }
            st = oylng_emit_scalar(E, s);
        } else {
            bool map = t->kind == OYLNG_NODE_MAPPING;
            st = map ? oylng_emit_map_start(E, E->rec[i].flow) : oylng_emit_seq_start(E, E->rec[i].flow);
            if (st == OYLNG_OK && t->n) {
                if (grow32(E, (void **)&E->opn, &E->capopn, (uint64_t)E->nopn + 1, sizeof *E->opn))
                    E->opn[E->nopn++] = t->n;
                else st = done(E);
                whole = false;
            } else if (st == OYLNG_OK) st = map ? oylng_emit_map_end(E) : oylng_emit_seq_end(E);
        }
        while (st == OYLNG_OK && whole && E->nopn && !--E->opn[E->nopn - 1]) {
            E->nopn--;
            st = E->fr[E->sp - 1].map ? oylng_emit_map_end(E) : oylng_emit_seq_end(E);
        }
    }
    E->replay = false;
    E->force = F_AUTO;
    E->held = NULL;
    oylng_typed_free(Y);
    if (st != OYLNG_OK) return st;
    if (!E->bol) out_nl(E);
    E->space = false;
    E->state = E_STREAM;
    return done(E);
}

/* ── Public API ──────────────────────────────────────────── */

oylng_emitter *oylng_emitter_new(void) {
    oylng_emitter *E = calloc(1, sizeof *E);
    if (!E) return NULL;
    E->bol = true;
    E->indent = 2;
    E->state = E_BEFORE;
    return E;
}

void oylng_emitter_set_indent(oylng_emitter *E, unsigned indent) {
    if (!E) return;
    E->indent = indent < 1 ? 1 : indent > 8 ? 8 : indent;
}

void oylng_emitter_set_flow(oylng_emitter *E, bool flow) {
    if (E) E->flow_all = flow;
}

oylng_status oylng_emit_stream_start(oylng_emitter *E) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (E->state != E_BEFORE) return fail(E, OYLNG_ERR_EMIT, "the stream has started already");
    OUT(E, "%YAML 2.0");
    out_nl(E);
    E->state = E_STREAM;
    return done(E);
}

oylng_status oylng_emit_stream_end(oylng_emitter *E) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (E->state != E_STREAM)
        return fail(E, OYLNG_ERR_EMIT, "%s", E->state == E_BEFORE ? "the stream hasn't started" :
                                             E->state == E_END ? "the stream has ended already" :
                                             "a document is still open");
    E->state = E_END;
    return done(E);
}

oylng_status oylng_emit_doc_start(oylng_emitter *E, oylng_str name) {
    if (!E || (name.len && !name.data)) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (E->state != E_STREAM)
        return fail(E, OYLNG_ERR_EMIT, "%s", E->state == E_BEFORE ? "the stream hasn't started" :
                                             E->state == E_END ? "the stream has ended" :
                                             "a document is still open: end it first");
    if (name.data) {
        const unsigned char *u = (const unsigned char *)name.data, *ue = u + name.len;
        if (!name.len) return fail(E, OYLNG_ERR_EMIT, "a document's name can't be empty");
        for (const unsigned char *q = u; q < ue;) {
            if (*q == '#' && q > u) {
                /* the fragment, which ends the name */
                const unsigned char *h = ++q;
                while (q < ue && oylng_frag_char(*q)) q++;
                if (q == h || q < ue)
                    return fail(E, OYLNG_ERR_EMIT, "a document's name can end in # and one or more ASCII "
                                "letters, digits and =, and this one has another character, at its byte %zu",
                                (size_t)(q - u));
                break;
            }
            size_t n;
            uint32_t c = oylng_utf8(q, ue, &n);
            if (c == OYLNG_BAD_CP || !oylng_name_cp(c))
                return fail(E, OYLNG_ERR_EMIT, "a document's name is letters, digits and . _ - /, "
                            "and this one has another character, at its byte %zu", (size_t)(q - u));
            q += n;
        }
    }
    if (!E->replay && E->schema) {   /* held till its end (section 13) */
        E->held = E->schema;
        E->mark = E->len;
        E->mbol = E->bol;
        E->mspace = E->space;
        E->nrec = E->tlen = 0;
        E->nopn = 0;
        E->dname = SIZE_MAX;
        if (name.data && (E->dname = text_add(E, name.data, name.len)) == SIZE_MAX) return done(E);
        E->dnlen = name.len;
    }
    new_line(E, 0);
    OUT(E, "---");
    if (name.data) {
        OUT(E, " !");
        out_add(E, name.data, name.len);
    }
    E->space = true;
    E->state = E_ROOT;
    return done(E);
}

oylng_status oylng_emit_doc_end(oylng_emitter *E) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (E->state != E_DONE)
        return fail(E, OYLNG_ERR_EMIT, "%s", E->state == E_ROOT && E->sp ? "a collection is still open" :
                                             E->state == E_ROOT ? "the document has no value: give it one, "
                                                                  "an empty string if nothing else" :
                                             "no document is open");
    if (holding(E)) return held_end(E);
    if (!E->bol) out_nl(E);
    E->space = false;
    E->state = E_STREAM;
    return done(E);
}

oylng_status oylng_emit_scalar(oylng_emitter *E, oylng_str value) {
    if (!E || (value.len && !value.data)) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    place pl;
    oylng_status st = where(E, &pl, false);
    if (st != OYLNG_OK) return st;
    if (pl.key && !key_add(E, pl.parent, value.data, value.len) && !E->oom) {
        int n = value.len > 60 ? 60 : (int)value.len;
        return fail(E, OYLNG_ERR_EMIT, "the key \"%.*s\" is given twice in one mapping", n, value.data);
    }
    if (holding(E)) {
        erec *r = rec_add(E, OYLNG_NODE_STRING, value.data, value.len,
                          !pl.key && can_plain(value.data, value.len, &pl));
        if (!r) return done(E);
        if (!pl.key) {
            r->want = OYLNG_T_STR;
            r->flow = pl.flow;
            r->empty = empty_ok(&pl);
        }
    }
    st = scalar_out(E, value.data, value.len, &pl, E->replay ? E->force : F_AUTO);
    if (st != OYLNG_OK) return st;
    node_done(E);
    return done(E);
}

static oylng_status coll_start(oylng_emitter *E, bool map, bool flow) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    place pl;
    oylng_status st = where(E, &pl, true);
    if (st != OYLNG_OK) return st;
    if (E->sp >= OYLNG_MAX_LEVELS)
        return fail(E, OYLNG_ERR_EMIT, "more than %u levels", (unsigned)OYLNG_MAX_LEVELS);
    if (!grow32(E, (void **)&E->fr, &E->capfr, (uint64_t)E->sp + 1, sizeof *E->fr)) return done(E);
    const eframe *p = E->sp ? &E->fr[E->sp - 1] : NULL;   /* the frames may have moved */
    bool fl = flow || E->flow_all || pl.flow;
    eframe f = { 0 };
    f.map = map;
    f.flow = fl;
    f.compact = !fl && p && !p->map;
    f.col = !p || fl ? 0 : p->map ? p->col + E->indent : p->col + 2;
    f.k0 = E->nkeys;
    f.b0 = E->klen;
    f.t0 = E->nlix;
    if (holding(E)) {
        uint32_t at = (uint32_t)E->nrec;
        erec *r = rec_add(E, map ? OYLNG_NODE_MAPPING : OYLNG_NODE_SEQUENCE, NULL, 0, false);
        if (!r || !grow32(E, (void **)&E->opn, &E->capopn, (uint64_t)E->nopn + 1, sizeof *E->opn)) return done(E);
        r->flow = fl;
        E->opn[E->nopn++] = at;
    }
    if (fl) {
        inline_space(E);
        if (map) OUT(E, "{");
        else OUT(E, "[");
    }
    E->fr[E->sp++] = f;
    return done(E);
}

static oylng_status coll_end(oylng_emitter *E, bool map) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (!E->sp || E->fr[E->sp - 1].map != map)
        return fail(E, OYLNG_ERR_EMIT, "%s", !E->sp ? (map ? "no mapping is open" : "no sequence is open") :
                                             map ? "this ends a mapping, and a sequence is open" :
                                                   "this ends a sequence, and a mapping is open");
    eframe *f = &E->fr[E->sp - 1];
    if (map && f->n % 2) return fail(E, OYLNG_ERR_EMIT, "the mapping's last key has no value");
    if (holding(E)) {
        uint32_t at = E->opn[--E->nopn];
        E->tn[at].n = (uint32_t)f->n;
        E->rec[at].end = (uint32_t)E->nrec;
    }
    if (f->flow) {
        if (map) OUT(E, "}");
        else OUT(E, "]");
    } else if (!f->n) {
        inline_space(E);
        if (map) OUT(E, "{}");
        else OUT(E, "[]");
    }
    E->nkeys = f->k0;
    E->klen = f->b0;
    E->nlix = f->t0;
    E->sp--;
    node_done(E);
    return done(E);
}

oylng_status oylng_emit_map_start(oylng_emitter *E, bool flow) { return coll_start(E, true, flow); }
oylng_status oylng_emit_map_end(oylng_emitter *E)              { return coll_end(E, true); }
oylng_status oylng_emit_seq_start(oylng_emitter *E, bool flow) { return coll_start(E, false, flow); }
oylng_status oylng_emit_seq_end(oylng_emitter *E)              { return coll_end(E, false); }

oylng_status oylng_emit(oylng_emitter *E, const oylng_event *evt) {
    if (!E || !evt) return OYLNG_ERR_INPUT;
    switch (evt->type) {
    case OYLNG_EVT_STREAM_START: return oylng_emit_stream_start(E);
    case OYLNG_EVT_STREAM_END:   return oylng_emit_stream_end(E);
    case OYLNG_EVT_DOC_START:    return oylng_emit_doc_start(E, evt->value);
    case OYLNG_EVT_DOC_END:      return oylng_emit_doc_end(E);
    case OYLNG_EVT_MAP_START:    return oylng_emit_map_start(E, false);
    case OYLNG_EVT_MAP_END:      return oylng_emit_map_end(E);
    case OYLNG_EVT_SEQ_START:    return oylng_emit_seq_start(E, false);
    case OYLNG_EVT_SEQ_END:      return oylng_emit_seq_end(E);
    case OYLNG_EVT_SCALAR:       return oylng_emit_scalar(E, evt->value);
    default:                     return OYLNG_ERR_INPUT;
    }
}

/* For oylng_emit_tree (oylng_internal.h) */
oylng_status oylng__emitter_fail(oylng_emitter *E, oylng_status st, const char *msg) {
    if (!live(E)) return E->status;
    return fail(E, st, "%s", msg);
}

oylng_str oylng_emitter_output(const oylng_emitter *E) {
    if (!E || !E->out) return (oylng_str){ "", 0 };
    return (oylng_str){ E->out, E->held ? E->mark : E->len };   /* a document held isn't output yet */
}

oylng_status oylng_emitter_take(oylng_emitter *E, oylng_str *out) {
    if (!E || !out) return OYLNG_ERR_INPUT;
    *out = (oylng_str){ "", 0 };
    if (!live(E)) return E->status;
    *out = oylng_emitter_output(E);
    E->len = E->mark = 0;   /* what a held document has written so far is thrown away at its end */
    return OYLNG_OK;
}

const char *oylng_emitter_error(const oylng_emitter *E) {
    return E && E->state == E_FAILED ? E->msg : NULL;
}

void oylng_emitter_free(oylng_emitter *E) {
    if (!E) return;
    free(E->out);
    free(E->fr);
    free(E->keys);
    free(E->kbuf);
    free(E->lix);
    free(E->rec);
    free(E->tn);
    free(E->opn);
    free(E->tb);
    free(E->cb);
    for (size_t i = 0; i < E->nbinds; i++) free(E->binds[i].name);
    free(E->binds);
    free(E);
}

/* ── Typed data: the calls (part 2, section 13) ──────────── */

oylng_status oylng_emitter_set_schema(oylng_emitter *E, const oylng_schema *s) {
    if (!E || (s && (s->st != OYLNG_OK || !s->root))) return OYLNG_ERR_INPUT;
    E->schema = s;
    return OYLNG_OK;
}

oylng_status oylng_emit_int(oylng_emitter *E, int64_t v) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    char b[24];
    int n = snprintf(b, sizeof b, "%lld", (long long)v);
    return typed_out(E, OYLNG_T_INT, b, (size_t)n, NULL);
}

oylng_status oylng_emit_uint(oylng_emitter *E, uint64_t v) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    char b[24];
    int n = snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    return typed_out(E, OYLNG_T_INT, b, (size_t)n, NULL);
}

/* f, which is finite, in the fewest significant digits that read back as
 * it (section 4.4): positional for 0 and from 1e-7 to below 1e21, with .0
 * where it has no fraction, so that it never reads as an integer, else
 * with an exponent and its sign: 0.1, 2.0, -0.0, 1e+21, 5e-324. The
 * digits are found by printf, whatever the locale's point, and read back
 * by the parser's own reader. */
static size_t float_text(double f, char *out) {
    char buf[48], dig[24] = "0";
    int nd = 1, x = 0;
    bool neg = signbit(f);
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, f);
        const char *q = buf;
        nd = 0;
        for (; *q && *q != 'e' && *q != 'E'; q++)
            if (*q >= '0' && *q <= '9' && nd < 20) dig[nd++] = *q;
        x = *q ? atoi(q + 1) : 0;
        char c[48];
        size_t cn = 0;
        if (neg) c[cn++] = '-';
        c[cn++] = dig[0];
        if (nd > 1) {
            c[cn++] = '.';
            memcpy(c + cn, dig + 1, (size_t)nd - 1);
            cn += (size_t)nd - 1;
        }
        cn += (size_t)snprintf(c + cn, sizeof c - cn, "e%d", x);
        double g;
        if (oylng__float(c, cn, &g) && g == f) break;
    }
    size_t k = 0;   /* the fewest digits never end in 0: one fewer would do */
    if (neg) out[k++] = '-';
    if (x >= 21 || x < -7) {
        out[k++] = dig[0];
        if (nd > 1) {
            out[k++] = '.';
            memcpy(out + k, dig + 1, (size_t)nd - 1);
            k += (size_t)nd - 1;
        }
        k += (size_t)snprintf(out + k, 16, "e%+d", x);
    } else if (x >= 0) {
        for (int i = 0; i <= x; i++) out[k++] = i < nd ? dig[i] : '0';
        out[k++] = '.';
        if (nd > x + 1) {
            memcpy(out + k, dig + x + 1, (size_t)(nd - x - 1));
            k += (size_t)(nd - x - 1);
        } else out[k++] = '0';
    } else {
        out[k++] = '0';
        out[k++] = '.';
        for (int i = 0; i < -x - 1; i++) out[k++] = '0';
        memcpy(out + k, dig, (size_t)nd);
        k += (size_t)nd;
    }
    return k;
}

oylng_status oylng_emit_float(oylng_emitter *E, double v) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (!isfinite(v)) return fail(E, OYLNG_ERR_EMIT, "a float that isn't finite: YAML 2.0 writes none");
    char b[64];
    size_t n = float_text(v, b);
    erec x = { .x.f = v };
    return typed_out(E, OYLNG_T_FLOAT, b, n, &x);
}

oylng_status oylng_emit_bool(oylng_emitter *E, bool v) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    return typed_out(E, OYLNG_T_BOOL, v ? "true" : "false", v ? 4 : 5, NULL);
}

oylng_status oylng_emit_null(oylng_emitter *E) {
    if (!E) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    return typed_out(E, OYLNG_T_NULL, "null", 4, NULL);
}

oylng_status oylng_emit_number(oylng_emitter *E, oylng_type type, oylng_str text) {
    if (!E || (type != OYLNG_T_BIGINT && type != OYLNG_T_DECIMAL) || (text.len && !text.data))
        return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    bool ok = type == OYLNG_T_BIGINT ? oylng__int_spelling(text.data, text.len)
                                     : oylng__float_spelling(text.data, text.len);
    if (!ok)
        return fail(E, OYLNG_ERR_EMIT, "'%.*s' isn't %s in the standard spelling", (int)(text.len > 40 ? 40 : text.len),
                    text.data ? text.data : "", type == OYLNG_T_BIGINT ? "an integer" : "a number");
    return typed_out(E, (uint8_t)type, text.data, text.len, NULL);
}

static bool same_dt(const oylng_datetime *a, const oylng_datetime *b, bool full) {
    if (a->year != b->year || a->month != b->month || a->day != b->day) return false;
    return !full || (a->hour == b->hour && a->minute == b->minute && a->second == b->second &&
                     a->nanosecond == b->nanosecond && a->offset == b->offset && a->zulu == b->zulu &&
                     a->unknown_offset == b->unknown_offset);
}

oylng_status oylng_emit_datetime(oylng_emitter *E, oylng_type type, const oylng_datetime *dt) {
    if (!E || !dt || (type != OYLNG_T_DATE && type != OYLNG_T_DATETIME)) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    bool full = type == OYLNG_T_DATETIME;
    char b[64];
    int n = snprintf(b, sizeof b, "%04d-%02u-%02u", (int)dt->year, dt->month, dt->day);
    if (full && n > 0 && n < 40) {
        n += snprintf(b + n, sizeof b - (size_t)n, "T%02u:%02u:%02u", dt->hour, dt->minute, dt->second);
        if (dt->nanosecond) {
            char f[16];
            snprintf(f, sizeof f, "%09lu", (unsigned long)dt->nanosecond);
            int fl = (int)strlen(f);
            while (fl > 1 && f[fl - 1] == '0') fl--;
            n += snprintf(b + n, sizeof b - (size_t)n, ".%.*s", fl, f);
        }
        int off = dt->offset < 0 ? -dt->offset : dt->offset;
        if (dt->zulu) n += snprintf(b + n, sizeof b - (size_t)n, "Z");
        else n += snprintf(b + n, sizeof b - (size_t)n, "%c%02d:%02d",
                           dt->offset < 0 || dt->unknown_offset ? '-' : '+', off / 60, off % 60);
    }
    oylng_datetime back;
    if (n <= 0 || (size_t)n >= sizeof b || oylng__date(b, (size_t)n, full, false, &back) != 1 ||
        !same_dt(&back, dt, full))
        return fail(E, OYLNG_ERR_EMIT, "%s, as RFC 3339 writes it, doesn't read back as that %s",
                    n > 0 && (size_t)n < sizeof b ? b : "a date", full ? "date and time" : "date");
    erec x = { .x.dt = *dt };
    return typed_out(E, (uint8_t)type, b, (size_t)n, &x);
}

oylng_status oylng_emit_bytes(oylng_emitter *E, oylng_str bytes) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!E || (bytes.len && !bytes.data)) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    if (bytes.len > (SIZE_MAX - 8) / 4 * 3) return fail(E, OYLNG_ERR_LIMIT, "too many bytes to write");
    if (!grow(E, (void **)&E->cb, &E->ccap, (bytes.len + 2) / 3 * 4 + 1, 1)) return done(E);
    const unsigned char *s = (const unsigned char *)bytes.data;
    size_t k = 0, n = bytes.len;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        E->cb[k++] = A[v >> 18 & 63];
        E->cb[k++] = A[v >> 12 & 63];
        E->cb[k++] = i + 1 < n ? A[v >> 6 & 63] : '=';
        E->cb[k++] = i + 2 < n ? A[v & 63] : '=';
    }
    erec x = { 0 };
    if (holding(E)) {
        if ((x.b = text_add(E, bytes.data, bytes.len)) == SIZE_MAX) return done(E);
        x.blen = bytes.len;
    }
    return typed_out(E, OYLNG_T_BINARY, E->cb, k, &x);
}

oylng_status oylng_emit_value(oylng_emitter *E, const oylng_typed *t, oylng_value v) {
    if (!E || !t || oylng_value_type(t, v) == OYLNG_T_NONE) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    const oylng_tree *T = oylng_typed_tree(t);
    /* the collections open: a value's or, below an untyped one, a node's */
    struct vw { bool node, map; uint32_t id; size_t next, n; } local[32], *stk = local;
    size_t n = 0, cap = 32;
    bool node = false;
    uint32_t id = v;
    oylng_status st = OYLNG_OK;
    for (;;) {
        bool coll = false, map = false;
        size_t count = 0;
        if (!node && oylng_value_type(t, id) == OYLNG_T_ANY) {
            node = true;
            id = oylng_value_node(t, id);
        }
        if (node) {
            oylng_kind k = oylng_node_kind(T, id);
            if (k == OYLNG_NODE_STRING) st = oylng_emit_scalar(E, oylng_node_text(T, id));
            else {
                coll = true;
                map = k == OYLNG_NODE_MAPPING;
                count = oylng_node_count(T, id);
            }
        } else {
            oylng_type ty = oylng_value_type(t, id);
            if (ty != OYLNG_T_STR && ty != OYLNG_T_SEQUENCE && ty != OYLNG_T_MAPPING) {
                /* how it was written reads back too, where the standard
                   spelling doesn't: a pattern's, say */
                oylng_node nd = oylng_value_node(t, id);
                E->hint = oylng_node_text(T, nd);
                E->hplain = oylng_node_plain(T, nd);
            }
            switch (ty) {
            case OYLNG_T_STR: st = oylng_emit_scalar(E, oylng_value_text(t, id)); break;
            case OYLNG_T_INT: {
                int64_t i;
                uint64_t u = 0;
                if (oylng_value_int(t, id, &i)) st = oylng_emit_int(E, i);
                else {
                    oylng_value_uint(t, id, &u);
                    st = oylng_emit_uint(E, u);
                }
                break;
            }
            case OYLNG_T_BIGINT:
            case OYLNG_T_DECIMAL: st = oylng_emit_number(E, ty, oylng_value_text(t, id)); break;
            case OYLNG_T_FLOAT:   st = oylng_emit_float(E, oylng_value_float(t, id)); break;
            case OYLNG_T_BOOL:    st = oylng_emit_bool(E, oylng_value_bool(t, id)); break;
            case OYLNG_T_NULL:    st = oylng_emit_null(E); break;
            case OYLNG_T_DATE:
            case OYLNG_T_DATETIME: {
                oylng_datetime d;
                oylng_value_datetime(t, id, &d);
                st = oylng_emit_datetime(E, ty, &d);
                break;
            }
            case OYLNG_T_BINARY: st = oylng_emit_bytes(E, oylng_value_bytes(t, id)); break;
            default:
                coll = true;
                map = ty == OYLNG_T_MAPPING;
                count = oylng_value_count(t, id);
                break;
            }
            E->hint = (oylng_str){ NULL, 0 };
        }
        if (coll) {
            st = map ? oylng_emit_map_start(E, false) : oylng_emit_seq_start(E, false);
            if (st == OYLNG_OK && n == cap) {
                struct vw *ns = malloc(2 * cap * sizeof *ns);
                if (!ns) st = fail(E, OYLNG_ERR_MEMORY, "out of memory");
                else {
                    memcpy(ns, stk, n * sizeof *ns);
                    if (stk != local) free(stk);
                    stk = ns;
                    cap *= 2;
                }
            }
            if (st == OYLNG_OK) stk[n++] = (struct vw){ node, map, id, 0, count };
        }
        /* the next: a child of the innermost collection with one left */
        while (st == OYLNG_OK && n && stk[n - 1].next == stk[n - 1].n) {
            st = stk[n - 1].map ? oylng_emit_map_end(E) : oylng_emit_seq_end(E);
            n--;
        }
        if (st != OYLNG_OK || !n) break;
        struct vw *w = &stk[n - 1];
        size_t i = w->next++;
        node = w->node;
        if (w->map) {
            st = oylng_emit_scalar(E, node ? oylng_node_text(T, oylng_node_key(T, w->id, i)) : oylng_value_key(t, w->id, i));
            if (st != OYLNG_OK) break;
            id = node ? oylng_node_value(T, w->id, i) : oylng_value_at(t, w->id, i);
        } else id = node ? oylng_node_item(T, w->id, i) : oylng_value_item(t, w->id, i);
    }
    if (stk != local) free(stk);
    return st;
}

static struct ebind *bind_of(oylng_emitter *E, oylng_str name) {
    for (size_t i = 0; i < E->nbinds; i++)
        if (E->binds[i].len == name.len && (!name.len || memcmp(E->binds[i].name, name.data, name.len) == 0))
            return &E->binds[i];
    return NULL;
}

oylng_status oylng_emitter_bind(oylng_emitter *E, oylng_str name, oylng_unbind_fn fn, void *ctx) {
    if (!E || (name.len && !name.data)) return OYLNG_ERR_INPUT;
    struct ebind *b = bind_of(E, name);
    if (b && fn) {
        b->fn = fn;
        b->ctx = ctx;
        return OYLNG_OK;
    }
    if (b) {
        free(b->name);
        *b = E->binds[--E->nbinds];
        return OYLNG_OK;
    }
    if (!fn) return OYLNG_OK;
    if (E->nbinds == E->capbinds) {
        size_t c = E->capbinds ? 2 * E->capbinds : 8;
        struct ebind *nb = realloc(E->binds, c * sizeof *nb);
        if (!nb) return OYLNG_ERR_MEMORY;
        E->binds = nb;
        E->capbinds = c;
    }
    char *nm = malloc(name.len + 1);
    if (!nm) return OYLNG_ERR_MEMORY;
    if (name.len) memcpy(nm, name.data, name.len);
    E->binds[E->nbinds++] = (struct ebind){ nm, name.len, fn, ctx };
    return OYLNG_OK;
}

oylng_status oylng_emit_object(oylng_emitter *E, oylng_str name, const void *object) {
    if (!E || (name.len && !name.data)) return OYLNG_ERR_INPUT;
    if (!live(E)) return E->status;
    const struct ebind *b = bind_of(E, name);
    if (!b) return OYLNG_ERR_INPUT;
    int nl = (int)(name.len > 40 ? 40 : name.len);
    if (E->sp && E->fr[E->sp - 1].map && E->fr[E->sp - 1].n % 2 == 0)
        return fail(E, OYLNG_ERR_EMIT, "a key is a string, and this is a %.*s object", nl, name.data);
    oylng_unbind_fn fn = b->fn;
    void *ctx = b->ctx;
    uint32_t sp = E->sp;
    uint64_t n0 = sp ? E->fr[sp - 1].n : 0;
    bool root = !sp && E->state == E_ROOT, rec = holding(E);
    size_t r0 = E->nrec;
    const char *why = fn(ctx, E, object);
    if (!live(E)) return E->status;
    if (why) return fail(E, OYLNG_ERR_EMIT, "a %.*s object: %s", nl, name.data, why);
    uint64_t wrote = sp ? (E->sp == sp ? E->fr[sp - 1].n - n0 : 0) : root && E->state == E_DONE;
    const char *bad = E->sp > sp ? "a collection it didn't end" : E->sp < sp ? "past its collection's end"
                    : !wrote ? "nothing" : wrote > 1 ? "more than one value" : NULL;
    if (bad) return fail(E, OYLNG_ERR_EMIT, "the code bound to %.*s wrote %s", nl, name.data, bad);
    if (rec) {
        erec *r = &E->rec[r0];
        if (r->name != SIZE_MAX && !(r->nlen == name.len && memcmp(E->tb + r->name, name.data, name.len) == 0))
            return fail(E, OYLNG_ERR_EMIT, "a value can't be both a %.*s object and a %.*s",
                        (int)(r->nlen > 40 ? 40 : r->nlen), E->tb + r->name, nl, name.data);
        size_t at = text_add(E, name.data, name.len);
        if (at == SIZE_MAX) return done(E);
        r = &E->rec[r0];
        r->name = at;
        r->nlen = name.len;
    }
    return OYLNG_OK;
}

