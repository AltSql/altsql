/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* The benchmark of bench.c, run on SQLite for comparison: the same one
 * million rows (time, machine, temp) and the same five queries, best of
 * three. SQLite gets one transaction for the inserts, no journal and no
 * fsync, so neither side waits for the disk. The last query then runs again
 * with an index on time, which is how SQLite would normally be set up.
 * Needs the SQLite library: make bench-sqlite                              */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static sqlite3 *db;

static void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) { fprintf(stderr, "%s: %s\n", sql, err); exit(1); }
}

static void query(const char *sql) {
    double best = 1e9;
    long rows = 0;
    int k;
    for (k = 0; k < 3; k++) {
        sqlite3_stmt *st;
        double t0 = now(), t;
        rows = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); exit(1); }
        while (sqlite3_step(st) == SQLITE_ROW) { (void)sqlite3_column_double(st, 0); rows++; }
        sqlite3_finalize(st);
        t = now() - t0;
        if (t < best) best = t;
    }
    printf("  %7.1f ms  %-86s -> %ld rows\n", best * 1e3, sql, rows);
}

int main(int argc, char **argv) {
    long rows = argc > 1 ? atol(argv[1]) : 1000000, i;
    const char *path = argc > 2 ? argv[2] : "build/bench_sqlite.db";
    const long long base = 1767225600;                         /* 2026-01-01 00:00 UTC */
    sqlite3_stmt *ins;
    double t0, t;
    long long pages = 0, psize = 0;
    char sql[200];

    unlink(path);
    if (sqlite3_open(path, &db) != SQLITE_OK) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    run("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF");
    run("CREATE TABLE readings (time INTEGER, machine INTEGER, temp REAL)");
    printf("SQLite %s benchmark: %ld rows (file %s)\n", sqlite3_libversion(), rows, path);
    t0 = now();
    run("BEGIN");
    sqlite3_prepare_v2(db, "INSERT INTO readings VALUES (?, ?, ?)", -1, &ins, NULL);
    for (i = 0; i < rows; i++) {
        sqlite3_bind_int64(ins, 1, base + i);
        sqlite3_bind_int(ins, 2, (int)(i % 16));
        sqlite3_bind_double(ins, 3, (double)(float)(20.0 + (double)(i % 1000) / 100.0));
        if (sqlite3_step(ins) != SQLITE_DONE) { fprintf(stderr, "insert: %s\n", sqlite3_errmsg(db)); return 1; }
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    run("COMMIT");
    t = now() - t0;
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &st, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) pages = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &st, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) psize = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    printf("  insert:  %.2f s for %ld rows = %.0f rows/s (one transaction); %.1f bytes/row on disk\n",
           t, rows, (double)rows / t, (double)(pages * psize) / (double)rows);
    printf("  SQL:\n");
    query("SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour");
    query("SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine");
    query("SELECT COUNT(*) FROM readings WHERE temp > 29.5");
    snprintf(sql, sizeof sql, "SELECT time, temp FROM readings WHERE time >= %lld AND machine = 3", base + rows - 3600);
    query(sql);
    query("SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5");
    t0 = now();
    run("CREATE INDEX readings_time ON readings (time)");
    printf("  index on time: built in %.0f ms; the query again:\n", (now() - t0) * 1e3);
    query(sql);
    sqlite3_close(db);
    unlink(path);
    return 0;
}
