/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: secondary indexes (0.3).
 * A random workload on three tables (an int key, a text and int key, no key) with up to
 * nine indexes, UNIQUE ones among them, made and dropped as it runs: SQL INSERT, INSERT OR
 * REPLACE, UPDATE (of indexed columns, of keys, through index plans), DELETE, the row calls,
 * transactions committed and rolled back, statements made to fail inside them. Checked:
 *   - altsql_db_check finds every index consistent with its table, at random points (after
 *     one step or transaction in eight) and at the end of each run;
 *   - every query a random WHERE makes gives the same rows through its plan as a full scan;
 *   - a failed statement leaves the tables and their indexes as they were;
 *   - index cursors give each index's rows in order, forwards, backwards and by prefix;
 *   - UNIQUE refuses what it should, and the limits and errors are the documented ones;
 *   - a synced table keeps its index as devices send rows and retention deletes them;
 *   - a version 2 file (0.2) opens and is written as version 3.
 *   test_index          the full run
 *   test_index quick    fewer steps */
#include "dbtest.h"

static altsql_db *D;
static uint32_t g_ps;
static long g_steps, g_checks, g_queries, g_iplans, g_refused, g_failed, g_ddl, g_curs, g_txns, g_prep;

typedef struct hsh { uint64_t h, sum; long rows; } hsh;
static uint64_t vhash(int n, const altsql_value *v) {
    uint64_t h = 1469598103934665603ULL;
    int i;
    size_t j;
    for (i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)&v[i].u;
        size_t len = v[i].type == ALTSQL_TEXT ? (size_t)v[i].len : sizeof v[i].u;
        if (v[i].type == ALTSQL_TEXT) p = (const uint8_t *)v[i].u.s;
        h = (h ^ (uint64_t)v[i].type) * 1099511628211ULL;
        for (j = 0; j < len; j++) h = (h ^ p[j]) * 1099511628211ULL;
    }
    return h;
}
static int hrow(void *ctx, int n, const altsql_value *v, const char *const *names) {
    hsh *x = (hsh *)ctx;
    uint64_t r = vhash(n, v);
    (void)names;
    x->h = (x->h ^ r) * 1099511628211ULL;       /* in order */
    x->sum += r * 0x9E3779B97F4A7C15ULL;        /* as a set of rows */
    x->rows++;
    return 0;
}
static void hinit(hsh *x) { x->h = 1469598103934665603ULL; x->sum = 0; x->rows = 0; }
static int qh(const char *sql, hsh *x) { hinit(x); return altsql_db_exec(D, sql, hrow, x); }

static char g_plan[64];
static int plan_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)names;
    if (n >= 1 && v[0].type == ALTSQL_TEXT) snprintf(g_plan, sizeof g_plan, "%.*s", v[0].len, v[0].u.s);
    return 0;
}

/* ---- the tables and their indexes ---- */
typedef struct idef { const char *name, *table, *cols, *order; int unique, on; } idef;
static idef g_idx[] = {
    { "a_k",  "a", "k",    "k, id",    0, 0 },
    { "a_gs", "a", "g, s", "g, s, id", 0, 0 },
    { "a_u",  "a", "u",    "u",        1, 0 },
    { "a_r",  "a", "r",    "r, id",    0, 0 },
    { "b_x",  "b", "x",    "x, name, n", 0, 0 },
    { "b_yx", "b", "y, x", "y, x, name, n", 0, 0 },
    { "b_ny", "b", "n, y", "n, y",     1, 0 },
    { "c_v",  "c", "v",    NULL,       0, 0 },
    { "c_w",  "c", "w",    NULL,       1, 0 },
};
#define NIDX (int)(sizeof g_idx / sizeof g_idx[0])
static int g_rollback_ddl;                          /* DDL inside the transaction: the index states are unsure after a rollback */

static int idx_state_refresh(void) {               /* what the catalog says, after a rollback */
    altsql_db_tableinfo ti;
    static const char *const tabs[3] = { "a", "b", "c" };
    int t, i, j;
    for (i = 0; i < NIDX; i++) g_idx[i].on = 0;
    for (t = 0; t < 3; t++) {
        CHECK(altsql_db_table_info(D, tabs[t], &ti) == ALTSQL_OK, "table_info %s: %s", tabs[t], altsql_db_errmsg(D));
        for (j = 0; j < ti.nindex; j++)
            for (i = 0; i < NIDX; i++) if (!strcmp(g_idx[i].name, ti.index[j].name)) g_idx[i].on = 1;
    }
    return 0;
}

static int toggle_index(uint32_t *s) {
    idef *I = &g_idx[xs(s) % NIDX];
    char sql[160];
    int rc;
    if (I->on) {
        if (xs(s) % 2) snprintf(sql, sizeof sql, "DROP INDEX %s", I->name), rc = altsql_db_exec(D, sql, NULL, NULL);
        else rc = altsql_db_index_drop(D, I->name);
        CHECK(rc == ALTSQL_OK, "drop index %s: %d %s", I->name, rc, altsql_db_errmsg(D));
        I->on = 0;
    } else {
        if (xs(s) % 2) {
            snprintf(sql, sizeof sql, "CREATE %sINDEX %s ON %s (%s)", I->unique ? "UNIQUE " : "", I->name, I->table, I->cols);
            rc = altsql_db_exec(D, sql, NULL, NULL);
        } else rc = altsql_db_index_create(D, I->table, I->name, I->cols, I->unique);
        CHECK(rc == ALTSQL_OK || (I->unique && rc == ALTSQL_EXISTS) || rc == ALTSQL_TOOBIG,
              "create index %s: %d %s", I->name, rc, altsql_db_errmsg(D));
        I->on = rc == ALTSQL_OK;
        if (rc) g_refused++;
    }
    g_ddl++;
    return 0;
}

