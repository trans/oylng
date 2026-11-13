/*
 * oylng_number.c — scalars as part 2 reads them: numbers, dates and base64
 *
 * Section numbers are those of spec/yaml-easy-schema.md at trans/yaml2.
 * Everything here reads text a schema typed, in time linear in its length:
 * a number is compared with a limit on its digits as written, never
 * converted to a bigger type (section 4.4).
 */

#include "oylng_yes.h"
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline bool digit(char c) { return c >= '0' && c <= '9'; }

/* ── Numbers as written ──────────────────────────────────── */

bool oylng__num_read(const char *s, size_t n, oylng__num *out) {
    size_t i = 0;
    *out = (oylng__num){ 0 };
    if (i < n && (s[i] == '-' || s[i] == '+')) out->neg = s[i++] == '-';
    size_t i0 = i;
    while (i < n && digit(s[i])) i++;
    size_t iend = i, f0 = i, fend = i;
    if (i < n && s[i] == '.') {
        f0 = ++i;
        while (i < n && digit(s[i])) i++;
        fend = i;
    }
    if (iend == i0 && fend == f0) return false;   /* no digits */
    const char *e = NULL;
    size_t ne = 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) out->eneg = s[i++] == '-';
        size_t e0 = i;
        while (i < n && digit(s[i])) i++;
        if (i == e0) return false;
        e = s + e0;
        ne = i - e0;
        while (ne && *e == '0') { e++; ne--; }
    }
    if (i != n) return false;
    if (!ne) out->eneg = false;
    out->exp = e;
    out->ne = ne;
    /* the digits, in two runs with the point between: the first
       significant one, and how many there are without the zeros after */
    size_t lead = 0;
    const char *first = NULL;
    for (size_t j = i0; j < iend && !first; j++) {
        if (s[j] != '0') first = s + j;
        else lead++;
    }
    int64_t intdigits = (int64_t)(iend - i0);
    if (!first) {
        for (size_t j = f0; j < fend && !first; j++) {
            if (s[j] != '0') first = s + j;
            else lead++;
        }
    }
    if (!first) { out->neg = false; return true; }   /* zero */
    /* the value is 0.D times 10^(E + k), D starting at first */
    out->k = intdigits - (int64_t)lead;
    out->dig = first;
    /* the last significant digit */
    const char *last = (fend > f0) ? s + fend - 1 : s + iend - 1;
    while (last > first && (*last == '0' || *last == '.')) last--;
    out->dig_end = last + 1;
    out->nd = 0;
    for (const char *q = first; q < out->dig_end; q++) out->nd += *q != '.';
    return true;
}

/* The next of a's significant digits at *q, -1 past them */
static int dig_at(const oylng__num *a, const char **q) {
    while (*q < a->dig_end && **q == '.') (*q)++;
    return *q < a->dig_end ? *(*q)++ - '0' : -1;
}

#define SMALL_EXP 18
#define HUGE_DIFF ((int64_t)1 << 56)

/* The written exponent as an int64, if it has at most 18 digits */
static bool exp_small(const oylng__num *a, int64_t *v) {
    if (a->ne > SMALL_EXP) return false;
    int64_t x = 0;
    for (size_t i = 0; i < a->ne; i++) x = x * 10 + (a->exp[i] - '0');
    *v = a->eneg ? -x : x;
    return true;
}

