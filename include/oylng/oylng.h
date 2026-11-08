/**
 * @file oylng.h
 * @brief Oyl NG: a YAML 2.0 parser in C11
 *
 * Reads YAML 2.0 as proposed at https://github.com/trans/yaml2,
 * spec/yaml-2.0.md (Draft 1). For YAML 1.x, use Oyl.
 *
 * A stream delivers documents in order. Each is a string, a sequence or a
 * mapping, and may have a name. References and merges are already
 * resolved in what the parser delivers: the table, `<<`, `-<<` and comments
 * are not data (section 11).
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

/** A UTF-8 string, not NUL-terminated. It points into the input or into
 *  memory the parser owns, and stays valid until the parser is freed. */
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
 *  valid until the next call. A document is checked whole before its
 *  first event, so a document in error is never delivered in part. After
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

/** A name for an event type, such as "SCALAR". */
OYLNG_API const char *oylng_event_type_str(oylng_event_type type);

#ifdef __cplusplus
}
#endif

#endif /* OYLNG_H */
