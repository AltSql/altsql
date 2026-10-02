/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: fault injection.
 * The Nth call to the file (read, write, sync, size or truncate) fails, for
 * every N in a run. The call that met the failure must report an error, the
 * run goes on, and at the end the file must hold exactly the commits that
 * were reported, pass every check, and reopen.
 *   test_fault          every N on four configurations
 *   test_fault quick    every third N */
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

static int run_config(wl *w, int step, unsigned long *runs, unsigned long *unsure) {
    ramfile rf;
    altsql_db *db;
    uint32_t b, t, n, total;
    int in_commit;
    model m, m2;
    memset(&m, 0, sizeof m);
    memset(&m2, 0, sizeof m2);
    ram_new(&rf, 4u << 20, 8u << 20);
    rf.r.cap = 4u << 20;
    rf.r.logcap = 8u << 20;
    if (setup(&rf, w, &db, &b)) return 1;
    total = (uint32_t)rf.r.calls;
    for (t = 0; t < w->ntx; t++) CHECK(wl_txn(w, db, b, t, &in_commit) == 0, "dry run");
    total = (uint32_t)rf.r.calls - total;
    altsql_db_close(db);
    for (n = 1; n <= total; n += (uint32_t)step) {
        uint64_t fails;
        if (setup(&rf, w, &db, &b)) return 1;
        rf.r.fail = n;
        m.n = 0;
        for (t = 0; t < w->ntx; t++) {
            int rc;
            fails = rf.r.fails;
            rc = wl_txn(w, db, b, t, &in_commit);
            if (rf.r.fails != fails && rc == 0)
                CHECK(rf.r.fail_kind == 5, "N=%u txn %u: a failed call (kind %d) was not reported", n, t, rf.r.fail_kind);
            if (rc == 0) { wl_model(w, &m, b, t); continue; }
            CHECK(rf.r.fails != fails, "N=%u txn %u: error %d without a failed call: %s", n, t, rc, altsql_db_errmsg(db));
            if (in_commit) {                       /* the commit failed: the file holds it or not */
                mcopy(&m2, &m);
                wl_model(w, &m2, b, t);
                if (same(db, &m2, b)) { mcopy(&m, &m2); (*unsure)++; }
                else CHECK(same(db, &m, b), "N=%u txn %u: after a failed commit the file holds neither state", n, t);
            } else CHECK(same(db, &m, b), "N=%u txn %u: a failed transaction left changes behind", n, t);
        }
        CHECK(same(db, &m, b), "N=%u: wrong state at the end of the run", n);
        if (check_slots(db, 1)) { printf("  N=%u\n", n); return 1; }
        altsql_db_close(db);
        CHECK(db_open_ram(&db, &rf, g_mem, g_msize, w->ps) == ALTSQL_OK, "N=%u: reopen", n);
        CHECK(altsql_db_bucket(db, "a", 0, &b) == ALTSQL_OK, "N=%u: bucket", n);
        CHECK(same(db, &m, b), "N=%u: wrong state after reopening", n);
        if (check_slots(db, 1)) { printf("  N=%u after reopening\n", n); return 1; }
        altsql_db_close(db);
        (*runs)++;
    }
    ram_free(&rf);
    mfree(&m);
    mfree(&m2);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    unsigned long runs = 0, unsure = 0;
    wl configs[] = {
        { 11, 25, 12, 512 },
        { 12, 25, 12, 512 },
        { 13, 12, 10, 4096 },
        { 14, 16, 25, 1024 },
    };
    unsigned i;
    g_mem = (uint8_t *)malloc(224 * 1024);
    printf("AltSql DB %s: fault injection\n", ALTSQL_DB_VERSION);
    for (i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        unsigned long r0 = runs;
        if (run_config(&configs[i], quick ? 3 : 1, &runs, &unsure)) return 1;
        printf("  seed %u, %u-byte pages: %lu runs, each with one failing call, all clean\n",
               configs[i].seed, configs[i].ps, runs - r0);
    }
    printf("  %lu runs; %lu failed commits turned out to be on disk after all\n", runs, unsure);
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
