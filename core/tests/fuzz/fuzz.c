/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Coverage-guided fuzzing with libFuzzer (clang -fsanitize=fuzzer).
 *
 * One file, four targets, picked with -DFUZZ_SQL, -DFUZZ_SYNC, -DFUZZ_IMPORT
 * or -DFUZZ_MOUNT:
 *
 *   sql     SQL text against a small database with a table and some keys
 *   sync    bytes handed to a gateway replica as if they came from a device
 *   import  text handed to the importer
 *   mount   a whole flash image: the database must open it or refuse it,
 *           then work on it, and still open after that
 *
 * The sync and mount targets repair checksums when the first input byte asks
 * for it, so the fuzzer gets past them and reaches the checks behind them.
 * Any crash, sanitizer report or broken rule below (abort) is a bug.
 *
 * Build and run: sh tools/fuzz.sh [seconds per target]                    */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "altsql.h"

#define RULE(cond) do { if (!(cond)) { fprintf(stderr, "rule broken: %s (line %d)\n", #cond, __LINE__); abort(); } } while (0)

static uint8_t mem[256 * 1024];

static int sink_row(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)n; (void)v; (void)names;
    return 0;
}
static int sink_text(void *ctx, const char *d, size_t n) { (void)ctx; (void)d; (void)n; return 0; }

typedef struct dev {
    uint8_t *flash;
    uint32_t ss, sc, align;
    int nomap, replica, create;
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql *db;
} dev;

static int dev_open(dev *d) {
    altsql_config cfg;
    altsql_ram_flash_init(&d->fl, &d->ram, d->flash, d->ss, d->sc, d->align);
    if (d->nomap) d->fl.map = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem;
    cfg.mem_size = sizeof mem;
    cfg.kv_slots = 256;
    cfg.max_series = 8;
    cfg.max_record = 256;
    cfg.create = (uint8_t)d->create;
    cfg.replica = (uint8_t)d->replica;
    d->db = NULL;
    return altsql_open(&d->db, &d->fl, &cfg);
}

/* Recomputes the checksums of sector headers and of every record that has a
 * plausible layout, so that mutated data gets past the checksum tests. */
static void fix_crcs(uint8_t *p, size_t n, uint32_t ss, int sectors) {
    size_t base, off;
    if (!sectors) {                               /* a sync batch: records back to back */
        off = 0;
        while (off + 12 <= n && p[off] == 0xA5) {
            uint32_t len = as_get16(p + off + 2);
            if (off + 12 + len > n) break;
            as_put32(p + off + 8, as_crc32(as_crc32(0, p + off + 1, 7), p + off + 12, len));
            off += 12 + len;
        }
        return;
    }
    for (base = 0; base + ss <= n; base += ss) {
        if (as_get32(p + base) == AS_MAGIC) as_put32(p + base + 28, as_crc32(0, p + base, 28));
        for (off = 32; off + 12 <= ss; off++) {
            uint32_t len;
            if (p[base + off] != 0xA5) continue;
            len = as_get16(p + base + off + 2);
            if (off + 12 + len > ss) continue;
            as_put32(p + base + off + 8, as_crc32(as_crc32(0, p + base + off + 1, 7), p + base + off + 12, len));
        }
    }
}

