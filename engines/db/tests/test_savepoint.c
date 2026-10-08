/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: statement savepoints (0.3).
 * Inside a caller's transaction, statements are made to fail partway: an
 * UPDATE that meets a NULL at a random row after changing the rows before it,
 * an UPDATE of keys that runs into a row it doesn't move, a multi-row INSERT
 * with a key that is already there, an UPDATE of keys in a table whose rows go to overflow
 * pages, so pages are freed and taken again by the same statement, an UPDATE that reads its
 * rows through an index, in another order than the leaves', so a small cache lets a page go
 * and comes back to it. t and w have indexes, kept by the same statements. After each one the tables must read
 * exactly as before the statement, and the transaction must go on and commit.
 * The same work runs on a second file that never runs the failing statements.
 * With a cache that holds the run, the two files must come out identical,
 * byte for byte. With a cache far too small, pages leave the cache and come
 * back in the middle of statements, and the two must still hold the same rows
 * and pass the checker. Last, a savepoint too small to keep every page: the
 * failure then fails the transaction, as in 0.2, and a rollback recovers.
 *   test_savepoint          the full run
 *   test_savepoint quick    fewer rounds */
#include "dbtest.h"

#define NROWS 3000
#define WROWS 300
#ifndef SMALLMEM
#define SMALLMEM (364u << 10)
#endif
static altsql_db *A, *B;                  /* A runs the failing statements, B never does */
static long g_failed, g_good, g_commits, g_rollbacks, g_identical, g_kept, g_evict, g_all;
static uint32_t g_frames;

typedef struct hsh { uint64_t h; long rows; } hsh;
static int hrow(void *ctx, int n, const altsql_value *v, const char *const *names) {
    hsh *x = (hsh *)ctx;
    int i;
    size_t j;
    (void)names;
    for (i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)&v[i].u;
        size_t len = sizeof v[i].u;
        if (v[i].type == ALTSQL_TEXT) { p = (const uint8_t *)v[i].u.s; len = (size_t)v[i].len; }
        x->h = (x->h ^ (uint64_t)v[i].type) * 1099511628211ULL;
        for (j = 0; j < len; j++) x->h = (x->h ^ p[j]) * 1099511628211ULL;
    }
    x->rows++;
    return 0;
}
static int same_rows(const char *when) {
    static const char *const q[3] = { "SELECT * FROM t", "SELECT * FROM u", "SELECT * FROM w" };
    int i;
    for (i = 0; i < 3; i++) {
        hsh a = { 1469598103934665603ULL, 0 }, b = { 1469598103934665603ULL, 0 };
        CHECK(altsql_db_exec(A, q[i], hrow, &a) == ALTSQL_OK, "%s: %s on A: %s", when, q[i], altsql_db_errmsg(A));
        CHECK(altsql_db_exec(B, q[i], hrow, &b) == ALTSQL_OK, "%s: %s on B: %s", when, q[i], altsql_db_errmsg(B));
        CHECK(a.rows == b.rows && a.h == b.h, "%s: %s: %ld rows on A, %ld on B, or other values", when, q[i], a.rows, b.rows);
    }
    return 0;
}

static int both(const char *sql) {
    int ra = altsql_db_exec(A, sql, NULL, NULL), rb = altsql_db_exec(B, sql, NULL, NULL);
    CHECK(ra == rb, "%s: A %d (%s), B %d (%s)", sql, ra, altsql_db_errmsg(A), rb, altsql_db_errmsg(B));
    g_good++;
    return 0;
}

/* Rows of table w are longer than a page holds: their values go to overflow pages. */
static const char *longtext(uint32_t seed, int which) {
    static char buf[2][256];
    int i;
    for (i = 0; i < 200; i++) buf[which][i] = (char)('a' + (seed * 31u + (uint32_t)i * 7u) % 26u);
    buf[which][200] = 0;
    return buf[which];
}
static int put_w(int id, uint32_t seed) {
    char sql[600];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO w VALUES (%d, %u, '%s', '%s')", id, seed % 1000, longtext(seed, 0), longtext(seed + 1, 1));
    return both(sql);
}

