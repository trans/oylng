# Changelog

## Unreleased

- **Part 2, the Easy Schema, begun**, a work in progress as the draft is
  (trans/yaml2 `57407f1`). `include/oylng/schema.h`: `oylng_parse_typed`
  types each document by its stamp, `--- !name` with an entry of the
  stream's own table, or else by an application's schema
  (`oylng_schema_new`: a table, a root and a size limit). A document that
  doesn't fit is refused, as a syntax error is, with `OYLNG_ERR_SCHEMA` and
  the data path of the value. It has the eleven standard types with limits
  and a decimal's precision, literals, optional keys, type keys, `!*`,
  unions (a mapping whose one key is `!|`, section 9 at yaml2 `0b88244`;
  ` | ` in a plain scalar is refused), sequences, named and recursive types, references and merges in a
  schema, a schema's full size, section 11's typed count, and the cut of a
  time's fraction (`oylng_parser_set_cut`). `!int` is -2^63 to 2^64-1,
  `!float` the nearest binary64 (ties to even), `!bigint` and `!decimal`
  are delivered as written, and a time holds nanoseconds, a leap second and
  the year 0000. Patterns (sections 4.3 and 4.5) are I-Regexp (RFC 9485),
  read and counted as the draft says (section 9 at yaml2 `c6ba341`: every
  part counts at least 1, and a group 1 more), with Unicode 16's categories for
  `\p{..}`, and matched without backtracking, in time linear in the text;
  a number a pattern matched is delivered in the standard spelling. `!**`
  (section 10) types what a mapping holds at every level below it, by its
  rules or its `!*`, and walks the rest; a type with `!**` takes a mapping
  only (yaml2 `e75a203`). An application's schema is read under the
  size limit as a stream of its own, and its full size held to it (section
  3, yaml2 `425db57`). A published name (section 3.1) stamps a document
  through a lookup the application gives (`oylng_parser_set_lookup`), which
  gets the file by its address or its hash; the parser checks that the
  file hashes to the name (SHA-256, its own), that it is one table and
  nothing else, and that it has an entry named like the file, reads it
  under the stream's limit, and compiles it once a stream. Without a
  lookup, a published name stamps nothing. `oylng --typed` types a stream and writes each value with its
  type; with `--every-entry`, a stream with a table and no document is
  checked as a schema file, every entry as a root; with `--store DIR`, a
  published name is looked up in DIR, by its hash.
- **Application types** (part 2, section 12). `oylng_parser_bind(p, name,
  fn, drop, ctx)` binds an application's code to a named type, whichever
  schema types the document: the stream's own, the application's or a
  published one. The code takes each value whose name it is
  (`oylng_value_name`) and makes an object of the application's, or
  refuses the document, which is then `OYLNG_ERR_SCHEMA` at the value's
  path. It runs once the document is typed, never on trial; innermost
  first, so a mapping's code finds the objects below it made and can take
  them; in the order written; and once for each place a value is used, so
  `*p` twice is two calls and two objects. A value reached twice stays one
  value except on the way to a bound one, where each place gets its own.
  `oylng_value_object` and `oylng_value_take`; `oylng_typed_free` lets go
  what wasn't taken, through `drop`, and so does a refusal. 26 new API
  checks; the fuzzer binds each name a stream writes and checks the
  order, that each bound value has its own object at one place, a
  binding's refusal, and that all that is made is let go.
- **An application may ignore a file's schema, or be handed it** (part 2,
  section 3). With `oylng_parser_set_stamps(p, false)` a stamp types
  nothing, and nothing is looked up or compiled for it, so a schema in
  error refuses nothing: a document is typed as one with no stamp, by the
  application's schema, or without one, read as it is. So an application
  checking files it didn't write holds each to its own schema, as section
  15's open point on ways past a stamp asks. It can change between
  documents. `oylng_parser_table`
  gives the stream's table as a tree, a mapping of the entries in the order
  first defined, each with its latest value and its nodes shared, so a
  stamp's schema can be looked at or kept; it can be called at any time
  and changes nothing in the parser. `oylng --typed --no-stamps`. 21 new
  API checks of the table (144 in all) and 17 of stamps ignored; the
  fuzzer checks the table after each tree, types each stream with its
  stamps ignored, which must read as its trees do, and ignores every third
  document's stamp when typing.
