/**
 * @file schema.h
 * @brief Oyl NG: YAML Easy Schema, part 2 of the YAML 2.0 proposal
 *
 * Part 2 is spec/yaml-easy-schema.md at https://github.com/trans/yaml2, a
 * work in progress, and so is this. A document is typed by its stamp,
 * `--- !name` with a name the stream's own table has, or else by a schema
 * the application gives (section 3). A document neither types stays
 * strings. A document that doesn't fit its schema is refused, as a syntax
 * error is: the stream ends with OYLNG_ERR_SCHEMA, and the parser's error
 * gives the path to the value, with the line and column of the value.
 *
 * Patterns are I-Regexp (RFC 9485), matched in time linear in the text,
 * with Unicode 16's categories for \p{..}; groups nest at most 256 deep.
 * Below a `!**` (section 10), what no rule picks out is walked: a mapping
 * or a sequence is typed OYLNG_T_MAPPING or OYLNG_T_SEQUENCE, its children
 * looked up in turn, and a scalar takes the rules' `!*`, or is
 * OYLNG_T_STR; what is walked has no name (oylng_value_name). A published
 * name (section 3.1) stamps a document only if the application gives the
 * parser a lookup (oylng_parser_set_lookup); without one it stamps
 * nothing, as any other global name. An application's code bound to a
 * named type (oylng_parser_bind, section 12) makes an object of each value
 * of that type, once the document is typed. An emitter given a schema
 * (oylng_emitter_set_schema, section 13) writes typed data so that the
 * schema reads it back as it was given, or refuses the document.
 */

#ifndef OYLNG_SCHEMA_H
#define OYLNG_SCHEMA_H

#include "oylng/oylng.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Schemas ─────────────────────────────────────────────── */

typedef struct oylng_schema oylng_schema;

/** An application's schema (section 3): the table of the YAML 2.0 stream
 *  of @p len bytes at @p yaml, and the entry @p root, which types each
 *  document that has no stamp. With root.data NULL, every entry is checked
 *  as a root, and the schema types nothing: a check of a schema file.
 *  @p limit is the size limit: the schema is a stream of its own, read
 *  under it (part 1, section 10), and its full size (section 9) is held to
 *  it, as section 3 has it; 0 for the default, UINT64_MAX for none. The text needn't outlive the call. NULL if memory runs out;
 *  otherwise see oylng_schema_status. */
OYLNG_API oylng_schema *oylng_schema_new(const char *yaml, size_t len, oylng_str root, uint64_t limit);

/** OYLNG_OK, or why the schema can't be used: the stream's error, or
 *  OYLNG_ERR_SCHEMA. */
OYLNG_API oylng_status oylng_schema_status(const oylng_schema *s);

/** What is wrong, and where in the schema's text; NULL if nothing is. */
OYLNG_API const char *oylng_schema_error(const oylng_schema *s);
OYLNG_API oylng_mark  oylng_schema_error_mark(const oylng_schema *s);

OYLNG_API void oylng_schema_free(oylng_schema *s);

/** Let the parser cut a time's fraction to the 9 digits it holds, never
 *  rounding up, rather than refuse a time with more (section 4.8). Off
 *  unless set. */
OYLNG_API void oylng_parser_set_cut(oylng_parser *p, bool cut);

/** Whether a stamp types its document; on unless set off (section 3: an
 *  application may ignore a file's schema). Off, a stamp types nothing, as
 *  one naming nothing doesn't: nothing is looked up or compiled for it, so
 *  a schema in error refuses nothing, and the document is typed as one
 *  with no stamp, by the application's schema, or with none, not at all,
 *  read as it is. So an application checking files it didn't write can
 *  hold each to its own schema, whatever the file says. It can be changed
 *  between documents. For a stamp in the stream's table, oylng_parser_table
 *  gives the schema ignored. */
OYLNG_API void oylng_parser_set_stamps(oylng_parser *p, bool on);

/** An application's lookup of published names (section 3.1). Given a
 *  name's address, the name before its #, to which https:// leads, and its
 *  SHA-256 in 64 lowercase hex digits: true, with *file set to the bytes
 *  of the file, or false if it can't get the file, which refuses the
 *  document. A copy from anywhere will do, a mirror or a folder of the
 *  schemas the application accepts, as the parser refuses a file that
 *  doesn't hash to the name. It reads the bytes before it returns to the
 *  application, and keeps none of them. */
typedef bool (*oylng_lookup_fn)(void *ctx, oylng_str address, oylng_str sha256, oylng_str *file);

