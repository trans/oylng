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
  full sizes, or to held alone with references kept (below). A larger
  document needs the limit raised;
- goes to 256 levels, with references followed, or up to 4096 if set;
- stops at `...` and says where the rest of the input starts;
- reports errors with a line, a column, a byte offset and a message, as
  part 1, section 2 says: the column counts characters, so a tab counts 1
  and so does `名`. A document in error may already have delivered events,
  but the error comes before its end, so a caller drops what that document
  gave.

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

## Keeping references

A short chain of references can read as terabytes: `a1: [*a0, *a0]`,
`a2: [*a1, *a1]` and so on, and after 40 lines the data has more than a
trillion nodes. A parser delivering data refuses that, as a program walking
it would walk all of them. A tool that edits files, or a program that wants the data
as written, reads it with references kept instead (sections 10 and 11):

```c
oylng_parser *p = oylng_parser_new(text, len);
oylng_parser_set_keep(p, true);   /* before the first read */
oylng_tree *t;
while (oylng_parse_tree(p, &t) == OYLNG_OK && t) {
    /* ... */
    oylng_tree_free(t);
}
```

The limit then applies to held alone, the size of what is written, and
nothing is spread:

- a reference is the node it names, shared, as in any tree;
- a `-<<` is an item of its own, of kind `OYLNG_NODE_SPLICE`, and
  `oylng_node_target` gives the sequence it names;
- a mapping's `<<:` lines are `OYLNG_NODE_MERGE` nodes, apart from its
  pairs: `oylng_node_merges` and `oylng_node_merge` give them in order, and
  `oylng_node_target` the mapping each one names. The pairs are only those
  written, and `oylng_node_get` finds only those. A key that isn't written
  is in the merged mappings, the last first (section 9.1).

A path through merged mappings, such as `*c:p`, still works. The parser
gathers a merged mapping's keys the first time a path looks into it. While
full, as it stands when the path is read, is within the limit, that costs no
more than the normal read's copies of merged pairs, and nothing is counted.
Past it, each lookup counts the `<<:` lines a walk that saves nothing would
follow, however the parser finds the key, and a stream's lookups together
may follow as many as the limit (section 10). So a path to the far end of a
chain of n merged mappings counts n - 1 lines. Events would deliver every
reference in full, so with references kept `oylng_parse_next` is
`OYLNG_ERR_INPUT`, and so is `oylng_emit_tree` on a kept tree. What walks a
kept tree in full should check its full size first, as section 10 says.

`oylng_parse_typed` types a kept document too. The limit applies to held,
and to typed, which counts a shared node once for each type it is typed as
and each nearest `!**` (part 2, section 11). So `[*s, *s, ...]` under
`[!str, ...]` counts `s` once, while three types for it count it three
times. Each place still gets its own value where bound code makes one
(section 12).

Typing spreads a kept mapping's `<<:` lines and a kept sequence's `-<<`
items, and the typed value is what a full read gives (part 1, section 9).
Typed counts what spreading builds, before it builds it: each line, and all
that its target gives, with nothing taken off for a key given twice. So a
chain of n mappings, each merging the one before it twice, counts some
2^(n+1) lines, though it gives no key at all. The spread is a new node of
the document's tree, made once for each node and shared, and spreading a
mapping visits each mapping its lines reach once, however many lines name
it. Under `!any`
alone, where nothing is typed, nothing is spread, and the value is
delivered as kept, as in an untyped document; a union with `!any` spreads.
A type of the stream's own schema written with `<<:` or `-<<` is compiled
from its merged form, and the entries a document's type reaches are first
held to the limit together, for their full size.

An application that keeps every document it reads says so with
`oylng_parser_set_together`. The documents' sizes then add up, rather than
each document starting again, and with references kept, `...` takes nothing
off held either, as the documents still name the old table.

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

It has the thirteen standard types with their limits, a decimal's
precision and the words of `!true`, `!false` and `!null`
(`{!|: [!true(yes|on), !false(no|off)]}`, `!null(~|null|)`), literals,
optional keys, type keys, `!*`, unions (a mapping
whose one key is `!|`: `{!|: [!int, !str]}`), sequences,
named and recursive types, references and merges in a schema, section 11's
typed count, and an application's cut of a time's fraction. `!int` is
-2^63 to 2^64-1, `!float` the nearest binary64, and `!bigint` and
`!decimal` are delivered as written; a time holds nanoseconds, a leap
second and the year 0000. Patterns (`!str(/[a-z][a-z0-9-]*/)` and the like)
are I-Regexp, matched without backtracking, so in time linear in the
text, with Unicode 16's categories for `\p{..}`. `!**` types what a
mapping holds at every level below it, by its rules or by its `!*`, and
walks the rest. How a scalar is written doesn't count for its type: `3`,
`"3"` and `'3'` are all 3 under `!int`. `!null` takes `null` and the empty
text, so `retries:` is null under `retries?: {!|: [!null, !int]}` and
refused under `retries?: !int`, where `?` only lets the key be left out. A
union reads any JSON value, though a string that reads as a number, a
boolean or null is read as one:
`json: {!|: [!null(null), !bool, !int, !float, !str, {!*: !json}, [!json, ...]]}`.
`include/oylng/schema.h` documents the rest.

