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
 * OYLNG_T_STR; what is walked has no name (oylng_value_name). Not yet: a
 * published name is never looked up, as section 3.1 has it unless the
 * application asks, so it stamps nothing.
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
} oylng_typed_by;

/** The next document, typed: by its stamp, else by @p app (which may be
 *  NULL), else not at all. Called where oylng_parse_tree can be, and like
 *  it gives *out NULL at the end of the stream. A schema the stream holds
 *  is checked before the document's data is read. Errors are final, as the
 *  parser's are; OYLNG_ERR_INPUT for an app schema in error. */
OYLNG_API oylng_status oylng_parse_typed(oylng_parser *p, const oylng_schema *app, oylng_typed **out);

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
 *  went through to reach it, for an application's bindings (section 12);
 *  data NULL if none */
OYLNG_API oylng_str oylng_value_name(const oylng_typed *t, oylng_value v);

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

#ifdef __cplusplus
}
#endif

#endif /* OYLNG_SCHEMA_H */
