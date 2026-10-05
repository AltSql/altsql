/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* The SQL workload the crash matrix and fault injection share since 0.3: a table with three
 * indexes, one of them UNIQUE, and transactions of INSERT, INSERT OR REPLACE, UPDATE and
 * DELETE through the indexes, each with a statement made to fail partway, which its
 * savepoint takes back. Deterministic by seed: a run without faults gives the table's
 * contents after each commit, as one hash. */
#ifndef WORKLOAD_SQL_H
#define WORKLOAD_SQL_H
#include "dbtest.h"

typedef struct swl { uint32_t seed, ntx, ops, ps; } swl;
static unsigned long g_swl_back;                         /* statements a savepoint took back */

static int swl_rows(void *ctx, int n, const altsql_value *v, const char *const *names) {
    uint64_t *h = (uint64_t *)ctx;
    int i;
    size_t j;
    (void)names;
    for (i = 0; i < n; i++) {
        const uint8_t *p = v[i].type == ALTSQL_TEXT ? (const uint8_t *)v[i].u.s : (const uint8_t *)&v[i].u;
        size_t len = v[i].type == ALTSQL_TEXT ? (size_t)v[i].len : sizeof v[i].u;
        for (j = 0; j < len; j++) *h = (*h ^ p[j]) * 1099511628211ULL;
    }
    *h = (*h ^ 0xFF) * 1099511628211ULL;
    return 0;
}

/* The table's rows as one hash; 0 when it cannot be read. */
static uint64_t swl_hash(altsql_db *db) {
    uint64_t h = 1469598103934665603ULL;
    return altsql_db_exec(db, "SELECT * FROM s", swl_rows, &h) == ALTSQL_OK ? h : 0;
}

static int swl_setup(altsql_db *db) {
    return altsql_db_exec(db, "CREATE TABLE s (id INT PRIMARY KEY, k INT, u INT, t TEXT); CREATE INDEX s_k ON s (k); "
                              "CREATE UNIQUE INDEX s_u ON s (u); CREATE INDEX s_tk ON s (t, k)", NULL, NULL);
}

static void swl_stmt(const swl *w, uint32_t t, uint32_t i, char *sql, size_t n, int *must_fail) {
    uint32_t s = (w->seed * 1000003u) ^ (t * 7919u + 1) ^ (i * 104729u) ^ 0x5bd1e995u, r, x, y;
    char pad[64];
    xs(&s); xs(&s); xs(&s);
    r = xs(&s) % 100; x = xs(&s); y = xs(&s);
    memset(pad, 'p', (size_t)(x % 48));                  /* rows of different lengths */
    pad[x % 48] = 0;
    *must_fail = 0;
    if (r < 35) snprintf(sql, n, "INSERT OR REPLACE INTO s VALUES (%u, %u, %u, 't%u%s')", x % 400, y % 20, x % 5000, y % 9, pad);
    else if (r < 50) snprintf(sql, n, "INSERT INTO s VALUES (%u, %u, %u, 'n%u'), (%u, %u, %u, 'n%u')", x % 400, y % 20, y % 5000, x % 7, y % 400, x % 20, x % 4999, y % 7);
    else if (r < 65) snprintf(sql, n, "UPDATE s SET t = 'u%u%s', u = u + %u WHERE k = %u", y % 9, pad, 5000 + x % 100000, y % 20);
    else if (r < 75) snprintf(sql, n, "UPDATE s SET id = id + 400 WHERE k = %u AND id < 400", x % 20);
    else if (r < 85) snprintf(sql, n, "DELETE FROM s WHERE t >= 't%u' AND t < 'u' AND k = %u", x % 9, y % 20);
    else { snprintf(sql, n, "UPDATE s SET t = 'gone', k = k / (id - %u) WHERE k < %u", x % 400, 1 + y % 20); *must_fail = 1; }
}

/* One transaction. 0, or the first error the file gave. */
static int swl_txn(const swl *w, altsql_db *db, uint32_t t, int *in_commit) {
    char sql[300];
    uint32_t i;
    int rc, must_fail;
    *in_commit = 0;
    if ((rc = altsql_db_begin(db, 1)) != 0) return rc;
    for (i = 0; i < w->ops; i++) {
        swl_stmt(w, t, i, sql, sizeof sql, &must_fail);
        rc = altsql_db_exec(db, sql, NULL, NULL);
        if (rc == ALTSQL_OK) continue;
        if (rc == ALTSQL_EXISTS || (rc == ALTSQL_SCHEMA && must_fail)) { g_swl_back++; continue; }   /* refusals: taken back */
        altsql_db_rollback(db);
        return rc;
    }
    *in_commit = 1;
    return altsql_db_commit(db);
}
#endif
