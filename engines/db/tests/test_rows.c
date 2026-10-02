/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: tables on the tree (step 4).
 * Keys of every type must sort as their values do, rows must come back as
 * they went in, and scans by key prefix must follow the key order. Checked
 * against a plain model with random runs, plus the errors a caller can meet.
 *   test_rows          the full run
 *   test_rows quick    fewer seeds */
#include "dbtest.h"
#include <math.h>

/* ---- key encodings sort as their values ---- */
static int t_encodings(void) {
    static const int64_t ints[] = { INT64_MIN, INT64_MIN + 1, -4294967296LL, -65537, -65536, -257, -256, -255, -2, -1,
                                    0, 1, 2, 255, 256, 65535, 65536, 1700000000LL, 4294967296LL, INT64_MAX - 1, INT64_MAX };
    static const double reals[] = { -1e300, -1.5, -1.0, -1e-300, 0.0, 1e-300, 0.5, 1.0, 2.0, 1e300 };
    uint8_t a[16], b[16];
    uint32_t an, bn, i;
    for (i = 0; i + 1 < sizeof ints / sizeof ints[0]; i++) {
        int64_t back = 0;
        an = asd_enc_int(a, ints[i]);
        bn = asd_enc_int(b, ints[i + 1]);
        CHECK(asd_cmp(a, an, b, bn) < 0, "int key %lld should sort below %lld", (long long)ints[i], (long long)ints[i + 1]);
        CHECK(asd_dec_int(a, an, &back) == an && back == ints[i], "int key %lld does not decode", (long long)ints[i]);
    }
    CHECK(asd_enc_int(a, 0) == 1 && asd_enc_int(a, 255) == 2 && asd_enc_int(a, 1700000000LL) == 5 && asd_enc_int(a, INT64_MAX) == 9,
          "int keys take 1, 2, 5 and 9 bytes for 0, 255, a Unix time and the largest");
    for (i = 0; i + 1 < sizeof reals / sizeof reals[0]; i++) {
        an = asd_enc_real(a, reals[i], 0);
        bn = asd_enc_real(b, reals[i + 1], 0);
        CHECK(asd_cmp(a, an, b, bn) < 0, "real key %g should sort below %g", reals[i], reals[i + 1]);
        an = asd_enc_real(a, reals[i], 1);
        bn = asd_enc_real(b, reals[i + 1], 1);
        CHECK(asd_cmp(a, an, b, bn) <= 0, "float key %g should not sort above %g", reals[i], reals[i + 1]);
    }
    an = asd_enc_real(a, -0.0, 0);
    bn = asd_enc_real(b, 0.0, 0);
    CHECK(an == bn && !memcmp(a, b, an), "-0 is stored as 0");
    {   /* text: zeros inside, prefixes first */
        uint8_t x[64], y[64];
        uint32_t xn = asd_enc_text(x, "ab", 2), yn = asd_enc_text(y, "ab\0", 3);
        CHECK(asd_cmp(x, xn, y, yn) < 0, "\"ab\" sorts below \"ab\\0\"");
        xn = asd_enc_text(x, "ab\0", 3); yn = asd_enc_text(y, "ab\001", 3);
        CHECK(asd_cmp(x, xn, y, yn) < 0, "\"ab\\0\" sorts below \"ab\\1\"");
        xn = asd_enc_text(x, "a", 1); yn = asd_enc_text(y, "a\0\0", 3);
        CHECK(asd_cmp(x, xn, y, yn) < 0, "\"a\" sorts below \"a\\0\\0\"");
    }
    return 0;
}

/* ---- the model: rows of (machine, time, temp, note) keyed by (machine, time) ---- */
typedef struct row { int64_t m, t; double temp; int nn; char note[40]; } row;
static row *g_rows;
static uint32_t g_nrows;
static int rcmp(int64_t m1, int64_t t1, int64_t m2, int64_t t2) {
    if (m1 != m2) return m1 < m2 ? -1 : 1;
    if (t1 != t2) return t1 < t2 ? -1 : 1;
    return 0;
}
static uint32_t rfind(int64_t m, int64_t t, int *found) {
    uint32_t lo = 0, hi = g_nrows;
    *found = 0;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = rcmp(g_rows[mid].m, g_rows[mid].t, m, t);
        if (c < 0) lo = mid + 1; else { hi = mid; if (!c) *found = 1; }
    }
    return lo;
}

