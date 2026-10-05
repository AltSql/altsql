/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB measurements (steps 7 and 11): Core's one-million-row benchmark on AltSql DB,
 * on AltSql Core and, built with -DBENCH_SQLITE, on SQLite; then sync from 100 Core
 * devices into one AltSql DB file.
 *   bench [rows] [dir]
 * Every database sits in a file with real syncs and gets the same 8 MB of memory. Query
 * times are the best of three runs, as in Core's benchmark (core/tests/bench.c). SQLite
 * gets its best layout for these queries: time as INTEGER PRIMARY KEY, WAL, an 8 MB cache,
 * one prepared statement for the load, 10,000 rows a transaction like AltSql DB. */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_FILE
#define ALTSQL_PORT_RAM
#include "altsql.h"
#define ALTSQL_DB_PORT_FILE
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef BENCH_SQLITE
#include "sqlite3.h"
#endif

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static long nout;
static int count_cb(void *ctx, int n, const altsql_value *v, const char *const *names) { (void)ctx; (void)n; (void)v; (void)names; nout++; return 0; }
static void die(const char *what, int rc, const char *msg) { fprintf(stderr, "%s failed: %d %s\n", what, rc, msg); exit(1); }
static uint64_t fsize(const char *p) { struct stat st; return stat(p, &st) ? 0 : (uint64_t)st.st_size; }

static const char *queries[5];
static char q4[200];
static void set_queries(long rows, int64_t base) {
    queries[0] = "SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour";
    queries[1] = "SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine";
    queries[2] = "SELECT COUNT(*) FROM readings WHERE temp > 29.5";
    snprintf(q4, sizeof q4, "SELECT time, temp FROM readings WHERE time >= %lld AND machine = 3", (long long)(base + rows - 3600));
    queries[3] = q4;
    queries[4] = "SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5";
}

static void q_core(altsql *db, const char *sql) {
    double best = 1e9;
    int k, rc;
    for (k = 0; k < 3; k++) {
        double t = now();
        nout = 0;
        if ((rc = altsql_exec(db, sql, count_cb, NULL)) != 0) die(sql, rc, altsql_errmsg(db));
        t = now() - t;
        if (t < best) best = t;
    }
    printf("core     query %7.1f ms  %ld rows  %s\n", best * 1e3, nout, sql);
}
static void q_db(altsql_db *db, const char *label, const char *sql) {
    double best = 1e9;
    int k, rc;
    for (k = 0; k < 3; k++) {
        double t = now();
        nout = 0;
        if ((rc = altsql_db_exec(db, sql, count_cb, NULL)) != 0) die(sql, rc, altsql_db_errmsg(db));
        t = now() - t;
        if (t < best) best = t;
    }
    printf("%-8s query %7.1f ms  %ld rows  %s\n", label, best * 1e3, nout, sql);
}

static int plan_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)n; (void)names;
    snprintf((char *)ctx, 120, "%.*s (%.*s)", v[0].len, v[0].u.s, v[1].len, v[1].u.s);
    return 0;
}

