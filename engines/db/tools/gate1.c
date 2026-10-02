/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB, Gate 1: is the direct path worth it?
 *
 * The same keys and values in AltSql DB, SQLite and LMDB, each in its own
 * process, on a real file with real syncs.
 *   gate1 <engine> <dir> [entries]
 * engines:
 *   altsql        AltSql DB's direct path: get, put and cursors, no SQL
 *   altsql-int    the same with 8-byte whole-number keys, beside sqlite-blob
 *   sqlite        SQLite, WITHOUT ROWID table, prepared statements, WAL, mmap off
 *   sqlite-mmap   the same with mmap on
 *   sqlite-blob   SQLite's second baseline: a rowid table read through one
 *                 blob handle moved with sqlite3_blob_reopen (whole-number keys)
 *   lmdb          LMDB, for reference only
 * Keys: 12 bytes, device (4) then time (8), big-endian, as a synced reading
 * is keyed. Values: 14 bytes, the payload of a one-value reading.
 * Output: one line per measure, "name value unit". */
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_PORT_FILE
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include "sqlite3.h"
#include "lmdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int intkeys;   /* altsql-int: 8-byte whole-number keys, i + 1, as the rowid baseline uses */
static void mkkey(uint32_t i, uint8_t *k) {
    uint32_t dev = i % 1000;
    uint64_t t = 1700000000ull + i / 1000;
    int j;
    k[0] = (uint8_t)(dev >> 24); k[1] = (uint8_t)(dev >> 16); k[2] = (uint8_t)(dev >> 8); k[3] = (uint8_t)dev;
    for (j = 0; j < 8; j++) k[4 + j] = (uint8_t)(t >> (56 - 8 * j));
}
static void mkval(uint32_t i, uint8_t *v) {
    uint64_t t = 1700000000ull + i / 1000;
    float f = 20.0f + (float)(i % 977) / 100.0f;
    v[0] = 1; v[1] = 0;
    memcpy(v + 2, &t, 8);
    memcpy(v + 10, &f, 4);
}

typedef struct eng {
    const char *name;
    int (*open)(const char *path, size_t cache, int fresh);
    void (*close)(void);
    int (*begin)(void);
    int (*commit)(void);
    int (*put)(uint32_t i);
    int (*get)(uint32_t i, uint8_t *v, size_t *vn);
    long (*scan)(uint64_t *sum);
} eng;