/* A statement that does some of its work, then fails. Only A runs it. A good statement
 * over the same rows comes first, so the pages it fails on were already written in this
 * transaction: those are the ones a savepoint has to keep. */
static int fail_on_a(uint32_t *s) {
    char sql[256];
    int kind = (int)(xs(s) % 8), rc;
    uint64_t rd;
    if (kind == 0) {                                      /* a NULL at row x, after the rows before it changed */
        int lo = (int)(xs(s) % NROWS), x = lo + 1 + (int)(xs(s) % 600);
        snprintf(sql, sizeof sql, "UPDATE t SET v = v + 7, s = 'changed' WHERE id BETWEEN %d AND %d", lo, x);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, 1, 1, 'x')", x);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE t SET v = v / (id - %d), s = 'never, a longer text than before' WHERE id >= %d", x, lo);
    } else if (kind == 1) {                               /* keys moved: all the old rows go, the last new one meets a row that stays */
        int lo = (int)(xs(s) % NROWS), span = 50 + (int)(xs(s) % 400), d = 1 + (int)(xs(s) % 30);
        snprintf(sql, sizeof sql, "UPDATE t SET v = v + 1 WHERE id BETWEEN %d AND %d", lo, lo + span + d);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, 2, 2, 'moves')", lo + span);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, 2, 2, 'stays')", lo + span + d);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE t SET id = id + %d WHERE id BETWEEN %d AND %d", d, lo, lo + span);
    } else if (kind == 2) {                               /* the same in the table with a text key */
        uint32_t nm = xs(s) % 8;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO u VALUES ('k%u', 5, 1)", nm);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO u VALUES ('k%u', 6, 1)", nm);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE u SET n = n + 1, w = w * 2 WHERE n <= 5");
    } else if (kind == 3) {                               /* keys moved in the table of long rows: overflow pages freed and reused */
        int lo = (int)(xs(s) % WROWS), span = 5 + (int)(xs(s) % 60), d = 1 + (int)(xs(s) % 8);
        snprintf(sql, sizeof sql, "UPDATE w SET n = n + 1 WHERE id BETWEEN %d AND %d", lo, lo + span + d);
        if (both(sql)) return 1;
        if (put_w(lo + span, xs(s)) || put_w(lo + span + d, xs(s))) return 1;
        snprintf(sql, sizeof sql, "UPDATE w SET id = id + %d WHERE id BETWEEN %d AND %d", d, lo, lo + span);
    } else if (kind == 4) {                               /* long rows made short: overflow pages freed, then a NULL */
        int lo = (int)(xs(s) % WROWS), x = lo + 1 + (int)(xs(s) % 40);
        snprintf(sql, sizeof sql, "UPDATE w SET n = n + 1 WHERE id BETWEEN %d AND %d", lo, x);
        if (both(sql)) return 1;
        if (put_w(x, xs(s))) return 1;
        snprintf(sql, sizeof sql, "UPDATE w SET a = 'short', n = n / (id - %d) WHERE id >= %d", x, lo);
    } else if (kind == 5) {                               /* keys moved down: the old rows go, the first new one meets a row */
        int lo = 40 + (int)(xs(s) % NROWS), span = 50 + (int)(xs(s) % 400), d = 1 + (int)(xs(s) % 30);
        snprintf(sql, sizeof sql, "UPDATE t SET v = v + 1 WHERE id BETWEEN %d AND %d", lo - d, lo + span);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, 2, 2, 'stays'), (%d, 2, 2, 'moves')", lo - d, lo);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE t SET id = id - %d WHERE id BETWEEN %d AND %d", d, lo, lo + span);
    } else if (kind == 6) {                               /* through the index on (k, s): rows in another order than the leaves' */
        int y = (int)(xs(s) % 7), x = (int)(xs(s) % NROWS);
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, %d, 5, 'zz, last for its k')", x, y);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE t SET v = v + 1 WHERE k = %d", y);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "UPDATE t SET v = v / (id - %d) WHERE k = %d", x, y);
    } else {                                              /* a multi-row INSERT with a key already there */
        int x = (int)(xs(s) % NROWS);
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, 3, 3, 'here')", x);
        if (both(sql)) return 1;
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (100001, 1, 1, 'a'), (100002, 1, 1, 'b'), (%d, 1, 1, 'c')", x);
    }
    rd = A->nread;
    rc = altsql_db_exec(A, sql, NULL, NULL);
    CHECK(rc != ALTSQL_OK && rc != ALTSQL_DB_FAILED, "%s: should fail and be taken back, got %d (%s)", sql, rc, altsql_db_errmsg(A));
    CHECK(!A->sp.broken, "%s: the savepoint ran out of room", sql);
    g_kept += A->sp.nkept;
    if (A->nread != rd) g_evict++;
    g_failed++;
    return same_rows("after a failed statement");
}