static int check_row(const altsql_value *v, const row *r) {
    CHECK(v[0].type == ALTSQL_INTEGER && v[0].u.i == r->m, "machine %lld, want %lld", (long long)v[0].u.i, (long long)r->m);
    CHECK(v[1].type == ALTSQL_INTEGER && v[1].u.i == r->t, "time differs");
    CHECK(v[2].type == ALTSQL_REAL && (float)v[2].u.r == (float)r->temp, "temp %g, want %g", v[2].u.r, (double)(float)r->temp);
    CHECK(v[3].type == ALTSQL_TEXT && v[3].len == r->nn && !memcmp(v[3].u.s, r->note, (size_t)r->nn), "note differs");
    return 0;
}

static int verify_rows(altsql_db *db, int64_t only_m, int use_prefix) {
    altsql_db_cursor c;
    altsql_value pre, v[8];
    uint32_t i;
    int rc;
    pre.type = ALTSQL_INTEGER; pre.len = 0; pre.u.i = only_m;
    rc = altsql_db_row_seek(&c, db, "readings", &pre, use_prefix ? 1 : 0);
    for (i = 0; i < g_nrows; i++) {
        if (use_prefix && g_rows[i].m != only_m) continue;
        CHECK(rc == ALTSQL_OK, "row scan ended early (%d %s)", rc, altsql_db_errmsg(db));
        CHECK(altsql_db_row_read(&c, v, 8) == ALTSQL_OK, "row_read: %s", altsql_db_errmsg(db));
        if (check_row(v, &g_rows[i])) return 1;
        rc = altsql_db_next(&c);
    }
    CHECK(rc == ALTSQL_NOTFOUND, "row scan went past the model (%d)", rc);
    if (use_prefix) {                                   /* and backwards from the end of the prefix */
        uint32_t j = g_nrows;
        int first = 1;
        while (j-- > 0) {
            if (g_rows[j].m != only_m) continue;
            rc = first ? altsql_db_prev(&c) : altsql_db_prev(&c);
            first = 0;
            CHECK(rc == ALTSQL_OK, "reverse row scan ended early (%d)", rc);
            CHECK(altsql_db_row_read(&c, v, 8) == ALTSQL_OK, "row_read");
            if (check_row(v, &g_rows[j])) return 1;
        }
        CHECK(altsql_db_prev(&c) == ALTSQL_NOTFOUND, "reverse row scan went past the prefix");
    }
    return 0;
}

