/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: the interface test (0.2).
 * One random workload runs twice, on two files: once as SQL text through
 * altsql_db_exec, once as direct calls (row_insert, row_put, row_get, row_del,
 * table_create, table_drop). After every step the two must give the same
 * answer and the same refusal, code for code; every few hundred steps they
 * must hold the same rows. With a cache that holds the whole run, the two files
 * must come out identical byte for byte, since both interfaces end in the same
 * tree writes.
 *   test_same          4 seeds of 25,000 steps
 *   test_same quick    4 seeds of 2,000 steps */
#include "dbtest.h"

#define MEM (16u << 20)
static uint8_t g_mem_a[MEM], g_mem_b[MEM];
static altsql_db *A, *B;                 /* A: SQL text, B: direct calls */
static uint32_t g_seed;
static long g_steps, g_refusals, g_writes, g_reads, g_ddl, g_txns, g_compares;

/* ---- what a SELECT prints, as one running hash ---- */
typedef struct hsh { uint64_t h; long rows; } hsh;
static void hadd(hsh *x, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) x->h = (x->h ^ b[i]) * 1099511628211ULL;
}
static void hval(hsh *x, const altsql_value *v) {
    hadd(x, &v->type, sizeof v->type);
    if (v->type == ALTSQL_INTEGER) hadd(x, &v->u.i, sizeof v->u.i);
    else if (v->type == ALTSQL_REAL) hadd(x, &v->u.r, sizeof v->u.r);
    else if (v->type == ALTSQL_TEXT) hadd(x, v->u.s, (size_t)v->len);
}
static int hrow(void *ctx, int n, const altsql_value *v, const char *const *names) {
    hsh *x = (hsh *)ctx;
    int i;
    (void)names;
    for (i = 0; i < n; i++) hval(x, &v[i]);
    x->rows++;
    return 0;
}

