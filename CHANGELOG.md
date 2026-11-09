# Changelog

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
- **Speed.** On one core, 280 to 550 MB/s on the benchmark's generated
  inputs (`make bench`): 3.8 to 8.1 times libyaml 0.2.5. Quoted scalars,
  comments and block scalars' lines are scanned with SSE2 on x86-64 and
  plain loops elsewhere. Flow collections on one line and simple block
  lines take fast paths, which give the same events, offsets and errors as
  the general path.
- **Tests.** 347 of the suite's 356 tests pass; the other 9 need `keep` or
  `together`, which the event API doesn't do. 88 API checks. All run again
  without SIMD and the fast paths, and under ASan and UBSan. `make
  test-diff` compares the parser with the draft's reference checker on
  generated and mutated streams, and `make fuzz` runs libFuzzer.
- **The `oylng` tool** prints a stream's events, or the stream as JSON.
