"""Write src/oylng_ucd.c: Unicode's general categories, for `\\p{..}` in
part 2's patterns (section 4.3 asks for Unicode 16 or later).

    python3 -I tools/gen_ucd.py > src/oylng_ucd.c

From this Python's unicodedata, whose version the file records.
"""
import sys, unicodedata

CATS = ['Lu', 'Ll', 'Lt', 'Lm', 'Lo', 'Mn', 'Mc', 'Me', 'Nd', 'Nl', 'No', 'Pc', 'Pd', 'Ps', 'Pe', 'Pi',
        'Pf', 'Po', 'Sm', 'Sc', 'Sk', 'So', 'Zs', 'Zl', 'Zp', 'Cc', 'Cf', 'Cs', 'Co', 'Cn']
major = tuple(int(x) for x in unicodedata.unidata_version.split('.'))
if major < (16,):
    sys.exit(f'Unicode {unicodedata.unidata_version}: part 2 asks for 16 or later')

runs, last = [], None
for c in range(0x110000):
    g = CATS.index(unicodedata.category(chr(c)))
    if g != last:
        runs.append((c, g)); last = g

out = sys.stdout
out.write(f'''/*
 * oylng_ucd.c — Unicode {unicodedata.unidata_version}'s general categories, for part 2's patterns
 *
 * Written by tools/gen_ucd.py; don't edit. Each run is the code point it
 * starts at, and its category in the top byte, numbered as in
 * oylng_yes.h's OYLNG__UC_ names.
 */

#include "oylng_yes.h"

const char oylng__ucd_version[] = "{unicodedata.unidata_version}";

static const uint32_t runs[{len(runs)}] = {{
''')
for i in range(0, len(runs), 6):
    out.write('    ' + ' '.join(f'0x{g:02x}{c:06x},' for c, g in runs[i:i + 6]) + '\n')
out.write(f'''}};

unsigned oylng__ucd_cat(uint32_t cp) {{
    size_t lo = 0, hi = {len(runs)};
    while (hi - lo > 1) {{
        size_t mid = lo + (hi - lo) / 2;
        if ((runs[mid] & 0xFFFFFFu) <= cp) lo = mid;
        else hi = mid;
    }}
    return runs[lo] >> 24;
}}
''')
