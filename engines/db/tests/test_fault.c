/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: fault injection.
 * The Nth call to the file (read, write, sync, size or truncate) fails, for
 * every N in a run. The call that met the failure must report an error, the
 * run goes on, and at the end the file must hold exactly the commits that
 * were reported, pass every check, and reopen. Since 0.3 the same with SQL on
 * a table with three indexes and statements that fail partway: the indexes
 * must check against the table at the end of every run.
 *   test_fault          every N on four configurations
 *   test_fault quick    every third N */
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

/* The same with the SQL workload: the state after each commit known by its hash. */
static int run_sql(swl *w, int step, unsigned long *runs, unsigned long *unsure) {
    static uint8_t mem[640u << 10];
    static uint64_t want[64];
    ramfile rf;
    altsql_db *db;
    altsql_db_config cfg;
    uint32_t t, n, total;
    int in_commit;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = w->ps == 512 ? 300u << 10 : sizeof mem; cfg.page_size = w->ps; cfg.create = 1; cfg.sql_mem = 192u << 10;
    ram_new(&rf, 4u << 20, 8u << 20);
    rf.r.cap = 4u << 20;
    rf.r.logcap = 8u << 20;
    altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
    CHECK(altsql_db_open(&db, &rf.f, &cfg) == ALTSQL_OK && swl_setup(db) == ALTSQL_OK, "setup");
    want[0] = swl_hash(db);
    total = (uint32_t)rf.r.calls;
    for (t = 0; t < w->ntx; t++) { CHECK(swl_txn(w, db, t, &in_commit) == 0, "dry run"); want[t + 1] = swl_hash(db); }
    total = (uint32_t)rf.r.calls - total;
    altsql_db_close(db);
    for (n = 1; n <= total; n += (uint32_t)step) {
        uint64_t fails, h;
        uint32_t at = 0;                                  /* commits the file holds */
        altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
        CHECK(altsql_db_open(&db, &rf.f, &cfg) == ALTSQL_OK && swl_setup(db) == ALTSQL_OK, "setup");
        rf.r.fail = n;
        for (t = 0; t < w->ntx; t++) {
            int rc;
            fails = rf.r.fails;
            rc = swl_txn(w, db, t, &in_commit);
            if (rf.r.fails != fails && rc == 0)
                CHECK(rf.r.fail_kind == 5, "sql N=%u txn %u: a failed call (kind %d) was not reported", n, t, rf.r.fail_kind);
            if (rc == 0) { CHECK(at == t && swl_hash(db) == want[t + 1], "sql N=%u txn %u: the commit gave another table", n, t); at = t + 1; continue; }
            CHECK(rf.r.fails != fails, "sql N=%u txn %u: error %d without a failed call: %s", n, t, rc, altsql_db_errmsg(db));
            h = swl_hash(db);
            if (in_commit && h == want[t + 1]) { (*unsure)++; at = t + 1; break; }   /* the failed commit is on disk after all: the rest of the run differs */
            CHECK(h == want[at], "sql N=%u txn %u: a failed transaction left changes behind", n, t);
            break;                                        /* the rest of the run would differ from the dry run's */
        }
        if (check_slots(db, 1)) { printf("  sql N=%u\n", n); return 1; }
        if (at < w->ntx) {                                /* after the failure the handle goes on: one more transaction */
            CHECK(swl_txn(w, db, 500 + n, &in_commit) == 0, "sql N=%u: a transaction after the failure: %s", n, altsql_db_errmsg(db));
            if (check_slots(db, 1)) { printf("  sql N=%u, the transaction after\n", n); return 1; }
            at = 1000;
        }
        altsql_db_close(db);
        cfg.create = 0;
        CHECK(altsql_db_open(&db, &rf.f, &cfg) == ALTSQL_OK, "sql N=%u: reopen", n);
        cfg.create = 1;
        CHECK(at == 1000 || swl_hash(db) == want[at], "sql N=%u: wrong state after reopening", n);
        if (check_slots(db, 1)) { printf("  sql N=%u after reopening\n", n); return 1; }
        altsql_db_close(db);
        (*runs)++;
    }
    ram_free(&rf);
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
    {
        swl sc[] = { { 31, 10, 12, 512 }, { 32, 6, 12, 4096 }, { 33, 8, 10, 1024 } };
        unsigned long r1 = 0, u1 = 0;
        for (i = 0; i < sizeof sc / sizeof sc[0]; i++) {
            unsigned long r0 = r1;
            if (run_sql(&sc[i], quick ? 3 : 1, &r1, &u1)) return 1;
            printf("  SQL with 3 indexes, seed %u, %u-byte pages: %lu runs, each with one failing call, all clean\n", sc[i].seed, sc[i].ps, r1 - r0);
        }
        printf("  %lu runs; %lu failed commits on disk after all; the indexes checked against the table after every run and reopen\n", r1, u1);
    }
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