static int t_model(uint32_t seed, uint32_t ps, uint32_t steps) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(1 << 20);
    uint32_t s = seed * 2654435761u + 99, step;
    int rc, intx = 0;
    row *saved = 0;
    uint32_t nsaved = 0;
    g_nrows = 0;
    g_rows = (row *)realloc(g_rows, 20000 * sizeof(row));
    ram_new(&rf, 1u << 26, 1u << 26);
    CHECK(db_open_ram(&db, &rf, mem, 1 << 20, ps) == ALTSQL_OK, "open");
    rc = altsql_db_table_create(db, "readings", "machine:int, time:time, temp:float, note:text", "machine,time");
    CHECK(rc == ALTSQL_OK, "create: %d %s", rc, altsql_db_errmsg(db));
    for (step = 0; step < steps; step++) {
        uint32_t r = xs(&s) % 100, ix;
        int64_t m = (int64_t)(xs(&s) % 11) - 5, t;
        int f;
        uint32_t tr = xs(&s) % 4;
        t = tr == 0 ? (int64_t)(xs(&s) % 50) - 25 : tr == 1 ? 1700000000LL + (int64_t)(xs(&s) % 1000)
          : tr == 2 ? -(int64_t)(((uint64_t)xs(&s) << 31) | xs(&s)) : (int64_t)(((uint64_t)xs(&s) << 31) | xs(&s));
        if (r < 45) {                                   /* put */
            altsql_value v[4];
            row nr;
            uint32_t i;
            nr.m = m; nr.t = t; nr.temp = (double)(int32_t)xs(&s) / 1000.0;
            nr.nn = (int)(xs(&s) % 40);
            for (i = 0; i < (uint32_t)nr.nn; i++) nr.note[i] = (char)(xs(&s) % 3);   /* zeros included */
            v[0].type = ALTSQL_INTEGER; v[0].u.i = m; v[0].len = 0;
            v[1].type = xs(&s) % 4 ? ALTSQL_INTEGER : ALTSQL_REAL;                     /* a real into a time column */
            if (v[1].type == ALTSQL_INTEGER) v[1].u.i = t; else { v[1].u.r = (double)(t % 1000000); nr.t = t % 1000000; t = nr.t; }
            v[1].len = 0;
            v[2].type = xs(&s) % 3 ? ALTSQL_REAL : ALTSQL_INTEGER;                     /* whole numbers widen */
            if (v[2].type == ALTSQL_REAL) v[2].u.r = nr.temp; else { v[2].u.i = (int64_t)nr.temp; nr.temp = (double)(int64_t)nr.temp; }
            v[2].len = 0;
            v[3].type = ALTSQL_TEXT; v[3].u.s = nr.note; v[3].len = nr.nn;
            rc = altsql_db_row_put(db, "readings", v, 4);
            CHECK(rc == ALTSQL_OK, "seed %u step %u: row_put %d %s", seed, step, rc, altsql_db_errmsg(db));
            ix = rfind(nr.m, nr.t, &f);
            if (!f) { memmove(&g_rows[ix + 1], &g_rows[ix], (g_nrows - ix) * sizeof(row)); g_nrows++; }
            g_rows[ix] = nr;
        } else if (r < 60) {                            /* delete */
            altsql_value k[2];
            if (g_nrows && xs(&s) % 2) { ix = xs(&s) % g_nrows; m = g_rows[ix].m; t = g_rows[ix].t; }
            k[0].type = ALTSQL_INTEGER; k[0].u.i = m; k[0].len = 0;
            k[1].type = ALTSQL_INTEGER; k[1].u.i = t; k[1].len = 0;
            ix = rfind(m, t, &f);
            rc = altsql_db_row_del(db, "readings", k, 2);
            CHECK(rc == (f ? ALTSQL_OK : ALTSQL_NOTFOUND), "seed %u step %u: row_del %d, model %d", seed, step, rc, f);
            if (f) { memmove(&g_rows[ix], &g_rows[ix + 1], (g_nrows - ix - 1) * sizeof(row)); g_nrows--; }
        } else if (r < 85) {                            /* get */
            altsql_value k[2], v[8];
            if (g_nrows && xs(&s) % 3) { ix = xs(&s) % g_nrows; m = g_rows[ix].m; t = g_rows[ix].t; }
            k[0].type = ALTSQL_INTEGER; k[0].u.i = m; k[0].len = 0;
            k[1].type = ALTSQL_INTEGER; k[1].u.i = t; k[1].len = 0;
            ix = rfind(m, t, &f);
            rc = altsql_db_row_get(db, "readings", k, 2, v, 8);
            CHECK(rc == (f ? ALTSQL_OK : ALTSQL_NOTFOUND), "seed %u step %u: row_get %d, model %d (%s)", seed, step, rc, f, altsql_db_errmsg(db));
            if (f && check_row(v, &g_rows[ix])) return 1;
        } else if (r < 90) {                            /* scans: all rows, and one machine both ways */
            if (verify_rows(db, 0, 0) || verify_rows(db, m, 1)) { printf("  seed %u step %u\n", seed, step); return 1; }
        } else if (r < 94) {                            /* transactions */
            if (!intx) {
                CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
                saved = (row *)realloc(saved, (g_nrows + 1) * sizeof(row));
                memcpy(saved, g_rows, g_nrows * sizeof(row));
                nsaved = g_nrows;
                intx = 1;
            } else if (xs(&s) % 3) {
                CHECK(altsql_db_commit(db) == ALTSQL_OK, "commit: %s", altsql_db_errmsg(db));
                intx = 0;
            } else {
                CHECK(altsql_db_rollback(db) == ALTSQL_OK, "rollback");
                memcpy(g_rows, saved, nsaved * sizeof(row));
                g_nrows = nsaved;
                intx = 0;
            }
        } else if (!intx) {                             /* reopen and check every page */
            altsql_db_close(db);
            CHECK(db_open_ram(&db, &rf, mem, 1 << 20, ps) == ALTSQL_OK, "reopen");
            if (check_slots(db, 1)) return 1;
        }
        CHECK(pins_clear(db), "pins left");
    }
    if (intx) CHECK(altsql_db_commit(db) == ALTSQL_OK, "final commit");
    if (verify_rows(db, 0, 0) || check_slots(db, 1)) return 1;
    altsql_db_close(db);
    ram_free(&rf);
    free(mem);
    free(saved);
    return 0;
}

