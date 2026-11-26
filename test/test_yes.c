/*
 * test_yes.c — runs part 2's suite (YAML Easy Schema)
 *
 *     test_yes DATA [-v]
 *
 * DATA is trans/yaml2's suite/yes written out one directory per test by
 * suite/bin/suite-to-data (suite/yes/README.md): in.yaml; typed.event, or
 * error with refused and at; and limit, cut, schema.yaml and root, holds,
 * leap-second, year-zero, keep and store where a test has them. Each test
 * is typed (oylng_parse_typed), by an application's schema where it has
 * one, and with a store, by the published files in it, which a lookup
 * gets by their hash (section 3.1); it must give typed.event, a !float compared by its bits; or be
 * refused, as refused says: the schema, or a document, whose path is
 * compared with at (a different one is counted, not failed: the suite's is
 * the first fault its checker meets). The limit applies to the stream, and
 * to an application's schema, which is read under it as a stream and then
 * held to it for its full size (section 3). A test with keep is read with
 * references kept, under the limit on held and typed (section 11), and
 * only checks that it is accepted or refused, as the suite's README says.
 * A test with bind binds each name in it (oylng_parser_bind, section 12)
 * to code that notes the name and makes no object; accepted, the names
 * noted in the stream must be calls, in order. A refused test's calls may
 * differ between parsers, so only its refusal is checked.
 *
 * Skipped, and counted: together, which part 2's suite has no test of
 * and this harness doesn't set; holds other than its
 * own (9 fraction digits, a second 60, the year 0000), and a test without
 * holds that it refuses for a time it can't hold; a limit of 0, which
 * the library takes as the default. Exits with 1 if any test fails.
 */

#define _POSIX_C_SOURCE 200809L
#include "oylng/oylng.h"
#include "oylng/schema.h"
#include "events.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passed, failed, verbose, paths;
static int skip_together, skip_holds, skip_zero, published, kept, binding;

static char *slurp(const char *dir, const char *name, size_t *len) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    strbuf b = { 0 };
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sb_add(&b, chunk, n);
    fclose(f);
    if (!b.data) sb_add(&b, "", 0);
    if (len) *len = b.len;
    return b.data;
}

static int has(const char *dir, const char *name) {
    char path[4096];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

/* A test's store: the file named by the hash, as an application's folder
 * of published schemas. The library reads it before the next lookup. */
typedef struct {
    char  dir[4096];
    char *last;
} store;

static bool store_lookup(void *ctx, oylng_str address, oylng_str sha256, oylng_str *file) {
    (void)address;
    store *st = ctx;
    free(st->last);
    char name[80];
    snprintf(name, sizeof name, "%.*s", (int)sha256.len, sha256.data);
    size_t n;
    st->last = slurp(st->dir, name, &n);
    if (!st->last) return false;
    *file = (oylng_str){ st->last, n };
    return true;
}

/* A name in bind: its code notes the name and makes no object, so the
 * typed events are what they'd be with nothing bound (section 12) */
typedef struct {
    char   *name;
    strbuf *calls;
} bound;

static const char *note(void *ctx, oylng_typed *t, oylng_value v, void **object) {
    (void)t;
    (void)v;
    bound *b = ctx;
    sb_str(b->calls, b->name);
    sb_str(b->calls, "\n");
    *object = NULL;
    return NULL;
}

/* A one-line file's line, without its break; NULL if there's none */
static char *line(const char *dir, const char *name) {
    char *s = slurp(dir, name, NULL);
    if (s) s[strcspn(s, "\n")] = '\0';
    return s;
}

static void ev(strbuf *b, const char *head, const char *s, size_t n) {
    sb_str(b, head);
    ev_esc(b, s, n);
    sb_str(b, "\n");
}

/* An untyped node, as part 1's events have it */
static void any_events(strbuf *b, const oylng_tree *t, oylng_node n) {
    oylng_kind k = oylng_node_kind(t, n);
    if (k == OYLNG_NODE_STRING) {
        oylng_str s = oylng_node_text(t, n);
        ev(b, "=VAL :", s.data, s.len);
        return;
    }
    bool map = k == OYLNG_NODE_MAPPING;
    sb_str(b, map ? "+MAP\n" : "+SEQ\n");
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++) {
        if (map) {
            any_events(b, t, oylng_node_key(t, n, i));
            any_events(b, t, oylng_node_value(t, n, i));
        } else any_events(b, t, oylng_node_item(t, n, i));
    }
    sb_str(b, map ? "-MAP\n" : "-SEQ\n");
}

