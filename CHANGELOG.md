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
  unions, sequences, named and recursive types, references and merges in a
  schema, a schema's full size, section 11's typed count, and the cut of a
  time's fraction (`oylng_parser_set_cut`). `!int` is -2^63 to 2^64-1,
  `!float` the nearest binary64 (ties to even), `!bigint` and `!decimal`
  are delivered as written, and a time holds nanoseconds, a leap second and
  the year 0000. Patterns (sections 4.3 and 4.5) are I-Regexp (RFC 9485),
  read and counted as the draft says, with Unicode 16's categories for
  `\p{..}`, and matched without backtracking, in time linear in the text;
  a number a pattern matched is delivered in the standard spelling. Not
  yet: `!**`, which a schema is refused for, saying so, and published
  names. `oylng --typed` types a stream and writes each value with its
  type; with `--every-entry`, a stream with a table and no document is
  checked as a schema file, every entry as a root.
- Tested: 60 API checks and the draft's 59 examples without `!**` (`make
  test`); part 2's suite (yaml2 `63e86ea`), 664 of its 806 tests passed and
  none failed, the rest skipped for `!**` (69), times held otherwise (25),
  `keep` (24), published names (23) and a limit of 0 (1) (`make test-yes`); 716 of the streams the draft's schema checker (yaml2's
  `schemacheck.py`) types in its examples and edge cases, each refused by
  both or accepted with the same values (`make test-schema-diff`, with
  `--every-entry`, as the checker checks a stream with no document); 50,000
  generated patterns and 199,553 strings, read, counted and matched as the
  checker's reading run by Python's `re` does (`make test-pattern-diff`);
  GitHub's workflow format and Compose as shadow schemas, on the 490
  workflow and 311 Compose files of yaml2's public sample, with the
  checker's verdict and refusal path on every file. The fuzzer types each
  stream too.
- `oylng_event` gains `plain`, at its end, and the tree
  `oylng_node_plain`: whether a scalar was written plain, as part 2 needs,
  since a quoted `"3"` is never an `!int`.

- **A document's name may end in a fragment**: `#`, then one or more ASCII
  letters, digits and `=`, as part 1, section 4.3, now allows (trans/yaml2
  `afbd21f`). So `--- !a#b` is the name `a#b`, and `--- !a #b` is the name
  `a` and a comment. The emitter writes such names, and refuses `a#`,
  `a#b#c` and the like. The suite's six new tests pass, and the emitter has
  9 new checks, 161 in all.

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