/* Early in a transaction, a statement changes every row of a table, then fails on the last.
 * Each page it copies frees one of the last commit: more than the list in memory holds, so
 * the list goes to pages in the middle of the statement. A good statement over part of the
 * other table comes first and leaves that list partly full. */
static int fail_all(uint32_t *s) {
    uint64_t rd;
    char sql[128];
    int rc;
    if (xs(s) % 2) {
        snprintf(sql, sizeof sql, "UPDATE w SET n = n + 1 WHERE id < %u", 100 + xs(s) % 200);
        if (both(sql) || both("INSERT OR REPLACE INTO t VALUES (99999, 0, 0, 'last')")) return 1;
        rd = A->nread;
        rc = altsql_db_exec(A, "UPDATE t SET v = v / (id - 99999), s = 'every row, and longer than before' WHERE id <= 99999", NULL, NULL);
    } else {                                              /* each row of w drops its overflow pages */
        snprintf(sql, sizeof sql, "UPDATE t SET k = k + 1 WHERE id < %u", xs(s) % NROWS);
        if (both(sql) || put_w(99999, 1)) return 1;
        rd = A->nread;
        rc = altsql_db_exec(A, "UPDATE w SET a = 'every row', n = n / (id - 99999) WHERE id <= 99999", NULL, NULL);
    }
    CHECK(rc == ALTSQL_SCHEMA, "the UPDATE of every row should fail and be taken back, got %d (%s)", rc, altsql_db_errmsg(A));
    CHECK(!A->sp.broken, "the UPDATE of every row: the savepoint ran out of room (%u of %u pages kept)", A->sp.nkept, A->sp.cap);
    g_kept += A->sp.nkept;
    if (A->nread != rd) g_evict++;
    g_failed++;
    g_all++;
    return same_rows("after the UPDATE of every row failed");
}

static int good(uint32_t *s) {
    char sql[256];
    int k = (int)(xs(s) % 7), a = (int)(xs(s) % (NROWS + 400));
    if (k == 0) {
        uint32_t r = xs(s) % 9999, kk = xs(s) % 1000, v = xs(s) % 50;   /* draws right to left, in the order gcc on x86-64 made them */
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t VALUES (%d, %u, %u, 'r%u')", a, v, kk, r);
    }
    else if (k == 1) snprintf(sql, sizeof sql, "UPDATE t SET v = v + 1, k = k + 2 WHERE id BETWEEN %d AND %d", a, a + 60);
    else if (k == 2) snprintf(sql, sizeof sql, "DELETE FROM t WHERE id BETWEEN %d AND %d", a, a + 9);
    else if (k == 3) snprintf(sql, sizeof sql, "UPDATE t SET id = id + 100000 WHERE id BETWEEN %d AND %d", a, a + 2);
    else if (k == 4) {
        uint32_t w = xs(s) % 100, n = xs(s) % 40, nm = xs(s) % 8;      /* draws right to left, in the order gcc on x86-64 made them */
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO u VALUES ('k%u', %u, %u)", nm, n, w);
    } else if (k == 5) {
        uint32_t v = xs(s);
        int id = (int)(xs(s) % (WROWS + 30));                           /* draws right to left, in the order gcc on x86-64 made them */
        return put_w(id, v);
    }
    else snprintf(sql, sizeof sql, "DELETE FROM w WHERE id = %u", xs(s) % WROWS);
    return both(sql);
}

