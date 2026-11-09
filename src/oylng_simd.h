/*
 * oylng_simd.h — scans for the next byte that needs a look (from Oyl's
 * oyl_simd.h and oyl_port.h)
 *
 * Each finds where a run of bytes the parser can pass over without a
 * second look ends. They are SSE2, which every x86-64 has, so they need no
 * check of the CPU; elsewhere, or built with OYLNG_NO_SIMD (which the
 * tests do, so this path stays tested), they are plain loops. Each may
 * read up to 15 bytes past the run it finds, but never past len.
 */

#ifndef OYLNG_SIMD_H
#define OYLNG_SIMD_H

#include <stddef.h>
#include <stdint.h>

#if !defined(OYLNG_NO_SIMD) && \
    (defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC)) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#  define OYLNG_SSE2 1
#  include <emmintrin.h>
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
static __forceinline unsigned oylng_ctz32(uint32_t x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return (unsigned)i;
}
#else
static inline unsigned oylng_ctz32(uint32_t x) { return (unsigned)__builtin_ctz(x); }
#endif

/* Where a run of printable ASCII but a and b ends in buf[0..len): the
 * first byte that is a or b, below 0x20 but a tab (line breaks among
 * them), or 0x7F and up; or len. What stops it is text a quoted scalar,
 * a comment or a block scalar's line has to look at: its end, an escape,
 * a character to check. */
static inline size_t oylng_ascii_run(const char *buf, size_t len, char a, char b) {
    size_t i = 0;
#if defined(OYLNG_SSE2)
    const __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b);
    const __m128i c1f = _mm_set1_epi8(0x1F), tab = _mm_set1_epi8('\t'), c7f = _mm_set1_epi8(0x7F);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i low = _mm_andnot_si128(_mm_cmpeq_epi8(v, tab), _mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v));
        __m128i high = _mm_cmpeq_epi8(_mm_max_epu8(v, c7f), v);
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb)),
                                 _mm_or_si128(low, high));
        int mask = _mm_movemask_epi8(m);
        if (mask) return i + oylng_ctz32((uint32_t)mask);
    }
#endif
    for (; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == (unsigned char)a || c == (unsigned char)b || (c < 0x20 && c != '\t') || c >= 0x7F) return i;
    }
    return len;
}

/* The first line feed or carriage return in buf[0..len), or len */
static inline size_t oylng_find_break(const char *buf, size_t len) {
    size_t i = 0;
#if defined(OYLNG_SSE2)
    const __m128i nl = _mm_set1_epi8('\n'), cr = _mm_set1_epi8('\r');
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        int mask = _mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v, nl), _mm_cmpeq_epi8(v, cr)));
        if (mask) return i + oylng_ctz32((uint32_t)mask);
    }
#endif
    for (; i < len; i++)
        if (buf[i] == '\n' || buf[i] == '\r') return i;
    return len;
}

#endif /* OYLNG_SIMD_H */
