"""Writes part 2's emit key where suite-to-data leaves it out.

    python3 -I test/suite-emit.py SRC DATA

SRC is trans/yaml2's suite/yes/src, and DATA what suite/bin/suite-to-data
wrote from it. For each test with `emit` (suite/yes/README.md: `refused` if an
emitter must refuse to write its typed events again, section 13), this writes
a file `emit` holding it in the test's directory, named as suite-to-data names
them. suite-to-data at yaml2 v0.2.0 writes every other key a harness reads, but
not this one; once it does, this leaves its file as it is.
"""

import os
import sys

import yaml


def main(src, out):
    made = 0
    for file in sorted(os.listdir(src)):
        if not file.endswith('.yaml'):
            continue
        with open(os.path.join(src, file), encoding='utf-8') as f:
            tests = yaml.safe_load(f)
        multi = len(tests) > 1
        width = len(str(len(tests) - 1)) + 1
        for n, test in enumerate(tests):
            if 'emit' not in test:
                continue
            d = os.path.join(out, file[:-5], f'{n:0{width}d}' if multi else '')
            if not os.path.isdir(d):
                sys.exit(f'{d}: no such test; was {out} written from {src}?')
            path = os.path.join(d, 'emit')
            if not os.path.exists(path):
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(f"{test['emit']}\n")
            made += 1
    print(f'marked {made} tests with emit in {out}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    main(sys.argv[1], sys.argv[2])