/* A time's text, its fraction cut to the 9 digits held, as the cut
 * delivers it */
static void cut_time(strbuf *b, oylng_str s) {
    const char *dot = memchr(s.data, '.', s.len);
    if (!dot) { ev(b, "=DATETIME :", s.data, s.len); return; }
    size_t i = (size_t)(dot - s.data) + 1, e = i;
    while (e < s.len && s.data[e] >= '0' && s.data[e] <= '9') e++;
    if (e - i <= 9) { ev(b, "=DATETIME :", s.data, s.len); return; }
    sb_str(b, "=DATETIME :");
    ev_esc(b, s.data, i + 9);
    ev_esc(b, s.data + e, s.len - e);
    sb_str(b, "\n");
}

/* Times by their fields, not as written: for values written again */
static bool time_fields;

static void typed_events(strbuf *b, const oylng_typed *y, oylng_value v, bool cut) {
    char num[48];
    oylng_str s = oylng_value_text(y, v);
    size_t c = oylng_value_count(y, v);
    switch (oylng_value_type(y, v)) {
    case OYLNG_T_ANY: any_events(b, oylng_typed_tree(y), oylng_value_node(y, v)); break;
    case OYLNG_T_STR: ev(b, "=VAL :", s.data, s.len); break;
    case OYLNG_T_INT: {
        int64_t i;
        uint64_t u;
        if (oylng_value_int(y, v, &i)) snprintf(num, sizeof num, "%lld", (long long)i);
        else if (oylng_value_uint(y, v, &u)) snprintf(num, sizeof num, "%llu", (unsigned long long)u);
        else snprintf(num, sizeof num, "?");
        ev(b, "=INT :", num, strlen(num));
        break;
    }
    case OYLNG_T_BIGINT: ev(b, "=BIG :", s.data, s.len); break;
    case OYLNG_T_FLOAT:
        snprintf(num, sizeof num, "%.17g", oylng_value_float(y, v));
        ev(b, "=FLT :", num, strlen(num));
        break;
    case OYLNG_T_DECIMAL: ev(b, "=DEC :", s.data, s.len); break;
    case OYLNG_T_BOOL: sb_str(b, oylng_value_bool(y, v) ? "=BOOL :true\n" : "=BOOL :false\n"); break;
    case OYLNG_T_NULL: sb_str(b, "=NULL\n"); break;
    case OYLNG_T_DATE: ev(b, "=DATE :", s.data, s.len); break;
    case OYLNG_T_DATETIME:
        if (time_fields) {
            oylng_datetime d;
            oylng_value_datetime(y, v, &d);
            snprintf(num, sizeof num, "%d-%u-%u %u:%u:%u.%lu %d%s%s", (int)d.year, d.month, d.day, d.hour, d.minute,
                     d.second, (unsigned long)d.nanosecond, d.offset, d.zulu ? " Z" : "", d.unknown_offset ? " ?" : "");
            ev(b, "=DATETIME :", num, strlen(num));
        } else if (cut) cut_time(b, s);
        else ev(b, "=DATETIME :", s.data, s.len);
        break;
    case OYLNG_T_BINARY: {
        oylng_str by = oylng_value_bytes(y, v);
        sb_str(b, "=BIN :");
        for (size_t i = 0; i < by.len; i++) {
            snprintf(num, sizeof num, "%02x", (unsigned char)by.data[i]);
            sb_str(b, num);
        }
        sb_str(b, "\n");
        break;
    }
    case OYLNG_T_SEQUENCE:
        sb_str(b, "+SEQ\n");
        for (size_t i = 0; i < c; i++) typed_events(b, y, oylng_value_item(y, v, i), cut);
        sb_str(b, "-SEQ\n");
        break;
    case OYLNG_T_MAPPING:
        sb_str(b, "+MAP\n");
        for (size_t i = 0; i < c; i++) {
            oylng_str k = oylng_value_key(y, v, i);
            ev(b, "=VAL :", k.data, k.len);
            typed_events(b, y, oylng_value_at(y, v, i), cut);
        }
        sb_str(b, "-MAP\n");
        break;
    default: sb_str(b, "?\n");
    }
}

/* got against want, line by line, a =FLT by the bits of the binary64 its
 * text reads as */
static bool same_events(const char *got, const char *want) {
    for (;;) {
        size_t gl = strcspn(got, "\n"), wl = strcspn(want, "\n");
        if (gl > 6 && wl > 6 && !memcmp(got, "=FLT :", 6) && !memcmp(want, "=FLT :", 6)) {
            double g = strtod(got + 6, NULL), w = strtod(want + 6, NULL);
            uint64_t gb, wb;
            memcpy(&gb, &g, 8);
            memcpy(&wb, &w, 8);
            if (gb != wb) return false;
        } else if (gl != wl || memcmp(got, want, gl)) return false;
        got += gl;
        want += wl;
        if (!*got || !*want) return !*got && !*want;
        got++;
        want++;
    }
}