static int run(uint32_t seed, int rounds, size_t mem, uint32_t ps, size_t sqlmem, int bytes) {
    ramfile fa, fb;
    altsql_db_config cfg;
    uint8_t *ma = (uint8_t *)malloc(mem), *mb = (uint8_t *)malloc(mem);
    uint32_t s = seed;
    int r, i;
    char sql[128];
    ram_new(&fa, 1u << 26, 1u << 26);
    ram_new(&fb, 1u << 26, 1u << 26);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = ma; cfg.mem_size = mem; cfg.page_size = ps; cfg.create = 1; cfg.sql_mem = sqlmem;
    CHECK(altsql_db_open(&A, &fa.f, &cfg) == ALTSQL_OK, "open A");
    cfg.mem = mb;
    CHECK(altsql_db_open(&B, &fb.f, &cfg) == ALTSQL_OK, "open B");
    if (both("CREATE TABLE t (id INT, k INT, v INT, s TEXT, PRIMARY KEY (id)); CREATE TABLE u (name TEXT, n INT, w INT, PRIMARY KEY (name, n))")) return 1;
    if (both("CREATE TABLE w (id INT, n INT, a TEXT, b TEXT, PRIMARY KEY (id))")) return 1;
    if (both("CREATE INDEX t_ks ON t (k, s); CREATE INDEX w_n ON w (n)")) return 1;
    CHECK(altsql_db_begin(A, 1) == ALTSQL_OK && altsql_db_begin(B, 1) == ALTSQL_OK, "begin");
    for (i = 0; i < NROWS; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, %d, %d, 'row %d')", i, i % 7, i * 3, i);
        if (both(sql)) return 1;
    }
    for (i = 0; i < WROWS; i++) if (put_w(i, (uint32_t)i * 2654435761u)) return 1;
    CHECK(altsql_db_commit(A) == ALTSQL_OK && altsql_db_commit(B) == ALTSQL_OK, "commit");
    if (!g_frames) g_frames = A->nfr;
    for (i = 0; i < 40; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO u VALUES ('k%d', %d, %d)", i % 8, i, i);
        if (both(sql)) return 1;
    }
    for (r = 0; r < rounds; r++) {
        int n = 1 + (int)(xs(&s) % 4), back = xs(&s) % 6 == 0;
        CHECK(altsql_db_begin(A, 1) == ALTSQL_OK && altsql_db_begin(B, 1) == ALTSQL_OK, "begin");
        if (xs(&s) % 4 == 0 && fail_all(&s)) return 1;
        for (i = 0; i < n; i++) if (good(&s)) return 1;
        if (fail_on_a(&s)) return 1;
        for (i = 0; i < n; i++) if (good(&s)) return 1;
        if (xs(&s) % 3 == 0 && fail_on_a(&s)) return 1;
        if (back) {
            CHECK(altsql_db_rollback(A) == ALTSQL_OK && altsql_db_rollback(B) == ALTSQL_OK, "rollback");
            g_rollbacks++;
        } else {
            int ca = altsql_db_commit(A), cb = altsql_db_commit(B);
            CHECK(ca == ALTSQL_OK && cb == ALTSQL_OK, "commit: A %d (%s), B %d", ca, altsql_db_errmsg(A), cb);
            g_commits++;
        }
        if (same_rows("after the transaction")) return 1;
    }
    if (check_slots(A, 0) || check_slots(B, 0)) return 1;
    if (bytes) {
        CHECK(fa.r.size == fb.r.size && memcmp(fa.mem, fb.mem, (size_t)fa.r.size) == 0,
              "seed %u: the files differ (%llu and %llu bytes)", seed, (unsigned long long)fa.r.size, (unsigned long long)fb.r.size);
        g_identical++;
    }
    altsql_db_close(A); altsql_db_close(B);
    ram_free(&fa); ram_free(&fb);
    free(ma); free(mb);
    return 0;
}