/* ---- AltSql DB ---- */
static altsql_db *adb;
static altsql_db_file afile;
static altsql_db_posix aposix;
static void *amem;
static uint32_t abk;
static int a_open(const char *path, size_t cache, int fresh) {
    altsql_db_config cfg;
    int rc;
    if (fresh) unlink(path);
    if ((rc = altsql_db_posix_open(&afile, &aposix, path, 1)) != 0) return rc;
    amem = malloc(cache + (1 << 20));
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = amem; cfg.mem_size = cache + (1 << 20); cfg.page_size = 4096; cfg.create = 1;
    if ((rc = altsql_db_open(&adb, &afile, &cfg)) != 0) { fprintf(stderr, "open: %s\n", altsql_db_errmsg(adb)); return rc; }
    return altsql_db_bucket(adb, "readings", 1, &abk);
}
static void a_close(void) { altsql_db_close(adb); altsql_db_posix_close(&aposix); free(amem); }
static int a_begin(void) { return altsql_db_begin(adb, 1); }
static int a_commit(void) { return altsql_db_commit(adb); }
static uint32_t a_key(uint32_t i, uint8_t *k) {
    int j;
    uint64_t x = (uint64_t)i + 1;
    if (!intkeys) { mkkey(i, k); return 12; }
    for (j = 0; j < 8; j++) k[j] = (uint8_t)(x >> (56 - 8 * j));
    return 8;
}
static int a_put(uint32_t i) { uint8_t k[12], v[14]; uint32_t kn = a_key(i, k); mkval(i, v); return altsql_db_put(adb, abk, k, kn, v, 14); }
static int a_get(uint32_t i, uint8_t *v, size_t *vn) { uint8_t k[12]; uint32_t kn = a_key(i, k); return altsql_db_get(adb, abk, k, kn, v, 64, vn); }
static long a_scan(uint64_t *sum) {
    altsql_db_cursor c;
    long n = 0;
    uint8_t v[64];
    size_t vn;
    int rc = altsql_db_seek(&c, adb, abk, NULL, 0);
    while (rc == ALTSQL_OK) {
        if (altsql_db_value(&c, v, sizeof v, &vn)) return -1;
        *sum += v[2] + v[10] + vn;
        n++;
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND ? n : -1;
}

/* ---- SQLite ---- */
static sqlite3 *sdb;
static sqlite3_stmt *s_get, *s_put, *s_scan, *s_begin, *s_commit, *s_bput;
static sqlite3_blob *s_blob;
static int s_mmap, s_rowid;
static int s_exec(const char *sql) {
    char *err = 0;
    int rc = sqlite3_exec(sdb, sql, 0, 0, &err);
    if (rc) { fprintf(stderr, "sqlite: %s: %s\n", sql, err ? err : "?"); sqlite3_free(err); }
    return rc;
}
static int s_open(const char *path, size_t cache, int fresh) {
    char sql[160], wal[600];
    if (fresh) { unlink(path); snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); snprintf(wal, sizeof wal, "%s-shm", path); unlink(wal); }
    if (sqlite3_open_v2(path, &sdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0)) return -1;
    if (s_exec("PRAGMA journal_mode=WAL") || s_exec("PRAGMA synchronous=FULL")) return -1;
    snprintf(sql, sizeof sql, "PRAGMA cache_size=-%lu", (unsigned long)(cache / 1024));
    if (s_exec(sql)) return -1;
    snprintf(sql, sizeof sql, "PRAGMA mmap_size=%s", s_mmap ? "4294967296" : "0");
    if (s_exec(sql)) return -1;
    if (s_rowid) {
        if (s_exec("CREATE TABLE IF NOT EXISTS r(v BLOB)")) return -1;
        sqlite3_prepare_v2(sdb, "INSERT INTO r(rowid, v) VALUES(?1, ?2)", -1, &s_bput, 0);
        sqlite3_prepare_v2(sdb, "SELECT v FROM r ORDER BY rowid", -1, &s_scan, 0);
    } else {
        if (s_exec("CREATE TABLE IF NOT EXISTS t(k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID")) return -1;
        sqlite3_prepare_v2(sdb, "SELECT v FROM t WHERE k = ?1", -1, &s_get, 0);
        sqlite3_prepare_v2(sdb, "INSERT OR REPLACE INTO t(k, v) VALUES(?1, ?2)", -1, &s_put, 0);
        sqlite3_prepare_v2(sdb, "SELECT k, v FROM t ORDER BY k", -1, &s_scan, 0);
    }
    sqlite3_prepare_v2(sdb, "BEGIN", -1, &s_begin, 0);
    sqlite3_prepare_v2(sdb, "COMMIT", -1, &s_commit, 0);
    return 0;
}
static void s_close(void) {
    if (s_blob) sqlite3_blob_close(s_blob);
    s_blob = 0;
    sqlite3_finalize(s_get); sqlite3_finalize(s_put); sqlite3_finalize(s_scan);
    sqlite3_finalize(s_begin); sqlite3_finalize(s_commit); sqlite3_finalize(s_bput);
    s_get = s_put = s_scan = s_begin = s_commit = s_bput = 0;
    sqlite3_close(sdb);
}
static int s_step_reset(sqlite3_stmt *st) { int rc = sqlite3_step(st); sqlite3_reset(st); return rc == SQLITE_DONE ? 0 : rc; }
static int s_begin_(void) { if (s_blob) { sqlite3_blob_close(s_blob); s_blob = 0; } return s_step_reset(s_begin); }
static int s_commit_(void) { return s_step_reset(s_commit); }
static int s_put_(uint32_t i) {
    uint8_t k[12], v[14];
    mkkey(i, k); mkval(i, v);
    if (s_rowid) {
        sqlite3_bind_int64(s_bput, 1, (sqlite3_int64)i + 1);
        sqlite3_bind_blob(s_bput, 2, v, 14, SQLITE_STATIC);
        return s_step_reset(s_bput);
    }
    sqlite3_bind_blob(s_put, 1, k, 12, SQLITE_STATIC);
    sqlite3_bind_blob(s_put, 2, v, 14, SQLITE_STATIC);
    return s_step_reset(s_put);
}
static int s_get_(uint32_t i, uint8_t *v, size_t *vn) {
    uint8_t k[12];
    int rc;
    if (s_rowid) {
        if (!s_blob) rc = sqlite3_blob_open(sdb, "main", "r", "v", (sqlite3_int64)i + 1, 0, &s_blob);
        else rc = sqlite3_blob_reopen(s_blob, (sqlite3_int64)i + 1);
        if (rc) return rc;
        *vn = (size_t)sqlite3_blob_bytes(s_blob);
        return sqlite3_blob_read(s_blob, v, (int)*vn, 0);
    }
    mkkey(i, k);
    sqlite3_bind_blob(s_get, 1, k, 12, SQLITE_STATIC);
    rc = sqlite3_step(s_get);
    if (rc == SQLITE_ROW) {
        *vn = (size_t)sqlite3_column_bytes(s_get, 0);
        memcpy(v, sqlite3_column_blob(s_get, 0), *vn);
        rc = 0;
    }
    sqlite3_reset(s_get);
    return rc;
}
static long s_scan_(uint64_t *sum) {
    long n = 0;
    int col = s_rowid ? 0 : 1;
    while (sqlite3_step(s_scan) == SQLITE_ROW) {
        const uint8_t *v = (const uint8_t *)sqlite3_column_blob(s_scan, col);
        int vn = sqlite3_column_bytes(s_scan, col);
        *sum += v[2] + v[10] + (uint64_t)vn;
        n++;
    }
    sqlite3_reset(s_scan);
    return n;
}

