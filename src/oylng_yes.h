/*
 * oylng_yes.h — what part 2's files share: numbers as written, and a
 * compiled schema
 *
 * Part 2 is YAML Easy Schema (YES), spec/yaml-easy-schema.md at trans/yaml2.
 * oylng_number.c reads scalars, oylng_schema.c compiles a schema from a
 * table, and oylng_typed.c types a tree with one.
 */

#ifndef OYLNG_YES_H
#define OYLNG_YES_H

#include "oylng_internal.h"
#include "oylng/schema.h"
#include <setjmp.h>

/* ── Numbers (oylng_number.c) ────────────────────────────── */

/* A number as written: 0.D times 10^(E + k), D its significant digits from
 * dig to dig_end with any '.' among them skipped, nd of them, and E the
 * written exponent's digits. Zero has nd 0. Pointers are into the text. */
typedef struct {
    bool        neg, eneg;
    const char *dig, *dig_end;
    size_t      nd;
    const char *exp;
    size_t      ne;
    int64_t     k;
} oylng__num;

/* A precision or scale past this is as good as infinite */
#define OYLNG__SATURATED ((uint64_t)1 << 62)

/* [-+]? digits (. digits)? ([eE] [-+]? digits)?, with digits on at least
 * one side of the point: false if s isn't that */
bool oylng__num_read(const char *s, size_t n, oylng__num *out);
int  oylng__num_cmp(const oylng__num *a, const oylng__num *b);
/* Section 4.6: at most scale digits after the point and prec - scale
 * before it, on the exact value */
bool oylng__num_fits(const oylng__num *a, uint64_t prec, uint64_t scale);
/* n as a number, its digits in buf (24 bytes) */
void oylng__num_of_count(uint64_t n, char *buf, oylng__num *out);
/* !int's spelling as -2^63 to 2^64-1: false if outside */
bool oylng__int(const char *s, size_t n, bool *neg, uint64_t *mag);
/* JSON's number spelling as the nearest binary64: false if too large */
bool oylng__float(const char *s, size_t n, double *f);
/* An RFC 3339 full-date, or date-time if full: 1, 0 if it isn't one, -1
 * if it is and this parser can't hold it */
int  oylng__date(const char *s, size_t n, bool full, bool cut, oylng_datetime *out);
/* Base64 as section 4.1 has it: false if it isn't; the bytes into out if
 * out isn't NULL (n * 3 / 4 bytes are enough), and how many */
bool oylng__base64(const char *s, size_t n, unsigned char *out, size_t *outlen);

/* ── Patterns (oylng_regex.c, oylng_ucd.c) ───────────────────
 * I-Regexp (RFC 9485) as section 4.3 has it, matched whole. */

typedef struct oylng__rx oylng__rx;

/* What matching needs, kept by the caller between matches */
typedef struct {
    uint32_t *buf;
    size_t    cap;
    uint32_t  gen;
} oylng__rxscr;

/* Reads the pattern p, n bytes of UTF-8 between the slashes: 0 if it is
 * I-Regexp, with *count its count (section 9, saturating) and *out its
 * matcher, in A, if the count is no more than build_max (else NULL); -1 if
 * it isn't, *why saying why; -2 if memory runs out. */
int  oylng__rx_compile(oylng_arena *A, const char *p, size_t n, uint64_t build_max, oylng__rx **out,
                       uint64_t *count, const char **why);
/* 1 if the whole of s matches, 0 if not, -1 if memory runs out */
int  oylng__rx_match(const oylng__rx *rx, const char *s, size_t n, oylng__rxscr *scr);
void oylng__rxscr_free(oylng__rxscr *scr);

/* A code point's general category: 0 Lu to 29 Cn, in oylng_regex.c's order */
unsigned oylng__ucd_cat(uint32_t cp);
extern const char oylng__ucd_version[];

/* ── A compiled schema (oylng_schema.c) ──────────────────────
 * A type expression is an xt. A schema is compiled from a table's nodes,
 * each once, so a node pasted in two places is one type. A place is an xt
 * a value is typed with; each has its alternatives, with names expanded
 * (section 9). */

enum { X_ALT, X_LIT, X_STD, X_NAME, X_SEQ, X_MAP };

enum { STD_STR, STD_INT, STD_BIGINT, STD_FLOAT, STD_DECIMAL, STD_BOOL, STD_NULL,
       STD_DATE, STD_DATETIME, STD_BINARY, STD_ANY };

#define XF_LO      1u    /* X_STD: a limit's lower end */
#define XF_HI      2u    /* and its upper end */
#define XF_PREC    4u    /* a precision and scale */
#define XF_SEEN    8u    /* compile: gone through */
#define XF_PLACE  16u    /* counted in the schema's full size */
#define XF_TYPES  32u    /* values are typed with it, so it has its alternatives */
#define XF_FILLED 64u    /* X_SEQ, X_MAP: its children are compiled */
#define XF_PAT   128u    /* X_STD: a pattern */
#define XF_BARE  256u    /* X_MAP: the rules of a `!**` written as a type, its
                            only rule !* (section 10) */

typedef struct oylng__xt    xt;
typedef struct oylng__entry centry;

typedef struct {
    const char *s;
    uint32_t    len;
} xstr;

