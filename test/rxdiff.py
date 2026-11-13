"""Differential test: part 2's patterns, the engine's against the checker's.

    python3 -I test/rxdiff.py SCHEMACHECK BATCH [-n N] [--seed S]

SCHEMACHECK is schemacheck.py on trans/yaml2's ai branch, whose iregexp()
reads a pattern as section 4.3 has it, counts it as section 9 does, and
hands it to Python's re. BATCH is build/rx_batch. Generates N patterns
(default 20000), each with strings meant to match it and near misses, some
patterns broken on purpose, and compares: both refuse the pattern, or both
give the same count and, within the ceiling, the same match for each
string. Python's re backtracks, so a match it takes more than a second
over is skipped, and counted. Exits with 1 if any differ.
"""

class Slow(Exception): pass

def alarm(*_): raise Slow()
import contextlib, io, os, random, re, signal, subprocess, sys

def load(path):
    src = open(path, encoding='utf-8').read().split("\nif __name__ == '__main__':")[0]
    ns = {'__name__': 'schemacheck', '__file__': os.path.abspath(path)}
    with contextlib.redirect_stdout(io.StringIO()):
        exec(compile(src, path, 'exec'), ns)
    return ns

CHARS = ['a', 'b', 'c', 'x', '-', ' ', '/', ':', '#', 'é', 'ß', '中', '\U0001F600', '0', '9', 'A', 'Z', '_', ',', '~']
ODD = ['^', '$', '*', '+', '?', '{', '}', ']', ')', '(', '[', '\\', '|', '\n', '\r', '.', '\t']
CATS = ['L', 'Lu', 'Ll', 'Lo', 'M', 'N', 'Nd', 'No', 'P', 'Pd', 'Po', 'Z', 'Zs', 'S', 'Sm', 'So', 'C', 'Cc', 'Cn', 'Co']
BADCATS = ['Cs', 'Xx', 'L&', 'l', '', 'Lul', 'IsLatin']
ESC = list('()*+-.?[\\]^nrt{|}')
ESCVAL = {'n': '\n', 'r': '\r', 't': '\t'}

class Gen:
    def __init__(s, rng): s.r = rng

    def char(s):
        return s.r.choice(CHARS)

    def member(s):
        """A class member, as written, and a sample character it holds (or None)."""
        r = s.r.random()
        if r < 0.45:
            c = s.char()
            if c in '-[]\\^': c = 'q'
            return c, c
        if r < 0.65:
            a, b = sorted([s.char(), s.char()])
            if any(c in '-[]\\^' for c in (a, b)): a, b = 'a', 'f'
            c = s.r.randint(ord(a), ord(b))
            return f'{a}-{b}', a if 0xD800 <= c <= 0xDFFF else chr(c)   # no string holds a surrogate
        if r < 0.8:
            e = s.r.choice(ESC)
            return '\\' + e, ESCVAL.get(e, e)
        cat = s.r.choice(CATS)
        neg = s.r.random() < 0.3
        return f'\\{"P" if neg else "p"}{{{cat}}}', None

    def cls(s):
        neg = s.r.random() < 0.25
        parts, samples = [], []
        if s.r.random() < 0.1: parts.append('-'); samples.append('-')
        for _ in range(s.r.randint(1, 4)):
            m, c = s.member(); parts.append(m)
            if c is not None: samples.append(c)
        if s.r.random() < 0.1: parts.append('-'); samples.append('-')
        sample = None if neg or not samples else s.r.choice(samples)
        return '[' + ('^' if neg else '') + ''.join(parts) + ']', sample

    def atom(s, depth):
        r = s.r.random()
        if r < 0.45:
            c = s.char()
            return re.sub(r'([\\.*+?()\[\]{}|^$])', r'\\\1', c) if c in '\\.*+?()[]{}|^$' else c, c
        if r < 0.55: return '.', s.r.choice('abé中 ')
        if r < 0.7: return s.cls()
        if r < 0.78:
            e = s.r.choice(ESC)
            return '\\' + e, ESCVAL.get(e, e)
        if r < 0.85:
            cat = s.r.choice(CATS)
            neg = s.r.random() < 0.3
            return f'\\{"P" if neg else "p"}{{{cat}}}', None
        if depth < 4:
            p, smp = s.regexp(depth + 1)
            return '(' + p + ')', smp
        return 'a', 'a'

    def quant(s):
        r = s.r.random()
        if r < 0.55: return '', (1, 1)
        if r < 0.65: return '*', (0, 3)
        if r < 0.75: return '+', (1, 3)
        if r < 0.83: return '?', (0, 1)
        lo = s.r.choice([0, 0, 1, 2, 3, 5])
        k = s.r.random()
        if k < 0.4: return f'{{{lo}}}', (lo, lo)
        if k < 0.6: return f'{{{lo},}}', (lo, lo + 2)
        hi = lo + s.r.choice([0, 1, 2, 4])
        return f'{{{lo},{hi}}}', (lo, hi)

    def branch(s, depth):
        out, smp = '', ''
        for _ in range(s.r.choice([0, 1, 1, 2, 2, 3, 4])):
            a, sa = s.atom(depth)
            q, (lo, hi) = s.quant()
            out += a + q
            if smp is not None:
                k = s.r.randint(lo, hi)
                if k and sa is None: smp = None
                else: smp += (sa or '') * k
        return out, smp

    def regexp(s, depth=0):
        bs = [s.branch(depth) for _ in range(s.r.choice([1, 1, 1, 2, 3]))]
        pick = s.r.choice(bs)
        return '|'.join(b for b, _ in bs), pick[1]

    def big(s):
        """Counts near and past the ceiling"""
        n = s.r.choice([1, 2, 10, 100, 511, 512, 513, 1000, 1023, 1024, 1025, 99999999999999999999])
        return s.r.choice([f'a{{{n}}}', f'a{{{n},}}', f'(a|b){{{n // 2}}}', f'(a{{{n}}}){{0}}', f'x{{0,{n}}}',
                           f'(ab){{{n}}}c?', f'[a-z]{{{n},{n}}}'])

    def broken(s, p):
        if not p or s.r.random() < 0.3:
            return s.r.choice(['a{', '{1}', 'a{,3}', 'a{3,2}', '(a', 'a)', '[]', '[^]', '[a', '[a-]b', '[b-a]',
                               '[--a]', '[a-\\p{L}]', '\\d', '\\x41', '\\p{Cs}', '\\p{L', '^a', 'a$', 'a**',
                               'a{2}{3}', '(?:a)', '\\', 'a|*', '[[a]]', '\\pL', '\\u0041'])
        i = s.r.randrange(len(p) + 1)
        return p[:i] + s.r.choice(ODD + ['\\p{' + s.r.choice(BADCATS) + '}', '[', '{2,1}']) + p[i:]

    def strings(s, p, sample):
        out = set()
        if sample is not None: out.add(sample)
        base = sample if sample is not None else ''
        for _ in range(4):
            t = list(base)
            for _ in range(s.r.randint(0, 2)):
                k = s.r.random()
                if k < 0.4 and t: del t[s.r.randrange(len(t))]
                elif k < 0.8: t.insert(s.r.randint(0, len(t)), s.r.choice(CHARS + ['\n']))
                elif t: t[s.r.randrange(len(t))] = s.r.choice(CHARS)
            out.add(''.join(t)[:24])
        out.add(''.join(s.r.choice(CHARS) for _ in range(s.r.randint(0, 6))))
        return sorted(out)