/* ---- LMDB ---- */
static MDB_env *lenv;
static MDB_dbi ldbi;
static MDB_txn *lrtxn, *lwtxn;
static int l_open(const char *path, size_t cache, int fresh) {
    char lock[600];
    MDB_txn *t;
    (void)cache;
    if (fresh) { unlink(path); snprintf(lock, sizeof lock, "%s-lock", path); unlink(lock); }
    if (mdb_env_create(&lenv) || mdb_env_set_mapsize(lenv, (size_t)4 << 30) ||
        mdb_env_open(lenv, path, MDB_NOSUBDIR, 0644)) return -1;
    if (mdb_txn_begin(lenv, 0, 0, &t) || mdb_dbi_open(t, 0, 0, &ldbi) || mdb_txn_commit(t)) return -1;
    return 0;
}
static void l_close(void) { if (lrtxn) mdb_txn_abort(lrtxn); lrtxn = 0; mdb_env_close(lenv); }
static int l_begin(void) { if (lrtxn) { mdb_txn_abort(lrtxn); lrtxn = 0; } return mdb_txn_begin(lenv, 0, 0, &lwtxn); }
static int l_commit(void) { int rc = mdb_txn_commit(lwtxn); lwtxn = 0; return rc; }
static int l_put(uint32_t i) {
    uint8_t k[12], v[14];
    MDB_val kv, vv;
    mkkey(i, k); mkval(i, v);
    kv.mv_size = 12; kv.mv_data = k; vv.mv_size = 14; vv.mv_data = v;
    return mdb_put(lwtxn, ldbi, &kv, &vv, 0);
}
static int l_get(uint32_t i, uint8_t *v, size_t *vn) {
    uint8_t k[12];
    MDB_val kv, vv;
    int rc;
    if (!lrtxn && (rc = mdb_txn_begin(lenv, 0, MDB_RDONLY, &lrtxn)) != 0) return rc;
    mkkey(i, k);
    kv.mv_size = 12; kv.mv_data = k;
    if ((rc = mdb_get(lrtxn, ldbi, &kv, &vv)) != 0) return rc;
    *vn = vv.mv_size;
    memcpy(v, vv.mv_data, vv.mv_size);
    return 0;
}
static long l_scan(uint64_t *sum) {
    MDB_cursor *c;
    MDB_val kv, vv;
    long n = 0;
    int rc;
    if (!lrtxn && mdb_txn_begin(lenv, 0, MDB_RDONLY, &lrtxn)) return -1;
    if (mdb_cursor_open(lrtxn, ldbi, &c)) return -1;
    for (rc = mdb_cursor_get(c, &kv, &vv, MDB_FIRST); !rc; rc = mdb_cursor_get(c, &kv, &vv, MDB_NEXT)) {
        const uint8_t *v = (const uint8_t *)vv.mv_data;
        *sum += v[2] + v[10] + vv.mv_size;
        n++;
    }
    mdb_cursor_close(c);
    return n;
}

static eng engines[] = {
    { "altsql", a_open, a_close, a_begin, a_commit, a_put, a_get, a_scan },
    { "altsql-int", a_open, a_close, a_begin, a_commit, a_put, a_get, a_scan },
    { "sqlite", s_open, s_close, s_begin_, s_commit_, s_put_, s_get_, s_scan_ },
    { "sqlite-mmap", s_open, s_close, s_begin_, s_commit_, s_put_, s_get_, s_scan_ },
    { "sqlite-blob", s_open, s_close, s_begin_, s_commit_, s_put_, s_get_, s_scan_ },
    { "lmdb", l_open, l_close, l_begin, l_commit, l_put, l_get, l_scan },
};

static uint64_t fsize(const char *path) { struct stat st; return stat(path, &st) ? 0 : (uint64_t)st.st_size; }

static double reads(eng *e, const uint32_t *look, uint32_t n, uint64_t *sum) {
    uint8_t v[64];
    size_t vn;
    uint32_t i;
    double t = now();
    for (i = 0; i < n; i++) {
        if (e->get(look[i], v, &vn) || vn != 14) { fprintf(stderr, "%s: read %u failed\n", e->name, look[i]); exit(2); }
        *sum += v[2] + v[10] + vn;
    }
    return n / (now() - t);
}