/* Section 13: typed document y written again, under the schema that typed
 * it, in block style and then in flow, and read back with that schema as
 * the application's: NULL if it gives the same values, else why not */
static int trips, trips_skipped;

/* s in double quotes, with JSON's escapes */
static void sb_json(strbuf *b, const char *s, size_t n) {
    sb_str(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char hex[8];
        if (c == '"' || c == '\\') {
            hex[0] = '\\';
            hex[1] = (char)c;
            sb_add(b, hex, 2);
        } else if (c < 0x20 || c == 0x7F) {
            snprintf(hex, sizeof hex, "\\u%04X", c);
            sb_str(b, hex);
        } else sb_add(b, s + i, 1);
    }
    sb_str(b, "\"");
}

/* A table's tree written as a schema's text: block style, a scalar plain
 * as it was, else in double quotes */
static void table_text(strbuf *b, const oylng_tree *t, oylng_node n, int ind) {
    oylng_kind k = oylng_node_kind(t, n);
    size_t c = oylng_node_count(t, n);
    for (size_t i = 0; i < c; i++) {
        for (int j = 0; j < ind; j++) sb_str(b, " ");
        oylng_node v = k == OYLNG_NODE_MAPPING ? oylng_node_value(t, n, i) : oylng_node_item(t, n, i);
        if (k == OYLNG_NODE_MAPPING) {
            oylng_node kn = oylng_node_key(t, n, i);
            oylng_str ks = oylng_node_text(t, kn);
            if (oylng_node_plain(t, kn)) sb_add(b, ks.data, ks.len);
            else sb_json(b, ks.data, ks.len);
            sb_str(b, ":");
        } else sb_str(b, "-");
        oylng_kind vk = oylng_node_kind(t, v);
        if (vk == OYLNG_NODE_STRING) {
            oylng_str vs = oylng_node_text(t, v);
            if (oylng_node_plain(t, v)) {
                if (vs.len) sb_str(b, " ");
                sb_add(b, vs.data, vs.len);
            } else {
                sb_str(b, " ");
                sb_json(b, vs.data, vs.len);
            }
            sb_str(b, "\n");
        } else if (!oylng_node_count(t, v)) sb_str(b, vk == OYLNG_NODE_MAPPING ? " {}\n" : " []\n");
        else {
            sb_str(b, "\n");
            table_text(b, t, v, ind + 2);
        }
    }
}

static const char *again(oylng_parser *rp, const oylng_typed *y, const oylng_schema *app, bool cut, uint64_t lim,
                         char *why, size_t whyn) {
    oylng_str name = oylng_tree_name(oylng_typed_tree(y));
    oylng_schema *own = NULL;
    const oylng_schema *sc = NULL;
    switch (oylng_typed_how(y)) {
    case OYLNG_TYPED_APP: sc = app; break;
    case OYLNG_TYPED_STAMP: {
        /* the stream's table as it stood for this document, as a schema */
        oylng_tree *tt;
        if (oylng_parser_table(rp, &tt) != OYLNG_OK || !tt) {
            snprintf(why, whyn, "its table can't be had");
            return why;
        }
        strbuf tx = { 0 };
        sb_str(&tx, "%YAML 2.0\n--- &\n");
        table_text(&tx, tt, oylng_tree_root(tt), 0);
        oylng_tree_free(tt);
        sc = own = oylng_schema_new(tx.data, tx.len, name, lim);
        if (oylng_schema_status(own) != OYLNG_OK) {
            snprintf(why, whyn, "its schema: %s\n%s", oylng_schema_error(own), tx.data);
            free(tx.data);
            oylng_schema_free(own);
            return why;
        }
        free(tx.data);
        break;
    }
    case OYLNG_TYPED_PUBLISHED: trips_skipped++; return NULL;
    default: break;
    }
    strbuf want = { 0 };
    time_fields = true;
    typed_events(&want, y, oylng_typed_root(y), cut);
    const char *res = NULL;
    for (int flow = 0; flow < 2 && !res; flow++) {
        oylng_emitter *e = oylng_emitter_new();
        oylng_emitter_set_flow(e, flow);
        oylng_emitter_set_schema(e, sc);
        oylng_status st = oylng_emit_stream_start(e);
        if (st == OYLNG_OK) st = oylng_emit_doc_start(e, name);
        if (st == OYLNG_OK) st = oylng_emit_value(e, y, oylng_typed_root(y));
        if (st == OYLNG_OK) st = oylng_emit_doc_end(e);
        if (st == OYLNG_OK) st = oylng_emit_stream_end(e);
        oylng_str out = oylng_emitter_output(e);
        const char *how = flow ? "in flow" : "in block";
        if (st != OYLNG_OK) {
            snprintf(why, whyn, "written %s: %s", how, oylng_emitter_error(e));
            res = why;
        } else {
            oylng_parser *p = oylng_parser_new(out.data, out.len);
            oylng_parser_set_limit(p, UINT64_MAX);
            oylng_parser_set_cut(p, cut);
            oylng_typed *back = NULL;
            st = oylng_parse_typed(p, sc, &back);
            strbuf got = { 0 };
            if (back) typed_events(&got, back, oylng_typed_root(back), cut);
            if (st != OYLNG_OK || !back) {
                snprintf(why, whyn, "written %s, it doesn't read: %s\n%.*s", how, st ? oylng_parser_error(p) : "no document",
                         (int)(out.len > 2000 ? 2000 : out.len), out.data);
                res = why;
            } else if (!same_events(got.data, want.data)) {
                snprintf(why, whyn, "written %s, it reads back otherwise:\n%.*s--- want\n%s--- got\n%s", how,
                         (int)(out.len > 2000 ? 2000 : out.len), out.data, want.data, got.data);
                res = why;
            }
            free(got.data);
            oylng_typed_free(back);
            oylng_parser_free(p);
        }
        oylng_emitter_free(e);
    }
    time_fields = false;
    free(want.data);
    oylng_schema_free(own);
    return res;
}

