"""Differential test: Oyl NG's typing (part 2) against the draft's schema checker.

    python3 -I test/schemadiff.py SCHEMACHECK SPEC TOOL [-v] [--seeds DIR]

SCHEMACHECK is schemacheck.py, on trans/yaml2's ai branch, with
draftcheck.py beside it: a throwaway checker written from part 2's text, not
a reference. SPEC is part 2's draft, spec/yaml-easy-schema.md. TOOL is
build/oylng.

Runs the checker on its own examples and edge cases, logging each stream it
types and each schema it compiles on its own, then gives each to the engine
(`oylng --typed`) and compares: both refuse, or both accept with the same
values. The checker is asked again for each, as the engine is: holding what
the engine holds of a date (9 fraction digits, a second of 60, the year
0000). A stream with a limit is run twice: with the limit the stream's too
(part 1, section 10), applied to held and full as the engine's tree applies
it, which the checker's typed leaves out; and with it for part 2's counts
alone (`--typed-limit`), typed counted as an interface that keeps
references counts it. In both, an application's schema is read under the
limit as a stream of its own, and held to it for its full size (section 3,
`--schema-limit`).

Skipped, and counted: streams the checker types with a store of published
schemas, which the engine doesn't look up, and a limit of 0, which the
tool takes as the default. Where both refuse, the data paths are compared too, and a different one is reported
but isn't a disagreement: the checker's path is the first fault it meets.
Exits with 1 if there are any disagreements. With --seeds, also writes each
stream, framed, into DIR as a fuzz input (fuzz/fuzz_parser.c), its first byte
0 for the default options.
"""
import base64, contextlib, io, json, math, os, subprocess, sys, tempfile, textwrap
from decimal import Decimal

ENGINE_HOLD = (9, True, True)

def load(path, spec):
    """The checker's names, with typed and Schema logged, and its main run."""
    src = open(path, encoding='utf-8').read()
    defs, main = src.split("\nif __name__ == '__main__':\n")
    ns = {'__name__': 'schemacheck', '__file__': os.path.abspath(path)}
    exec(compile(defs, path, 'exec'), ns)
    typed0, Schema0, read0 = ns['typed'], ns['Schema'], ns['read']
    streams, schemas, texts, depth = [], [], {}, [0]

    def typed(stream, app=None, store=None, limit=ns['LIMIT'], cut=False, keep=False):
        if not depth[0]:
            streams.append(dict(stream=stream, app=app, store=store, limit=limit, cut=cut))
        depth[0] += 1
        try: return typed0(stream, app, store, limit, cut, keep)
        finally: depth[0] -= 1

    def read(t, *a, **k):
        r = read0(t, *a, **k)
        if not depth[0]: texts[id(r.table)] = (t, r)
        return r

    def Schema(table, root, cut=False, limit=ns['LIMIT'], kept=False):
        if not depth[0] and id(table) in texts:
            schemas.append(dict(text=texts[id(table)][0], root=root, limit=limit))
        depth[0] += 1
        try: return Schema0(table, root, cut, limit, kept)
        finally: depth[0] -= 1

    ns['typed'], ns['Schema'], ns['read'] = typed, Schema, read
    argv = sys.argv
    sys.argv = [path, spec]
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            exec(compile(textwrap.dedent(main), path, 'exec'), ns)
    except SystemExit:
        pass
    finally:
        sys.argv = argv
    ns['typed'], ns['Schema'], ns['read'] = typed0, Schema0, read0
    return ns, streams, schemas, out.getvalue()

def checker(ns, c, whole):
    """The checker's verdict as the engine would give it: ('ok', values) or ('no', message).
    With whole, the limit is the stream's too (part 1, section 10), applied to held and full,
    as an interface that delivers data with no references left applies it; the checker's typed
    leaves that out. Without, it is part 2's counts' alone, typed counted as an interface that
    keeps references counts it."""
    hold = ns['DATEHOLD']
    ns['DATEHOLD'] = ENGINE_HOLD
    try:
        if whole:
            read = ns['g']['read']
            read(c['stream'], c['limit'])
            if c['app']: read(c['app'][0], c['limit'])
        return 'ok', ns['typed'](c['stream'], c['app'], None, c['limit'], c['cut'], not whole)
    except ns['REFUSED'] + (RecursionError,) as e:
        return 'no', str(e)
    finally:
        ns['DATEHOLD'] = hold