/* A savepoint too small to keep every page the statement changes: the failure fails the
 * transaction, as in 0.2, and a rollback brings back the last commit. */
static int too_small(void) {
    ramfile f;
    altsql_db_config cfg;
    static uint8_t mem[1 << 21];
    altsql_db *db;
    hsh before = { 1469598103934665603ULL, 0 }, after = { 1469598103934665603ULL, 0 };
    int i, rc;
    char sql[128];
    ram_new(&f, 1u << 26, 1u << 26);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = sizeof mem; cfg.page_size = 512; cfg.create = 1; cfg.sql_mem = 48 << 10;
    CHECK(altsql_db_open(&db, &f.f, &cfg) == ALTSQL_OK, "open");
    CHECK(altsql_db_exec(db, "CREATE TABLE t (id INT, v INT, s TEXT, PRIMARY KEY (id))", NULL, NULL) == ALTSQL_OK, "create");
    for (i = 0; i < 3000; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, %d, 'a row of some length, %d')", i, i, i);
        CHECK(altsql_db_exec(db, sql, NULL, NULL) == ALTSQL_OK, "insert");
    }
    CHECK(altsql_db_exec(db, "SELECT * FROM t", hrow, &before) == ALTSQL_OK, "select");
    CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
    rc = altsql_db_exec(db, "UPDATE t SET v = v + 1", NULL, NULL);
    CHECK(rc == ALTSQL_OK, "update every row: %d %s", rc, altsql_db_errmsg(db));
    rc = altsql_db_exec(db, "UPDATE t SET v = v / (id - 2999)", NULL, NULL);
    CHECK(rc == ALTSQL_SCHEMA, "the failing UPDATE: %d %s", rc, altsql_db_errmsg(db));
    rc = altsql_db_commit(db);
    CHECK(rc == ALTSQL_DB_FAILED, "a savepoint that ran out of room fails the transaction: %d", rc);
    CHECK(altsql_db_rollback(db) == ALTSQL_OK, "rollback");
    CHECK(altsql_db_exec(db, "SELECT * FROM t", hrow, &after) == ALTSQL_OK && after.rows == before.rows && after.h == before.h, "the last commit is back");
    if (check_slots(db, 0)) return 1;
    altsql_db_close(db);
    ram_free(&f);
    printf("  a savepoint too small for the statement: the failure fails the transaction, and a rollback brings back the last commit\n");
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick"), rounds = quick ? 15 : 60;
    uint32_t seed;
    long f1;
    printf("AltSql DB %s: statement savepoints\n", ALTSQL_DB_VERSION);
    for (seed = 1; seed <= 4; seed++)                     /* a cache that holds the run: files compared byte for byte */
        if (run(seed * 7919u, rounds, 8u << 20, 1024, 0, 1)) { printf("FAILED\n"); return 1; }
    printf("  a cache of %u pages of 1 KiB: %ld failed statements kept %ld pages\n", g_frames, g_failed, g_kept);
    g_frames = 0; g_kept = 0; g_evict = 0; f1 = g_failed;
    for (seed = 5; seed <= 8; seed++)                     /* a cache of a few dozen 512-byte pages */
        if (run(seed * 7919u, rounds, SMALLMEM, 512, 320u << 10, 0)) { printf("FAILED\n"); return 1; }
    printf("  a cache of %u pages of 512 bytes: %ld failed statements kept %ld pages, %ld of them read pages back from the file\n",
           g_frames, g_failed - f1, g_kept, g_evict);
    printf("  8 seeds, %d rounds each: %ld statements on both files, %ld failed on one and taken back (%ld of them over every row), %ld commits, %ld rollbacks\n",
           rounds, g_good, g_failed, g_all, g_commits, g_rollbacks);
    printf("  after every failed statement both files held the same rows; %ld runs with a cache that holds them ended byte for byte the same\n", g_identical);
    if (too_small()) { printf("FAILED\n"); return 1; }
    printf("test_savepoint: all checks passed\n");
    return g_fail ? 1 : 0;
}