- **Typed data written back** (part 2, section 13). With
  `oylng_emitter_set_schema(e, s)`, the emitter writes each document so
  that a parser with `s` as its application's schema reads back what it
  was given. A document is held until it ends, typed with `s` as the
  parser types it, and each scalar written in the first of its spellings
  that reads back as given. A string is plain, else quoted, so under
  `{!|: [!int, !str]}` the string `3` is written `'3'`. A number is in its
  standard spelling, then with a `+`, then with up to 20 zeros before its
  digits, and for a float after its point too, as a pattern may ask, so 5
  under `!signed` is `+5`. Null is `null`, `~` or an empty value. A
  value that reads back as nothing given, or a document that doesn't fit,
  refuses the document, `OYLNG_ERR_EMIT` with the path, and none of it is
  written. Typed calls: `oylng_emit_int`, `_uint`, `_float` (the fewest
  digits that read back), `_bool`, `_null`, `_number` (a bigint or
  decimal's text), `_datetime` (RFC 3339) and `_bytes` (base64), and
  `oylng_emit_value`, which writes a typed document again; a value's own
  spelling is tried second, so one a pattern took reads back where no
  standard one does. `oylng_emitter_bind` and `oylng_emit_object` give a
  bound type its way back: the application's code writes its object as the
  value its entry takes, which must read back as of that name, so that a
  parser's binding gets it. 76 new API checks; each typed document of the
  draft's examples (49) and of part 2's suite (392) is written again, in
  block and in flow style, and reads back the same; the fuzzer writes each
  document the application's schema typed again, which must be accepted
  in block style and read back the same.
- Tested: 254 API checks and the draft's 70 examples (`make test`); part 2's
  suite (yaml2 `c5203b6`), 801 of its 850 tests passed and none failed, 26
  of them with a store of published files, the rest skipped for times held
  otherwise (25) and `keep` (24) (`make test-yes`); all 863 of the streams
  the draft's schema checker (yaml2's `schemacheck.py`) types in its
  examples and edge cases that the engine can run, 26 with a store, each
  refused by both or accepted with the same values (`make
  test-schema-diff`, with `--every-entry`, as the checker checks a stream
  with no document, `--schema-limit` and `--store`); 50,000
  generated patterns and 199,553 strings, read, counted and matched as the
  checker's reading run by Python's `re` does (`make test-pattern-diff`);
  GitHub's workflow format and Compose as shadow schemas, on the 490
  workflow and 311 Compose files of yaml2's public sample, with the
  checker's verdict and refusal path on every file. The fuzzer types each
  stream too, and a stream after a NUL with what comes before it as a
  published file.
- `oylng_event` gains `plain`, at its end, and the tree
  `oylng_node_plain`: whether a scalar was written plain, as part 2 needs,
  since a quoted `"3"` is never an `!int`.

- **A document's name may end in a fragment**: `#`, then one or more ASCII
  letters, digits and `=`, as part 1, section 4.3, now allows (trans/yaml2
  `afbd21f`). So `--- !a#b` is the name `a#b`, and `--- !a #b` is the name
  `a` and a comment. The emitter writes such names, and refuses `a#`,
  `a#b#c` and the like. The suite's six new tests pass, and the emitter has
  9 new checks, 161 in all.

- **The outermost bound name's code runs** for a value typed through
  several (part 2, section 12, trans/yaml2 `6a116a6`). With `spot: !point`,
  a `!spot` value goes through `spot` and then `point`: it gets spot's
  code if spot is bound, whether point is or not, and point's if only
  point is. A union's name counts the same way, so code bound to `shape`
  takes the place of its alternatives'. `oylng_value_bound` gives the name
  whose code made a value's object; `oylng_value_name` is still the
  nearest. The emitter checks an object reads back with its bound name,
  the names bound to it taken as bound. The other names a value went
  through aren't kept when a schema compiles, as for a chain of n names
  that would be n²/2 of them: typing with bindings walks each type used
  again, once a document. 13 new API checks, 254 in all, and the fuzzer
  checks that each call is the bound name's.

- **A name in the table can't start with `!`**, plain or quoted, as part
  1, section 8.1, now says (trans/yaml2 `c5203b6`): `--- !spot` stamps a
  document with the entry `spot`, so an entry named `!spot` was one no
  stamp could reach. `!` inside a name, and at the start of a key below an
  entry, is still an ordinary character. The suite's three new tests pass
  (`QVU4`, `JDM9`, `3GJP`), all 365 of its tests in all.

- **References kept** (part 1, sections 10 and 11). With
  `oylng_parser_set_keep(p, true)`, documents are read as kept trees, under
  the size limit on held alone, and nothing is spread: a reference is the
  node it names, shared; a `-<<` is an item of kind `OYLNG_NODE_SPLICE`;
  and a mapping's `<<:` lines are `OYLNG_NODE_MERGE` nodes before its
  pairs (`oylng_node_target`, `oylng_node_merges`, `oylng_node_merge`). So
  a chain doubling 62 times through `-<<`, 2^63 items as data, is a tree of
  a few hundred nodes. A merged mapping in the table keeps its lines as
  written, without a copy of its pairs, and a path through it looks in each
  mapping once. Events, `oylng_emit_tree` of a kept tree and, for now,
  `oylng_parse_typed` are `OYLNG_ERR_INPUT` with references kept. With
  `oylng_parser_set_together(p, true)`, for an application that keeps
  every document, the documents' sizes add up, and with references kept,
  `...` takes nothing off held. The suite's 7 `keep` and 2 `together`
  tests pass, and every other test is also read with references kept:
  spread as section 9 says, each gives the same events and offsets, or the
  same refusal. 52 new API checks, 196 in all. The fuzzer reads every
  stream with references kept, and with its documents together.