/* Every row of a table read with the direct cursor, as a hash. */
static int scan_hash(altsql_db *db, const char *table, int ncols, hsh *x) {
    altsql_db_cursor c;
    altsql_value v[ALTSQL_DB_MAXCOLS];
    int rc;
    x->h = 1469598103934665603ULL; x->rows = 0;
    rc = altsql_db_row_seek(&c, db, table, NULL, 0);
    while (rc == ALTSQL_OK) {
        if ((rc = altsql_db_row_read(&c, v, ALTSQL_DB_MAXCOLS)) != 0) return rc;
        hrow(x, ncols, v, NULL);
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND ? ALTSQL_OK : rc;
}

/* ---- the tables ---- */
/* t1 (id int key, a long, r real, s text); t2 (k text, n int key, v float); t3 (k int key, v text), dropped and made again */
static int t3_exists;
static int make_t3(int with_rows) {
    int ra = altsql_db_exec(A, "CREATE TABLE t3 (k INT, v TEXT, PRIMARY KEY (k))", NULL, NULL);
    int rb = altsql_db_table_create(B, "t3", "k:int,v:text", "k");
    (void)with_rows;
    CHECK(ra == rb, "create t3: SQL %d, direct %d (%s | %s)", ra, rb, altsql_db_errmsg(A), altsql_db_errmsg(B));
    if (ra == ALTSQL_OK) t3_exists = 1;
    g_ddl++;
    return 0;
}

static void rnd_text(uint32_t *s, char *out, int maxlen) {
    static const char al[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    int n = 1 + (int)(xs(s) % (uint32_t)maxlen), i;
    for (i = 0; i < n; i++) out[i] = al[xs(s) % 36];
    out[n] = 0;
}

static altsql_value vint(int64_t i) { altsql_value v; v.type = ALTSQL_INTEGER; v.len = 0; v.u.i = i; return v; }
static altsql_value vreal(double r) { altsql_value v; v.type = ALTSQL_REAL; v.len = 0; v.u.r = r; return v; }
static altsql_value vtext(const char *s) { altsql_value v; v.type = ALTSQL_TEXT; v.len = (int)strlen(s); v.u.s = s; return v; }

static uint64_t changed(altsql_db *db) { altsql_db_info i; altsql_db_info_get(db, &i); return i.sql_changed; }

/* Values printed as SQL literals the parser reads back exactly. */
static void lit_real(char *b, size_t n, double r) { snprintf(b, n, "%.17g", r); if (!strpbrk(b, ".eE")) strcat(b, ".0"); }

static int step(uint32_t *s, int in_tx) {
    char sql[512], txt[40], rl[40];
    int op = (int)(xs(s) % 100), ra, rb;
    int64_t id = (int64_t)(xs(s) % 400), a = (int64_t)(xs(s) % 2000000) - 1000000;
    double r = (double)((int64_t)(xs(s) % 20001) - 10000) / 8.0;
    rnd_text(s, txt, 12);
    lit_real(rl, sizeof rl, r);
    g_steps++;
    if (op < 25) {                                            /* INSERT, against row_insert */
        altsql_value row[4];
        row[0] = vint(id); row[1] = vint(a); row[2] = vreal(r); row[3] = vtext(txt);
        snprintf(sql, sizeof sql, "INSERT INTO t1 VALUES (%lld, %lld, %s, '%s')", (long long)id, (long long)a, rl, txt);
        ra = altsql_db_exec(A, sql, NULL, NULL);
        rb = altsql_db_row_insert(B, "t1", row, 4);
        CHECK(ra == rb, "%s: SQL %d, direct %d (%s | %s)", sql, ra, rb, altsql_db_errmsg(A), altsql_db_errmsg(B));
        if (ra) g_refusals++; else g_writes++;
    } else if (op < 40) {                                     /* INSERT OR REPLACE, against row_put */
        altsql_value row[4];
        row[0] = vint(id); row[1] = vint(a); row[2] = vreal(r); row[3] = vtext(txt);
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t1 VALUES (%lld, %lld, %s, '%s')", (long long)id, (long long)a, rl, txt);
        ra = altsql_db_exec(A, sql, NULL, NULL);
        rb = altsql_db_row_put(B, "t1", row, 4);
        CHECK(ra == rb, "%s: SQL %d, direct %d", sql, ra, rb);
        g_writes++;
    } else if (op < 50) {                                     /* t2: a text and int key, a float */
        altsql_value row[3];
        int64_t n = (int64_t)(xs(s) % 50);
        char k[8];
        snprintf(k, sizeof k, "k%u", (unsigned)(xs(s) % 20));
        row[0] = vtext(k); row[1] = vint(n); row[2] = vreal(r);
        if (xs(s) % 2) {
            snprintf(sql, sizeof sql, "INSERT INTO t2 VALUES ('%s', %lld, %s)", k, (long long)n, rl);
            ra = altsql_db_exec(A, sql, NULL, NULL);
            rb = altsql_db_row_insert(B, "t2", row, 3);
        } else {
            snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t2 VALUES ('%s', %lld, %s)", k, (long long)n, rl);
            ra = altsql_db_exec(A, sql, NULL, NULL);
            rb = altsql_db_row_put(B, "t2", row, 3);
        }
        CHECK(ra == rb, "%s: SQL %d, direct %d", sql, ra, rb);
        if (ra) g_refusals++; else g_writes++;
    } else if (op < 60) {                                     /* DELETE by key, against row_del */
        altsql_value key = vint(id);
        uint64_t ca;
        snprintf(sql, sizeof sql, "DELETE FROM t1 WHERE id = %lld", (long long)id);
        ra = altsql_db_exec(A, sql, NULL, NULL);
        ca = changed(A);
        rb = altsql_db_row_del(B, "t1", &key, 1);
        CHECK(ra == ALTSQL_OK && (rb == ALTSQL_OK || rb == ALTSQL_NOTFOUND), "%s: SQL %d, direct %d", sql, ra, rb);
        CHECK(ca == (rb == ALTSQL_OK ? 1u : 0u), "%s: SQL changed %llu rows, direct %d", sql, (unsigned long long)ca, rb);
        g_writes++;
    } else if (op < 70) {                                     /* UPDATE by key, against get, change, put */
        altsql_value key = vint(id), row[4];
        uint64_t ca;
        snprintf(sql, sizeof sql, "UPDATE t1 SET a = %lld, s = '%s' WHERE id = %lld", (long long)a, txt, (long long)id);
        ra = altsql_db_exec(A, sql, NULL, NULL);
        ca = changed(A);
        rb = altsql_db_row_get(B, "t1", &key, 1, row, 4);
        if (rb == ALTSQL_OK) {
            char keep[16];
            row[1] = vint(a);
            memcpy(keep, txt, sizeof keep);
            row[3] = vtext(keep);
            rb = altsql_db_row_put(B, "t1", row, 4);
            CHECK(rb == ALTSQL_OK, "put after get: %d %s", rb, altsql_db_errmsg(B));
            rb = 1;
        } else {
            CHECK(rb == ALTSQL_NOTFOUND, "get: %d", rb);
            rb = 0;
        }
        CHECK(ra == ALTSQL_OK && ca == (uint64_t)rb, "%s: SQL %d changed %llu, direct changed %d", sql, ra, (unsigned long long)ca, rb);
        g_writes++;
    } else if (op < 85) {                                     /* SELECT by key, against row_get */
        altsql_value key = vint(id), row[4];
        hsh x, y;
        x.h = y.h = 1469598103934665603ULL; x.rows = y.rows = 0;
        snprintf(sql, sizeof sql, "SELECT * FROM t1 WHERE id = %lld", (long long)id);
        ra = altsql_db_exec(A, sql, hrow, &x);
        rb = altsql_db_row_get(B, "t1", &key, 1, row, 4);
        if (rb == ALTSQL_OK) hrow(&y, 4, row, NULL);
        CHECK(ra == ALTSQL_OK && (rb == ALTSQL_OK || rb == ALTSQL_NOTFOUND), "%s: SQL %d, direct %d", sql, ra, rb);
        CHECK(x.rows == y.rows && x.h == y.h, "%s: SQL gave %ld rows, direct %ld, or other values", sql, x.rows, y.rows);
        g_reads++;
    } else if (op < 90) {                                     /* refusals: a wrong type, a missing table */
        altsql_value row[4];
        row[0] = vint(id); row[1] = vtext(txt); row[2] = vreal(r); row[3] = vtext(txt);
        snprintf(sql, sizeof sql, "INSERT INTO t1 VALUES (%lld, '%s', %s, '%s')", (long long)id, txt, rl, txt);
        ra = altsql_db_exec(A, sql, NULL, NULL);
        rb = altsql_db_row_insert(B, "t1", row, 4);
        CHECK(ra == rb && ra != ALTSQL_OK, "text into a long column: SQL %d, direct %d", ra, rb);
        ra = altsql_db_exec(A, "INSERT INTO nosuch VALUES (1)", NULL, NULL);
        rb = altsql_db_row_insert(B, "nosuch", row, 1);
        CHECK(ra == rb && ra == ALTSQL_SCHEMA, "a missing table: SQL %d, direct %d", ra, rb);
        g_refusals += 2;
    } else if (op < 97) {                                     /* t3: rows, then sometimes dropped and made again */
        if (!t3_exists) { if (!in_tx && make_t3(0)) return 1; }   /* DDL outside transactions, so the test knows what exists */
        else if (xs(s) % 4 == 0 && !in_tx) {
            ra = altsql_db_exec(A, "DROP TABLE t3", NULL, NULL);
            rb = altsql_db_table_drop(B, "t3");
            CHECK(ra == rb && ra == ALTSQL_OK, "drop t3: SQL %d, direct %d (%s | %s)", ra, rb, altsql_db_errmsg(A), altsql_db_errmsg(B));
            t3_exists = 0;
            g_ddl++;
        } else {
            altsql_value row[2];
            row[0] = vint(id % 50); row[1] = vtext(txt);
            snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t3 VALUES (%lld, '%s')", (long long)(id % 50), txt);
            ra = altsql_db_exec(A, sql, NULL, NULL);
            rb = altsql_db_row_put(B, "t3", row, 2);
            CHECK(ra == rb, "%s: SQL %d, direct %d", sql, ra, rb);
            g_writes++;
        }
    } else {                                                  /* the whole table, both ways, on both files */
        hsh x, y, p, q;
        CHECK(scan_hash(A, "t1", 4, &x) == ALTSQL_OK && scan_hash(B, "t1", 4, &y) == ALTSQL_OK, "scan t1");
        CHECK(x.rows == y.rows && x.h == y.h, "t1 differs: %ld rows against %ld", x.rows, y.rows);
        p.h = q.h = 1469598103934665603ULL; p.rows = q.rows = 0;
        CHECK(altsql_db_exec(A, "SELECT * FROM t2 ORDER BY v, k, n", hrow, &p) == ALTSQL_OK &&
              altsql_db_exec(B, "SELECT * FROM t2 ORDER BY v, k, n", hrow, &q) == ALTSQL_OK, "select t2");
        CHECK(p.rows == q.rows && p.h == q.h, "t2 differs: %ld rows against %ld", p.rows, q.rows);
        g_compares++;
    }
    return 0;
}

static int run_seed(uint32_t seed, long steps) {
    ramfile fa, fb;
    long i;
    uint32_t s = seed;
    int ra, rb;
    ram_new(&fa, 1u << 26, 1u << 26);
    ram_new(&fb, 1u << 26, 1u << 26);
    CHECK(db_open_ram(&A, &fa, g_mem_a, MEM, 4096) == ALTSQL_OK && db_open_ram(&B, &fb, g_mem_b, MEM, 4096) == ALTSQL_OK, "open");
    ra = altsql_db_exec(A, "CREATE TABLE t1 (id INT, a LONG, r REAL, s TEXT, PRIMARY KEY (id)); "
                           "CREATE TABLE t2 (k TEXT, n INT, v FLOAT, PRIMARY KEY (k, n))", NULL, NULL);
    CHECK(ra == ALTSQL_OK, "create: %s", altsql_db_errmsg(A));
    CHECK(altsql_db_table_create(B, "t1", "id:int,a:long,r:real,s:text", "id") == ALTSQL_OK &&
          altsql_db_table_create(B, "t2", "k:text,n:int,v:float", "k,n") == ALTSQL_OK, "create direct: %s", altsql_db_errmsg(B));
    t3_exists = 0;
    for (i = 0; i < steps; ) {
        if (xs(&s) % 10 == 0) {                               /* a transaction of a few steps, committed or rolled back */
            int n = 1 + (int)(xs(&s) % 8), j, back = xs(&s) % 4 == 0;
            CHECK(altsql_db_begin(A, 1) == ALTSQL_OK && altsql_db_begin(B, 1) == ALTSQL_OK, "begin");
            for (j = 0; j < n && i < steps; j++, i++) if (step(&s, 1)) return 1;
            if (back) { ra = altsql_db_rollback(A); rb = altsql_db_rollback(B); }
            else { ra = altsql_db_commit(A); rb = altsql_db_commit(B); }
            CHECK(ra == rb && ra == ALTSQL_OK, "%s: SQL %d, direct %d", back ? "rollback" : "commit", ra, rb);
            g_txns++;
        } else {
            if (step(&s, 0)) return 1;
            i++;
        }
    }
    {   /* the end: the same rows, the same checks, the same file, byte for byte */
        hsh x, y;
        altsql_db_check_report r1, r2;
        CHECK(scan_hash(A, "t1", 4, &x) == ALTSQL_OK && scan_hash(B, "t1", 4, &y) == ALTSQL_OK && x.h == y.h && x.rows == y.rows, "t1 at the end");
        CHECK(scan_hash(A, "t2", 3, &x) == ALTSQL_OK && scan_hash(B, "t2", 3, &y) == ALTSQL_OK && x.h == y.h && x.rows == y.rows, "t2 at the end");
        CHECK(altsql_db_check(A, -1, g_chk, g_chksize, &r1) == ALTSQL_OK && altsql_db_check(B, -1, g_chk, g_chksize, &r2) == ALTSQL_OK, "check");
        CHECK(fa.r.size == fb.r.size && memcmp(fa.mem, fb.mem, (size_t)fa.r.size) == 0,
              "seed %u: the two files differ (%llu and %llu bytes)", seed, (unsigned long long)fa.r.size, (unsigned long long)fb.r.size);
        printf("  seed %u: %ld steps, t1 %ld rows, %llu-byte files identical, %llu commits\n", seed, steps, x.rows,
               (unsigned long long)fa.r.size, (unsigned long long)r1.txn);
    }
    altsql_db_close(A);
    altsql_db_close(B);
    ram_free(&fa);
    ram_free(&fb);
    return 0;
}

int main(int argc, char **argv) {
    static const uint32_t seeds[] = { 1, 2, 3, 4 };
    long steps = argc > 1 && !strcmp(argv[1], "quick") ? 2000 : 25000;
    unsigned i;
    printf("AltSql DB %s: the interface test, SQL text against direct calls\n", ALTSQL_DB_VERSION);
    if (!g_chk && !(g_chk = (uint8_t *)malloc(g_chksize))) return 1;
    for (i = 0; i < 4; i++) { g_seed = seeds[i]; if (run_seed(seeds[i], steps)) { printf("FAILED\n"); return 1; } }
    printf("  %ld steps: %ld writes, %ld reads, %ld refusals, %ld table drops and creations, %ld transactions, %ld whole-table comparisons\n",
           g_steps, g_writes, g_reads, g_refusals, g_ddl, g_txns, g_compares);
    printf("  same answers, same refusals code for code, same rows, identical files\n");
    printf("test_same: all checks passed\n");
    return 0;
}
