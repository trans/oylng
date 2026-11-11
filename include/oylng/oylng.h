/**
 * @file oylng.h
 * @brief Oyl NG: a YAML 2.0 parser, tree and emitter in C11
 *
 * Reads YAML 2.0 as proposed at https://github.com/trans/yaml2,
 * spec/yaml-2.0.md (Draft 1). For YAML 1.x, use Oyl.
 *
 * A stream delivers documents in order. Each is a string, a sequence or a
 * mapping, and may have a name. References and merges are already
 * resolved in what the parser delivers: the table, `<<`, `-<<` and comments
 * are not data (section 11). A document comes as events, or whole as a
 * tree, and either can be written again by the emitter.
 */

#ifndef OYLNG_H
#define OYLNG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define OYLNG_API __attribute__((visibility("default")))
#else
#  define OYLNG_API
#endif

/* ── ABI ─────────────────────────────────────────────────────
 * oylng_event is only ever allocated by the library and handed out by
 * const pointer, so fields can be appended later without breaking
 * compiled callers. oylng_str and oylng_mark are plain values and will
 * not change. The parser is opaque, configured through functions. */

#define OYLNG_VERSION_MAJOR 0
#define OYLNG_VERSION_MINOR 1
#define OYLNG_VERSION_PATCH 0

/** A UTF-8 string, not NUL-terminated. It points into the input, and
 *  lasts as long as the input does, or, for a value the parser had to
 *  build from escapes or lines, into memory the parser owns, valid until
 *  the next oylng_parse_next. */
typedef struct {
    const char *data;
    size_t      len;
} oylng_str;

typedef enum {
    OYLNG_OK = 0,
    OYLNG_ERR_MEMORY,   /**< An allocation failed. */
    OYLNG_ERR_INPUT,    /**< A bad argument, such as a NULL pointer. */
    OYLNG_ERR_PARSE,    /**< The stream breaks a rule of the language. */
    OYLNG_ERR_LIMIT,    /**< The stream passes the size limit or the levels
                             this parser goes to (section 10). */
    OYLNG_ERR_EMIT,     /**< The emitter was given what YAML 2.0 can't hold,
                             or events out of order. */
} oylng_status;

/** A place in the input. Lines and columns count from 1, and a column
 *  counts bytes. */
typedef struct {
    size_t offset;
    size_t line;
    size_t col;
} oylng_mark;

typedef enum {
    OYLNG_EVT_NONE = 0,
    OYLNG_EVT_STREAM_START,
    OYLNG_EVT_STREAM_END,
    OYLNG_EVT_DOC_START,    /**< value: the document's name, or data NULL */
    OYLNG_EVT_DOC_END,
    OYLNG_EVT_MAP_START,
    OYLNG_EVT_MAP_END,
    OYLNG_EVT_SEQ_START,
    OYLNG_EVT_SEQ_END,
    OYLNG_EVT_SCALAR,       /**< value: the string */
} oylng_event_type;

/** An event. A mapping's events alternate key and value. */
typedef struct {
    oylng_event_type type;
    oylng_str        value;
    size_t           offset;  /**< Where the node is written in the input.
                                   A node reached through a reference is
                                   where the table holds it. */
} oylng_event;

/* ── Parser ──────────────────────────────────────────────── */

typedef struct oylng_parser oylng_parser;

/** The default size limit, 64 MiB (section 10). */
#define OYLNG_DEFAULT_LIMIT ((uint64_t)64 << 20)

/** The default number of levels, the least every parser must accept. */
#define OYLNG_DEFAULT_LEVELS 256

/** The most levels a parser can be set to go to. */
#define OYLNG_MAX_LEVELS 4096

/** A parser for @p len bytes at @p input, which must stay unchanged and
 *  valid until the parser is freed. NULL if memory runs out. */
OYLNG_API oylng_parser *oylng_parser_new(const char *input, size_t len);

/** The size limit, applied to both held and full (section 10). Set it
 *  before the first oylng_parse_next. UINT64_MAX means no limit. */
OYLNG_API void oylng_parser_set_limit(oylng_parser *p, uint64_t limit);

/** How many levels to go to, with references followed: at least 256, at
 *  most OYLNG_MAX_LEVELS. Past it the stream is refused. */
OYLNG_API void oylng_parser_set_levels(oylng_parser *p, unsigned levels);

/** The next event. On OYLNG_OK, *evt points to an event the parser owns,
 *  valid until the next call. A document is delivered as it is read, so
 *  one in error may already have delivered events, but the error comes
 *  before its DOC_END (section 2): drop what it delivered. After
 *  STREAM_END, further calls give STREAM_END again. */