/* ---- random values ---- */
static void txt(uint32_t *s, char *out, const char *pre, uint32_t dom) { sprintf(out, "%s%u", pre, xs(s) % dom); }
static void lit_real(char *b, size_t n, double r) { snprintf(b, n, "%.17g", r); if (!strpbrk(b, ".eE")) strcat(b, ".0"); }
static double rreal(uint32_t *s) { return (double)((int)(xs(s) % 4001) - 2000) / 16.0; }

static int row_a(uint32_t *s, char *out, size_t n) {
    char sv[16], rl[40];
    txt(s, sv, "s", 31);
    lit_real(rl, sizeof rl, rreal(s));
    return snprintf(out, n, "(%u, %u, %u, '%s', %u, %s)", xs(s) % 3000, xs(s) % 50, xs(s) % 10, sv, xs(s) % 20000, rl);
}
static int row_b(uint32_t *s, char *out, size_t n) {
    char nm[16], rl[40];
    txt(s, nm, "n", 40);
    lit_real(rl, sizeof rl, rreal(s));
    return snprintf(out, n, "('%s', %u, %s, %u)", nm, xs(s) % 60, rl, xs(s) % 30);
}
static int row_c(uint32_t *s, char *out, size_t n) {
    char w[16];
    txt(s, w, "w", 50000);
    return snprintf(out, n, "(%u, %u, '%s')", xs(s) % 100, xs(s) % 40, w);
}

/* A WHERE for table t (0 a, 1 b, 2 c) that indexes may serve. */
static void pred(uint32_t *s, int t, char *w, size_t n) {
    char sv[16], rl[40];
    unsigned x = xs(s), y = xs(s);
    lit_real(rl, sizeof rl, rreal(s));
    if (t == 0) {
        txt(s, sv, "s", 31);
        switch (xs(s) % 12) {
        case 0: snprintf(w, n, "k = %u", x % 50); break;
        case 1: snprintf(w, n, "k BETWEEN %u AND %u", x % 50, x % 50 + y % 10); break;
        case 2: snprintf(w, n, "k > %u", x % 55); break;
        case 3: snprintf(w, n, "g = %u AND s = '%s'", x % 10, sv); break;
        case 4: snprintf(w, n, "g = %u AND s > '%s'", x % 10, sv); break;
        case 5: snprintf(w, n, "g = %u", x % 11); break;
        case 6: snprintf(w, n, "u = %u", x % 20000); break;
        case 7: snprintf(w, n, "u BETWEEN %u AND %u", x % 20000, x % 20000 + y % 500); break;
        case 8: snprintf(w, n, "r > %s", rl); break;
        case 9: snprintf(w, n, "id > %u AND k = %u", x % 3000, y % 50); break;
        case 10: snprintf(w, n, "k = %u AND g = %u AND r <= %s", x % 50, y % 10, rl); break;
        default: snprintf(w, n, "s = '%s' AND id < %u", sv, x % 3000); break;
        }
    } else if (t == 1) {
        txt(s, sv, "n", 40);
        switch (xs(s) % 7) {
        case 0: snprintf(w, n, "x > %s", rl); break;
        case 1: snprintf(w, n, "y = %u", x % 31); break;
        case 2: snprintf(w, n, "y = %u AND x < %s", x % 30, rl); break;
        case 3: snprintf(w, n, "name = '%s' AND y = %u", sv, x % 30); break;
        case 4: snprintf(w, n, "n = %u AND y >= %u", x % 60, y % 30); break;
        case 5: snprintf(w, n, "n = %u AND y = %u", x % 60, y % 30); break;
        default: snprintf(w, n, "name >= '%s' AND x BETWEEN %s AND %u", sv, rl, y % 100); break;
        }
    } else {
        txt(s, sv, "w", 50000);
        switch (xs(s) % 4) {
        case 0: snprintf(w, n, "v = %u", x % 40); break;
        case 1: snprintf(w, n, "w = '%s'", sv); break;
        case 2: snprintf(w, n, "v BETWEEN %u AND %u AND t > %u", x % 40, x % 40 + 3, y % 100); break;
        default: snprintf(w, n, "w > '%s' AND v < %u", sv, y % 40); break;
        }
    }
}

static const char *const g_tab[3] = { "a", "b", "c" };
static const char *const g_key[3] = { "id", "name, n", "t, v, w" };

