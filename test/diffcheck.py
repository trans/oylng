"""Differential test: Oyl NG against the draft's checker, on mutated streams.

    python3 -I test/diffcheck.py DRAFTCHECK SPEC SUITE BATCH [-n N] [--seed S]

DRAFTCHECK is draftcheck.py, on trans/yaml2's ai branch: a throwaway parser
written from the draft's grammar, not a reference. SPEC is the draft,
spec/yaml-2.0.md, and SUITE the suite's directory, which the checker runs
first. BATCH is build/test_batch.

Mutates the suite's streams, runs each through both parsers, and reports
where they disagree: one refuses and the other doesn't, or the events or
the rest after a stopping ... differ. Each disagreement is shrunk to a
small stream that still shows it. Exits with 1 if there are any.
"""
import contextlib, io, os, random, struct, subprocess, sys

def load_checker(path, spec, suite):
    """The checker's names. Running it runs the draft and the suite too."""
    ns = {'__name__': 'draftcheck'}
    argv = sys.argv
    sys.argv = [path, spec, suite]
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            exec(compile(open(path, encoding='utf-8').read(), path, 'exec'), ns)
    except SystemExit:
        pass
    finally:
        sys.argv = argv
    return ns

def corpus(suite):
    import yaml
    src = os.path.join(suite, 'src')
    out = []
    for f in sorted(os.listdir(src)):
        if not f.endswith('.yaml'): continue
        for t in yaml.safe_load(open(os.path.join(src, f), encoding='utf-8')):
            # the doubling tests take the checker seconds each, building what they name
            if 'yaml' in t and not t.get('keep') and not t.get('together') and 'limit' not in t \
                    and 'oubling' not in t.get('name', '') and len(t['yaml']) < 4000:
                out.append(t['yaml'])
    return out

TOKENS = [' ', '  ', '\t', '\n', '\r\n', '\r', '-', '- ', '-  ', ':', ': ', ':\t', '#', ' #', ' # c',
          '*', '*a', '*a:b', '*<a:b>', '<<: ', '<<: *a', '-<< ', '-<< *s', '[', ']', '{', '}',
          ',', ', ', '"', "'", "''", '|', '|-', '>', '>-', '---', '--- ', '--- &', '--- !n', '...',
          '!', '&', '%', '%YAML 2.0', '<', '>', '\\', '\\n', '\\u00e9', '\\uD83D\\uDE00', 'é', '中',
          ' ', '\x01', '\x7f', '\x85', '﻿', 'a', 'b', 'key', 'a: 1', '- x', '\n---\n',
          '\n...\n', '\n--- &\n', '\n--- &\na: [x, y]\ns: {k: v}\n---\n', '\n  ', '\n    ', '?', '@',
          '`', '<<', '"<<"', '::', ':::']

# ── Streams from the grammar ─────────────────────────────────

WORDS = ['a', 'b', 'x', 'yy', 'port', 'name', 'a b', 'a:b', 'a#b', '-x', '?y', ':z', '%p', '@h', '`c`',
         'é', '中文', 'no', '0755', '1.10', '<< x >>', '-<<x', 'Fn::Sub', 'k=v', 'a,b', '[x]', '{y}', '"q"']
FLOWSAFE = [w for w in WORDS if not any(c in w for c in ',[]{}')]