/* ---- every key type, in key order ---- */
static int t_keytypes(void) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(1 << 20);
    altsql_db_cursor c;
    altsql_value v[4];
    double prev = -1e308;
    int rc, n = 0, i;
    ram_new(&rf, 1u << 24, 1u << 24);
    CHECK(db_open_ram(&db, &rf, mem, 1 << 20, 4096) == ALTSQL_OK, "open");
    CHECK(altsql_db_table_create(db, "r", "x:real,y:text", "x") == ALTSQL_OK, "create r: %s", altsql_db_errmsg(db));
    CHECK(altsql_db_table_create(db, "f", "x:float,y:int", "x") == ALTSQL_OK, "create f");
    CHECK(altsql_db_table_create(db, "t", "name:text,v:long", "name") == ALTSQL_OK, "create t");
    for (i = 0; i < 500; i++) {
        uint32_t s = (uint32_t)i * 2654435761u + 1;
        double x = ((double)(int32_t)xs(&s)) * pow(10.0, (double)((int)(xs(&s) % 40) - 20));
        v[0].type = ALTSQL_REAL; v[0].u.r = x; v[0].len = 0;
        v[1].type = ALTSQL_TEXT; v[1].u.s = "r"; v[1].len = 1;
        CHECK(altsql_db_row_put(db, "r", v, 2) == ALTSQL_OK, "put real");
        v[1].type = ALTSQL_INTEGER; v[1].u.i = i;
        CHECK(altsql_db_row_put(db, "f", v, 2) == ALTSQL_OK, "put float");
    }
    rc = altsql_db_row_seek(&c, db, "r", NULL, 0);
    while (rc == ALTSQL_OK) {
        CHECK(altsql_db_row_read(&c, v, 4) == ALTSQL_OK, "read");
        CHECK(v[0].u.r > prev, "reals out of order: %g after %g", v[0].u.r, prev);
        prev = v[0].u.r;
        n++;
        rc = altsql_db_next(&c);
    }
    CHECK(n == 500, "500 reals, saw %d", n);
    prev = -1e308;
    rc = altsql_db_row_seek(&c, db, "f", NULL, 0);
    while (rc == ALTSQL_OK) {
        CHECK(altsql_db_row_read(&c, v, 4) == ALTSQL_OK, "read");
        CHECK(v[0].u.r > prev, "floats out of order");
        prev = v[0].u.r;
        rc = altsql_db_next(&c);
    }
    {   /* text keys with zeros sort as bytes */
        static const char *names[] = { "b", "a\0b", "a", "", "a\0", "ab", "a\001" };
        static const int lens[] = { 1, 3, 1, 0, 2, 2, 2 };
        static const int order[] = { 3, 2, 4, 1, 6, 5, 0 };
        for (i = 0; i < 7; i++) {
            v[0].type = ALTSQL_TEXT; v[0].u.s = names[i]; v[0].len = lens[i];
            v[1].type = ALTSQL_INTEGER; v[1].u.i = i; v[1].len = 0;
            CHECK(altsql_db_row_put(db, "t", v, 2) == ALTSQL_OK, "put text %d", i);
        }
        rc = altsql_db_row_seek(&c, db, "t", NULL, 0);
        for (i = 0; i < 7; i++) {
            CHECK(rc == ALTSQL_OK, "text scan short");
            CHECK(altsql_db_row_read(&c, v, 4) == ALTSQL_OK && v[1].u.i == order[i], "text keys out of order at %d: got %lld", i, (long long)v[1].u.i);
            CHECK(v[0].type == ALTSQL_TEXT && v[0].len == lens[order[i]] && !memcmp(v[0].u.s, names[order[i]], (size_t)v[0].len),
                  "text key %d read back wrong (length %d)", i, v[0].len);   /* the zeros inside come back */
            rc = altsql_db_next(&c);
        }
        CHECK(rc == ALTSQL_NOTFOUND, "text scan long");
    }
    {   /* a text column with a zero at its end, then another key column: order and prefixes hold */
        static const char *names[] = { "a\0", "a", "a", "a\0\0" };
        static const int lens[] = { 2, 1, 1, 3 }, nums[] = { 1, 2, 1, 7 };
        static const int order[] = { 2, 1, 0, 3 };
        CHECK(altsql_db_table_create(db, "z", "name:text,n:int", "name,n") == ALTSQL_OK, "create z");
        for (i = 0; i < 4; i++) {
            v[0].type = ALTSQL_TEXT; v[0].u.s = names[i]; v[0].len = lens[i];
            v[1].type = ALTSQL_INTEGER; v[1].u.i = nums[i]; v[1].len = 0;
            CHECK(altsql_db_row_put(db, "z", v, 2) == ALTSQL_OK, "put z %d", i);
        }
        rc = altsql_db_row_seek(&c, db, "z", NULL, 0);
        for (i = 0; i < 4; i++) {
            CHECK(rc == ALTSQL_OK && altsql_db_row_read(&c, v, 4) == ALTSQL_OK && v[0].len == lens[order[i]] && v[1].u.i == nums[order[i]],
                  "z out of order at %d", i);
            rc = altsql_db_next(&c);
        }
        v[0].type = ALTSQL_TEXT; v[0].u.s = "a"; v[0].len = 1;
        rc = altsql_db_row_seek(&c, db, "z", v, 1);
        for (n = 0; rc == ALTSQL_OK; n++) rc = altsql_db_next(&c);
        CHECK(n == 2, "the prefix (\"a\") holds 2 rows, found %d", n);
        n = 0;
    }
    /* what a caller can get wrong */
    v[0].type = ALTSQL_TEXT; v[0].u.s = "x"; v[0].len = 1;
    CHECK(altsql_db_row_put(db, "r", v, 2) == ALTSQL_SCHEMA, "text into a real column");
    v[0].type = ALTSQL_NULL;
    CHECK(altsql_db_row_put(db, "r", v, 2) == ALTSQL_SCHEMA, "NULL refused");
    v[0].type = ALTSQL_REAL; v[0].u.r = 0.0 / 0.0;
    CHECK(altsql_db_row_put(db, "r", v, 2) == ALTSQL_SCHEMA, "NaN refused");
    v[0].u.r = 1;
    CHECK(altsql_db_row_put(db, "r", v, 1) == ALTSQL_SCHEMA, "wrong column count");
    CHECK(altsql_db_row_put(db, "nosuch", v, 2) == ALTSQL_SCHEMA, "unknown table");
    CHECK(altsql_db_table_create(db, "r", "a:int", "a") == ALTSQL_EXISTS, "table twice");
    CHECK(altsql_db_table_create(db, "bad name", "a:int", "a") == ALTSQL_SYNTAX, "bad name");
    CHECK(altsql_db_table_create(db, "u", "a:int,a:long", "a") == ALTSQL_SYNTAX, "repeated column");
    CHECK(altsql_db_table_create(db, "u", "a:int,b:blob", "a") == ALTSQL_SYNTAX, "unknown type");
    CHECK(altsql_db_table_create(db, "u", "a:int,b:int", "c") == ALTSQL_SCHEMA, "key not a column");
    CHECK(altsql_db_table_create(db, "u", "a:int,b:int", "") == ALTSQL_SCHEMA, "no key");
    {
        uint32_t b;
        CHECK(altsql_db_bucket(db, "r", 0, &b) == ALTSQL_MISUSE, "a table is not a bucket");
        CHECK(altsql_db_bucket(db, "bk", 1, &b) == ALTSQL_OK, "bucket");
        CHECK(altsql_db_row_put(db, "bk", v, 2) == ALTSQL_MISUSE, "a bucket is not a table");
        CHECK(altsql_db_get(db, 64, "x", 1, NULL, 0, NULL) == ALTSQL_MISUSE, "a table's key space is not a bucket");
    }
    /* a table made in a transaction that rolls back is gone */
    CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
    CHECK(altsql_db_table_create(db, "gone", "a:int", "a") == ALTSQL_OK, "create in txn");
    CHECK(altsql_db_rollback(db) == ALTSQL_OK, "rollback");
    CHECK(altsql_db_row_put(db, "gone", v, 1) == ALTSQL_SCHEMA, "rolled-back table is gone");
    {
        altsql_db_tableinfo ti;
        CHECK(altsql_db_table_info(db, "t", &ti) == ALTSQL_OK && ti.ncols == 2 && ti.nkey == 1 && ti.key[0] == 0 &&
              ti.types[0] == 6 && !strcmp(ti.names[1], "v") && ti.kind == ALTSQL_DB_TABLE, "table info");
    }
    if (check_slots(db, 1)) return 1;
    altsql_db_close(db);
    ram_free(&rf);
    free(mem);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    uint32_t seed, n = quick ? 4 : 20;
    printf("AltSql DB %s: tables\n", ALTSQL_DB_VERSION);
    if (t_encodings()) return 1;
    printf("  key encodings sort as their values: ok\n");
    if (t_keytypes()) return 1;
    printf("  real, float and text keys in order; caller errors: ok\n");
    for (seed = 1; seed <= n; seed++) {
        if (t_model(seed, 512, 3000)) return 1;
        if (t_model(seed, 4096, 3000)) return 1;
    }
    printf("  model runs: %u seeds, 512 and 4096-byte pages: ok\n", n);
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