int main(int argc, char **argv) {
    eng *e = 0;
    const char *dir;
    char path[512];
    uint32_t n, r, i, *perm, *look, next;
    uint64_t sum = 0, sum2 = 0, disk;
    size_t big = (size_t)256 << 20;
    double t, rate;
    long cnt;
    struct rusage ru;
    unsigned k;
    if (argc < 3) { fprintf(stderr, "usage: gate1 <altsql|altsql-int|sqlite|sqlite-mmap|sqlite-blob|lmdb> <dir> [entries]\n"); return 1; }
    for (k = 0; k < sizeof engines / sizeof engines[0]; k++) if (!strcmp(argv[1], engines[k].name)) e = &engines[k];
    if (!e) { fprintf(stderr, "unknown engine %s\n", argv[1]); return 1; }
    sqlite3_initialize();                      /* built with SQLITE_OMIT_AUTOINIT, as recommended */
    s_mmap = !strcmp(e->name, "sqlite-mmap");
    s_rowid = !strcmp(e->name, "sqlite-blob");
    intkeys = !strcmp(e->name, "altsql-int");
    dir = argv[2];
    n = argc > 3 ? (uint32_t)atol(argv[3]) : 1000000u;
    r = n;
    snprintf(path, sizeof path, "%s/gate1.%s", dir, e->name);
    perm = (uint32_t *)malloc(n * sizeof *perm);
    look = (uint32_t *)malloc(r * sizeof *look);
    for (i = 0; i < n; i++) perm[i] = i;
    for (i = n - 1; i > 0; i--) { uint32_t j = rnd() % (i + 1), x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
    for (i = 0; i < r; i++) look[i] = rnd() % n;

    /* load, in random order, 10,000 per transaction */
    if (e->open(path, big, 1)) { fprintf(stderr, "open failed\n"); return 2; }
    t = now();
    for (i = 0; i < n; i++) {
        if (i % 10000 == 0 && e->begin()) return 2;
        if (e->put(perm[i])) { fprintf(stderr, "load put failed at %u\n", i); return 2; }
        if (i % 10000 == 9999 || i == n - 1) if (e->commit()) return 2;
    }
    printf("%s load %.0f entries/s\n", e->name, n / (now() - t));
    e->close();

    /* random reads with the data in the cache */
    if (e->open(path, big, 0)) return 2;
    reads(e, look, r, &sum2);                                  /* warm-up */
    rate = reads(e, look, r, &sum);
    printf("%s reads_cached %.0f reads/s\n", e->name, rate);
    printf("%s read_checksum %llu -\n", e->name, (unsigned long long)sum);
    /* a full ordered scan */
    sum = 0;
    t = now();
    cnt = e->scan(&sum);
    rate = cnt / (now() - t);
    if (cnt != (long)n) { fprintf(stderr, "%s: scan saw %ld entries\n", e->name, cnt); return 2; }
    printf("%s scan %.0f entries/s\n", e->name, rate);
    printf("%s scan_checksum %llu -\n", e->name, (unsigned long long)sum);
    e->close();
    disk = fsize(path);
    printf("%s bytes_on_disk %llu bytes\n", e->name, (unsigned long long)disk);
    printf("%s bytes_per_entry %.1f bytes\n", e->name, (double)disk / n);

    /* random reads with four times more data than the cache */
    if (strcmp(e->name, "lmdb") && strcmp(e->name, "sqlite-mmap")) {
        if (e->open(path, disk / 4, 0)) return 2;
        reads(e, look, r / 4, &sum2);
        sum = 0;
        rate = reads(e, look, r, &sum);
        printf("%s reads_4x %.0f reads/s\n", e->name, rate);
        e->close();
    }

    /* synced writes in transactions of 1, 100 and 10,000: new readings */
    if (e->open(path, big, 0)) return 2;
    next = n;
    {
        static const uint32_t sizes[3] = { 1, 100, 10000 }, counts[3] = { 300, 30, 5 };
        for (k = 0; k < 3; k++) {
            uint32_t tx, w;
            t = now();
            for (tx = 0; tx < counts[k]; tx++) {
                if (e->begin()) return 2;
                for (w = 0; w < sizes[k]; w++) if (e->put(next++)) return 2;
                if (e->commit()) return 2;
            }
            t = now() - t;
            printf("%s writes_tx%u %.0f writes/s\n", e->name, sizes[k], sizes[k] * counts[k] / t);
            printf("%s commits_tx%u %.0f commits/s\n", e->name, sizes[k], counts[k] / t);
        }
    }
    e->close();
    getrusage(RUSAGE_SELF, &ru);
    printf("%s peak_memory %ld KB\n", e->name, ru.ru_maxrss);
    return 0;
}