class Gen:
    def __init__(s, rng):
        s.r = rng; s.entries = {}; s.intable = False; s.n = 0
    def key(s):
        s.n += 1
        k = s.r.choice(['k', 'key', 'a', 'b', 'é', 'Fn::Sub', 'a:b', 'x y', 'n']) + str(s.n)
        return k
    def nkeys(s, least):
        """how many keys a mapping writes: a few, or now and then more than
        the engine compares one by one"""
        return s.r.randint(6, 14) if s.r.random() < .1 else s.r.randint(least, 3)
    def mkey(s, keys):
        """a new key, or now and then one the mapping has, merged or written"""
        if keys and s.r.random() < .03: return s.r.choice(sorted(keys))
        return s.key()
    def keytext(s, k):
        c = s.r.random()
        if c < .15: return '"' + k + '"'
        if c < .25: return "'" + k + "'"
        return k
    def plain(s, flow):
        w = s.r.choice(FLOWSAFE if flow else WORDS)
        if w[0] in '-?:' and s.r.random() < .5: w = 'w' + w
        if s.intable and s.r.random() < .1: w = '!' + w
        return w
    def scalar(s, flow=False):
        c = s.r.random()
        if c < .5: return s.plain(flow)
        if c < .7: return "'" + s.r.choice(["it''s", 'a: b', '# no', '', ' lead', 'x']) + "'"
        return '"' + s.r.choice(['\\n', '\\u00e9', '\\"q\\"', '\\uD83D\\uDE00', 'a: b', '', '\\t', '\\/', 'x']) + '"'
    def ref(s, want=None):
        names = [k for k, v in s.entries.items() if want is None or v[0] == want]
        if not names: return None
        k = s.r.choice(names)
        path = ('<' + k + '>') if (':' in k or ' ' in k or k.startswith('<')) else k
        shape = s.entries[k]
        while want is None and shape[0] == 'm' and shape[1] and s.r.random() < .5:
            sub = s.r.choice(sorted(shape[1]))
            if '>' in sub or not sub: break
            path += ':' + (('<' + sub + '>') if (':' in sub or ' ' in sub or sub.startswith('<')) and '::' not in sub else sub)
            shape = shape[1][sub]
        if ' ' in path: return None
        return '*' + path, shape
    def flow(s, depth):
        if depth > 3 or s.r.random() < .3:
            r = s.ref() if s.r.random() < .2 else None
            if r: return r
            return s.scalar(True), ('s',)
        nl = lambda: s.r.choice([' ', ' ', '\n  ', ' # c\n  ', '\t'])
        if s.r.random() < .5:
            items = []
            for _ in range(s.r.randint(0, 3)):
                sp = s.ref('q') if s.r.random() < .15 else None
                items.append('-<< ' + sp[0] if sp else s.flow(depth + 1)[0])
            return '[' + ','.join(nl() + i for i in items) + (',' if items and s.r.random() < .2 else '') + ']', ('q',)
        keys = {}
        items = []
        m = s.ref('m') if s.r.random() < .2 else None
        if m: items.append('<<: ' + m[0]); keys.update(m[1][1])
        for _ in range(s.nkeys(0)):
            k = s.mkey(keys); v, sh = s.flow(depth + 1)
            kt = s.keytext(k)
            sep = ':' if kt[0] in '"\'' and s.r.random() < .3 else ': '
            items.append(kt + sep + v); keys[k] = sh
        return '{' + ','.join(nl() + i for i in items) + '}', ('m', keys)
    def block_scalar(s, ind):
        h = s.r.choice(['|', '|-', '>', '>-']) + s.r.choice(['', '', ' # c'])
        lines = []
        for _ in range(s.r.randint(0, 4)):
            c = s.r.random()
            if c < .2: lines.append('')
            elif c < .3 and h[0] == '|': lines.append(' ' * (ind + 2) + '  more')
            else: lines.append(' ' * (ind + 2) + s.r.choice(['text', '# not a comment', 'a: b', '---x', 'line two']))
        return h, lines
    def value(s, ind, depth):
        """(text after the key's ': ', lines after it, shape)"""
        c = s.r.random()
        if depth > 4: c = 0
        if c < .35: return ' ' + s.scalar(), [], ('s',)
        if c < .45:
            r = s.ref()
            if r: return ' ' + r[0], [], r[1]
            return ' ' + s.scalar(), [], ('s',)
        if c < .55:
            f, sh = s.flow(depth)
            return ' ' + f, [], sh
        if c < .65:
            h, lines = s.block_scalar(ind)
            return ' ' + h, lines, ('s',)
        if c < .82:
            lines, sh = s.bmap(ind + s.r.choice([1, 2, 2, 4]), depth + 1)
            return s.r.choice(['', ' # c']), lines, sh
        lines, sh = s.bseq(ind + s.r.choice([0, 2, 2]), depth + 1)
        return '', lines, sh
    def bmap(s, ind, depth):
        sp = ' ' * ind
        lines, keys = [], {}
        m = s.ref('m') if s.r.random() < .2 else None
        if m: lines.append(sp + '<<: ' + m[0]); keys.update(m[1][1])
        for _ in range(s.nkeys(1)):
            k = s.mkey(keys); first, rest, sh = s.value(ind, depth)
            lines.append(sp + s.keytext(k) + ':' + first); lines += rest; keys[k] = sh
            if s.r.random() < .1: lines.append(s.r.choice(['', sp + '# comment']))
        return lines, ('m', keys)
    def bseq(s, ind, depth):
        sp = ' ' * ind
        lines = []
        for _ in range(s.r.randint(1, 3)):
            c = s.r.random()
            if c < .15:
                r = s.ref('q')
                if r: lines.append(sp + '-<< ' + r[0]); continue
            if c < .3 and depth < 5:
                sub, _ = s.bmap(ind + 2, depth + 1)
                lines.append(sp + '- ' + sub[0][ind + 2:]); lines += sub[1:]; continue
            first, rest, _ = s.value(ind, depth)
            if first.strip() == '' and rest and rest[0].startswith(sp + '-'):
                lines.append(sp + '-'); lines += [l for l in rest]; continue
            lines.append(sp + '-' + first); lines += rest
        return lines, ('q',)
    def table(s):
        s.intable = True
        if s.r.random() < .2:
            f, sh = s.flow(1)
            if f.startswith('{'):
                s.intable = False
                for k, v in sh[1].items(): s.entries[k] = v
                return ['--- & ' + f] if s.r.random() < .5 else ['--- &', f]
        lines = ['--- &']
        here = set()
        for _ in range(s.r.randint(1, 4)):
            k = s.r.choice(['a', 'b', 'm', 's', 'base', 'list', '.setup', 'a:b']) if s.r.random() < .5 else s.key()
            if k in here: continue
            here.add(k)
            first, rest, sh = s.value(0, 0)
            lines.append(s.keytext(k) + ':' + first); lines += rest
            s.entries[k] = sh
        s.intable = False
        return lines
    def doc(s):
        head = '---' + (' !' + s.r.choice(['job', 'github.com/a/b', '配置', 'n.v-1_x']) if s.r.random() < .2 else '')
        c = s.r.random()
        if c < .2:
            first, rest, _ = s.value(0, 1)
            if first.strip() and not first.strip().startswith('#'):
                return [head + first] + rest
        if c < .6: lines, _ = s.bmap(0, 1)
        elif c < .9: lines, _ = s.bseq(0, 1)
        else: lines = []
        return [head] + lines
    def stream(s):
        lines = ['%YAML 2.0']
        for _ in range(s.r.randint(1, 4)):
            lines += s.table() if s.r.random() < .4 else s.doc()
            if s.r.random() < .08:
                lines.append('...'); s.entries.clear()
                if s.r.random() < .3: lines.append('%YAML 2.0')
        if s.r.random() < .08: lines += ['...', '# Heading', '', 'not yaml: [']
        t = '\n'.join(lines) + s.r.choice(['\n', '\n', '', '\n\n'])
        if s.r.random() < .1: t = t.replace('\n', '\r\n')
        return t