def main():
    args = sys.argv[1:]
    n, seed = 20000, 1
    if '-n' in args: i = args.index('-n'); n = int(args[i + 1]); del args[i:i + 2]
    if '--seed' in args: i = args.index('--seed'); seed = int(args[i + 1]); del args[i:i + 2]
    check, batch = args
    ns = load(check)
    pattern, SchemaError = ns['pattern'], ns['SchemaError']
    g = Gen(random.Random(seed))
    cases = []
    for k in range(n):
        r = g.r.random()
        if r < 0.75: p, smp = g.regexp()
        elif r < 0.85: p, smp = g.big(), None
        else: p, smp = g.broken(g.regexp()[0]), None
        cases.append((p, g.strings(p, smp)))
    want, recs, slow = [], [], 0
    signal.signal(signal.SIGALRM, alarm)
    for p, ss in cases:
        try:
            pat = pattern(p, p)[1]
            bad, count = False, pat.count
        except SchemaError:
            bad, count, pat = True, None, None
        except (RecursionError, OverflowError):
            continue
        for t in ss:
            m = None
            if not bad and count <= 1024:
                signal.setitimer(signal.ITIMER_REAL, 1.0)
                try: m = 1 if pat.fullmatch(t) else 0
                except Slow: slow += 1; continue
                finally: signal.setitimer(signal.ITIMER_REAL, 0)
            want.append((p, t, bad, count, m))
            pb, tb = p.encode('utf-8'), t.encode('utf-8')
            recs.append(f'{len(pb)} {len(tb)}\n'.encode() + pb + tb)
    r = subprocess.run([batch], input=b''.join(recs), capture_output=True, timeout=600)
    if r.returncode:
        print('BATCH FAILED', r.returncode, r.stderr.decode('utf-8', 'replace')[-2000:]); sys.exit(1)
    got = r.stdout.decode('utf-8').splitlines()
    if len(got) != len(want):
        print('BATCH GAVE', len(got), 'LINES FOR', len(want)); sys.exit(1)
    differ = 0; nbad = nmatch = nyes = nbig = 0
    for (p, t, bad, count, m), line in zip(want, got):
        if bad:
            nbad += 1
            ok = line.startswith('bad ')
        else:
            parts = line.split(' ')
            ok = parts[0] == str(min(count, 2 ** 64 - 1)) and parts[1] == ('-' if m is None else str(m))
            if m is None: nbig += 1
            else: nmatch += 1; nyes += m
        if not ok:
            differ += 1
            if differ <= 30: print('DIFFER', repr(p), repr(t), 'checker:', 'bad' if bad else (count, m), 'engine:', line)
    print(f'{len(cases)} patterns, {len(want)} cases: {differ} differ; {nbad} refused, {nbig} past the ceiling, '
          f'{nmatch} matched against ({nyes} match); {slow} skipped, too slow for Python\'s re')
    sys.exit(1 if differ else 0)

if __name__ == '__main__':
    main()
