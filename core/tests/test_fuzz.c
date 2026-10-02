/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Robustness: damaged or hostile input must give an error, never a crash
 * or a broken database. Mutates SQL text, sync batches (including batches
 * whose checksums are fixed up, so the layout checks behind them are hit)
 * and text exports. Best run under the sanitizers (make sanitize).
 * Usage: test_fuzz [rounds]                                                */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "testutil.h"

static uint32_t rs = 777;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void mutate(char *s, size_t *n, size_t cap, int text) {
    int k, edits = 1 + (int)(rnd() % 4);
    static const char pool[] = "(),'*;=<>+-/% \n0123456789.eE_aZ\"\\";
    for (k = 0; k < edits; k++) {
        size_t at = *n ? rnd() % *n : 0;
        char c = text ? pool[rnd() % (sizeof pool - 1)] : (char)rnd();
        switch (rnd() % 4) {
        case 0: if (*n) { memmove(s + at, s + at + 1, *n - at - 1); (*n)--; } break;         /* delete */
        case 1: if (*n + 1 < cap) { memmove(s + at + 1, s + at, *n - at); s[at] = c; (*n)++; } break;
        case 2: if (*n) s[at] = c; break;                                                  /* replace */
        default: *n = at; break;                                                           /* cut */
        }
    }
    s[*n] = 0;
}

static int sink(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)n; (void)v; (void)names;
    return 0;
}

static int wsink(void *ctx, const char *d, size_t n) { (void)ctx; (void)d; (void)n; return 0; }

static const char *const stmts[] = {
    "CREATE TABLE t (time TIME, a INT, b FLOAT, c TEXT)",
    "INSERT INTO t VALUES (1, 2, 3.5, 'x'), (2, -4, 1e3, 'it''s')",
    "SELECT a, COUNT(*), AVG(b) FROM t WHERE time >= 1 AND c LIKE '%x%' GROUP BY a HAVING COUNT(*) > 0 ORDER BY 2 DESC LIMIT 5 OFFSET 1",
    "SELECT UPPER(c), LOWER(c), LENGTH(c), ABS(a), ROUND(b, 1), a / 0, a % 3 FROM t WHERE b BETWEEN 1 AND 2000 OR c IS NULL",
    "SELECT key, value FROM kv WHERE key > 'a' ORDER BY key",
    "SELECT time * 2 AS x, x + 1 FROM t WHERE NOT (a <> 2) ORDER BY x",
    "INSERT INTO kv (key, value) VALUES ('k', 'v'), ('n', 42)",
};

int main(int argc, char **argv) {
    long rounds = argc > 1 ? (strcmp(argv[1], "quick") == 0 ? 3000 : atol(argv[1])) : 20000, i;
    long n_sql = 0, n_sync = 0, n_imp = 0;
    rig dev, rep, imp;
    tbuf exp = {0, 0, 0};
    uint8_t batch[2048], m[2048];
    size_t blen = 0;
    uint32_t last;
    char q[1024];

    /* SQL */
    rig_init(&dev, 1024, 16, 4, 128 * 1024, 0);
    dev.cfg.kv_slots = 4096;
    CHECK_OK(dev.db, rig_open(&dev));
    for (i = 0; i < (long)(sizeof stmts / sizeof stmts[0]); i++) CHECK_OK(dev.db, altsql_exec(dev.db, stmts[i], sink, NULL));
    for (i = 0; i < rounds; i++) {
        const char *base = stmts[rnd() % (sizeof stmts / sizeof stmts[0])];
        size_t n = strlen(base);
        int rc;
        memcpy(q, base, n + 1);
        mutate(q, &n, sizeof q, 1);
        rc = altsql_exec(dev.db, q, sink, NULL);
        n_sql++;
        CHECK(rc == ALTSQL_OK || (rc < 0 && altsql_errmsg(dev.db)[0]));
        if (!(rc == ALTSQL_OK || (rc < 0 && altsql_errmsg(dev.db)[0]))) fprintf(stderr, "  rc %d for: %s\n", rc, q);
        if (rc == ALTSQL_FULL) break;
    }

    /* sync batches */
    CHECK(altsql_sync_read(dev.db, 0, batch, sizeof batch, &blen, &last, NULL, NULL) >= 0 && blen > 0);
    rig_init(&rep, 1024, 16, 4, 128 * 1024, 0);
    rep.cfg.replica = 1;
    CHECK_OK(rep.db, rig_open(&rep));
    for (i = 0; i < rounds; i++) {
        size_t n = blen;
        memcpy(m, batch, blen);
        mutate((char *)m, &n, sizeof m, 0);
        if (rnd() % 2) {                   /* fix up checksums so the layout checks are reached */
            size_t off = 0;
            while (off + 12 <= n && m[off] == 0xA5) {
                uint32_t len = as_get16(m + off + 2);
                if (off + 12 + len > n) break;
                as_put32(m + off + 8, as_crc32(as_crc32(0, m + off + 1, 7), m + off + 12, len));
                off += 12 + len;
            }
        }
        {
            int rc = altsql_sync_apply(rep.db, m, n, NULL);
            n_sync++;
            CHECK(rc == ALTSQL_OK || rc == ALTSQL_CORRUPT || rc == ALTSQL_TOOBIG || rc == ALTSQL_FULL);
        }
        if (i % 500 == 0) {                /* the replica stays readable */
            CHECK_OK(rep.db, altsql_exec(rep.db, "SELECT * FROM kv", sink, NULL));
            altsql_exec(rep.db, "SELECT * FROM t", sink, NULL);
            CHECK_OK(rep.db, altsql_export(rep.db, wsink, NULL));
        }
    }
    rig_close(&rep);
    CHECK_OK(rep.db, rig_open(&rep));
    CHECK_OK(rep.db, altsql_export(rep.db, wsink, NULL));

    /* text import */
    CHECK_OK(dev.db, altsql_export(dev.db, tbuf_write, &exp));
    rig_init(&imp, 1024, 16, 4, 128 * 1024, 0);
    imp.cfg.kv_slots = 4096;
    for (i = 0; exp.p && i < rounds / 4; i++) {
        char *t = (char *)malloc(exp.n + 64);
        size_t n = exp.n;
        int rc;
        memcpy(t, exp.p, exp.n + 1);
        mutate(t, &n, exp.n + 64, 1);
        memset(imp.flash, 0xFF, (size_t)imp.ss * imp.sc);
        CHECK_OK(imp.db, rig_open(&imp));
        rc = altsql_import(imp.db, t, n);
        n_imp++;
        CHECK(rc == ALTSQL_OK || rc < 0);
        CHECK_OK(imp.db, altsql_export(imp.db, wsink, NULL));
        rig_close(&imp);
        free(t);
    }
    printf("  %ld mutated SQL statements, %ld sync batches, %ld text imports: no crash, database intact\n",
           n_sql, n_sync, n_imp);
    tbuf_free(&exp);
    rig_free(&dev);
    rig_free(&rep);
    rig_free(&imp);
    return t_report("test_fuzz");
}