def mutate(rng, t, pool):
    for _ in range(rng.randint(1, 3)):
        k = rng.randrange(8)
        i = rng.randint(0, len(t))
        if k <= 2:
            t = t[:i] + rng.choice(TOKENS) + t[i:]
        elif k == 3:
            t = t[:i] + t[i + rng.randint(1, 4):]
        elif k == 4:
            lines = t.split('\n')
            j = rng.randrange(len(lines))
            lines.insert(j, lines[j])
            t = '\n'.join(lines)
        elif k == 5:
            lines = t.split('\n')
            j = rng.randrange(len(lines))
            lines[j] = (' ' * rng.randint(1, 2) + lines[j]) if rng.random() < .5 else lines[j][1:]
            t = '\n'.join(lines)
        elif k == 6:
            o = rng.choice(pool)
            j = rng.randint(0, len(o))
            t = t[:i] + o[j:j + rng.randint(1, 40)] + t[i:]
        else:
            lines = t.split('\n')
            a, b = rng.randrange(len(lines)), rng.randrange(len(lines))
            lines[a], lines[b] = lines[b], lines[a]
            t = '\n'.join(lines)
    return t

def by_checker(ck, t):
    try:
        r = ck['P'](t, 64 * 2 ** 20, False, False, 256).stream().finish()
    except ck['No']:
        return ('ERR',)
    except RecursionError:
        return ('SKIP',)
    except Exception as e:
        return ('CRASH', repr(e))
    rest = None if r.stopped is None else r.t[r.stopped:]
    return ('OK', ck['events'](r.docs), rest)