OYLNG_API oylng_status oylng_parse_next(oylng_parser *p, const oylng_event **evt);

/** What went wrong, after a call returned an error. */
OYLNG_API const char *oylng_parser_error(const oylng_parser *p);

/** Where it went wrong. */
OYLNG_API oylng_mark  oylng_parser_error_mark(const oylng_parser *p);

/** After STREAM_END: true if a `...` stopped the parser before the end of
 *  the input (section 4.4). *offset is then where the rest starts, the line
 *  after the `...`. The rest wasn't read or checked. */
OYLNG_API bool oylng_parser_stopped(const oylng_parser *p, size_t *offset);

OYLNG_API void oylng_parser_free(oylng_parser *p);

/* ── Emitter ─────────────────────────────────────────────── */

/* The emitter writes YAML 2.0 from events, in the order the parser gives
 * them: STREAM_START (DOC_START node DOC_END)* STREAM_END, where a node is
 * a SCALAR or a collection, and a mapping's nodes alternate key and value.
 * It writes `%YAML 2.0` first, `---` (or `--- !name`) for each document,
 * keys in the order given, and every node in full (section 11). Reading
 * the output gives the same events back.
 *
 * A scalar is written plain where that reads back as the same string,
 * else in single quotes, else in double quotes with JSON's escapes. Text
 * with line breaks is written as a literal block scalar (| or |-) where
 * that reads back exactly, else in double quotes. Collections are block
 * collections, indented, unless flow is asked for; an empty one is {} or [].
 *
 * What YAML 2.0 can't hold is OYLNG_ERR_EMIT: invalid UTF-8, a key given
 * twice in a mapping, a collection as a key, a name with characters a name
 * can't have, events out of order, or more than OYLNG_MAX_LEVELS levels.
 * An error is final: every later call gives it again, and the output may
 * stop partway. oylng_emitter_error() says what was wrong. */

typedef struct oylng_emitter oylng_emitter;

/** A new emitter, writing into a buffer of its own. NULL if memory runs
 *  out. */
OYLNG_API oylng_emitter *oylng_emitter_new(void);

/** Spaces per level of a block collection, 1 to 8 (2 by default). */
OYLNG_API void oylng_emitter_set_indent(oylng_emitter *e, unsigned indent);

/** Write every collection in flow style, [a, b] and {k: v}, so that each
 *  document is one line. */
OYLNG_API void oylng_emitter_set_flow(oylng_emitter *e, bool flow);

/** Write an event, such as one from oylng_parse_next(). A collection's
 *  start is written in block style unless the emitter is set to flow. */
OYLNG_API oylng_status oylng_emit(oylng_emitter *e, const oylng_event *evt);

OYLNG_API oylng_status oylng_emit_stream_start(oylng_emitter *e);
OYLNG_API oylng_status oylng_emit_stream_end(oylng_emitter *e);

/** Start a document, named @p name, or with no name if name.data is
 *  NULL. */
OYLNG_API oylng_status oylng_emit_doc_start(oylng_emitter *e, oylng_str name);
OYLNG_API oylng_status oylng_emit_doc_end(oylng_emitter *e);

/** A string. It is copied or written at once, so it needn't outlive the
 *  call. */
OYLNG_API oylng_status oylng_emit_scalar(oylng_emitter *e, oylng_str value);

/** Start a mapping or a sequence; @p flow writes this one in flow style,
 *  and everything in it with it. */
OYLNG_API oylng_status oylng_emit_map_start(oylng_emitter *e, bool flow);
OYLNG_API oylng_status oylng_emit_map_end(oylng_emitter *e);
OYLNG_API oylng_status oylng_emit_seq_start(oylng_emitter *e, bool flow);
OYLNG_API oylng_status oylng_emit_seq_end(oylng_emitter *e);

/** The output written since the last oylng_emitter_take(), valid until the
 *  next call on the emitter. */
OYLNG_API oylng_str oylng_emitter_output(const oylng_emitter *e);

/** Take the output written since the last take, at any point, so that a
 *  large stream needn't be held whole: @p *out is valid until the next call
 *  on the emitter, which then starts its buffer again. The pieces joined
 *  are the whole output. A failed emitter gives its error, and @p *out
 *  empty. */
OYLNG_API oylng_status oylng_emitter_take(oylng_emitter *e, oylng_str *out);

