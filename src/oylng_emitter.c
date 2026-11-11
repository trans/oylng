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
 */

#include "oylng_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_KEYS 8          /* keys a mapping compares one by one, before it hashes them */
#define EMPTY_SLOT UINT32_MAX

enum { E_BEFORE, E_STREAM, E_ROOT, E_DONE, E_END, E_FAILED };

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
    char   msg[192];

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

static oylng_status scalar_out(oylng_emitter *E, const char *s, size_t n, const place *pl) {
    const unsigned char *u = (const unsigned char *)s;
    /* the common string, one pass: such bytes throughout, written plain
       unless its first or last byte, or what it is as a key, says not */
    if (n) {
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
    else if (!lf && !dq && !tab && plain_ok(s, n, pl)) out_add(E, s, n);
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
            size_t n;
            uint32_t c = oylng_utf8(q, ue, &n);
            if (c == OYLNG_BAD_CP || !oylng_name_cp(c))
                return fail(E, OYLNG_ERR_EMIT, "a document's name is letters, digits and . _ - /, "
                            "and this one has another character, at its byte %zu", (size_t)(q - u));
            q += n;
        }
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
    st = scalar_out(E, value.data, value.len, &pl);
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
    return (oylng_str){ E->out, E->len };
}

oylng_status oylng_emitter_take(oylng_emitter *E, oylng_str *out) {
    if (!E || !out) return OYLNG_ERR_INPUT;
    *out = (oylng_str){ "", 0 };
    if (!live(E)) return E->status;
    *out = oylng_emitter_output(E);
    E->len = 0;
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
    free(E);
}
