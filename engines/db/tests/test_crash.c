/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: the crash matrix.
 * A file in RAM records every write and sync. A cut is simulated at each
 * write of a run: any mix of the unsynced writes survives, and the write
 * that was cut may be torn, sector by sector. After every cut the file must
 * open, hold the last committed state or the one being committed, show
 * complete trees under both headers, and take new commits.
 *   test_crash          three ways of losing writes per cut
 *   test_crash quick    one */
#include "workload.h"

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
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
