# Changelog

## Unreleased

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
- **The tree.** `oylng_parse_tree` reads a document whole, into strings,
  sequences and mappings. A node of the table is made once and shared by
  every place that names it, so a tree takes room in proportion to what is
  written: the API tests read ten million strings, through references of
  references, as 22 nodes. Nodes are numbered, a mapping of more than 8
  pairs has an index for finding keys, a tree outlives its parser, and
  `oylng_emit_tree` writes one. Reading the benchmark's inputs as trees
  takes 1.4 to 2.1 times as long as reading their events, and goes 4.1 to
  7 times as fast as libyaml's documents (`yaml_parser_load`).
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
