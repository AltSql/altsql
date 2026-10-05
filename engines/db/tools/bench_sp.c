/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* What statement savepoints cost (0.3): SQL statements inside transactions, each now under a
 * savepoint, against the same work in transactions of their own, which need none. A file in
 * RAM, so the time is the engine's own. Built against 0.2's header too, for the same figures
 * from before savepoints (see tools/README or results/bench_sp.log).
 *   bench_sp [rows] */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "altsql.h"
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static void die(const char *what, int rc, altsql_db *db) { fprintf(stderr, "%s failed: %d %s\n", what, rc, db ? altsql_db_errmsg(db) : ""); exit(1); }

int main(int argc, char **argv) {
    long rows = argc > 1 ? atol(argv[1]) : 200000, i;
    uint64_t cap = 1u << 28;
    uint8_t *fm = (uint8_t *)malloc(cap), *fd = (uint8_t *)malloc(cap), *lg = (uint8_t *)malloc(64u << 20);
    size_t memsz = 16u << 20;
    void *mem = malloc(memsz);
    altsql_db_file f;
    altsql_db_ram r;
    altsql_db_config cfg;
    altsql_db *db = NULL;
    char sql[200];
    double t;
    int rc, best;
    altsql_db_ram_init(&f, &r, fm, fd, cap, lg, 64u << 20);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = memsz; cfg.create = 1;
    if ((rc = altsql_db_open(&db, &f, &cfg)) != 0) die("open", rc, db);
    printf("AltSql DB %s: SQL statements inside transactions and in transactions of their own (a file in RAM)\n", ALTSQL_DB_VERSION);
    if ((rc = altsql_db_exec(db, "CREATE TABLE t (id INT PRIMARY KEY, k INT, s TEXT)", NULL, NULL)) != 0) die("create", rc, db);
    for (best = 0; best < 1; best++) {
        t = now();
        for (i = 0; i < rows; i++) {
            if (i % 1000 == 0 && (rc = altsql_db_begin(db, 1)) != 0) die("begin", rc, db);
            snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%ld, %ld, 'row %ld')", i, i % 97, i);
            if ((rc = altsql_db_exec(db, sql, NULL, NULL)) != 0) die("insert", rc, db);
            if (i % 1000 == 999 && (rc = altsql_db_commit(db)) != 0) die("commit", rc, db);
        }
        if (rows % 1000 && (rc = altsql_db_commit(db)) != 0) die("commit", rc, db);
        t = now() - t;
        printf("  INSERT, one row a statement, 1,000 statements a transaction:  %8.0f statements/s\n", rows / t);
        t = now();
        for (i = 0; i < rows; i++) {
            if (i % 1000 == 0 && (rc = altsql_db_begin(db, 1)) != 0) die("begin", rc, db);
            snprintf(sql, sizeof sql, "UPDATE t SET k = k + 1, s = 'changed' WHERE id = %ld", (i * 7919) % rows);
            if ((rc = altsql_db_exec(db, sql, NULL, NULL)) != 0) die("update", rc, db);
            if (i % 1000 == 999 && (rc = altsql_db_commit(db)) != 0) die("commit", rc, db);
        }
        if (rows % 1000 && (rc = altsql_db_commit(db)) != 0) die("commit", rc, db);
        t = now() - t;
        printf("  UPDATE by key, 1,000 statements a transaction:                %8.0f statements/s\n", rows / t);
        t = now();
        for (i = 0; i < rows / 10; i++) {
            snprintf(sql, sizeof sql, "UPDATE t SET k = k + 1 WHERE id = %ld", (i * 7919) % rows);
            if ((rc = altsql_db_exec(db, sql, NULL, NULL)) != 0) die("update", rc, db);
        }
        t = now() - t;
        printf("  UPDATE by key, each statement its own transaction (no savepoint): %5.0f statements/s\n", rows / 10 / t);
    }
    altsql_db_close(db);
    free(fm); free(fd); free(lg); free(mem);
    return 0;
}
