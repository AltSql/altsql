/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Benchmark: one million sensor rows on a file-backed store (gateway side).
 * The same data and queries run on SQLite in bench_sqlite.c (make bench-sqlite).
 * Query times are the best of three runs.
 * Usage: bench [rows] [file]                                               */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_FILE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "altsql.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long nrows_out;
static int count_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)n; (void)v; (void)names;
    nrows_out++;
    return 0;
}

static void die(altsql *db, const char *what, int rc) {
    fprintf(stderr, "%s failed: %d %s\n", what, rc, db ? altsql_errmsg(db) : "");
    exit(1);
}

/* Best of three runs, so a busy moment on the machine does not count. */
static void query(altsql *db, const char *sql) {
    double best = 1e9;
    int rc, k;
    for (k = 0; k < 3; k++) {
        double t0 = now(), t;
        nrows_out = 0;
        rc = altsql_exec(db, sql, count_cb, NULL);
        if (rc) die(db, sql, rc);
        t = now() - t0;
        if (t < best) best = t;
    }
    printf("  %7.1f ms  %-86s -> %ld rows\n", best * 1e3, sql, nrows_out);
}

int main(int argc, char **argv) {
    long rows = argc > 1 ? atol(argv[1]) : 1000000, i;
    const char *path = argc > 2 ? argv[2] : "build/bench.db";
    uint32_t ss = 4096, sc = (uint32_t)(rows / 120 + 64);     /* ~127 rows per 4 KB sector */
    size_t memsz = 8u << 20;
    void *mem = malloc(memsz);
    altsql_flash fl;
    altsql_config cfg;
    altsql_info info;
    altsql_stats st;
    altsql *db = NULL;
    double t0, t;
    int rc;
    const int64_t base = 1767225600;                          /* 2026-01-01 00:00 UTC */
    char sql[160];

    unlink(path);
    if (altsql_file_flash_open(&fl, path, ss, sc, 4, 1)) { perror(path); return 1; }
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem;
    cfg.mem_size = memsz;
    cfg.kv_slots = 16384;
    cfg.create = 1;
    if ((rc = altsql_open(&db, &fl, &cfg))) die(db, "open", rc);
    if ((rc = altsql_ts_create(db, "readings", "time:time,machine:int,temp:float"))) die(db, "create", rc);

    printf("AltSql %s benchmark: %ld rows, %u sectors of %u bytes (file %s)\n",
           ALTSQL_VERSION, rows, sc, ss, path);
    t0 = now();
    for (i = 0; i < rows; i++) {
        rc = altsql_append(db, "readings", base + (int64_t)i, (int)(i % 16), 20.0 + (double)(i % 1000) / 100.0);
        if (rc) die(db, "append", rc);
    }
    t = now() - t0;
    altsql_info_get(db, &info);
    printf("  append:  %.2f s for %ld rows = %.0f rows/s; %u sectors used = %.1f bytes/row on flash\n",
           t, rows, (double)rows / t, info.used_sectors, (double)info.used_sectors * ss / (double)rows);

    t0 = now();
    for (i = 0; i < 10000; i++) {
        char k[32], v[32];
        snprintf(k, sizeof k, "device.%ld", i % 1000);
        snprintf(v, sizeof v, "value-%ld", i);
        if ((rc = altsql_put(db, k, v, strlen(v)))) die(db, "put", rc);
    }
    t = now() - t0;
    printf("  put:     %.0f puts/s (1000 keys, 10000 writes)\n", 10000.0 / t);
    t0 = now();
    for (i = 0; i < 100000; i++) {
        char k[32], v[32];
        size_t n;
        snprintf(k, sizeof k, "device.%ld", i % 1000);
        if ((rc = altsql_get(db, k, v, sizeof v, &n))) die(db, "get", rc);
    }
    t = now() - t0;
    printf("  get:     %.0f gets/s\n", 100000.0 / t);

    t0 = now();
    rc = altsql_ts_window(db, "readings", "temp", base + rows - 60, &st);
    if (rc) die(db, "window", rc);
    printf("  window:  %.3f ms for the last 60 s (%u rows, avg %.2f)\n", (now() - t0) * 1e3, st.count, st.avg);

    printf("  SQL:\n");
    query(db, "SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour");
    query(db, "SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine");
    query(db, "SELECT COUNT(*) FROM readings WHERE temp > 29.5");
    snprintf(sql, sizeof sql, "SELECT time, temp FROM readings WHERE time >= %lld AND machine = 3",
             (long long)(base + rows - 3600));
    query(db, sql);
    query(db, "SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5");

    altsql_close(db);
    t0 = now();
    if ((rc = altsql_open(&db, &fl, &cfg))) die(db, "reopen", rc);
    altsql_info_get(db, &info);
    printf("  reopen:  %.1f ms (scans the whole log, rebuilds the key index); engine memory %u bytes\n",
           (now() - t0) * 1e3, info.mem_used);
    altsql_close(db);
    altsql_file_flash_close(&fl);
    unlink(path);
    free(mem);
    return 0;
}