/** Look published names up with @p fn, which is given @p ctx; NULL, the
 *  default, to look nothing up. A name is looked up once a stream: the
 *  file is compiled, as a stream of its own under the parser's size
 *  limit, and kept for the stream's other documents with that name. Set it
 *  before the first document. */
OYLNG_API void oylng_parser_set_lookup(oylng_parser *p, oylng_lookup_fn fn, void *ctx);

/* ── Typed documents ─────────────────────────────────────── */

typedef enum {
    OYLNG_T_NONE = 0,   /**< not a value */
    OYLNG_T_ANY,        /**< untyped: a node of the tree, as !any gives it
                             and as a document no schema types is */
    OYLNG_T_STR,
    OYLNG_T_INT,        /**< -2^63 to 2^64-1 */
    OYLNG_T_BIGINT,     /**< an integer of any size, its text as written,
                             or, matched by a pattern, as !int spells it */
    OYLNG_T_FLOAT,      /**< the nearest binary64 */
    OYLNG_T_DECIMAL,    /**< a number exactly as written, its text; matched
                             by a pattern, in the standard spelling: no +,
                             no leading zeros, a digit each side of a point */
    OYLNG_T_BOOL,
    OYLNG_T_NULL,
    OYLNG_T_DATE,
    OYLNG_T_DATETIME,
    OYLNG_T_BINARY,
    OYLNG_T_SEQUENCE,
    OYLNG_T_MAPPING,
} oylng_type;

/** A date, or a date and time, as RFC 3339 writes it (section 4.8) */
typedef struct {
    int32_t  year;            /**< 0 to 9999 */
    uint8_t  month, day;
    uint8_t  hour, minute, second;   /**< a second of 60 is a leap second */
    uint32_t nanosecond;      /**< the fraction, cut to 9 digits if asked */
    int16_t  offset;          /**< minutes east of UTC */
    bool     zulu;            /**< written Z */
    bool     unknown_offset;  /**< written -00:00, RFC 3339's unknown offset */
} oylng_datetime;

typedef struct oylng_typed oylng_typed;

/** A value of a typed document, numbered from 0 */
typedef uint32_t oylng_value;

#define OYLNG_NO_VALUE UINT32_MAX

/** How a document was typed */
typedef enum {
    OYLNG_TYPED_NOT = 0,   /**< not at all: its root is OYLNG_T_ANY */
    OYLNG_TYPED_STAMP,     /**< by its stamp and the stream's own table */
    OYLNG_TYPED_APP,       /**< by the application's schema */
    OYLNG_TYPED_PUBLISHED, /**< by its stamp, a published name the
                                application's lookup found */
} oylng_typed_by;

/** The next document, typed: by its stamp, else by @p app (which may be
 *  NULL), else not at all. Called where oylng_parse_tree can be, and like
 *  it gives *out NULL at the end of the stream. A schema the stream holds,
 *  or a published one, is checked before the document's data is read.
 *  Errors are final, as the parser's are; OYLNG_ERR_INPUT for an app
 *  schema in error. A published file that can't be had, or doesn't hash to
 *  its name, refuses the document, its path /; one that is bad refuses the
 *  schema, the error naming the file's address. Both are OYLNG_ERR_SCHEMA,
 *  at the stamp. */
OYLNG_API oylng_status oylng_parse_typed(oylng_parser *p, const oylng_schema *app, oylng_typed **out);

/** Lets the document go, and with it each object of a binding's that
 *  wasn't taken (oylng_value_take), through the binding's drop. */
OYLNG_API void oylng_typed_free(oylng_typed *t);

/** The document as a tree, valid while the typed document is: its name,
 *  and the nodes the values were read from */
OYLNG_API const oylng_tree *oylng_typed_tree(const oylng_typed *t);
OYLNG_API oylng_typed_by    oylng_typed_how(const oylng_typed *t);
OYLNG_API oylng_value       oylng_typed_root(const oylng_typed *t);

OYLNG_API oylng_type oylng_value_type(const oylng_typed *t, oylng_value v);

/** The tree's node the value was read from: for OYLNG_T_ANY, the node to
 *  read; for others, its mark and its text */
OYLNG_API oylng_node oylng_value_node(const oylng_typed *t, oylng_value v);

/** A scalar's text, as the tree has it, but for a number a pattern matched
 *  (OYLNG_T_BIGINT, OYLNG_T_DECIMAL), in the standard spelling; data NULL
 *  for a collection */
OYLNG_API oylng_str oylng_value_text(const oylng_typed *t, oylng_value v);

/** The named type the value took, the nearest entry of the schema a type
 *  went through to reach it; data NULL if none */