def checker_schema(ns, c):
    try:
        ns['Schema'](ns['read'](c['text']).table, c['root'], limit=c['limit'])
        return 'ok', []
    except ns['REFUSED'] as e:
        return 'no', str(e)

def engine(tool, tmp, framed, stream, app=None, limit=None, cut=False, counts=None, schema_limit=None):
    sf = os.path.join(tmp, 'stream.yaml')
    with open(sf, 'w', encoding='utf-8', newline='') as f: f.write(framed(stream))
    cmd = [tool, '--typed', '--every-entry']   # typed() checks every entry of a stream with no document
    if app:
        af = os.path.join(tmp, 'app.yaml')
        with open(af, 'w', encoding='utf-8', newline='') as f: f.write(framed(app[0]))
        cmd += ['--schema', af, app[1]]
    if cut: cmd.append('--cut')
    if limit is not None: cmd += ['--limit', str(limit)]
    if counts is not None: cmd += ['--typed-limit', str(counts)]
    if schema_limit is not None: cmd += ['--schema-limit', str(schema_limit)]
    r = subprocess.run(cmd + [sf], capture_output=True, timeout=120)
    err = r.stderr.decode('utf-8', 'replace').strip()
    if r.returncode == 0:
        return 'ok', [json.loads(l, object_pairs_hook=Pairs) for l in r.stdout.decode('utf-8').splitlines()]
    if r.returncode == 1:
        # FILE:LINE:COL: message
        return 'no', err.split(': ', 1)[1] if ': ' in err else err
    return 'crash', f'exit {r.returncode}: {err}'

class Pairs(list):
    """A JSON object, its pairs in order"""

def same(e, c):
    """The engine's [type, value] e against the checker's value c."""
    if e is None: return False
    t, v = e[0], e[1]
    if t == 'any': return same_tree(v, c)
    if t == 'str': return type(c) is not bool and isinstance(c, str) and c == v
    if t in ('int', 'bigint'): return type(c) is int and c == int(v)
    if t == 'float':
        f = float(v)
        return type(c) is float and c == f and math.copysign(1, c) == math.copysign(1, f)
    if t == 'decimal':
        if isinstance(c, Decimal): return Decimal(v).as_tuple() == c.as_tuple() or written(v) == c.as_tuple()
        return isinstance(c, str) and c == v   # Written: past Decimal's exponents
    if t == 'bool': return type(c) is bool and c == v
    if t == 'null': return c is None
    if t in ('date', 'datetime'): return isinstance(c, str) and c == v
    if t == 'binary':   # the checker gives the text without its breaks: compare the bytes
        return isinstance(c, str) and base64.b64decode(c).hex() == e[2]
    if t == 'seq': return isinstance(c, list) and len(c) == len(v) and all(same(a, b) for a, b in zip(v, c))
    if t == 'map':
        return isinstance(c, dict) and [k for k, _ in v] == list(c) and all(same(a, c[k]) for k, a in v)
    return False

def written(v):
    """v as Decimal reads it, without a sign: the checker drops a written +."""
    return Decimal(v.lstrip('+')).as_tuple()

def same_tree(e, c):
    if isinstance(e, Pairs):
        return isinstance(c, dict) and [k for k, _ in e] == list(c) and all(same_tree(a, c[k]) for k, a in e)
    if isinstance(e, list):
        return isinstance(c, list) and len(e) == len(c) and all(same_tree(a, b) for a, b in zip(e, c))
    return isinstance(e, str) and isinstance(c, str) and e == c

def path_of(msg):
    return msg.split(':', 1)[0] if msg.startswith('/') else None

def short(s, n=160):
    s = repr(s)
    return s if len(s) <= n else s[:n] + '...'