/* The rows through the plan against a full scan: WHERE (p) OR 0 = 1 gives the planner nothing. */
static int compare(uint32_t *s) {
    char w[200], sql[400];
    hsh x, y;
    int t = (int)(xs(s) % 3), rc;
    pred(s, t, w, sizeof w);
    snprintf(sql, sizeof sql, "EXPLAIN SELECT * FROM %s WHERE %s", g_tab[t], w);
    g_plan[0] = 0;
    CHECK(altsql_db_exec(D, sql, plan_cb, NULL) == ALTSQL_OK, "%s: %s", sql, altsql_db_errmsg(D));
    if (!strncmp(g_plan, "index", 5)) g_iplans++;
    snprintf(sql, sizeof sql, "SELECT * FROM %s WHERE %s", g_tab[t], w);
    CHECK((rc = qh(sql, &x)) == ALTSQL_OK, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
    snprintf(sql, sizeof sql, "SELECT * FROM %s WHERE (%s) OR 0 = 1", g_tab[t], w);
    CHECK(qh(sql, &y) == ALTSQL_OK, "%s: %s", sql, altsql_db_errmsg(D));
    CHECK(x.rows == y.rows && x.sum == y.sum, "%s (%s): %ld rows through the plan, %ld in a full scan, or other rows", w, g_plan, x.rows, y.rows);
    if (t < 2) {                                         /* sorted, the same rows in the same order */
        snprintf(sql, sizeof sql, "SELECT * FROM %s WHERE %s ORDER BY %s", g_tab[t], w, g_key[t]);
        CHECK(qh(sql, &x) == ALTSQL_OK, "%s", sql);
        snprintf(sql, sizeof sql, "SELECT * FROM %s WHERE (%s) OR 0 = 1 ORDER BY %s", g_tab[t], w, g_key[t]);
        CHECK(qh(sql, &y) == ALTSQL_OK && x.h == y.h, "%s: sorted rows differ", w);
    }
    snprintf(sql, sizeof sql, "SELECT COUNT(*), SUM(%s) FROM %s WHERE %s", t == 2 ? "v" : t ? "y" : "k", g_tab[t], w);
    CHECK(qh(sql, &x) == ALTSQL_OK, "%s", sql);
    snprintf(sql, sizeof sql, "SELECT COUNT(*), SUM(%s) FROM %s WHERE (%s) OR 0 = 1", t == 2 ? "v" : t ? "y" : "k", g_tab[t], w);
    CHECK(qh(sql, &y) == ALTSQL_OK && x.h == y.h, "%s: aggregates differ", w);
    g_queries++;
    return 0;
}

/* Each index walked with its cursor, forwards and backwards, against SQL sorted the same way. */
static uint64_t g_fw[20000];
static int cursors(void) {
    int i;
    for (i = 0; i < NIDX; i++) {
        const idef *I = &g_idx[i];
        altsql_db_cursor c;
        altsql_value v[ALTSQL_DB_MAXCOLS];
        hsh x, y;
        long n = 0, m;
        int rc, nc = I->table[0] == 'a' ? 6 : I->table[0] == 'b' ? 4 : 3;
        char sql[200];
        if (!I->on) continue;
        hinit(&x);
        rc = altsql_db_index_seek(&c, D, I->name, NULL, 0);
        while (rc == ALTSQL_OK) {
            CHECK(altsql_db_row_read(&c, v, ALTSQL_DB_MAXCOLS) == ALTSQL_OK, "row_read through %s: %s", I->name, altsql_db_errmsg(D));
            hrow(&x, nc, v, NULL);
            if (n < 20000) g_fw[n] = vhash(nc, v);
            n++;
            rc = altsql_db_next(&c);
        }
        CHECK(rc == ALTSQL_NOTFOUND, "walk of %s: %d %s", I->name, rc, altsql_db_errmsg(D));
        if (I->order) snprintf(sql, sizeof sql, "SELECT * FROM %s ORDER BY %s", I->table, I->order);
        else snprintf(sql, sizeof sql, "SELECT * FROM %s", I->table);
        CHECK(qh(sql, &y) == ALTSQL_OK, "%s", sql);
        CHECK(x.rows == y.rows && x.sum == y.sum && (!I->order || x.h == y.h), "%s: the cursor gave %ld rows, SQL %ld, or another order", I->name, x.rows, y.rows);
        m = n;                                           /* backwards: the same rows, the other way */
        rc = altsql_db_index_last(&c, D, I->name, NULL, 0);
        while (rc == ALTSQL_OK) {
            CHECK(altsql_db_row_read(&c, v, ALTSQL_DB_MAXCOLS) == ALTSQL_OK, "row_read back");
            CHECK(m > 0 && (m > 20000 || g_fw[m - 1] == vhash(nc, v)), "%s backwards: row %ld differs", I->name, m);
            m--;
            rc = altsql_db_prev(&c);
        }
        CHECK(rc == ALTSQL_NOTFOUND && m == 0, "%s backwards: %ld rows left", I->name, m);
        if (I->table[0] == 'a' && !strcmp(I->cols, "k")) {   /* a prepared SELECT through the index, batch after batch */
            altsql_db_stmt *st;
            static const char *const q = "SELECT * FROM a WHERE k BETWEEN 3 AND 46";
            hsh p2;
            CHECK(altsql_db_prepare(D, q, &st) == ALTSQL_OK, "prepare: %s", altsql_db_errmsg(D));
            hinit(&x);
            while ((rc = altsql_db_step(st)) == ALTSQL_OK) {
                int k2;
                for (k2 = 0; k2 < 6; k2++) CHECK(altsql_db_column(st, k2, &v[k2]) == ALTSQL_OK, "column");
                hrow(&x, 6, v, NULL);
            }
            altsql_db_finalize(st);
            CHECK(rc == ALTSQL_DONE && qh(q, &p2) == ALTSQL_OK && x.rows == p2.rows && x.h == p2.h,
                  "prepared through the index: %ld rows, exec %ld, or another order (%d)", x.rows, p2.rows, rc);
            g_prep += x.rows;
        }
        if (I->table[0] == 'a' && !strcmp(I->cols, "k")) {   /* by prefix: k = 7 */
            altsql_value p;
            p.type = ALTSQL_INTEGER; p.len = 0; p.u.i = 7;
            hinit(&x);
            rc = altsql_db_index_seek(&c, D, I->name, &p, 1);
            while (rc == ALTSQL_OK) { CHECK(altsql_db_row_read(&c, v, ALTSQL_DB_MAXCOLS) == ALTSQL_OK, "row_read"); hrow(&x, nc, v, NULL); rc = altsql_db_next(&c); }
            CHECK(qh("SELECT * FROM a WHERE k = 7 ORDER BY id", &y) == ALTSQL_OK && x.h == y.h && x.rows == y.rows, "prefix k = 7: %ld rows, SQL %ld", x.rows, y.rows);
        }
        g_curs += n;
    }
    return 0;
}

static int check_now(void) {
    altsql_db_check_report r;
    int rc, i, on = 0;
    rc = altsql_db_check(D, -1, g_chk, g_chksize, &r);
    CHECK(rc == ALTSQL_OK, "check: %d %s", rc, altsql_db_errmsg(D));
    for (i = 0; i < NIDX; i++) on += g_idx[i].on;
    CHECK((int)r.indexes == on, "check saw %u indexes, %d are made", r.indexes, on);
    CHECK(pins_clear(D), "pins left");
    g_checks++;
    return 0;
}

/* One random write. Refusals are fine: UNIQUE, keys already there, values that don't fit. */
static int step(uint32_t *s, int in_tx) {
    char sql[700], r1[200], r2[200], w[200];
    int op = (int)(xs(s) % 100), rc, t = (int)(xs(s) % 3);
    g_steps++;
    if (op < 22) {                                       /* INSERT, one row or a few */
        int k, nr = xs(s) % 4 == 0 ? 2 + (int)(xs(s) % 3) : 1, len;
        len = snprintf(sql, sizeof sql, "INSERT %sINTO %s VALUES ", xs(s) % 3 == 0 ? "OR REPLACE " : "", g_tab[t]);
        for (k = 0; k < nr; k++) {
            if (t == 0) row_a(s, r1, sizeof r1); else if (t == 1) row_b(s, r1, sizeof r1); else row_c(s, r1, sizeof r1);
            len += snprintf(sql + len, sizeof sql - (size_t)len, "%s%s", k ? ", " : "", r1);
        }
        rc = altsql_db_exec(D, sql, NULL, NULL);
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_EXISTS, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
        if (rc) g_refused++;
    } else if (op < 32) {                                /* the row calls */
        altsql_value v[6];
        char sv[16];
        if (t == 2) t = 0;
        if (t == 0) {
            txt(s, sv, "s", 31);
            v[0].type = v[1].type = v[2].type = v[4].type = ALTSQL_INTEGER;
            v[0].u.i = xs(s) % 3000; v[1].u.i = xs(s) % 50; v[2].u.i = xs(s) % 10; v[4].u.i = xs(s) % 20000;
            v[3].type = ALTSQL_TEXT; v[3].u.s = sv; v[3].len = (int)strlen(sv);
            v[5].type = ALTSQL_REAL; v[5].u.r = rreal(s);
            v[0].len = v[1].len = v[2].len = v[4].len = v[5].len = 0;
        } else {
            txt(s, sv, "n", 40);
            v[0].type = ALTSQL_TEXT; v[0].u.s = sv; v[0].len = (int)strlen(sv);
            v[1].type = ALTSQL_INTEGER; v[1].u.i = xs(s) % 60; v[1].len = 0;
            v[2].type = ALTSQL_REAL; v[2].u.r = rreal(s); v[2].len = 0;
            v[3].type = ALTSQL_INTEGER; v[3].u.i = xs(s) % 30; v[3].len = 0;
        }
        switch (xs(s) % 3) {
        case 0: rc = altsql_db_row_put(D, g_tab[t], v, t ? 4 : 6); break;
        case 1: rc = altsql_db_row_insert(D, g_tab[t], v, t ? 4 : 6); break;
        default: rc = altsql_db_row_del(D, g_tab[t], v, t ? 2 : 1); if (rc == ALTSQL_NOTFOUND) rc = ALTSQL_OK; break;
        }
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_EXISTS, "row call on %s: %d %s", g_tab[t], rc, altsql_db_errmsg(D));
        if (rc) g_refused++;
    } else if (op < 52) {                                /* UPDATE: indexed columns, through plans */
        pred(s, t, w, sizeof w);
        if (t == 0) {
            static const char *const sets[] = { "k = k + 1", "g = %u, s = 's%u'", "u = u + %u", "r = r * 2, k = %u", "s = 's%u'", "id = id + 3000" };
            unsigned j = xs(s) % 6;
            snprintf(r1, sizeof r1, sets[j], xs(s) % 10, xs(s) % 31);
            if (j == 5) snprintf(r1, sizeof r1, "id = id + %u", 3000 + xs(s) % 3000);
        } else if (t == 1) snprintf(r1, sizeof r1, xs(s) % 2 ? "x = x + 1, y = %u" : "n = n + %u", xs(s) % 30);
        else snprintf(r1, sizeof r1, xs(s) % 2 ? "v = %u" : "w = 'w%u'", xs(s) % 50000);
        snprintf(sql, sizeof sql, "UPDATE %s SET %s WHERE %s", g_tab[t], r1, w);
        rc = altsql_db_exec(D, sql, NULL, NULL);
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_EXISTS || rc == ALTSQL_SCHEMA || rc == ALTSQL_NOMEM, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
        if (rc) g_refused++;
    } else if (op < 62) {                                /* DELETE through plans */
        pred(s, t, w, sizeof w);
        snprintf(sql, sizeof sql, "DELETE FROM %s WHERE %s", g_tab[t], w);
        CHECK((rc = altsql_db_exec(D, sql, NULL, NULL)) == ALTSQL_OK, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
    } else if (op < 70 && in_tx) {                       /* a statement that fails partway: nothing of it stays */
        hsh before[3], after[3];
        int i;
        for (i = 0; i < 3; i++) { snprintf(r2, sizeof r2, "SELECT * FROM %s", g_tab[i]); CHECK(qh(r2, &before[i]) == ALTSQL_OK, "before"); }
        if (t == 0) {
            unsigned x = xs(s) % 3000;
            snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO a VALUES (%u, %u, 3, 'zz', %u, 1.0)", x, xs(s) % 50, 20000 + xs(s) % 10000);
            rc = altsql_db_exec(D, sql, NULL, NULL);
            CHECK(rc == ALTSQL_OK || rc == ALTSQL_EXISTS, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
            if (rc == ALTSQL_EXISTS) return 0;
            for (i = 0; i < 3; i++) { snprintf(r2, sizeof r2, "SELECT * FROM %s", g_tab[i]); CHECK(qh(r2, &before[i]) == ALTSQL_OK, "before"); }
            snprintf(sql, sizeof sql, "UPDATE a SET r = r + 1, s = 'gone', k = k / (id - %u) WHERE g = 3", x);
        } else if (t == 1) snprintf(sql, sizeof sql, "UPDATE b SET y = y + 1, n = n + 100, x = x / (n - n) WHERE y < %u", 1 + xs(s) % 30);
        else snprintf(sql, sizeof sql, "INSERT INTO c VALUES (1, 1, 'fresh%u'), (2, 2, 'fresh%u'), (3, 3, %u)", xs(s), xs(s), xs(s));
        rc = altsql_db_exec(D, sql, NULL, NULL);
        if (rc == ALTSQL_OK) {                           /* no row to fail on: then it changed none */
            altsql_db_info inf;
            altsql_db_info_get(D, &inf);
            CHECK(inf.sql_changed == 0, "%s: should fail partway, changed %llu rows", sql, (unsigned long long)inf.sql_changed);
            return 0;
        }
        CHECK(rc == ALTSQL_SCHEMA, "%s: should fail partway, got %d %s", sql, rc, altsql_db_errmsg(D));
        for (i = 0; i < 3; i++) {
            snprintf(r2, sizeof r2, "SELECT * FROM %s", g_tab[i]);
            CHECK(qh(r2, &after[i]) == ALTSQL_OK && after[i].h == before[i].h && after[i].rows == before[i].rows,
                  "%s: table %s changed by a failed statement", sql, g_tab[i]);
        }
        for (i = 0; i < 4; i++) if (compare(s)) return 1;    /* and the indexes read as before */
        g_failed++;
    } else if (op < 74) {                                /* an index made or dropped, inside a transaction or not */
        if (in_tx) g_rollback_ddl = 1;
        return toggle_index(s);
    } else return compare(s);
    return 0;
}

static int run(uint32_t seed, long steps, uint32_t ps, size_t mem) {
    ramfile f;
    altsql_db_config cfg;
    uint8_t *m = (uint8_t *)malloc(mem);
    uint32_t s = seed;
    long i;
    int k, rc;
    g_ps = ps;
    ram_new(&f, 1u << 27, 1u << 27);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = m; cfg.mem_size = mem; cfg.page_size = ps; cfg.create = 1; cfg.sql_mem = 512u << 10;
    CHECK(altsql_db_open(&D, &f.f, &cfg) == ALTSQL_OK, "open");
    CHECK(altsql_db_exec(D, "CREATE TABLE a (id INT PRIMARY KEY, k INT, g INT, s TEXT, u INT, r REAL); "
                            "CREATE TABLE b (name TEXT, n INT, x REAL, y INT, PRIMARY KEY (name, n)); "
                            "CREATE TABLE c (t INT, v INT, w TEXT)", NULL, NULL) == ALTSQL_OK, "create: %s", altsql_db_errmsg(D));
    for (k = 0; k < NIDX; k++) g_idx[k].on = 0;
    for (k = 0; k < 6; k++) if (toggle_index(&s)) return 1;      /* some indexes first, the rest come and go */
    CHECK(altsql_db_begin(D, 1) == ALTSQL_OK, "begin");
    for (k = 0; k < 1500; k++) {                         /* rows to start with */
        char sql[300], r1[200];
        int t = k % 3;
        if (t == 0) row_a(&s, r1, sizeof r1); else if (t == 1) row_b(&s, r1, sizeof r1); else row_c(&s, r1, sizeof r1);
        snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO %s VALUES %s", g_tab[t], r1);
        rc = altsql_db_exec(D, sql, NULL, NULL);
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_EXISTS, "%s: %d %s", sql, rc, altsql_db_errmsg(D));
    }
    CHECK(altsql_db_commit(D) == ALTSQL_OK, "commit");
    if (check_now()) return 1;
    for (i = 0; i < steps; ) {
        if (xs(&s) % 4 == 0) {                           /* a transaction of a few steps */
            int n = 1 + (int)(xs(&s) % 10), j, back = xs(&s) % 4 == 0;
            g_rollback_ddl = 0;
            CHECK(altsql_db_begin(D, 1) == ALTSQL_OK, "begin");
            for (j = 0; j < n && i < steps; j++, i++) if (step(&s, 1)) return 1;
            if (back) CHECK(altsql_db_rollback(D) == ALTSQL_OK, "rollback");
            else CHECK((rc = altsql_db_commit(D)) == ALTSQL_OK, "commit: %d %s", rc, altsql_db_errmsg(D));
            if (back && g_rollback_ddl && idx_state_refresh()) return 1;
            g_txns++;
        } else {
            if (step(&s, 0)) return 1;
            i++;
        }
        if (xs(&s) % 8 == 0 && check_now()) return 1;
        if (xs(&s) % 60 == 0 && cursors()) return 1;
    }
    if (check_now() || cursors()) return 1;
    if (check_slots(D, 1)) return 1;
    altsql_db_close(D);
    ram_free(&f);
    free(m);
    return 0;
}

/* ---- UNIQUE, limits and errors, one by one ---- */
static int rules(void) {
    ramfile f;
    static uint8_t mem[4u << 20];
    altsql_value v[3];
    hsh x;
    char sql[160];
    int i;
    ram_new(&f, 1u << 24, 1u << 24);
    CHECK(db_open_ram(&D, &f, mem, sizeof mem, 4096) == ALTSQL_OK, "open");
    CHECK(altsql_db_exec(D, "CREATE TABLE p (id INT PRIMARY KEY, e TEXT, n INT); INSERT INTO p VALUES (1, 'x', 1), (2, 'x', 2), (3, 'y', 3)", NULL, NULL) == ALTSQL_OK, "setup");
    CHECK(altsql_db_exec(D, "CREATE UNIQUE INDEX pe ON p (e)", NULL, NULL) == ALTSQL_EXISTS, "a UNIQUE index over two equal rows is refused");
    CHECK(altsql_db_exec(D, "EXPLAIN SELECT * FROM p WHERE e = 'x'", plan_cb, NULL) == ALTSQL_OK && !strcmp(g_plan, "full scan"), "and is not there: %s", g_plan);
    CHECK(altsql_db_exec(D, "CREATE UNIQUE INDEX pn ON p (n)", NULL, NULL) == ALTSQL_OK, "unique on n");
    CHECK(altsql_db_exec(D, "INSERT INTO p VALUES (4, 'z', 2)", NULL, NULL) == ALTSQL_EXISTS, "INSERT of a value a UNIQUE index has");
    CHECK(altsql_db_exec(D, "INSERT OR REPLACE INTO p VALUES (2, 'w', 2)", NULL, NULL) == ALTSQL_OK, "REPLACE of the same row keeps its own value");
    CHECK(altsql_db_exec(D, "INSERT OR REPLACE INTO p VALUES (4, 'w', 2)", NULL, NULL) == ALTSQL_EXISTS, "REPLACE that takes another row's value");
    CHECK(altsql_db_exec(D, "UPDATE p SET n = 3 WHERE id = 1", NULL, NULL) == ALTSQL_EXISTS, "UPDATE to another row's value");
    CHECK(altsql_db_exec(D, "UPDATE p SET n = n + 10", NULL, NULL) == ALTSQL_OK, "UPDATE of every row, no clash");
    CHECK(altsql_db_exec(D, "UPDATE p SET id = id + 100 WHERE n = 12", NULL, NULL) == ALTSQL_OK, "a key move keeps the UNIQUE entry's row");
    v[0].type = ALTSQL_INTEGER; v[0].u.i = 9; v[0].len = 0;
    v[1].type = ALTSQL_TEXT; v[1].u.s = "q"; v[1].len = 1;
    v[2].type = ALTSQL_INTEGER; v[2].u.i = 11; v[2].len = 0;
    CHECK(altsql_db_row_insert(D, "p", v, 3) == ALTSQL_EXISTS && altsql_db_row_put(D, "p", v, 3) == ALTSQL_EXISTS, "the row calls keep UNIQUE too");
    CHECK(altsql_db_begin(D, 1) == ALTSQL_OK, "begin");
    CHECK(altsql_db_exec(D, "INSERT INTO p VALUES (50, 'a', 50)", NULL, NULL) == ALTSQL_OK, "insert in the transaction");
    CHECK(altsql_db_exec(D, "INSERT INTO p VALUES (51, 'b', 51), (52, 'c', 50)", NULL, NULL) == ALTSQL_EXISTS, "two rows, the second clashes");
    CHECK(qh("SELECT * FROM p WHERE id BETWEEN 50 AND 60", &x) == ALTSQL_OK && x.rows == 1, "the failed statement left nothing: %ld rows", x.rows);
    CHECK(altsql_db_commit(D) == ALTSQL_OK, "the transaction goes on and commits");
    CHECK(altsql_db_exec(D, "CREATE INDEX p ON p (e)", NULL, NULL) == ALTSQL_EXISTS, "a table's name");
    CHECK(altsql_db_exec(D, "CREATE INDEX pn ON p (e)", NULL, NULL) == ALTSQL_EXISTS, "an index's name");
    CHECK(altsql_db_exec(D, "CREATE INDEX IF NOT EXISTS pn ON p (e)", NULL, NULL) == ALTSQL_OK, "IF NOT EXISTS");
    CHECK(altsql_db_exec(D, "CREATE TABLE pn (a INT)", NULL, NULL) == ALTSQL_EXISTS, "a table may not take an index's name");
    CHECK(altsql_db_exec(D, "SELECT * FROM pn", NULL, NULL) == ALTSQL_MISUSE, "an index is not read as a table");
    CHECK(altsql_db_exec(D, "CREATE INDEX q1 ON p (nosuch)", NULL, NULL) == ALTSQL_SCHEMA, "a column the table lacks");
    CHECK(altsql_db_exec(D, "CREATE INDEX q2 ON nosuch (a)", NULL, NULL) == ALTSQL_SCHEMA, "a table that is not there");
    CHECK(altsql_db_exec(D, "CREATE INDEX q3 ON p (e, n, id, e)", NULL, NULL) == ALTSQL_SCHEMA, "a column twice");
    CHECK(altsql_db_exec(D, "CREATE INDEX q4 ON p (e, n, id, e, n)", NULL, NULL) == ALTSQL_SCHEMA, "five columns");
    CHECK(altsql_db_index_create(D, "p", "q5", "e,n,id,e,n", 0) == ALTSQL_SCHEMA, "five columns, directly");
    CHECK(altsql_db_exec(D, "DROP INDEX nosuch", NULL, NULL) == ALTSQL_SCHEMA && altsql_db_exec(D, "DROP INDEX IF EXISTS nosuch", NULL, NULL) == ALTSQL_OK, "drop what is not there");
    CHECK(altsql_db_index_drop(D, "p") == ALTSQL_MISUSE, "a table is not dropped as an index");
    for (i = 0; i < 7; i++) {
        snprintf(sql, sizeof sql, "CREATE INDEX m%d ON p (e, n)", i);
        CHECK(altsql_db_exec(D, sql, NULL, NULL) == ALTSQL_OK, "index %d: %s", i, altsql_db_errmsg(D));
    }
    CHECK(altsql_db_exec(D, "CREATE INDEX m7 ON p (e)", NULL, NULL) == ALTSQL_FULL, "a ninth index");
    CHECK(altsql_db_begin(D, 1) == ALTSQL_OK && altsql_db_exec(D, "DROP INDEX m0; DROP INDEX m1; CREATE INDEX m8 ON p (n)", NULL, NULL) == ALTSQL_OK &&
          altsql_db_rollback(D) == ALTSQL_OK, "DDL rolled back");
    CHECK(altsql_db_exec(D, "CREATE INDEX m8 ON p (n)", NULL, NULL) == ALTSQL_FULL, "the rollback brought the two back");
    CHECK(altsql_db_exec(D, "DROP TABLE p", NULL, NULL) == ALTSQL_OK, "drop the table with its indexes");
    CHECK(altsql_db_exec(D, "CREATE TABLE m0 (a INT); CREATE TABLE pn (b INT)", NULL, NULL) == ALTSQL_OK, "the indexes' names are free again: %s", altsql_db_errmsg(D));
    CHECK(check_slots(D, 1) == 0, "check");
    altsql_db_close(D);
    ram_free(&f);
    {   /* 512-byte pages: keys of up to 113 bytes. Long text in an index is refused, not cut */
        static uint8_t m2[1u << 20];
        char big[120];
        ram_new(&f, 1u << 24, 1u << 24);
        CHECK(db_open_ram(&D, &f, m2, sizeof m2, 512) == ALTSQL_OK, "open 512");
        CHECK(altsql_db_exec(D, "CREATE TABLE l (id INT PRIMARY KEY, t TEXT); INSERT INTO l VALUES (1, 'short'); CREATE INDEX lt ON l (t)", NULL, NULL) == ALTSQL_OK, "setup 512");
        memset(big, 'x', 110); big[110] = 0;
        snprintf(sql, sizeof sql, "INSERT INTO l VALUES (2, '%s')", big);
        CHECK(altsql_db_exec(D, sql, NULL, NULL) == ALTSQL_TOOBIG, "a text too long for the index's key");
        CHECK(altsql_db_exec(D, "DROP INDEX lt", NULL, NULL) == ALTSQL_OK && altsql_db_exec(D, sql, NULL, NULL) == ALTSQL_OK, "without the index it goes in");
        CHECK(altsql_db_exec(D, "CREATE INDEX lt ON l (t)", NULL, NULL) == ALTSQL_TOOBIG, "an index its rows don't fit is refused");
        CHECK(check_slots(D, 1) == 0, "check 512");
        altsql_db_close(D);
        ram_free(&f);
    }
    printf("  UNIQUE, names, limits (8 indexes, 4 columns, keys of the page size), DDL in transactions: as documented\n");
    return 0;
}

/* ---- a synced table with an index ---- */
typedef struct dev { uint8_t *flash, *mem; altsql_ram_flash ram; altsql_flash fl; altsql_config cfg; altsql *db; uint32_t confirmed, t; } dev;
static int dev_sync(dev *d, int64_t id) {
    static uint8_t buf[1 << 16];
    size_t n;
    uint32_t last, conf;
    int rc;
    do {
        rc = altsql_sync_read(d->db, d->confirmed, buf, sizeof buf, &n, &last, NULL, NULL);
        CHECK(rc >= 0, "sync_read");
        if (n) {
            CHECK(altsql_db_sync_apply(D, id, d->confirmed, buf, n, &conf) == ALTSQL_OK, "sync_apply: %s", altsql_db_errmsg(D));
            d->confirmed = conf;
        }
    } while (rc == ALTSQL_OK);
    return 0;
}
static int synced(void) {
    ramfile f;
    static uint8_t mem[4u << 20];
    dev d[3];
    uint32_t s = 99;
    int i, j, k;
    hsh x, y;
    ram_new(&f, 1u << 24, 1u << 24);
    CHECK(db_open_ram(&D, &f, mem, sizeof mem, 1024) == ALTSQL_OK, "open");
    for (i = 0; i < 3; i++) {
        memset(&d[i], 0, sizeof d[i]);
        d[i].flash = (uint8_t *)malloc(64 * 4096);
        d[i].mem = (uint8_t *)malloc(48 * 1024);
        memset(d[i].flash, 0xFF, 64 * 4096);
        altsql_ram_flash_init(&d[i].fl, &d[i].ram, d[i].flash, 4096, 64, 4);
        d[i].ram.budget = -1;
        d[i].cfg.mem = d[i].mem; d[i].cfg.mem_size = 48 * 1024; d[i].cfg.create = 1;
        d[i].t = 1700000000u;
        CHECK(altsql_open(&d[i].db, &d[i].fl, &d[i].cfg) == ALTSQL_OK, "device");
        CHECK(altsql_ts_create(d[i].db, "temps", "time:time,machine:int,temp:float") == ALTSQL_OK, "series");
    }
    for (k = 0; k < 6; k++) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 300; j++) {
                CHECK(altsql_append(d[i].db, "temps", (int64_t)d[i].t, (int)(xs(&s) % 12), (double)(xs(&s) % 4000) / 100.0) == ALTSQL_OK, "append");
                d[i].t += 1 + xs(&s) % 3;
            }
            if (dev_sync(&d[i], 100 + i)) return 1;
        }
        if (k == 1) {
            CHECK(altsql_db_exec(D, "CREATE UNIQUE INDEX tm ON temps (machine)", NULL, NULL) == ALTSQL_MISUSE, "no UNIQUE on a synced table");
            CHECK(altsql_db_exec(D, "CREATE INDEX tm ON temps (machine, temp)", NULL, NULL) == ALTSQL_OK, "index on the synced table: %s", altsql_db_errmsg(D));
        }
        if (k == 3) CHECK(altsql_db_exec(D, "DELETE FROM temps WHERE time < 1700000500", NULL, NULL) == ALTSQL_OK, "retention");
        if (k >= 1) {
            CHECK(check_slots(D, 0) == 0, "check after sync %d", k);
            CHECK(qh("SELECT * FROM temps WHERE machine = 5 ORDER BY device, time, seq", &x) == ALTSQL_OK &&
                  qh("SELECT * FROM temps WHERE (machine = 5) OR 0 = 1 ORDER BY device, time, seq", &y) == ALTSQL_OK &&
                  x.h == y.h && x.rows == y.rows && x.rows > 0, "machine = 5: %ld rows through the index, %ld by scan", x.rows, y.rows);
            CHECK(altsql_db_exec(D, "EXPLAIN SELECT * FROM temps WHERE machine = 5 AND temp > 10", plan_cb, NULL) == ALTSQL_OK && !strcmp(g_plan, "index range"), "plan: %s", g_plan);
        }
    }
    printf("  a synced table: an index made after the first rows, kept through 18 batches from 3 devices and a retention DELETE; %ld rows of machine 5 read through it\n", x.rows);
    for (i = 0; i < 3; i++) { altsql_close(d[i].db); free(d[i].flash); free(d[i].mem); }
    altsql_db_close(D);
    ram_free(&f);
    return 0;
}