/* The sign of (Ea + ka) - (Eb + kb), the adjusted exponents, exactly */
static int adj_cmp(const oylng__num *a, const oylng__num *b) {
    int64_t ea, eb;
    if (exp_small(a, &ea) && exp_small(b, &eb)) {
        int64_t x = ea + a->k, y = eb + b->k;
        return (x > y) - (x < y);
    }
    int64_t d = b->k - a->k;   /* compare Ea - Eb with d */
    bool apos = !a->eneg, bpos = !b->eneg;
    if (apos != bpos) return apos ? 1 : -1;   /* one is at least 10^18 */
    /* |Ea| - |Eb|, from the most significant digit, until it is too big
       for d to matter */
    size_t L = a->ne > b->ne ? a->ne : b->ne;
    int64_t v = 0;
    for (size_t i = 0; i < L; i++) {
        int da = i + a->ne >= L ? a->exp[i + a->ne - L] - '0' : 0;
        int db = i + b->ne >= L ? b->exp[i + b->ne - L] - '0' : 0;
        v = v * 10 + (da - db);
        if (v > HUGE_DIFF || v < -HUGE_DIFF) return (v > 0) == apos ? 1 : -1;
    }
    if (!apos) v = -v;
    return (v > d) - (v < d);
}

int oylng__num_cmp(const oylng__num *a, const oylng__num *b) {
    int sa = a->nd ? (a->neg ? -1 : 1) : 0, sb = b->nd ? (b->neg ? -1 : 1) : 0;
    if (sa != sb) return (sa > sb) - (sa < sb);
    if (!sa) return 0;
    int c = adj_cmp(a, b);
    if (!c) {
        const char *p = a->dig, *q = b->dig;
        for (;;) {
            int x = dig_at(a, &p), y = dig_at(b, &q);
            if (x < 0 || y < 0) { c = (x >= 0) - (y >= 0); break; }
            if (x != y) { c = x > y ? 1 : -1; break; }
        }
    }
    return sa * c;
}

bool oylng__num_fits(const oylng__num *a, uint64_t prec, uint64_t scale) {
    if (!a->nd) return true;
    int64_t e;
    if (!exp_small(a, &e)) return a->eneg ? scale >= OYLNG__SATURATED : prec >= OYLNG__SATURATED;
    int64_t adj = e + a->k;   /* digits before the point, if positive */
    int64_t frac = (int64_t)a->nd - adj;
    if (frac > 0 && (uint64_t)frac > scale) return false;
    return adj <= 0 || (uint64_t)adj <= prec - scale;
}

void oylng__num_of_count(uint64_t n, char *buf, oylng__num *out) {
    size_t len = 0;
    char t[24];
    do { t[len++] = (char)('0' + n % 10); n /= 10; } while (n);
    for (size_t i = 0; i < len; i++) buf[i] = t[len - 1 - i];
    oylng__num_read(buf, len, out);
}

/* ── !int ────────────────────────────────────────────────── */