typedef struct {
    xstr     name;
    xt      *type;
    bool     opt;
} xfield;

typedef struct {
    xt *key, *val;
} xrow;

/* An alternative with names expanded: a literal, a standard type, a
 * sequence or a mapping, and the nearest entry it was reached through */
typedef struct {
    xt     *t;
    centry *from;
} xpos;

typedef struct {
    xpos     *p;
    uint32_t  n;
    uint32_t *lits;       /* a hash table of literal positions, each its
                             number + 1, or 0; litcap slots */
    uint32_t  litcap;
    uint32_t  seq;        /* the sequence position, or UINT32_MAX */
    uint32_t *maps;       /* the mapping positions */
    uint32_t  nmaps;
    uint32_t  any;        /* the first !any position, or UINT32_MAX */
    bool      all_lits;   /* literals only */
} xalts;

struct oylng__xt {
    uint8_t  kind, std;
    uint16_t flags;
    uint32_t pos;         /* where it is written, in the schema's text */
    uint32_t node;        /* X_SEQ, X_MAP: its node in the table, or UINT32_MAX */
    uint32_t mark;        /* the walk last through it */
    xalts   *alts;        /* XF_TYPES: its alternatives */
    xt      *kind_of;     /* XF_TYPES: the type it is, for the typed count
                             (section 11): a name is its entry's */
    union {
        struct { xt **v; uint32_t n; } alt;
        xstr lit;
        struct {
            oylng__num lo, hi;
            uint64_t   prec, scale;
            oylng__rx *rx;        /* XF_PAT: its matcher, if within the ceiling */
            uint64_t   rxcount;   /* XF_PAT: its count (section 9) */
        } std;
        centry *name;
        struct { xt **items; uint32_t n; xt *rest; } seq;
        struct {
            xfield   *f;
            uint32_t  nf, nreq;
            uint32_t *ix;     /* fields by name: each its number + 1, or 0 */
            uint32_t  ixcap;
            xrow     *rows;   /* type keys, in the order written */
            uint32_t  nrows;
            xt       *other;  /* !* */
            xt       *deep;   /* !**: its rules, a mapping (section 10) */
        } map;
    } u;
};

struct oylng__entry {
    xstr     name;
    uint32_t node;        /* its node in the table */
    uint32_t pentry;      /* the parser's number for it */
    xt      *top;         /* compiled, once reached */
    xt      *root;        /* a name for it, as a document's root */
    uint8_t  state;       /* the check for names that only name themselves */
    bool     reached, root_ok;
    uint32_t mark;        /* the union last expanding it */
    uint64_t full;
    xt      *kind_of;
    uint32_t tname_gen, tname;   /* its name in the typed document gen */
    centry  *next;        /* in the hash chain */
};

typedef struct oylng__sctx sctx;

struct oylng__sctx {
    oylng_arena   *A;
    oylng_hash_key key;
    oylng_parser  *P;          /* the table, while compiling */
    const char    *in;         /* the table's text, for marks */
    size_t         inlen;
    uint64_t       limit;
    bool           stamp;      /* the stream's own table: reached entries are marked */
    centry       **buckets;
    uint32_t       nbuckets, nentries;
    struct { uint32_t node; xt *t; } *memo;   /* table node to xt */
    uint32_t       memocap, nmemo;
    uint64_t       full_all;   /* of every place */
    uint32_t       gen;        /* for marks */
    uint32_t       std_used;   /* the standard names a type used, a bit each */
    uint32_t       entries_seen;   /* the table's entries looked at so far */
    /* what the root being compiled added */
    xt           **fresh, **fresh_types;
    uint32_t       nfresh, capfresh, nfresh_types, capfresh_types;
    centry       **fresh_entries;
    uint32_t       nfresh_entries, capfresh_entries;
    xt           **fresh_deeps;   /* the rules of each `!**` it reached */
    uint32_t       nfresh_deeps, capfresh_deeps;
    /* scratch */
    xt           **stk;
    uint32_t       nstk, capstk;
    void          *scr32;
    uint32_t       capscr32;
    void          *scrpos;
    uint32_t       capscrpos;
    /* errors */
    jmp_buf        jb;
    oylng_status   st;
    size_t         errpos;
    char           msg[200];
};

/* The standard names, by STD_ */
extern const char *const oylng__std_names[];

sctx *oylng__sctx_new(oylng_parser *p, bool stamp);
void  oylng__sctx_free(void *ctx);
/* The root type for entry name of the table, compiled and checked: NULL
 * if the table has none, or if compiling it failed (s->st says) */
xt   *oylng__sctx_root(sctx *s, const char *name, size_t len, bool *found);
uint64_t oylng__hash_str(const sctx *s, const char *p, size_t n);
/* The field of map type m named k, or NULL */
const xfield *oylng__field(const sctx *s, const xt *m, const char *k, size_t n);
/* The literal position of a with text k, or UINT32_MAX */
uint32_t oylng__lit(const sctx *s, const xalts *a, const char *k, size_t n);

struct oylng_schema {
    sctx        *ctx;
    xt          *root;
    oylng_status st;
    char         msg[200];
    oylng_mark   mark;
};

#endif /* OYLNG_YES_H */