## 0.2.0 (2026-11-12)

The tree and the emitter, and faster reading of real files. It is still
the parser for part 1 of YAML 2.0, Draft 1, tested against the suite in
[trans/yaml2](https://github.com/trans/yaml2) at `213471d`. The draft's
later changes to part 1, up to `0e2e6b3`, ask nothing new of a parser.
Part 2 (the Easy Schema) is still to come.

- **The emitter.** It writes YAML 2.0 from events, the parser's or a
  program's own: `%YAML 2.0` first, `---` or `--- !name` for each document,
  keys in the order given, every node in full (section 11). A string is
  plain where that reads back as the same string, else in single quotes,
  else in double quotes with JSON's escapes, and text with line breaks is a
  `|` or `|-` block where that reads back exactly. Collections are block
  style, indented, or flow. It refuses a repeated key, a collection as a
  key, invalid UTF-8, a bad name and events out of order, so what it writes
  always reads back as the events it was given. The output can be taken in
  pieces. Read and written again, the benchmark's inputs go at 80 to 120
  MB/s, 2.9 to 4.6 times libyaml's parser and emitter.
- **The tree.** `oylng_parse_tree` reads the next document of a stream to
  its end, into a tree of strings, sequences and mappings: one call per
  document. A node of the table is made once and shared by every place that
  names it, so a tree takes room in proportion to what is written: the API
  tests read ten million strings, through references of references, as 22
  nodes. Nodes are numbered, a mapping of more than 8 pairs has an index for
  finding keys, a tree outlives its parser, and `oylng_emit_tree` writes
  one. Reading the benchmark's inputs as trees takes 1.4 to 2.1 times as
  long as reading their events, and goes 4.1 to 7 times as fast as libyaml's
  documents (`yaml_parser_load`).
- **Faster on real files.** One-line plain values, and the keys and flow
  scalars the fast paths take, are scanned 16 bytes at a time with SSE2,
  values past the spaces between their words. The test files were 3,409
  public files of ten kinds (Ansible, Compose, GitLab CI, Helm values,
  Jekyll, Kubernetes, MkDocs, OpenAPI, Rails locales, workflows) that read
  as 2.0 once `%YAML 2.0` is put in front. On them, events go 4.1 times as
  fast as libyaml (3.3 to 5.4 by kind), up from 3.4, and trees 4.5 times as
  fast as its documents (3.9 to 6), up from 4.0: `make bench-files`, the two
  builds back to back. The generated inputs show no change.
- **`make bench-files`** times the parser on directories of real files,
  against libyaml where it can, and counts the files it refuses, by reason.
- **`oylng --emit`** writes a stream again, its references and merges
  resolved, in block style or with `--flow` one line per document.
- **`OYLNG_ERR_EMIT`**, the emitter's error.
- **Tests.** 123 API checks and the emitter's 152. Each suite test that
  passes is also written again four ways and read back, and read as trees,
  which must give the same events, offsets and errors. The fuzzer does both
  for every stream.

## 0.1.0 (2026-11-09)

The first release: the event parser for part 1 of YAML 2.0, Draft 1, as
proposed in [trans/yaml2](https://github.com/trans/yaml2)
(`spec/yaml-2.0.md`, at `213471d`). A tree, an emitter, and part 2 (the
Easy Schema) are to come.

- **Events with the table applied.** References, merges (`<<:`) and
  splices (`-<<`) are resolved before their events are delivered. The
  table, the directives and the comments are not delivered. Every value is
  a string.
- **Streaming.** A document's events are delivered as it is read. The
  parser holds the table, the keys of each open mapping, and a mapping
  with `<<:` lines until it closes. A scalar points into the input where it
  can; a value built from escapes or lines is valid until the next
  `oylng_parse_next`.
- **The draft's limits.** Repeated keys are refused, in time linear in the
  input. The size limit of section 10, 64 MiB by default, applies to both
  held and full sizes. Nesting goes to 256 levels with references
  followed, or up to 4096 if set.
- **Errors** come with a line, a column and a message, before the end of
  the document they are in. The parser stops at `...` and says where the
  rest of the input starts.
- **Speed.** On one core, about 270 to 540 MB/s on the benchmark's
  generated inputs (`make bench`): 3.7 to 8.3 times libyaml 0.2.5.
  Quoted scalars, comments and block scalars' lines are scanned with SSE2
  on x86-64 and plain loops elsewhere. Flow collections on one line and
  simple block lines take fast paths, which give the same events, offsets
  and errors as the general path.
- **Tests.** 347 of the suite's 356 tests pass; the other 9 need `keep` or
  `together`, which the event API doesn't do. 90 API checks. All run again
  without SIMD and the fast paths, and under ASan and UBSan. `make
  test-diff` compares the parser with the draft's reference checker on
  generated and mutated streams, and `make fuzz` runs libFuzzer.
- **The `oylng` tool** prints a stream's events, or the stream as JSON.