bool oylng__int(const char *s, size_t n, bool *neg, uint64_t *mag) {
    *neg = n && s[0] == '-';
    size_t j = *neg;
    if (n - j > 20) return false;
    uint64_t v = 0;
    for (; j < n; j++) {
        unsigned d = (unsigned)(s[j] - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    if (*neg && v > (uint64_t)INT64_MAX + 1) return false;
    if (!v) *neg = false;   /* -0 is 0 */
    *mag = v;
    return true;
}

/* ── !float ──────────────────────────────────────────────────
 * From Oyl's float_value (oyl_schema.c): strtod on a copy, a long literal
 * as .DDDe[-]N with its first 768 significant digits and a digit standing
 * for the rest, in the locale's terms. glibc's, musl's and macOS's strtod
 * round correctly, and MSVC's does on 768 digits. */

#define FLOAT_DIGITS 768

static double locale_strtod(char *buf, size_t n, size_t point) {
    char *end;
    errno = 0;
    double f = strtod(buf, &end);
    if (*end && point < n) {   /* it stopped short: the locale's decimal point isn't '.' */
        const char *dp = localeconv()->decimal_point;
        size_t dl = strlen(dp);
        char loc[FLOAT_DIGITS + 64];
        if (dl < 16 && n + dl < sizeof loc) {
            memcpy(loc, buf, point);
            memcpy(loc + point, dp, dl);
            memcpy(loc + point + dl, buf + point + 1, n - point);   /* and the NUL */
            f = strtod(loc, NULL);
        }
    }
    return f;
}

static double exp_strtod(char *buf, size_t n, size_t point, long long exp10) {
    buf[n++] = 'e';
    if (exp10 < 0) { buf[n++] = '-'; exp10 = -exp10; }
    char digits[24];
    int nd = 0;
    do { digits[nd++] = (char)('0' + exp10 % 10); exp10 /= 10; } while (exp10);
    while (nd) buf[n++] = digits[--nd];
    buf[n] = '\0';
    return locale_strtod(buf, n, point);
}

bool oylng__float(const char *s, size_t len, double *f) {
    char buf[FLOAT_DIGITS + 32];
    size_t i = 0, n = 0;
    if (len < 64) {   /* most */
        size_t point = SIZE_MAX;
        for (; i < len; i++) {
            if (s[i] == '.') point = n;
            buf[n++] = s[i];
        }
        buf[n] = '\0';
        *f = locale_strtod(buf, n, point);
        return !isinf(*f);
    }
    if (s[i] == '+' || s[i] == '-') buf[n++] = s[i++];
    buf[n++] = '.';
    size_t point = n - 1, nsig = 0;
    long long place = 0;   /* the point's place, from just before the first significant digit */
    bool fraction = false, sticky = false;
    for (; i < len && s[i] != 'e' && s[i] != 'E'; i++) {
        char ch = s[i];
        if (ch == '.') { fraction = true; continue; }
        if (!nsig && !sticky && ch == '0') {   /* a leading zero */
            if (fraction) place--;
            continue;
        }
        if (!fraction) place++;
        if (nsig < FLOAT_DIGITS) buf[n + nsig++] = ch;
        else if (ch != '0') sticky = true;
    }
    if (!nsig) {   /* zero, keeping its sign */
        *f = n == 2 && buf[0] == '-' ? -0.0 : 0.0;
        return true;
    }
    n += nsig;
    long long e = 0;
    bool eneg = false;
    for (i += i < len; i < len; i++) {   /* the exponent, past its e */
        if (s[i] == '-') eneg = true;
        else if (digit(s[i])) e = e < 1000000000000000LL ? e * 10 + (s[i] - '0') : e;
    }
    long long exp10 = place + (eneg ? -e : e);   /* the value is .DDD times 10^exp10 */
    if (exp10 > 100000000) exp10 = 100000000;
    if (exp10 < -100000000) exp10 = -100000000;
    *f = exp_strtod(buf, n, point, exp10);
    if (sticky) {
        /* The literal lies strictly between the copy, t, and u, one more
         * in t's last digit. No halfway point between doubles lies
         * strictly between them, so they round alike, or one is a halfway
         * point: if t is, it rounds to the even double and the literal
         * up, to u's; if u is, the literal rounds down, to t's, the odd
         * one. So where they differ, it's u's if t's is even. */
        size_t k = n;
        while (k > point + 1 && buf[k - 1] == '9') buf[--k] = '0';
        if (k > point + 1) buf[k - 1]++;
        else { buf[point + 1] = '1'; exp10++; }   /* .999... up to .1000... */
        double u = exp_strtod(buf, n, point, exp10);
        uint64_t bits;
        memcpy(&bits, f, sizeof bits);
        if (u != *f && !(bits & 1)) *f = u;
    }
    return !isinf(*f);
}

/* ── Dates (section 4.8) ─────────────────────────────────── */

static bool leapyear(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int lastday(long y, int m) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leapyear(y) ? 29 : days[m - 1];
}

static bool num2(const char *s, int *v) {
    if (!digit(s[0]) || !digit(s[1])) return false;
    *v = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

/* Days from 1970-01-01, proleptic Gregorian */
static long days_from_civil(long y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

int oylng__date(const char *s, size_t n, bool full, bool cut, oylng_datetime *out) {
    memset(out, 0, sizeof *out);
    int y1, y2, mo, d;
    if (n < 10 || !num2(s, &y1) || !num2(s + 2, &y2) || s[4] != '-' || !num2(s + 5, &mo) || s[7] != '-' ||
        !num2(s + 8, &d))
        return 0;
    long y = y1 * 100 + y2;
    if (mo < 1 || mo > 12 || d < 1 || d > lastday(y, mo)) return 0;
    out->year = (int32_t)y;
    out->month = (uint8_t)mo;
    out->day = (uint8_t)d;
    if (!full) return n == 10;
    int hh, mm, ss;
    if (n < 20 || (s[10] != 'T' && s[10] != 't') || !num2(s + 11, &hh) || s[13] != ':' || !num2(s + 14, &mm) ||
        s[16] != ':' || !num2(s + 17, &ss))
        return 0;
    size_t i = 19, frac = 0;
    uint32_t ns = 0;
    if (s[i] == '.') {
        size_t f0 = ++i;
        while (i < n && digit(s[i])) {
            if (i - f0 < 9) ns = ns * 10 + (uint32_t)(s[i] - '0');
            i++;
        }
        frac = i - f0;
        if (!frac) return 0;
        for (size_t k = frac; k < 9; k++) ns *= 10;
    }
    int oh = 0, om = 0, sign = 1;
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
        i++;
        out->zulu = true;
    } else if (i + 6 == n && (s[i] == '+' || s[i] == '-') && num2(s + i + 1, &oh) && s[i + 3] == ':' &&
               num2(s + i + 4, &om)) {
        sign = s[i] == '-' ? -1 : 1;
        out->unknown_offset = sign < 0 && !oh && !om;
        i += 6;
    } else return 0;
    if (i != n || hh > 23 || mm > 59 || ss > 60 || oh > 23 || om > 59) return 0;
    if (ss == 60) {
        /* at 23:59 UTC on the last day of a month, once the offset is off */
        long utc = days_from_civil(y, mo, d) * 1440 + hh * 60 + mm - sign * (oh * 60 + om);
        long z = utc >= 0 ? utc / 1440 : -((-utc + 1439) / 1440);
        long rest = utc - z * 1440, uy;
        int um, ud;
        civil_from_days(z, &uy, &um, &ud);
        if (rest != 23 * 60 + 59 || ud != lastday(uy, um)) return 0;
    }
    out->hour = (uint8_t)hh;
    out->minute = (uint8_t)mm;
    out->second = (uint8_t)ss;
    out->nanosecond = ns;
    out->offset = (int16_t)(sign * (oh * 60 + om));
    /* this parser holds 9 fraction digits, a second 60 and year 0000 */
    if (frac > 9 && !cut) return -1;
    return 1;
}

/* ── !binary ─────────────────────────────────────────────────
 * RFC 4648 section 4, padded, its pad bits zero, line feeds skipped */

static int b64(unsigned c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool oylng__base64(const char *s, size_t n, unsigned char *out, size_t *outlen) {
    uint32_t acc = 0;
    size_t q = 0, o = 0, pad = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        if (c == '\n') continue;
        int v;
        if (c == '=') {
            if (q < 2) return false;
            pad++;
            v = 0;
        } else {
            if (pad || (v = b64(c)) < 0) return false;
        }
        acc = acc << 6 | (uint32_t)v;
        if (++q == 4) {
            if (pad == 2 && (acc & 0xFFFF)) return false;
            if (pad == 1 && (acc & 0xFF)) return false;
            if (out) {
                out[o] = (unsigned char)(acc >> 16);
                if (pad < 2) out[o + 1] = (unsigned char)(acc >> 8);
                if (pad < 1) out[o + 2] = (unsigned char)acc;
            }
            o += 3 - pad;
            q = 0;
            acc = 0;
            if (pad) {
                /* only line feeds may follow */
                for (i++; i < n; i++)
                    if (s[i] != '\n') return false;
                break;
            }
        }
    }
    if (q) return false;
    *outlen = o;
    return true;
}