/** What went wrong, after a call gave an error; NULL before. */
OYLNG_API const char *oylng_emitter_error(const oylng_emitter *e);

OYLNG_API void oylng_emitter_free(oylng_emitter *e);

/** A name for an event type, such as "SCALAR". */
OYLNG_API const char *oylng_event_type_str(oylng_event_type type);

/* ── Tree ────────────────────────────────────────────────────
 * A document read whole into nodes: strings, sequences and mappings, with
 * the table applied as in the events. A node of the table is made once and
 * shared by every place that names it, so `*a` written twice is the same
 * node twice, and a tree takes room in proportion to what is written, not
 * to what it reads as. A program that keeps what it made of each node, by
 * number, makes a shared node once too.
 *
 * A tree needs its input to outlive it, not its parser: a string points
 * into the input where it can, else into memory the tree owns. */

typedef struct oylng_tree oylng_tree;

/** A node, numbered from 0 to oylng_tree_nodes() - 1. */
typedef uint32_t oylng_node;

/** No node: what a lookup gives when there is none. */
#define OYLNG_NO_NODE UINT32_MAX

typedef enum {
    OYLNG_NODE_NONE = 0,   /**< not a node of the tree */
    OYLNG_NODE_STRING,
    OYLNG_NODE_SEQUENCE,
    OYLNG_NODE_MAPPING,
} oylng_kind;

/** The next document as a tree. On OYLNG_OK, *tree is the document, to be
 *  freed with oylng_tree_free, or NULL at the end of the stream.
 *
 *  Call it before any document or after one has ended, where the next event
 *  is STREAM_START, DOC_START or STREAM_END, so that documents can be read
 *  as trees and as events in turn. Inside a document it is OYLNG_ERR_INPUT,
 *  and changes nothing. Any other error is the parser's, as from
 *  oylng_parse_next, and final: *tree is NULL, and oylng_parser_error says
 *  what was wrong. An input of 4 GiB or more is OYLNG_ERR_LIMIT. */
OYLNG_API oylng_status oylng_parse_tree(oylng_parser *p, oylng_tree **tree);

OYLNG_API void oylng_tree_free(oylng_tree *t);

/** The document's node. */
OYLNG_API oylng_node oylng_tree_root(const oylng_tree *t);

/** The document's name, or data NULL if it has none. */
OYLNG_API oylng_str oylng_tree_name(const oylng_tree *t);

/** How many nodes the tree has, a shared node counted once. */
OYLNG_API size_t oylng_tree_nodes(const oylng_tree *t);

/** What node @p n is: OYLNG_NODE_NONE for OYLNG_NO_NODE, or any number
 *  that isn't a node of the tree. */
OYLNG_API oylng_kind oylng_node_kind(const oylng_tree *t, oylng_node n);

/** A string's text, valid while the tree is; data NULL for a collection. */
OYLNG_API oylng_str oylng_node_text(const oylng_tree *t, oylng_node n);

/** A sequence's items or a mapping's pairs; 0 for a string. */
OYLNG_API size_t oylng_node_count(const oylng_tree *t, oylng_node n);

/** A sequence's item @p i, from 0, or OYLNG_NO_NODE. */
OYLNG_API oylng_node oylng_node_item(const oylng_tree *t, oylng_node seq, size_t i);

/** A mapping's pair @p i, from 0, in the order the events give: its key,
 *  a string, and its value. OYLNG_NO_NODE past the end. */
OYLNG_API oylng_node oylng_node_key(const oylng_tree *t, oylng_node map, size_t i);
OYLNG_API oylng_node oylng_node_value(const oylng_tree *t, oylng_node map, size_t i);

/** The value of @p key in a mapping, or OYLNG_NO_NODE. A mapping of more
 *  than 8 pairs has an index, so this takes about the same time however
 *  many it has. */
OYLNG_API oylng_node oylng_node_get(const oylng_tree *t, oylng_node map, oylng_str key);

/** Where node @p n is written in the input, as an event's offset says: a
 *  node of the table where the table holds it. Lines and columns are
 *  counted from the input's start, which takes time in proportion to the
 *  offset, so this is for messages, not for every node. */
OYLNG_API oylng_mark oylng_node_mark(const oylng_tree *t, oylng_node n);

/** Write the tree as one document, between the emitter's documents: its
 *  name, then each node in full, a shared one wherever it appears. */
OYLNG_API oylng_status oylng_emit_tree(oylng_emitter *e, const oylng_tree *t);

#ifdef __cplusplus
}
#endif

#endif /* OYLNG_H */