OYLNG_API oylng_str oylng_value_name(const oylng_typed *t, oylng_value v);

/** The bound name whose code made the value's object (section 12): of the
 *  entries the value was typed through, the outermost that a binding
 *  names. With `spot: !point`, a `!spot` value goes through spot, then
 *  point: it is spot's if spot is bound, and point's if only point is.
 *  Data NULL if none of them is bound, or if nothing was bound when the
 *  document was typed. */
OYLNG_API oylng_str oylng_value_bound(const oylng_typed *t, oylng_value v);

/** OYLNG_T_INT: true if it fits in *out */
OYLNG_API bool oylng_value_int(const oylng_typed *t, oylng_value v, int64_t *out);
OYLNG_API bool oylng_value_uint(const oylng_typed *t, oylng_value v, uint64_t *out);

/** OYLNG_T_FLOAT; 0 for any other */
OYLNG_API double oylng_value_float(const oylng_typed *t, oylng_value v);

/** OYLNG_T_BOOL; false for any other */
OYLNG_API bool oylng_value_bool(const oylng_typed *t, oylng_value v);

/** OYLNG_T_BINARY: its bytes; data NULL for any other */
OYLNG_API oylng_str oylng_value_bytes(const oylng_typed *t, oylng_value v);

/** OYLNG_T_DATE or OYLNG_T_DATETIME: true, with its fields in *out */
OYLNG_API bool oylng_value_datetime(const oylng_typed *t, oylng_value v, oylng_datetime *out);

/** A sequence's items or a mapping's pairs, in the document's order; 0 for
 *  others */
OYLNG_API size_t      oylng_value_count(const oylng_typed *t, oylng_value v);
OYLNG_API oylng_value oylng_value_item(const oylng_typed *t, oylng_value seq, size_t i);
/** A mapping's key @p i, a string, and its value */
OYLNG_API oylng_str   oylng_value_key(const oylng_typed *t, oylng_value map, size_t i);
OYLNG_API oylng_value oylng_value_at(const oylng_typed *t, oylng_value map, size_t i);
/** The value of @p key in a mapping, or OYLNG_NO_VALUE */
OYLNG_API oylng_value oylng_value_get(const oylng_typed *t, oylng_value map, oylng_str key);

/* ── Application types (section 12) ─────────────────────── */

/** An application's code for a named type: it takes value @p v of @p t
 *  and sets *object to a value of the application's own (NULL is one too),
 *  returning NULL; or it returns why the document is refused, which is
 *  copied, having let go what it made or took. It may read @p t, and take
 *  the objects of the values below @p v (oylng_value_take). */
typedef const char *(*oylng_bind_fn)(void *ctx, oylng_typed *t, oylng_value v, void **object);

/** Lets go an object that oylng_typed_free finds untaken */
typedef void (*oylng_drop_fn)(void *ctx, void *object);

/** Binds @p fn to the name @p name: each value whose bound name it is
 *  (oylng_value_bound), the outermost bound one of the names it was typed
 *  through, gets an object @p fn makes, and the code of no other name it
 *  was typed through runs for it, from the next document
 *  typed on, whichever schema types it: the stream's own, the
 *  application's or a published one. A name nobody bound makes nothing, so
 *  the bindings are all that a stream can get made (section 12). @p fn runs
 *  once the document is typed, never while it is, so never on trial; once
 *  for each place a value is used, so `*p` twice is two calls and two
 *  objects; innermost first, so the code finds the objects of the values
 *  below its own made; and in the order written. It runs for no key, even
 *  one a type key names a bound entry for, and for no optional key written
 *  with no value, whose null has no name. Its refusal refuses the
 *  document, OYLNG_ERR_SCHEMA with the value's path and where it is
 *  written, and the objects made are let go. Binding a name again
 *  replaces its binding, and @p fn NULL unbinds it; from inside a binding,
 *  a change takes effect with the next document. @p drop may be NULL.
 *  OYLNG_ERR_MEMORY if memory runs out. */
OYLNG_API oylng_status oylng_parser_bind(oylng_parser *p, oylng_str name, oylng_bind_fn fn, oylng_drop_fn drop,
                                         void *ctx);

/** The object a binding made for value v, or NULL. Where a name is bound,
 *  each value on the way to a bound one is a value of its own at each
 *  place it is used; elsewhere, a value reached twice may be one value. */
OYLNG_API void *oylng_value_object(const oylng_typed *t, oylng_value v);

/** The same, and the document no longer holds it: oylng_typed_free won't
 *  let it go */