def by_engine(batch, texts):
    data = b''.join(struct.pack('<I', len(b)) + b + struct.pack('<I', 0)
                    for b in (t.encode('utf-8') for t in texts))
    out = subprocess.run([batch], input=data, capture_output=True, check=True).stdout
    res = []
    for t, rec in zip(texts, out.split(b'\0')):
        head, _, body = rec.partition(b'\n')
        head = head.decode()
        if head.startswith('ERR'):
            res.append(('ERR', head[4:]))
            continue
        off = head[3:]
        rest = None
        if off != '-':
            rest = t.encode('utf-8')[int(off):].decode('utf-8').replace('\r\n', '\n')
        res.append(('OK', body.decode('utf-8'), rest))
    return res

def same(a, b):
    if a[0] in ('SKIP', 'CRASH'): return True
    if a[0] != b[0]: return False
    return a[0] == 'ERR' or (a[1] == b[1] and a[2] == b[2])

def shrink(ck, batch, t):
    """A smaller stream on which the two still disagree"""
    def bad(s):
        try: s.encode('utf-8')
        except UnicodeEncodeError: return False
        return not same(by_checker(ck, s), by_engine(batch, [s])[0])
    n = max(len(t) // 2, 1)
    while n >= 1:
        i, cut = 0, False
        while i < len(t):
            s = t[:i] + t[i + n:]
            if s.startswith('%YAML 2.0') and bad(s): t, cut = s, True
            else: i += n
        if not cut: n //= 2
    return t

def main():
    args = sys.argv[1:]
    n, seed = 20000, 1
    if '-n' in args: i = args.index('-n'); n = int(args[i + 1]); del args[i:i + 2]
    if '--seed' in args: i = args.index('--seed'); seed = int(args[i + 1]); del args[i:i + 2]
    if len(args) != 4: sys.exit(__doc__.strip())
    draftcheck, spec, suite, batch = args
    ck = load_checker(draftcheck, spec, suite)
    pool = corpus(suite)
    rng = random.Random(seed)
    texts = []
    while len(texts) < n:
        k = rng.random()
        if k < .4: t = Gen(rng).stream()
        elif k < .7:
            t = Gen(rng).stream()
            for _ in range(rng.randint(1, 2)):
                i = rng.randint(0, len(t)); t = t[:i] + rng.choice(TOKENS) + t[i:]
        else: t = mutate(rng, rng.choice(pool), pool)
        if len(t) > 4000: continue
        try: t.encode('utf-8')
        except UnicodeEncodeError: continue
        texts.append(t)
    texts = pool + texts
    engine = []
    for i in range(0, len(texts), 2000):
        engine += by_engine(batch, texts[i:i + 2000])
    found, crashes, kinds = [], 0, {'OK': 0, 'ERR': 0}
    for t, e in zip(texts, engine):
        c = by_checker(ck, t)
        if c[0] == 'CRASH': crashes += 1; continue
        if c[0] in kinds: kinds[c[0]] += 1
        if not same(c, e): found.append(t)
    print(f'{len(texts)} streams: the checker read {kinds["OK"]} and refused {kinds["ERR"]}; '
          f'{crashes} crashed it; {len(found)} disagree')
    seen = set()
    for t in found:
        s = shrink(ck, batch, t)
        if s in seen: continue
        seen.add(s)
        c, e = by_checker(ck, s), by_engine(batch, [s])[0]
        print('---', repr(s))
        print('  checker:', c[0], '' if c[0] != 'OK' else repr(c[1]) + (f' rest={c[2]!r}' if c[2] is not None else ''))
        print('  engine: ', e[0], e[1] if e[0] == 'ERR' else repr(e[1]) + (f' rest={e[2]!r}' if e[2] is not None else ''))
    sys.exit(1 if found else 0)

main()
