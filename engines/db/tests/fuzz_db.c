/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AltSql DB fuzz target (libFuzzer). The first byte picks what the rest of
 * the input is:
 *   0  SQL for altsql_db_exec, on a file with three tables and rows
 *   1  a prepared statement: the text up to a zero byte, then values to
 *      bind (a type byte and eight bytes each), stepped to the end
 *   2  a damaged file: a good file, then the input's (offset, byte) pairs
 *      written over it; opened, checked, read and written to
 *   3  a sync batch from a device, after a good batch that defined its
 *      series; 4: the same with every record's checksum made valid, so the
 *      checks behind the checksum must hold
 * What must hold: no crash, no memory error, no undefined behaviour; after
 * SQL and after sync the file passes altsql_db_check, both commit headers;
 * a damaged file gives an error, never a crash.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined ... tests/fuzz_db.c
 */
#define ALTSQL_PORT_RAM
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include <stdio.h>
#include <stdlib.h>

#define CAP   (2u << 20)
static uint8_t MEM[CAP], DISK[CAP], LOG[1u << 16], WORK[1u << 20], CHK[1u << 16], VBUF[4096];
static uint8_t GOOD[CAP];
static uint64_t GOODN;
static uint8_t BATCH0[4096], DFLASH[16 * 4096], DMEM[32768];
static size_t BATCH0N;
static int ready;

static altsql_db *open_file(altsql_db_file *f, altsql_db_ram *r, int create) {
    altsql_db_config cfg;
    altsql_db *db = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = WORK;
    cfg.mem_size = sizeof WORK;
    cfg.page_size = 1024;
    cfg.create = (uint8_t)create;
    (void)r;
    if (altsql_db_open(&db, f, &cfg) != ALTSQL_OK) return NULL;
    return db;
}

static void must_check(altsql_db *db) {
    altsql_db_check_report rep;
    if (altsql_db_check(db, -1, CHK, sizeof CHK, &rep) != ALTSQL_OK) { fprintf(stderr, "check: %s\n", altsql_db_errmsg(db)); abort(); }
    if (altsql_db_check(db, 0, CHK, sizeof CHK, &rep) != ALTSQL_OK && rep.txn) abort();
    if (altsql_db_check(db, 1, CHK, sizeof CHK, &rep) != ALTSQL_OK && rep.txn) abort();
}

static const char *SETUP =
    "CREATE TABLE t1 (a INT, b INT, c REAL, d TEXT, PRIMARY KEY (a, b));"
    "CREATE TABLE t2 (k TEXT PRIMARY KEY, v INT, w FLOAT);"
    "CREATE TABLE t3 (x TIME, y LONG, z TEXT);"
    "INSERT INTO t1 VALUES (1, 1, 1.5, 'a'), (1, 2, -2.5, 'b'), (2, 1, 0, ''), (3, 7, 1e10, 'it''s');"
    "INSERT INTO t2 VALUES ('k1', 1, 1.25), ('k2', -5, 0), ('zz', 7, 2);"
    "INSERT INTO t3 VALUES (100, 1, 'x'), (100, 2, 'y'), (50, 3, 'z')";

static void setup(void) {
    altsql_db_file f;
    altsql_db_ram r;
    altsql_db *db;
    altsql_flash fl;
    altsql_ram_flash ram;
    altsql_config cfg;
    altsql *dev = NULL;
    uint32_t last = 0;
    int i;
    /* a good file, kept as bytes */
    altsql_db_ram_init(&f, &r, MEM, DISK, CAP, LOG, sizeof LOG);
    if (!(db = open_file(&f, &r, 1)) || altsql_db_exec(db, SETUP, NULL, NULL) != ALTSQL_OK) abort();
    {
        uint32_t b;
        altsql_db_bucket(db, "bk", 1, &b);
        for (i = 0; i < (int)sizeof VBUF; i++) VBUF[i] = (uint8_t)(i * 31 + 7);
        for (i = 0; i < 300; i++) { char k[16]; snprintf(k, sizeof k, "key%03d", i); altsql_db_put(db, b, k, strlen(k), VBUF, (size_t)(i * 7 % 600)); }
    }
    altsql_db_close(db);
    GOODN = r.size;
    memcpy(GOOD, MEM, (size_t)GOODN);
    /* a device's first batch: its series defined, some rows and pairs */
    memset(DFLASH, 0xFF, sizeof DFLASH);
    altsql_ram_flash_init(&fl, &ram, DFLASH, 4096, 16, 4);
    ram.budget = -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = DMEM; cfg.mem_size = sizeof DMEM; cfg.create = 1;
    if (altsql_open(&dev, &fl, &cfg) != ALTSQL_OK) abort();
    if (altsql_ts_create(dev, "temps", "time:time,machine:int,temp:float") != ALTSQL_OK) abort();
    for (i = 0; i < 20; i++) altsql_append(dev, "temps", (int64_t)(1700000000 + i), i % 3, 20.0 + i);
    altsql_put(dev, "site", "north", 5);
    if (altsql_sync_read(dev, 0, BATCH0, sizeof BATCH0, &BATCH0N, &last, NULL, NULL) < 0) abort();
    altsql_close(dev);
    ready = 1;
}

static int sink(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)names;
    if (n > 0 && v[0].type == ALTSQL_TEXT && v[0].len > 0) (void)v[0].u.s[v[0].len - 1];
    return 0;
}

static altsql_db *fresh(altsql_db_file *f, altsql_db_ram *r) {
    altsql_db_ram_init(f, r, MEM, DISK, CAP, LOG, sizeof LOG);
    memcpy(MEM, GOOD, (size_t)GOODN);
    memcpy(DISK, GOOD, (size_t)GOODN);
    r->size = r->dsize = GOODN;
    return open_file(f, r, 0);
}