OYLNG_API void *oylng_value_take(oylng_typed *t, oylng_value v);

/* ── Emitting typed data (section 13) ───────────────────── */

/** Writes each document from the next on so that a parser given @p s as
 *  its application's schema reads back what the emitter was given: a
 *  string for oylng_emit_scalar, a value of a type for the calls below. A
 *  document is held until it ends, and the output stops short of it till
 *  then. It is typed with s, as the parser would type it, and each scalar
 *  is written in the first of its spellings that reads back as given. A
 *  string is plain, else quoted. A number is in its standard spelling,
 *  then with a +, then with zeros before its digits, and for a float after
 *  its point too, as a pattern may ask (section 4.5), so 5 under `!signed`
 *  is +5. Null is null, ~, or empty. What reads back is a value of the
 *  type given, the same value, and for an object, of its named type; an
 *  integer may read as OYLNG_T_INT or OYLNG_T_BIGINT. Where s doesn't type
 *  a value (OYLNG_T_ANY), any value reads back. If one doesn't, or the
 *  document doesn't fit s, the document is refused, OYLNG_ERR_EMIT with
 *  the path to the value, and none of it is written. @p s must have a
 *  root, else OYLNG_ERR_INPUT, and outlive its use; NULL, the default, for
 *  none, when a value is written in its standard spelling and nothing is
 *  held. Set it between documents. */
OYLNG_API oylng_status oylng_emitter_set_schema(oylng_emitter *e, const oylng_schema *s);

/** Values of the standard types, where a node goes: a key is a string,
 *  so in a key's place they are OYLNG_ERR_EMIT. A float must be finite,
 *  as YAML 2.0 writes no other. */
OYLNG_API oylng_status oylng_emit_int(oylng_emitter *e, int64_t v);
OYLNG_API oylng_status oylng_emit_uint(oylng_emitter *e, uint64_t v);
OYLNG_API oylng_status oylng_emit_float(oylng_emitter *e, double v);
OYLNG_API oylng_status oylng_emit_bool(oylng_emitter *e, bool v);
OYLNG_API oylng_status oylng_emit_null(oylng_emitter *e);
/** OYLNG_T_BIGINT or OYLNG_T_DECIMAL, its text in the standard spelling,
 *  as oylng_value_text gives it: digits with no + and no leading zeros,
 *  and for a decimal, a fraction and an exponent if it has them. */
OYLNG_API oylng_status oylng_emit_number(oylng_emitter *e, oylng_type type, oylng_str text);
/** OYLNG_T_DATE, of which the date is written, or OYLNG_T_DATETIME, which
 *  must be one RFC 3339 writes: one this library reads back the same */
OYLNG_API oylng_status oylng_emit_datetime(oylng_emitter *e, oylng_type type, const oylng_datetime *dt);
/** OYLNG_T_BINARY, written in base64 */
OYLNG_API oylng_status oylng_emit_bytes(oylng_emitter *e, oylng_str bytes);
/** Value @p v of @p t, as typed, and everything in it: a typed document
 *  written again. Its scalars untyped (OYLNG_T_ANY) are strings. */
OYLNG_API oylng_status oylng_emit_value(oylng_emitter *e, const oylng_typed *t, oylng_value v);

/** An application's way back for a named type (section 13): it writes
 *  @p object with @p e, as the one value of the type it is, a mapping,
 *  say, and returns NULL; or it returns why it can't, which is copied,
 *  and fails the emitter. */
typedef const char *(*oylng_unbind_fn)(void *ctx, oylng_emitter *e, const void *object);

/** Binds @p fn to the name @p name, for oylng_emit_object, which gives it
 *  @p ctx. Binding a name again replaces its binding, and @p fn NULL
 *  unbinds it. OYLNG_ERR_MEMORY if memory runs out, and the emitter is as
 *  it was. */
OYLNG_API oylng_status oylng_emitter_bind(oylng_emitter *e, oylng_str name, oylng_unbind_fn fn, void *ctx);

/** Writes @p object with the code bound to @p name, which must write one
 *  value, where a node goes. With a schema, it must read back with that
 *  bound name (oylng_value_bound), the names bound to this emitter taken
 *  as bound, so that a parser with the same bindings gets it from that
 *  name's code, or the document is refused. OYLNG_ERR_INPUT if nothing is bound to
 *  the name, and the emitter is as it was. */
OYLNG_API oylng_status oylng_emit_object(oylng_emitter *e, oylng_str name, const void *object);

#ifdef __cplusplus
}
#endif

#endif /* OYLNG_SCHEMA_H */