A published name (section 3.1), such as
`--- !tabcomputing.com/address/v1/address.yes#sha256=8789cc…`, ends in the
SHA-256 of a schema file anyone can use. The parser looks one up only
through a lookup the application gives it, which gets the file's bytes
from wherever the application keeps them, by the address or by the hash.
The parser checks that the file hashes to the name, and that it is one
table with an entry named like the file. It reads the file under the
stream's size limit and compiles it once per stream. Without a lookup, a
published name stamps nothing, as any other global name.

```c
static bool lookup(void *ctx, oylng_str address, oylng_str sha256, oylng_str *file) {
    /* from a folder, a cache or https:// and the address: false if it can't be had */
}
oylng_parser_set_lookup(p, lookup, ctx);
```

An application may also ignore a file's schema (section 3). With
`oylng_parser_set_stamps(p, false)`, a stamp types nothing, and nothing is
looked up or compiled for it: each document is typed as one with no stamp
is, by the application's schema, or without one, read as it is. So an
application checking files it didn't write can hold every file to its own
schema, whatever schema the file gives itself. And `oylng_parser_table`
hands the application the stream's table, the schema a stamp names, as a
tree: YAML like any other, which it can look at, keep or write out. A
tree of the table shares its nodes as a document's tree does, so it is as
large as the table is written.

An application binds its own code to a named type (section 12), and that
code turns each value of the type into one of the application's objects,
or refuses the document. Only a bound name makes anything, so a file can
never choose what code runs. The code runs once the document is typed, so
never on trial, innermost first, and once for each place a value is used:
`*p` named twice makes two objects. A value typed through several names,
as a `!spot` is through `spot` and then `point` with `spot: !point`, gets
the code of the outermost one bound, and no other's; a union's name counts
the same way (`oylng_value_bound` says which). Code runs only for a typed
value: none for a key, even one a type key names a bound entry for. The
document holds the
objects until the application takes them, and lets go the rest when it is
freed.

```c
static const char *make_point(void *ctx, oylng_typed *t, oylng_value v, void **obj) {
    struct point *pt = malloc(sizeof *pt);
    if (!pt || !oylng_value_int(t, oylng_value_get(t, v, (oylng_str){ "x", 1 }), &pt->x) ||
        !oylng_value_int(t, oylng_value_get(t, v, (oylng_str){ "y", 1 }), &pt->y)) {
        free(pt);
        return "a point needs x and y";   /* refuses the document, at the value */
    }
    *obj = pt;
    return NULL;
}
static void drop(void *ctx, void *obj) { free(obj); }

oylng_parser_bind(p, (oylng_str){ "point", 5 }, make_point, drop, ctx);
/* then for each !point value: oylng_value_object(t, v), or oylng_value_take(t, v) */
```

`oylng --typed` types a stream and writes each document as JSON, each
value with its type:

```sh
build/oylng --typed file.yaml                      # by its stamps
build/oylng --typed --schema app.yaml svc file.yaml
build/oylng --typed --store schemas/ file.yaml     # published names: schemas/SHA256
build/oylng --typed --no-stamps --schema app.yaml svc file.yaml   # app.yaml's svc, whatever the stamps
build/oylng --typed --every-entry app.yaml         # a schema file: every entry checked
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

With a schema (part 2, section 13), the emitter writes typed data so that
the same schema reads it back unchanged. `oylng_emitter_set_schema` holds
each document until it ends, types it as a parser would, and writes each
scalar so that it reads back as what it was given. Quoting doesn't change
a type, so a string its type would read as something else, or that would
refuse the document, is refused: the string `3` under
`{!|: [!int, !str]}`. Other strings are plain where that reads back the
same, else quoted. A number, a boolean, null or a time has one spelling,
its standard one: a float in the fewest digits that read back, with `.0`
where it has no fraction (`2.0`) and a signed exponent past 1e21
(`1e+21`); null as `null`. Under words, a boolean or null is the first
word of the first alternative that takes it: true as `yes` under
`{!|: [!true(yes|on), !false(no|off)]}`, and `""` where that word is
empty. A value written
again with `oylng_emit_value` may also take the text it was read from.
Nothing else is tried: 5 under `!signed` below is refused, as `5` doesn't
match its pattern, and an application that wants `+5` writes it with its
own code (`oylng_emit_object`). A document with a value it can't write so
is refused, with the path to the value, and none of it is written.

```c
const char *app = "%YAML 2.0\n--- &\nsigned: !int(/[-+][0-9]+/)\nrow:\n  id: {!|: [!int, !str]}\n  at: !signed\n";
oylng_schema *sc = oylng_schema_new(app, strlen(app), (oylng_str){ "row", 3 }, 0);
oylng_emitter_set_schema(em, sc);
oylng_emit_doc_start(em, (oylng_str){ NULL, 0 });
oylng_emit_map_start(em, false);
oylng_emit_scalar(em, (oylng_str){ "id", 2 });
oylng_emit_scalar(em, (oylng_str){ "a7", 2 });      /* id: a7; the string 3 would refuse it, as it reads as 3 */
oylng_emit_scalar(em, (oylng_str){ "at", 2 });
oylng_emit_int(em, -5);                             /* at: -5; a 5 would refuse it */
oylng_emit_map_end(em);
oylng_emit_doc_end(em);                             /* written now, or refused */
```

`oylng_emit_value` writes a typed document again. `oylng_emitter_bind`
and `oylng_emit_object` give a bound type its way back: the application's
code writes its object as the value its entry takes, and the emitter
checks that it reads back with that bound name, the names bound to the
emitter taken as bound, so that a parser with the same bindings gets it
from that name's code. Under `!any`, where nothing is typed, an object is
written through its code and reads back as the data that wrote.

The `oylng` tool prints a stream's events, the stream as JSON, or the
stream written again, with its references and merges resolved:

```sh
build/oylng --events file.yaml
build/oylng --json --limit 1000000 file.yaml
build/oylng --emit file.yaml
build/oylng --emit --flow file.yaml      # one line per document
```

## Building and testing

`make` builds `build/liboylng.a` and `build/oylng`. `src/oylng_ucd.c`, the
Unicode categories part 2's patterns use, is written by
`python3 -I tools/gen_ucd.py > src/oylng_ucd.c`, from Python's Unicode 16
or later.

- `make test` runs the API tests, the emitter's tests, part 2's tests and
  the YAML 2.0 test suite. Part 2's run the draft's examples, read from
  `../yaml2/spec/yaml-easy-schema.md` (`SCHEMA_SPEC=`). The emitter's write every string of up to three characters
  from the ones that matter to YAML, and random longer ones, in each place
  a scalar can go, and read them back; and each suite test that passes is
  written again four ways and must read back the same. Each suite test is
  also read as trees, which must give the same events, offsets and errors,
  and be written as the events are. The suite is
  read from `../yaml2/suite` (set `SUITE=` for another place) and converted
  with the suite's `bin/suite-to-data`, which needs PyYAML. Part 2's suite,
  `suite/yes` (`make test-yes`), types each test and compares its typed
  events, or its refusal and the refusal's kind, with a lookup of the
  test's store of published files where it has one. A test with `keep` is
  read with references kept, and only its acceptance or refusal is
  compared, as the suite says. A test with `bind` binds each name in it
  to code that notes the name, and an accepted one's names noted must be
  its `calls`, in order. Each accepted test's documents are also written
  again from their typed events alone, as its `emit` says: after the
  stream's table documents, read back the same, or refused where it says
  `emit: refused`. The names in `bind` are bound to the emitter too, each
  one's code writing what the test's `writes` says, else the value it was
  given, and read back with them bound, a document gives the same calls.
  It skips times held otherwise than the engine holds them. Then
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
  both accept it with the same values.
- `make test-pattern-diff` generates patterns (`PATTERN_N=`, 20,000 by
  default), with strings that match them and strings that nearly do, and
  some patterns broken on purpose, and gives each to this parser and to
  the checker's reading of it, run by Python's `re`: both must refuse the
  pattern, or count it the same and match each string the same.
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
- `make bench-typed` times typed reading (part 2) on the same inputs, each
  with the schema an application would give for it, against events and
  trees of the same text, in turn: typed with no schema, by the schema,
  and with code bound to the item type. Then it gives each way's peak heap.
  `BENCH_CPU=` pins the timing.
- `make bench-files FILES='DIR ...'` times the parser on real files: each
  directory is a group, and every `.yaml` and `.yml` file under it is read
  as 2.0, with `%YAML 2.0` in front (and `---` where the file doesn't start
  a document). If pkg-config finds libyaml, libyaml reads the files as
  written, for comparison. It also lists the parser's reasons for the files
  it refuses: those need converting to 2.0 first.

## License

MIT. See [LICENSE](LICENSE).