#ifdef FUZZ_SQL
static uint8_t flash[16 * 1024], snapshot[16 * 1024];
static int ready;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static char q[4097];
    dev d;
    int rc;
    memset(&d, 0, sizeof d);
    d.flash = flash; d.ss = 1024; d.sc = 16; d.align = 4; d.create = 1;
    if (!ready) {
        memset(flash, 0xFF, sizeof flash);
        RULE(dev_open(&d) == ALTSQL_OK);
        RULE(altsql_exec(d.db, "CREATE TABLE t (time TIME, a INT, b FLOAT, c TEXT, d REAL, e LONG);"
                               "INSERT INTO t VALUES (1, 2, 3.5, 'x', 0.1, 7), (2, -4, 1e3, 'it''s', -2.5, 9),"
                               "(3, 0, 0.0, '', 1e300, -1), (4, 5, -7.25, 'Zz', 3.0, 0);"
                               "INSERT INTO kv VALUES ('k', 'v'), ('n', 42), ('r', 1.5)", NULL, NULL) == ALTSQL_OK);
        memcpy(snapshot, flash, sizeof flash);
        ready = 1;
    }
    memcpy(flash, snapshot, sizeof flash);
    RULE(dev_open(&d) == ALTSQL_OK);
    if (size > 4096) size = 4096;
    memcpy(q, data, size);
    q[size] = 0;
    rc = altsql_exec(d.db, q, sink_row, NULL);
    RULE(rc >= 0 || altsql_errmsg(d.db)[0]);
    if (rc != ALTSQL_IOERR) RULE(altsql_exec(d.db, "SELECT COUNT(*), SUM(a), MAX(c) FROM t", sink_row, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    RULE(dev_open(&d) == ALTSQL_OK);                /* still opens */
    RULE(altsql_export(d.db, sink_text, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    return 0;
}
#endif

#ifdef FUZZ_SYNC
static uint8_t flash[16 * 1024];
static uint8_t buf[8192];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dev d;
    int rc;
    if (size < 1) return 0;
    memset(&d, 0, sizeof d);
    d.flash = flash; d.ss = 1024; d.sc = 16; d.align = 4; d.create = 1; d.replica = 1;
    d.nomap = data[0] & 2;
    memset(flash, 0xFF, sizeof flash);
    RULE(dev_open(&d) == ALTSQL_OK);
    size--;
    if (size > sizeof buf) size = sizeof buf;
    memcpy(buf, data + 1, size);
    if (data[0] & 1) fix_crcs(buf, size, 0, 0);
    rc = altsql_sync_apply(d.db, buf, size, NULL);
    RULE(rc >= 0 || altsql_errmsg(d.db)[0]);
    altsql_exec(d.db, "SELECT * FROM kv", sink_row, NULL);
    {   /* every series it learned can be read */
        static const char *const names[] = { "t", "raw", "minute", "a" };
        int i;
        for (i = 0; i < 4; i++) {
            char sql[48];
            snprintf(sql, sizeof sql, "SELECT * FROM %s", names[i]);
            altsql_exec(d.db, sql, sink_row, NULL);
        }
    }
    if (rc != ALTSQL_IOERR) RULE(altsql_export(d.db, sink_text, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    d.create = 0;
    RULE(dev_open(&d) == ALTSQL_OK);                /* still opens */
    RULE(altsql_export(d.db, sink_text, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    return 0;
}
#endif

#ifdef FUZZ_IMPORT
static uint8_t flash[32 * 1024];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dev d;
    int rc;
    memset(&d, 0, sizeof d);
    d.flash = flash; d.ss = 1024; d.sc = 32; d.align = 4; d.create = 1;
    memset(flash, 0xFF, sizeof flash);
    RULE(dev_open(&d) == ALTSQL_OK);
    rc = altsql_import(d.db, (const char *)data, size);
    RULE(rc >= 0 || altsql_errmsg(d.db)[0]);
    if (rc != ALTSQL_IOERR) RULE(altsql_export(d.db, sink_text, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    d.create = 0;
    RULE(dev_open(&d) == ALTSQL_OK);                /* still opens */
    RULE(altsql_export(d.db, sink_text, NULL) == ALTSQL_OK);
    altsql_close(d.db);
    return 0;
}
#endif

#ifdef FUZZ_MOUNT
/* The image: 8 sectors of 256 bytes. First input byte: bit 0 repairs
 * checksums, bit 1 reads through read() instead of map(), bits 2-4 pick
 * the write alignment. */
static uint8_t flash[8 * 256];
static uint8_t buf[512];

static int series_cb(void *ctx, const char *name, const char *schema) {
    char sql[64];
    (void)schema;
    snprintf(sql, sizeof sql, "SELECT * FROM %s", name);
    altsql_exec((altsql *)ctx, sql, sink_row, NULL);
    return 0;
}

static int kv_cb(void *ctx, const char *k, size_t kl, const void *v, size_t vl) {
    (void)ctx; (void)k; (void)kl; (void)v; (void)vl;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const uint32_t aligns[8] = { 4, 1, 2, 8, 16, 4, 4, 4 };
    dev d;
    int rc;
    size_t n, len;
    uint32_t last;
    if (size < 1) return 0;
    memset(&d, 0, sizeof d);
    d.flash = flash; d.ss = 256; d.sc = 8;
    d.nomap = data[0] & 2;
    d.align = aligns[(data[0] >> 2) & 7];
    n = size - 1 < sizeof flash ? size - 1 : sizeof flash;
    memset(flash, 0xFF, sizeof flash);
    memcpy(flash, data + 1, n);
    if (data[0] & 1) fix_crcs(flash, sizeof flash, 256, 1);
    rc = dev_open(&d);
    if (rc != ALTSQL_OK) {                          /* refused: fine, but it must say why */
        RULE(rc == ALTSQL_NOTFOUND || rc == ALTSQL_MISUSE || rc == ALTSQL_CORRUPT || rc == ALTSQL_NOMEM);
        return 0;
    }
    {
        altsql_info info;
        char v[64];
        size_t vn;
        RULE(altsql_info_get(d.db, &info) == ALTSQL_OK);
        altsql_get(d.db, "key", v, sizeof v, &vn);
        altsql_kv_each(d.db, kv_cb, NULL);
        altsql_series_each(d.db, series_cb, d.db);
        altsql_exec(d.db, "SELECT key, LENGTH(value) FROM kv", sink_row, NULL);
        rc = altsql_sync_read(d.db, 0, buf, sizeof buf, &len, &last, NULL, NULL);
        RULE(rc >= 0 || altsql_errmsg(d.db)[0]);
        /* writes on top of whatever was there */
        rc = altsql_put(d.db, "fuzz", "value", 5);
        if (rc == ALTSQL_OK) {
            rc = altsql_get(d.db, "fuzz", v, sizeof v, &vn);
            RULE(rc == ALTSQL_OK && vn == 5 && memcmp(v, "value", 5) == 0);
        }
        altsql_del(d.db, "key");
        altsql_exec(d.db, "INSERT INTO kv VALUES ('a', 1), ('b', 'two')", NULL, NULL);
        altsql_close(d.db);
    }
    RULE(dev_open(&d) == ALTSQL_OK);                /* a database it opened still opens */
    altsql_export(d.db, sink_text, NULL);
    altsql_close(d.db);
    return 0;
}
#endif