def main():
    argv = sys.argv[1:]
    seeds = None
    if '--seeds' in argv:
        i = argv.index('--seeds'); seeds = argv[i + 1]; del argv[i:i + 2]
    args = [a for a in argv if not a.startswith('-')]
    verbose = '-v' in argv
    if len(args) != 3: sys.exit(__doc__)
    path, spec, tool = args
    ns, streams, schemas, said = load(path, spec)
    framed = ns['g']['framed']
    if seeds:
        import hashlib
        os.makedirs(seeds, exist_ok=True)
        for c in streams:
            b = b'\0' + framed(c['stream']).encode('utf-8', 'surrogatepass')
            if len(b) > 4096: continue
            with open(os.path.join(seeds, 'schema-' + hashlib.sha1(b).hexdigest()[:16]), 'wb') as f: f.write(b)
    tail = said.strip().splitlines()[-2:]
    print('checker:', '; '.join(tail))
    seen, n, agree, store, differ, paths, zero = set(), 0, 0, 0, [], [], 0

    def compare(c, want, got):
        nonlocal agree
        if got[0] == 'crash' or want[0] != got[0]: differ.append((c, want, got)); return
        if want[0] == 'no':
            agree += 1
            if path_of(want[1]) and path_of(got[1]) and path_of(want[1]) != path_of(got[1]):
                paths.append((c, want, got))
            if verbose: print('both refuse:', short(c['stream'], 80), '|', want[1][:70], '|', got[1][:70])
        elif len(want[1]) == len(got[1]) and all(same(e, v) for e, v in zip(got[1], want[1])): agree += 1
        else: differ.append((c, want, got))

    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
        for c in streams:
            key = (c['stream'], c['app'], c['limit'], c['cut'], c['store'] is not None)
            if key in seen: continue
            seen.add(key)
            if c['store'] is not None: store += 1; continue
            limited = c['limit'] != ns['LIMIT']
            n += 1
            compare(dict(c, how='the whole limit'), checker(ns, c, True),
                    engine(tool, tmp, framed, c['stream'], c['app'], c['limit'] if limited else None, c['cut']))
            if limited:
                n += 1
                # an application's schema is a stream of its own, read under the limit (section 3)
                compare(dict(c, how="part 2's counts alone"), checker(ns, c, False),
                        engine(tool, tmp, framed, c['stream'], c['app'], None, c['cut'], c['limit'],
                               c['limit'] if c['app'] else None))
        for c in schemas:
            key = ('schema', c['text'], c['root'], c['limit'])
            if key in seen: continue
            seen.add(key)
            if c['limit'] == 0: zero += 1; continue   # the tool's 0 is the default
            n += 1
            limited = c['limit'] != ns['LIMIT']
            compare(dict(stream='(a schema alone)', app=(c['text'], c['root']), limit=c['limit'], cut=False,
                         how="part 2's counts alone"),
                    checker_schema(ns, c), engine(tool, tmp, framed, '', (c['text'], c['root']), None, False,
                                                  c['limit'] if limited else None))
    for c, want, got in differ:
        print('DIFFER:', short(c['stream']))
        if c['app']: print('  app schema', short(c['app'][0]), 'root', c['app'][1])
        if c['limit'] != ns['LIMIT'] or c['cut']: print('  limit', c['limit'], 'cut', c['cut'], 'as', c['how'])
        print('  checker:', short(want[1] if want[0] == 'no' else want[1], 300), '(refuses)' if want[0] == 'no' else '')
        print('  engine: ', short(got[1], 300), '(refuses)' if got[0] != 'ok' else '')
    for c, want, got in paths[:20] if verbose else []:
        print('PATH:', short(c['stream'], 120), '| checker', want[1][:80], '| engine', got[1][:80])
    print(f'{n} cases: {agree} agree, {len(differ)} differ, '
          f'{store} with a store and {zero} with a limit of 0 not run; {len(paths)} refused at different paths')
    sys.exit(1 if differ else 0)

if __name__ == '__main__':
    main()
