/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: the crash matrix.
 * A file in RAM records every write and sync. A cut is simulated at each
 * write of a run: any mix of the unsynced writes survives, and the write
 * that was cut may be torn, sector by sector. After every cut the file must
 * open, hold the last committed state or the one being committed, show
 * complete trees under both headers, and take new commits. Since 0.3 the
 * same again with SQL on a table with three indexes, each transaction with a
 * statement that fails partway: after every cut the indexes must check
 * against the table under both headers.
 *   test_crash          three ways of losing writes per cut
 *   test_crash quick    one */
#include "workload.h"
#include "workload_sql.h"

static uint8_t *g_mem;
static size_t g_msize;

static int setup(ramfile *rf, const wl *w, altsql_db **db, uint32_t *b) {
    int rc;
    altsql_db_ram_init(&rf->f, &rf->r, rf->mem, rf->disk, rf->r.cap, rf->log, rf->r.logcap);
    g_msize = w->ps > 1024 ? 224 * 1024 : 96 * 1024;
    rc = db_open_ram(db, rf, g_mem, g_msize, w->ps);
    CHECK(rc == ALTSQL_OK, "setup open: %d", rc);
    CHECK(altsql_db_bucket(*db, "a", 1, b) == ALTSQL_OK, "setup bucket");
    return 0;
}

static int run_config(wl *w, int nways, unsigned long *cuts, unsigned long *newer, unsigned long *older) {
    ramfile rf;
    altsql_db *db;
    uint32_t b, t, k, total, acked;
    int in_commit = 0, way;
    model ma, mb;
    memset(&ma, 0, sizeof ma);
    memset(&mb, 0, sizeof mb);
    ram_new(&rf, 4u << 20, 8u << 20);
    rf.r.cap = 4u << 20;
    rf.r.logcap = 8u << 20;
    /* a run without a cut: how many writes it makes */
    if (setup(&rf, w, &db, &b)) return 1;
    total = (uint32_t)rf.r.writes;
    for (t = 0; t < w->ntx; t++) CHECK(wl_txn(w, db, b, t, &in_commit) == 0, "dry run txn %u: %s", t, altsql_db_errmsg(db));
    total = (uint32_t)rf.r.writes - total;
    altsql_db_close(db);
    for (k = 1; k <= total; k++) {
        for (way = 0; way < nways; way++) {
            int rc = 0;
            if (setup(&rf, w, &db, &b)) return 1;
            rf.r.cut = k;
            acked = 0;
            for (t = 0; t < w->ntx; t++) {
                if ((rc = wl_txn(w, db, b, t, &in_commit)) != 0) break;
                acked++;
            }
            CHECK(rc != 0 && rf.r.dead, "cut %u: the run should have hit the cut", k);
            altsql_db_close(db);
            altsql_db_ram_powercut(&rf.r, k * 31u + (uint32_t)way * 7u + w->seed);
            (*cuts)++;
            /* the file opens and holds the last commit or the one under way */
            rc = db_open_ram(&db, &rf, g_mem, g_msize, w->ps);
            CHECK(rc == ALTSQL_OK, "cut %u way %d: open after the cut: %d %s", k, way, rc, altsql_db_errmsg(db));
            CHECK(altsql_db_bucket(db, "a", 0, &b) == ALTSQL_OK, "cut %u: bucket lost", k);
            ma.n = 0;
            for (t = 0; t < acked; t++) wl_model(w, &ma, b, t);
            if (same(db, &ma, b)) (*older)++;
            else {
                mcopy(&mb, &ma);
                wl_model(w, &mb, b, acked);
                CHECK(in_commit && same(db, &mb, b), "cut %u way %d (seed %u, page %u): the file holds neither the last commit (%u) nor the one under way", k, way, w->seed, w->ps, acked);
                mcopy(&ma, &mb);
                (*newer)++;
            }
            if (check_slots(db, 0)) { printf("  after cut %u way %d\n", k, way); return 1; }
            /* and it takes new commits */
            CHECK(wl_txn(w, db, b, 1000 + k, &in_commit) == 0, "cut %u: a commit after recovery failed: %s", k, altsql_db_errmsg(db));
            wl_model(w, &ma, b, 1000 + k);
            CHECK(same(db, &ma, b), "cut %u: wrong state after the commit that followed recovery", k);
            if (check_slots(db, 1)) { printf("  after the commit that followed cut %u\n", k); return 1; }
            altsql_db_close(db);
        }
    }
    ram_free(&rf);
    mfree(&ma);
    mfree(&mb);
    return 0;
}