static void fix_crcs(uint8_t *p, size_t n) {
    size_t off = 0;
    while (off + AS_RH <= n) {
        uint32_t plen = as_get16(p + off + 2);
        p[off] = AS_MARK;
        if (off + AS_RH + plen > n) break;
        as_put32(p + off + 8, as_crc32(as_crc32(0, p + off + 1, 7), p + off + AS_RH, plen));
        off += AS_RH + plen;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static char sql[8192];
    static uint8_t batch[8192];
    altsql_db_file f;
    altsql_db_ram r;
    altsql_db *db;
    uint32_t last;
    size_t n, i;
    if (!ready) setup();
    if (size < 1) return 0;
    switch (data[0] % 5) {
    case 0:
        n = size - 1 < sizeof sql - 1 ? size - 1 : sizeof sql - 1;
        memcpy(sql, data + 1, n);
        sql[n] = 0;
        if (!(db = fresh(&f, &r))) abort();
        altsql_db_exec(db, sql, sink, NULL);
        must_check(db);
        altsql_db_close(db);
        break;
    case 1: {
        altsql_db_stmt *st;
        size_t tl = 0;
        while (1 + tl < size && data[1 + tl] && tl < sizeof sql - 1) tl++;
        memcpy(sql, data + 1, tl);
        sql[tl] = 0;
        if (!(db = fresh(&f, &r))) abort();
        if (altsql_db_prepare(db, sql, &st) == ALTSQL_OK) {
            const uint8_t *p = data + 1 + tl + 1;
            int k = 1, steps = 0;
            while (p + 9 <= data + size && k <= 16) {
                altsql_value v;
                char txt[8];
                memset(&v, 0, sizeof v);
                switch (p[0] % 4) {
                case 0: v.type = ALTSQL_INTEGER; v.u.i = (int64_t)as_get64(p + 1); break;
                case 1: v.type = ALTSQL_REAL; { uint64_t x = as_get64(p + 1); memcpy(&v.u.r, &x, 8); } break;
                case 2: v.type = ALTSQL_TEXT; memcpy(txt, p + 1, 8); v.u.s = txt; v.len = p[1] % 9; break;
                default: v.type = ALTSQL_NULL;
                }
                altsql_db_bind(st, k++, &v);
                p += 9;
            }
            while (steps++ < 100000) {
                int c, rc = altsql_db_step(st);
                altsql_value v;
                if (rc != ALTSQL_OK) break;
                for (c = 0; c < altsql_db_column_count(st); c++) if (altsql_db_column(st, c, &v) == ALTSQL_OK && v.type == ALTSQL_TEXT && v.len > 0) (void)v.u.s[v.len - 1];
            }
            altsql_db_finalize(st);
        }
        must_check(db);
        altsql_db_close(db);
        break; }
    case 2: {
        altsql_db_check_report rep;
        altsql_db_cursor c;
        uint32_t b;
        altsql_db_ram_init(&f, &r, MEM, DISK, CAP, LOG, sizeof LOG);
        memcpy(MEM, GOOD, (size_t)GOODN);
        for (i = 1; i + 3 <= size; i += 3) {
            uint32_t off = ((uint32_t)data[i] | (uint32_t)data[i + 1] << 8) * 7u;
            if (off < GOODN) MEM[off] = data[i + 2];
        }
        memcpy(DISK, MEM, (size_t)GOODN);
        r.size = r.dsize = GOODN;
        if (!(db = open_file(&f, &r, 0))) break;
        altsql_db_check(db, -1, CHK, sizeof CHK, &rep);
        altsql_db_check(db, 0, CHK, sizeof CHK, &rep);
        altsql_db_check(db, 1, CHK, sizeof CHK, &rep);
        altsql_db_exec(db, "SELECT COUNT(*), SUM(a), MAX(d) FROM t1; SELECT * FROM t2 WHERE k IN ('k1', 'zz'); SELECT * FROM t3", sink, NULL);
        if (altsql_db_bucket(db, "bk", 0, &b) == ALTSQL_OK && altsql_db_begin(db, 0) == ALTSQL_OK) {
            int k = 0;
            if (altsql_db_seek(&c, db, b, "", 0) == ALTSQL_OK)
                do { size_t vn; altsql_db_value(&c, VBUF, sizeof VBUF, &vn); } while (++k < 1000 && altsql_db_next(&c) == ALTSQL_OK);
            altsql_db_commit(db);
        }
        altsql_db_exec(db, "INSERT INTO t2 VALUES ('new', 1, 1); UPDATE t1 SET c = c + 1; DELETE FROM t3 WHERE y = 2", NULL, NULL);
        altsql_db_close(db);
        break; }
    default: {
        if (!(db = fresh(&f, &r))) abort();
        if (altsql_db_sync_apply(db, 7, 0, BATCH0, BATCH0N, &last) != ALTSQL_OK) abort();
        n = size - 1 < sizeof batch ? size - 1 : sizeof batch;
        memcpy(batch, data + 1, n);
        if (data[0] % 5 == 4) fix_crcs(batch, n);
        altsql_db_sync_apply(db, 7, 0, batch, n, &last);
        altsql_db_sync_apply(db, 8, 0, batch, n, &last);
        must_check(db);
        altsql_db_exec(db, "SELECT COUNT(*), MAX(seq) FROM temps; SELECT * FROM kv", sink, NULL);
        altsql_db_close(db);
    }
    }
    return 0;
}
