# Oyl NG

Oyl NG reads YAML 2.0, the version proposed in
[trans/yaml2](https://github.com/trans/yaml2) (`spec/yaml-2.0.md`, Draft 1).
For YAML 1.x, use [Oyl](https://github.com/openbohemians/oyl).

It is a C11 library built from Oyl's work. So far it has the event parser
for part 1 of the draft. A tree, an emitter, and part 2 (the Easy Schema)
are to come.

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

The `oylng` tool prints a stream's events, or the stream as JSON:

```sh
build/oylng --events file.yaml
build/oylng --json --limit 1000000 file.yaml
```

## Building and testing

`make` builds `build/liboylng.a` and `build/oylng`.

- `make test` runs the API tests and the YAML 2.0 test suite. The suite is
  read from `../yaml2/suite` (set `SUITE=` for another place) and converted
  with the suite's `bin/suite-to-data`, which needs PyYAML. It currently
  lives in trans/yaml2's working copy, not yet committed there.
- `make test-asan` runs both again with AddressSanitizer and UBSan.
- `make test-diff` reads generated and mutated streams with this parser and
  with the draft's reference checker, and reports any stream they disagree
  on. Set `DRAFTCHECK=` to the checker and `DIFF_N=` for how many streams.
- `make fuzz` runs libFuzzer for five minutes (`FUZZ_TIME=` seconds), with
  one worker, seeded from the suite. It needs clang.
- `make bench` times the parser on generated inputs. If pkg-config finds
  libyaml, it times libyaml on the same inputs, too. Set `BENCH_CPU=` to pin
  it to one core.

## License

MIT. See [LICENSE](LICENSE).