/* ---- a file written by 0.2 ---- */
static int version2(void) {
    ramfile f;
    static uint8_t mem[1u << 20];
    altsql_db_config cfg;
    hsh x;
    int slot;
    ram_new(&f, 1u << 22, 1u << 22);
    CHECK(db_open_ram(&D, &f, mem, sizeof mem, 1024) == ALTSQL_OK, "open");
    CHECK(altsql_db_exec(D, "CREATE TABLE v (a INT PRIMARY KEY, b TEXT); INSERT INTO v VALUES (1, 'one'), (2, 'two')", NULL, NULL) == ALTSQL_OK, "setup");
    altsql_db_close(D);
    for (slot = 0; slot < 2; slot++) {                   /* both headers as 0.2 wrote them: version 2 */
        uint8_t *h = f.mem + (size_t)slot * 1024;
        CHECK(as_get32(h + 8) == 3, "0.3 writes version 3");
        as_put32(h + 8, 2);
        as_put32(h + 60, as_crc32(0, h, 60));
    }
    memcpy(f.disk, f.mem, (size_t)f.r.size);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = sizeof mem;
    CHECK(altsql_db_open(&D, &f.f, &cfg) == ALTSQL_OK, "a version 2 file opens: %s", D ? altsql_db_errmsg(D) : "");
    CHECK(qh("SELECT * FROM v", &x) == ALTSQL_OK && x.rows == 2, "its rows");
    CHECK(altsql_db_exec(D, "CREATE INDEX vb ON v (b); INSERT INTO v VALUES (3, 'three')", NULL, NULL) == ALTSQL_OK, "write");
    altsql_db_close(D);
    CHECK(as_get32(f.mem + 1024 * (size_t)0 + 8) == 3 || as_get32(f.mem + 1024 + 8) == 3, "the new header is version 3");
    CHECK(altsql_db_open(&D, &f.f, &cfg) == ALTSQL_OK && check_slots(D, 1) == 0, "reopen and check");
    altsql_db_close(D);
    ram_free(&f);
    printf("  a version 2 file (0.2) opens, reads, takes an index and is written as version 3\n");
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    long steps = quick ? 1500 : 12000;
    static const struct { uint32_t seed, ps; size_t mem; } runs[] = {
        { 1, 4096, 16u << 20 }, { 2, 1024, 4u << 20 }, { 3, 512, 2u << 20 }, { 4, 512, 700u << 10 }, { 5, 4096, 1u << 20 }, { 6, 1024, 900u << 10 },
    };
    unsigned i;
    printf("AltSql DB %s: secondary indexes\n", ALTSQL_DB_VERSION);
    if (!g_chk && !(g_chk = (uint8_t *)malloc(g_chksize))) return 1;
    for (i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        if (run(runs[i].seed, steps, runs[i].ps, runs[i].mem)) { printf("FAILED (seed %u)\n", runs[i].seed); return 1; }
    }
    printf("  6 runs (pages of 512 bytes to 4 KB, caches large and small): %ld steps, %ld transactions, %ld refusals, %ld index makes and drops\n",
           g_steps, g_txns, g_refused, g_ddl);
    printf("  %ld queries through their plans against full scans (%ld read an index), %ld checks, %ld rows walked by index cursors,\n"
           "  %ld rows read by a prepared SELECT through an index, batch after batch, as exec reads them\n",
           g_queries, g_iplans, g_checks, g_curs, g_prep);
    printf("  %ld statements failed partway inside transactions: tables and indexes as before each time\n", g_failed);
    if (rules() || synced() || version2()) { printf("FAILED\n"); return 1; }
    printf("test_index: all checks passed\n");
    return g_fail ? 1 : 0;
}