static void run(const char *dir) {
    if (has(dir, "together")) { skip_together++; return; }
    /* with keep, read with references kept, and only accepted or refused */
    bool keep = has(dir, "keep");
    char *holds = line(dir, "holds");
    bool ours = !holds || (!strcmp(holds, "9") && has(dir, "leap-second") && has(dir, "year-zero"));
    if (!ours) { skip_holds++; free(holds); return; }

    size_t len, slen = 0;
    char *in = slurp(dir, "in.yaml", &len);
    char *name = line(dir, "===");
    char *limit = line(dir, "limit");
    char *schema = slurp(dir, "schema.yaml", &slen);
    char *root = line(dir, "root");
    char *refused = line(dir, "refused");
    char *at = line(dir, "at");
    bool want_error = has(dir, "error"), cut = has(dir, "cut");
    char *want = want_error ? NULL : slurp(dir, "typed.event", NULL);
    char *bind = slurp(dir, "bind", NULL), *calls = slurp(dir, "calls", NULL);
    uint64_t lim = limit ? strtoull(limit, NULL, 10) : 0;
    bound names[64];
    size_t nnames = 0;
    strbuf noted = { 0 };
    sb_add(&noted, "", 0);
    for (char *s = bind; s && *s && nnames < 64;) {
        size_t n = strcspn(s, "\n");
        bool more = s[n] != '\0';
        s[n] = '\0';
        if (n) names[nnames++] = (bound){ s, &noted };
        s += n + more;
    }

    const char *err = NULL, *why = NULL;
    bool schema_err = false, typed_limit = false;
    char detail[8192] = "";
    oylng_mark m = { 0, 0, 0 };
    strbuf got = { 0 };
    oylng_parser *p = NULL;
    oylng_schema *app = NULL;
    store st = { .last = NULL };
    snprintf(st.dir, sizeof st.dir, "%s/store", dir);
    if (limit && !lim) { skip_zero++; goto done; }   /* the library's 0 is the default */
    if (schema) {
        app = oylng_schema_new(schema, slen, root ? (oylng_str){ root, strlen(root) } : (oylng_str){ NULL, 0 }, lim);
        if (oylng_schema_status(app) != OYLNG_OK) {
            err = oylng_schema_error(app);
            m = oylng_schema_error_mark(app);
            schema_err = true;   /* whatever refused it: its text, its size or its types */
        }
    }
    if (!err) {
        p = oylng_parser_new(in, len);
        if (lim) oylng_parser_set_limit(p, lim);
        oylng_parser_set_cut(p, cut);
        oylng_parser_set_keep(p, keep);
        if (keep) kept++;
        for (size_t i = 0; i < nnames; i++)
            oylng_parser_bind(p, (oylng_str){ names[i].name, strlen(names[i].name) }, note, NULL, &names[i]);
        if (nnames) binding++;
        if (has(dir, "store")) {
            oylng_parser_set_lookup(p, store_lookup, &st);
            published++;
        }
        sb_str(&got, "+STR\n");
        for (;;) {
            oylng_typed *y;
            oylng_status st = oylng_parse_typed(p, app, &y);
            if (st != OYLNG_OK) {
                err = oylng_parser_error(p);
                m = oylng_parser_error_mark(p);
                schema_err = st == OYLNG_ERR_SCHEMA;
                typed_limit = st == OYLNG_ERR_LIMIT;
                break;
            }
            if (!y) break;
            oylng_str dn = oylng_tree_name(oylng_typed_tree(y));
            if (dn.data) ev(&got, "+DOC !", dn.data, dn.len);
            else sb_str(&got, "+DOC\n");
            typed_events(&got, y, oylng_typed_root(y), cut);
            sb_str(&got, "-DOC\n");
            if (!want_error && !why && !keep) {   /* the emitter takes no kept tree */
                if (again(p, y, app, cut, lim, detail, sizeof detail)) why = "written again:";
                else trips++;
            }
            oylng_typed_free(y);
        }
        sb_str(&got, "-STR\n");
    }

    /* the engine's path, where it refused a document: its error up to ": ".
       Typed past the limit (section 11), a document is refused at a path too */
    bool doc_err = (schema_err || typed_limit) && err && err[0] == '/';
    size_t plen = doc_err ? strcspn(err, ":") : 0;
    if (err && !holds && strstr(err, "can't hold")) { skip_holds++; goto done; }
    if (want_error) {
        if (!err) why = "not refused";
        else if (refused && !strcmp(refused, "schema") && (!schema_err || doc_err)) why = "refused, not for the schema";
        else if (refused && !strcmp(refused, "document") && !doc_err) why = "refused, not for a document";
        else if (doc_err && at && (strlen(at) != plen || memcmp(at, err, plen))) {
            paths++;
            if (verbose) printf("PATH %s (%s): the suite's %s, the engine's %s\n", dir, name ? name : "?", at, err);
        }
        if (why && err) snprintf(detail, sizeof detail, "%zu:%zu: %s", m.line, m.col, err);
    } else if (why) {
        /* written again, it didn't read back: detail says how */
    } else if (err) {
        why = "refused";
        snprintf(detail, sizeof detail, "%zu:%zu: %s", m.line, m.col, err);
    } else if (!keep && (!want || !same_events(got.data, want))) why = "wrong events";
    else if (nnames && strcmp(noted.data, calls ? calls : "")) {
        /* a refused test's calls may differ between parsers: only an accepted one's are checked */
        why = "wrong calls";
        snprintf(detail, sizeof detail, "\n--- want\n%s--- got\n%s", calls ? calls : "", noted.data);
    }

    if (why) {
        failed++;
        printf("FAIL %s (%s): %s %s\n", dir, name ? name : "?", why, detail);
        if (verbose && !want_error)
            printf("--- in.yaml\n%s\n--- want\n%s--- got\n%s", in, want ? want : "(none)\n", got.data ? got.data : "");
    } else {
        passed++;
        if (verbose > 1) printf("ok   %s (%s)%s%s\n", dir, name ? name : "?", err ? ": " : "", err ? err : "");
    }
done:
    oylng_parser_free(p);
    oylng_schema_free(app);
    free(st.last);
    free(got.data);
    free(in);
    free(name);
    free(limit);
    free(schema);
    free(root);
    free(refused);
    free(at);
    free(want);
    free(holds);
    free(bind);
    free(calls);
    free(noted.data);
}

static int cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every directory under dir holding an in.yaml, in order */
static void walk(const char *dir) {
    if (has(dir, "in.yaml")) { run(dir); return; }
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) names = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof *names);
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp);
    for (size_t i = 0; i < n; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) walk(path);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv) {
    const char *data = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "-vv")) verbose += 2;
        else data = argv[i];
    }
    if (!data) { fprintf(stderr, "usage: test_yes DATA [-v]\n"); return 2; }
    walk(data);
    printf("yes: %d passed, %d failed (%d run with a store, %d with references kept, %d with names bound);"
           " skipped %d with together, %d holding more, %d with a limit of 0; %d refused at another path\n",
           passed, failed, published, kept, binding, skip_together, skip_holds, skip_zero, paths);
    printf("yes: %d typed documents written again read back the same (%d with published names not tried)\n", trips,
           trips_skipped);
    return failed ? 1 : 0;
}
