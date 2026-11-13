# Oyl NG

Oyl NG reads and writes YAML 2.0, the version proposed in
[trans/yaml2](https://github.com/trans/yaml2) (`spec/yaml-2.0.md`, Draft 1).
For YAML 1.x, use [Oyl](https://github.com/openbohemians/oyl).

It is a C11 library built from Oyl's work. It has the event parser, the
tree and the emitter for part 1 of the draft, and the start of part 2, the
Easy Schema (`spec/yaml-easy-schema.md`), which types a document by a
schema. Part 2 is a work in progress, as the draft is.

## What it reads

The parser delivers each document's events with the table already applied:
references, merges (`<<:`) and splices (`-<<`) are resolved, and the table
itself, the directives and the comments are not delivered. Every value is a
string. The parser:

- delivers a document as it reads it. What it holds is the table, the keys
  of each open mapping, and a mapping with `<<:` lines until it closes,
  since a key written after them can still replace a merged one;
- refuses repeated keys, in time linear in the input;
- applies the size limit of section 10, 64 MiB by default, to both held and
  full sizes. A larger document needs the limit raised;
- goes to 256 levels, with references followed, or up to 4096 if set;
- stops at `...` and says where the rest of the input starts;
- reports errors with a line, a column and a message. A document in error
  may already have delivered events, but the error comes before its end,
  so a caller drops what that document gave.

## Using it

```c
#include <oylng/oylng.h>

oylng_parser *p = oylng_parser_new(text, len);   /* text must outlive p */
const oylng_event *e;
while (oylng_parse_next(p, &e) == OYLNG_OK && e->type != OYLNG_EVT_STREAM_END)
    if (e->type == OYLNG_EVT_SCALAR)
        printf("%.*s\n", (int)e->value.len, e->value.data);
if (oylng_parser_error(p)) {
    oylng_mark m = oylng_parser_error_mark(p);
    fprintf(stderr, "%zu:%zu: %s\n", m.line, m.col, oylng_parser_error(p));
}
oylng_parser_free(p);
```

Scalars point into the input where they can, so the input must stay valid
until the parser is freed. A value built from escapes or lines is valid
until the next `oylng_parse_next`, so copy it to keep it.
`include/oylng/oylng.h` documents the rest.

## Reading a document as a tree

Events come while the parser reads. `oylng_parse_tree` instead reads the
next document of the stream to its end and gives it as a tree, whose nodes
can be visited in any order. Each call gives one document, and NULL after
the last:

```c
oylng_parser *p = oylng_parser_new(text, len);
oylng_tree *t;
while (oylng_parse_tree(p, &t) == OYLNG_OK && t) {   /* NULL: the end */
    oylng_node port = oylng_node_get(t, oylng_tree_root(t), (oylng_str){ "port", 4 });
    if (oylng_node_kind(t, port) == OYLNG_NODE_STRING) {
        oylng_str s = oylng_node_text(t, port);
        printf("%.*s\n", (int)s.len, s.data);
    }
    oylng_tree_free(t);
}
/* then oylng_parser_error(p), as above */
oylng_parser_free(p);
```

A tree holds what the events would: strings, sequences and mappings, with
the table applied. A node of the table is made once and shared by every
place that names it, so `*a` written twice is the same node twice, and a
tree takes room in proportion to what is written, not to what it reads as.
Nodes are numbered, so a program that turns a tree into its own objects can
keep what it made of each node and make a shared one once. A mapping of more
than 8 pairs has an index, so `oylng_node_get` takes about the same time
however many it has. A document in error gives no tree. A tree needs only
the input to outlive it, not the parser, and `oylng_emit_tree` writes it.
Documents can be read as trees and as events in turn.

## Typing a document (part 2)

`oylng_parse_typed` reads the next document as a tree and types it: by its
stamp, `--- !name` with a name the stream's own table has, or else by the
application's schema, an entry of a table it compiles once with
`oylng_schema_new`. A document neither types stays strings. A document that
doesn't fit its schema is refused, as a syntax error is: the stream ends
with `OYLNG_ERR_SCHEMA`, and the error gives the data path to the value and
where it is written.

```c
#include <oylng/schema.h>

const char *app = "%YAML 2.0\n--- &\nsvc:\n  port: !int(1..65535)\n  debug?: !bool\n";
oylng_schema *sc = oylng_schema_new(app, strlen(app), (oylng_str){ "svc", 3 }, 0);
/* oylng_schema_status(sc) is OYLNG_OK, or oylng_schema_error(sc) says why */
oylng_parser *p = oylng_parser_new(text, len);
oylng_typed *t;
while (oylng_parse_typed(p, sc, &t) == OYLNG_OK && t) {   /* NULL: the end */
    oylng_value port = oylng_value_get(t, oylng_typed_root(t), (oylng_str){ "port", 4 });
    int64_t n;
    if (oylng_value_int(t, port, &n)) printf("%lld\n", (long long)n);
    oylng_typed_free(t);
}
/* "/port: '99999' is out of range", with its line and column */
oylng_parser_free(p);
oylng_schema_free(sc);
```

It has the eleven standard types with their limits and a decimal's
precision, literals, optional keys, type keys, `!*`, unions, sequences,
named and recursive types, references and merges in a schema, section 11's
typed count, and an application's cut of a time's fraction. `!int` is
-2^63 to 2^64-1, `!float` the nearest binary64, and `!bigint` and
`!decimal` are delivered as written; a time holds nanoseconds, a leap
second and the year 0000. Not yet: patterns (`!str(/.../)` and the like)
and `!**`, for which a schema is refused, saying so, and published names,
which stamp nothing. `include/oylng/schema.h` documents the rest.

`oylng --typed` types a stream and writes each document as JSON, each
value with its type:

```sh
build/oylng --typed file.yaml                      # by its stamps
build/oylng --typed --schema app.yaml svc file.yaml
```

## Writing YAML 2.0

The emitter takes the same events, from the parser or made by hand, and
writes YAML 2.0 that reads back as those events:

```c
oylng_emitter *em = oylng_emitter_new();
oylng_emit_stream_start(em);
oylng_emit_doc_start(em, (oylng_str){ NULL, 0 });   /* or a name */
oylng_emit_map_start(em, false);                     /* true: {flow} */
oylng_emit_scalar(em, (oylng_str){ "port", 4 });
oylng_emit_scalar(em, (oylng_str){ "80", 2 });
oylng_emit_map_end(em);
oylng_emit_doc_end(em);
oylng_emit_stream_end(em);
oylng_str out = oylng_emitter_output(em);           /* %YAML 2.0, ---, port: 80 */
oylng_emitter_free(em);
```

It writes `%YAML 2.0` first and a `---` for each document, keys in the
order given, and every node in full, as section 11 asks. A string is plain
where that reads back as the same string, else quoted; text with line
breaks is a `|` block where that reads back exactly. Collections are block
style, or flow with `oylng_emitter_set_flow`. It refuses what YAML 2.0 can't
hold: a key given twice in a mapping, a collection as a key, invalid
UTF-8, a bad name, or events out of order. `oylng_emitter_take` hands over
the output so far, so a large stream needn't be held whole.

The `oylng` tool prints a stream's events, the stream as JSON, or the
stream written again, with its references and merges resolved:

```sh
build/oylng --events file.yaml
build/oylng --json --limit 1000000 file.yaml
build/oylng --emit file.yaml
build/oylng --emit --flow file.yaml      # one line per document
```

## Building and testing

`make` builds `build/liboylng.a` and `build/oylng`.

- `make test` runs the API tests, the emitter's tests, part 2's tests and
  the YAML 2.0 test suite. Part 2's run the draft's examples, read from
  `../yaml2/spec/yaml-easy-schema.md` (`SCHEMA_SPEC=`), skipping those with
  patterns or `!**`. The emitter's write every string of up to three characters
  from the ones that matter to YAML, and random longer ones, in each place
  a scalar can go, and read them back; and each suite test that passes is
  written again four ways and must read back the same. Each suite test is
  also read as trees, which must give the same events, offsets and errors,
  and be written as the events are. The suite is
  read from `../yaml2/suite` (set `SUITE=` for another place) and converted
  with the suite's `bin/suite-to-data`, which needs PyYAML. Then
  `make test-plain` runs them all again, built without SIMD and without the
  fast paths for flow collections on one line and simple block lines, so
  the code those stand in for stays tested.
- `make test-asan` runs them all again with AddressSanitizer and UBSan.
- `make test-diff` reads generated and mutated streams with this parser and
  with the draft's reference checker, and reports any stream they disagree
  on. Set `DRAFTCHECK=` to the checker and `DIFF_N=` for how many streams.
- `make test-schema-diff` runs part 2's checker, yaml2's `schemacheck.py`
  on its `ai` branch (`SCHEMACHECK=`), on its examples and edge cases, and
  gives each stream it types to `oylng --typed`: both must refuse it, or
  both accept it with the same values. It skips those with patterns or
  `!**`.
- `make fuzz` runs libFuzzer for five minutes (`FUZZ_TIME=` seconds), with
  one worker, seeded from the suite, and from the schema checker's streams
  if it is there. Each stream read without error is written again by the
  emitter and must read back the same, each stream read as trees must give
  the same events or the same error, and each stream typed must be refused
  where its events were, or for a schema, with every value what its type
  says. It needs clang.
- `make bench` times the parser on generated inputs, then the inputs read
  and written again by the emitter. If pkg-config finds libyaml, it times
  libyaml on the same inputs, too. Set `BENCH_CPU=` to pin it to one core.
- `make bench-files FILES='DIR ...'` times the parser on real files: each
  directory is a group, and every `.yaml` and `.yml` file under it is read
  as 2.0, with `%YAML 2.0` in front (and `---` where the file doesn't start
  a document). If pkg-config finds libyaml, libyaml reads the files as
  written, for comparison. It also lists the parser's reasons for the files
  it refuses: those need converting to 2.0 first.

## License

MIT. See [LICENSE](LICENSE).
