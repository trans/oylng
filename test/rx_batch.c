/*
 * rx_batch.c — part 2's patterns, for test/rxdiff.py
 *
 * Reads records from standard input, each "PLEN SLEN\n", the pattern's
 * PLEN bytes, then SLEN bytes of text, and writes a line for each: "bad
 * WHY" if the pattern isn't I-Regexp, else its count and, if it is within
 * the ceiling, 1 or 0 for whether the text matches it whole.
 */

#include "oylng_yes.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    oylng__rxscr scr = { 0 };
    size_t pl, sl;
    while (scanf("%zu %zu", &pl, &sl) == 2) {
        if (getchar() != '\n') return 2;
        char *buf = malloc(pl + sl + 1);
        if (!buf || fread(buf, 1, pl + sl, stdin) != pl + sl) return 2;
        oylng_arena *A = oylng_arena_new(4096);
        oylng__rx *rx;
        uint64_t count;
        const char *why = NULL;
        int r = oylng__rx_compile(A, buf, pl, 1024, &rx, &count, &why);
        if (r < 0) printf("bad %s\n", why);
        else if (!rx) printf("%llu -\n", (unsigned long long)count);
        else printf("%llu %d\n", (unsigned long long)count, oylng__rx_match(rx, buf + pl, sl, &scr));
        oylng_arena_free(A);
        free(buf);
    }
    oylng__rxscr_free(&scr);
    return 0;
}