/* The same with the SQL workload: a commit's state is known by its hash from a run without cuts. */
static int run_sql(swl *w, int nways, unsigned long *cuts, unsigned long *newer, unsigned long *older) {
    static uint8_t mem[640u << 10];
    size_t msize = w->ps == 512 ? 300u << 10 : sizeof mem;
    static uint64_t want[64];
    ramfile rf;
    altsql_db *db;
    altsql_db_config cfg;
    uint32_t t, k, total, acked;
    int in_commit = 0, way;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = msize; cfg.page_size = w->ps; cfg.create = 1; cfg.sql_mem = 192u << 10;
    ram_new(&rf, 4u << 20, 8u << 20);
    rf.r.cap = 4u << 20;
    rf.r.logcap = 8u << 20;
    altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
    CHECK(altsql_db_open(&db, &rf.f, &cfg) == ALTSQL_OK && swl_setup(db) == ALTSQL_OK, "setup: %s", altsql_db_errmsg(db));
    want[0] = swl_hash(db);
    total = (uint32_t)rf.r.writes;
    for (t = 0; t < w->ntx; t++) {
        CHECK(swl_txn(w, db, t, &in_commit) == 0, "dry run txn %u: %s", t, altsql_db_errmsg(db));
        want[t + 1] = swl_hash(db);
    }
    total = (uint32_t)rf.r.writes - total;
    altsql_db_close(db);
    for (k = 1; k <= total; k++) {
        for (way = 0; way < nways; way++) {
            uint64_t h;
            int rc = 0;
            altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
            CHECK(altsql_db_open(&db, &rf.f, &cfg) == ALTSQL_OK && swl_setup(db) == ALTSQL_OK, "setup");
            rf.r.cut = k;
            acked = 0;
            for (t = 0; t < w->ntx; t++) {
                if ((rc = swl_txn(w, db, t, &in_commit)) != 0) break;
                acked++;
            }
            CHECK(rc != 0 && rf.r.dead, "sql cut %u: the run should have hit the cut", k);
            altsql_db_close(db);
            altsql_db_ram_powercut(&rf.r, k * 37u + (uint32_t)way * 11u + w->seed);
            (*cuts)++;
            cfg.create = 0;
            rc = altsql_db_open(&db, &rf.f, &cfg);
            cfg.create = 1;
            CHECK(rc == ALTSQL_OK, "sql cut %u way %d: open after the cut: %d", k, way, rc);
            h = swl_hash(db);
            if (h == want[acked]) (*older)++;
            else {
                CHECK(in_commit && h == want[acked + 1], "sql cut %u way %d (seed %u, page %u): the table is neither the last commit (%u) nor the one under way",
                      k, way, w->seed, w->ps, acked);
                acked++;
                (*newer)++;
            }
            if (check_slots(db, 0)) { printf("  after sql cut %u way %d\n", k, way); return 1; }
            CHECK(swl_txn(w, db, acked, &in_commit) == 0, "sql cut %u: a commit after recovery failed: %s", k, altsql_db_errmsg(db));
            if (check_slots(db, 1)) { printf("  after the commit that followed sql cut %u\n", k); return 1; }
            altsql_db_close(db);
        }
    }
    ram_free(&rf);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick"), nways = quick ? 1 : 3;
    unsigned long cuts = 0, newer = 0, older = 0;
    wl configs[] = {
        { 1, 30, 12, 512 },      /* small pages: deep trees, many splits */
        { 2, 30, 12, 512 },
        { 3, 14, 10, 4096 },     /* big values on overflow pages */
        { 4, 20, 25, 1024 },
    };
    unsigned i;
    g_mem = (uint8_t *)malloc(224 * 1024);
    printf("AltSql DB %s: crash matrix\n", ALTSQL_DB_VERSION);
    for (i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        unsigned long c0 = cuts;
        if (run_config(&configs[i], nways, &cuts, &newer, &older)) return 1;
        printf("  seed %u, %u-byte pages, %u transactions of %u changes: %lu cuts, all clean\n",
               configs[i].seed, configs[i].ps, configs[i].ntx, configs[i].ops, cuts - c0);
    }
    printf("  %lu cuts: %lu opened at the last commit, %lu at the commit under way\n", cuts, older, newer);
    {
        swl sc[] = { { 21, 14, 12, 512 }, { 22, 8, 12, 4096 }, { 23, 10, 10, 1024 } };
        unsigned long c1 = 0, n1 = 0, o1 = 0;
        for (i = 0; i < sizeof sc / sizeof sc[0]; i++) {
            unsigned long c0 = c1;
            if (run_sql(&sc[i], nways, &c1, &n1, &o1)) return 1;
            printf("  SQL with 3 indexes and a failing statement per transaction, seed %u, %u-byte pages, %u transactions: %lu cuts, all clean\n",
                   sc[i].seed, sc[i].ps, sc[i].ntx, c1 - c0);
        }
        printf("  %lu cuts: %lu opened at the last commit, %lu at the commit under way; the indexes checked against the table under both headers each time;\n"
               "  %lu statements refused or failed partway and were taken back by their savepoints on the way\n", c1, o1, n1, g_swl_back);
    }
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