int main(int argc, char **argv) {
    long rows = argc > 1 ? atol(argv[1]) : 1000000, i;
    const char *dir = argc > 2 ? argv[2] : "build";
    char pcore[512], pdb[512], psync[512];
    const int64_t base = 1767225600;                         /* 2026-01-01 00:00 UTC, as in Core's benchmark */
    size_t memsz = 8u << 20;
    void *mem = malloc(memsz);
    double t;
    int rc, k;
    snprintf(pcore, sizeof pcore, "%s/bench.core", dir);
    snprintf(pdb, sizeof pdb, "%s/bench.adb", dir);
    snprintf(psync, sizeof psync, "%s/bench-sync.adb", dir);
    set_queries(rows, base);
    printf("AltSql DB %s measurements: %ld rows, 8 MB of memory each, files with real syncs\n", ALTSQL_DB_VERSION, rows);

    {   /* AltSql Core, as its own benchmark runs it */
        altsql_flash fl;
        altsql_config cfg;
        altsql *db = NULL;
        altsql_info info;
        uint32_t sc = (uint32_t)(rows / 120 + 64);
        unlink(pcore);
        if (altsql_file_flash_open(&fl, pcore, 4096, sc, 4, 1)) die("core file", -1, pcore);
        memset(&cfg, 0, sizeof cfg);
        cfg.mem = mem; cfg.mem_size = memsz; cfg.kv_slots = 16384; cfg.create = 1;
        if ((rc = altsql_open(&db, &fl, &cfg)) != 0) die("core open", rc, "");
        if ((rc = altsql_ts_create(db, "readings", "time:time,machine:int,temp:float")) != 0) die("core create", rc, "");
        t = now();
        for (i = 0; i < rows; i++)
            if ((rc = altsql_append(db, "readings", base + (int64_t)i, (int)(i % 16), 20.0 + (double)(i % 1000) / 100.0)) != 0) die("append", rc, "");
        t = now() - t;
        altsql_info_get(db, &info);
        printf("core     load  %.0f rows/s, %.1f bytes per row on flash\n", rows / t, (double)info.used_sectors * 4096 / (double)rows);
        for (k = 0; k < 5; k++) q_core(db, queries[k]);
        altsql_close(db);
        altsql_file_flash_close(&fl);
        unlink(pcore);
    }

    {   /* AltSql DB: the same rows in a table without a primary key, loaded by the direct path */
        altsql_db_file f;
        altsql_db_posix px;
        altsql_db_config cfg;
        altsql_db *db = NULL;
        altsql_value v[3];
        unlink(pdb);
        if (altsql_db_posix_open(&f, &px, pdb, 1)) die("db file", -1, pdb);
        memset(&cfg, 0, sizeof cfg);
        cfg.mem = mem; cfg.mem_size = memsz; cfg.create = 1;
        if ((rc = altsql_db_open(&db, &f, &cfg)) != 0) die("db open", rc, altsql_db_errmsg(db));
        if ((rc = altsql_db_exec(db, "CREATE TABLE readings (time TIME, machine INT, temp FLOAT)", NULL, NULL)) != 0) die("create", rc, altsql_db_errmsg(db));
        t = now();
        for (i = 0; i < rows; i++) {
            if (i % 10000 == 0 && (rc = altsql_db_begin(db, 1)) != 0) die("begin", rc, altsql_db_errmsg(db));
            v[0].type = ALTSQL_INTEGER; v[0].len = 0; v[0].u.i = base + i;
            v[1].type = ALTSQL_INTEGER; v[1].len = 0; v[1].u.i = i % 16;
            v[2].type = ALTSQL_REAL; v[2].len = 0; v[2].u.r = 20.0 + (double)(i % 1000) / 100.0;
            if ((rc = altsql_db_row_put(db, "readings", v, 3)) != 0) die("row_put", rc, altsql_db_errmsg(db));
            if ((i % 10000 == 9999 || i == rows - 1) && (rc = altsql_db_commit(db)) != 0) die("commit", rc, altsql_db_errmsg(db));
        }
        t = now() - t;
        printf("altsqldb load  %.0f rows/s (direct path, 10,000 rows per transaction), %.1f bytes per row on disk\n",
               rows / t, (double)fsize(pdb) / (double)rows);
        for (k = 0; k < 5; k++) {
            char plan[120] = "";
            char ex[260];
            snprintf(ex, sizeof ex, "EXPLAIN %s", queries[k]);
            altsql_db_exec(db, ex, plan_cb, plan);
            q_db(db, "altsqldb", queries[k]);
            printf("               plan: %s\n", plan);
        }
        altsql_db_close(db);
        altsql_db_posix_close(&px);
        t = now();
        if (altsql_db_posix_open(&f, &px, pdb, 0) || (rc = altsql_db_open(&db, &f, &cfg)) != 0) die("reopen", -1, "");
        printf("altsqldb reopen %.2f ms\n", (now() - t) * 1e3);
        altsql_db_close(db);
        altsql_db_posix_close(&px);
        unlink(pdb);
    }

#ifdef BENCH_SQLITE
    {   /* SQLite: the same rows and queries */
        sqlite3 *sq;
        sqlite3_stmt *st;
        char psq[512], pw[600];
        snprintf(psq, sizeof psq, "%s/bench.sqlite", dir);
        snprintf(pw, sizeof pw, "%s-wal", psq);
        unlink(psq); unlink(pw);
        sqlite3_initialize();
        if (sqlite3_open(psq, &sq) != SQLITE_OK) die("sqlite open", -1, psq);
        sqlite3_exec(sq, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-8192;"
                         "CREATE TABLE readings (time INTEGER PRIMARY KEY, machine INTEGER, temp REAL)", NULL, NULL, NULL);
        if (sqlite3_prepare_v2(sq, "INSERT INTO readings VALUES (?, ?, ?)", -1, &st, NULL) != SQLITE_OK) die("sqlite prepare", -1, sqlite3_errmsg(sq));
        t = now();
        for (i = 0; i < rows; i++) {
            if (i % 10000 == 0) sqlite3_exec(sq, "BEGIN", NULL, NULL, NULL);
            sqlite3_bind_int64(st, 1, base + i);
            sqlite3_bind_int64(st, 2, i % 16);
            sqlite3_bind_double(st, 3, 20.0 + (double)(i % 1000) / 100.0);
            if (sqlite3_step(st) != SQLITE_DONE) die("sqlite insert", -1, sqlite3_errmsg(sq));
            sqlite3_reset(st);
            if (i % 10000 == 9999 || i == rows - 1) sqlite3_exec(sq, "COMMIT", NULL, NULL, NULL);
        }
        t = now() - t;
        sqlite3_finalize(st);
        sqlite3_exec(sq, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        printf("sqlite   load  %.0f rows/s (prepared INSERT, 10,000 rows per transaction), %.1f bytes per row on disk (SQLite %s)\n",
               rows / t, (double)(fsize(psq) + fsize(pw)) / (double)rows, sqlite3_libversion());
        for (k = 0; k < 5; k++) {
            double best = 1e9;
            int r;
            for (r = 0; r < 3; r++) {
                double t0 = now();
                nout = 0;
                if (sqlite3_prepare_v2(sq, queries[k], -1, &st, NULL) != SQLITE_OK) die(queries[k], -1, sqlite3_errmsg(sq));
                while (sqlite3_step(st) == SQLITE_ROW) nout++;
                sqlite3_finalize(st);
                t0 = now() - t0;
                if (t0 < best) best = t0;
            }
            printf("sqlite   query %7.1f ms  %ld rows  %s\n", best * 1e3, nout, queries[k]);
        }
        sqlite3_close(sq);
        unlink(psq); unlink(pw);
    }
#endif

    {   /* sync: 100 Core devices, rows / 100 readings each, batches of 4 KB, one commit per batch */
        enum { NDEV = 100 };
        static altsql_ram_flash ram[NDEV];
        static altsql_flash fl[NDEV];
        static altsql *dev[NDEV];
        static uint32_t conf[NDEV];
        static uint8_t buf[4096];
        long per = rows / NDEV;
        uint32_t sc = (uint32_t)(per / 120 + 16);
        uint8_t *flash = (uint8_t *)malloc((size_t)NDEV * sc * 4096), *dmem = (uint8_t *)malloc((size_t)NDEV * 32768);
        altsql_db_file f;
        altsql_db_posix px;
        altsql_db_config cfg;
        altsql_db *db = NULL;
        long batches = 0, records = 0;
        int d, busy;
        double tsync = 0;
        for (d = 0; d < NDEV; d++) {
            altsql_config c;
            memset(flash + (size_t)d * sc * 4096, 0xFF, (size_t)sc * 4096);
            altsql_ram_flash_init(&fl[d], &ram[d], flash + (size_t)d * sc * 4096, 4096, sc, 4);
            ram[d].budget = -1;
            memset(&c, 0, sizeof c);
            c.mem = dmem + (size_t)d * 32768; c.mem_size = 32768; c.create = 1;
            if ((rc = altsql_open(&dev[d], &fl[d], &c)) != 0) die("device open", rc, "");
            if ((rc = altsql_ts_create(dev[d], "readings", "time:time,machine:int,temp:float")) != 0) die("device series", rc, "");
            for (i = 0; i < per; i++)
                if ((rc = altsql_append(dev[d], "readings", base + (int64_t)i, (int)(i % 16), 20.0 + (double)(i % 1000) / 100.0)) != 0) die("device append", rc, "");
        }
        unlink(psync);
        if (altsql_db_posix_open(&f, &px, psync, 1)) die("sync file", -1, psync);
        memset(&cfg, 0, sizeof cfg);
        cfg.mem = mem; cfg.mem_size = memsz; cfg.create = 1;
        if ((rc = altsql_db_open(&db, &f, &cfg)) != 0) die("sync open", rc, altsql_db_errmsg(db));
        do {                                                  /* round robin: a batch from each device in turn */
            busy = 0;
            for (d = 0; d < NDEV; d++) {
                size_t n;
                uint32_t last;
                rc = altsql_sync_read(dev[d], conf[d], buf, sizeof buf, &n, &last, NULL, NULL);
                if (rc < 0) die("sync_read", rc, "");
                if (!n) continue;
                busy = 1;
                t = now();
                if ((rc = altsql_db_sync_apply(db, 5000 + d, conf[d], buf, n, &conf[d])) != 0) die("sync_apply", rc, altsql_db_errmsg(db));
                tsync += now() - t;
                batches++;
                records += (long)n / 30;
            }
        } while (busy);
        {
            uint64_t sz = fsize(psync);
            printf("sync     %.0f readings/s into one file (%d devices, %ld batches of up to 4 KB, %.0f commits/s)\n",
                   (double)rows / tsync, NDEV, batches, batches / tsync);
            printf("sync     %.1f bytes per reading on disk (a reading here: time, machine, temp)\n", (double)sz / (double)rows);
        }
        set_queries(per, base);                              /* each device holds rows / 100 readings */
        for (k = 0; k < 5; k++) {
            char plan[120] = "";
            char ex[260];
            snprintf(ex, sizeof ex, "EXPLAIN %s", queries[k]);
            altsql_db_exec(db, ex, plan_cb, plan);
            q_db(db, "synced", queries[k]);
            printf("               plan: %s\n", plan);
        }
        altsql_db_close(db);
        altsql_db_posix_close(&px);
        unlink(psync);
        for (d = 0; d < NDEV; d++) altsql_close(dev[d]);
        free(flash);
        free(dmem);
    }
    free(mem);
    return 0;
}
