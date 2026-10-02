/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: SQL compared against SQLite (step 10).
 * The same tables and rows in SQLite and in AltSql DB, then the same random
 * statements on both: SELECT with WHERE, expressions, IN lists, GROUP BY,
 * HAVING, ORDER BY, LIMIT and OFFSET, aggregates; INSERT and INSERT OR
 * REPLACE; UPDATE of values and of keys; DELETE. Every answer is compared
 * row by row (reals to nine significant digits, rows sorted when the
 * statement gives no order), every write must succeed or fail on both, and
 * after each write the whole table is compared. The generator keeps to SQL
 * whose meaning both engines share: no NULL stored, whole numbers kept small,
 * no division in values that are stored, % on whole numbers only (on reals
 * Core keeps the fraction and SQLite drops it), ORDER BY made total with every
 * column, and key changes that cannot meet another row's key part way
 * through (SQLite checks each row as it goes, AltSql DB at the end).
 *   test_vs_sqlite          four seeds of 25,000 statements
 *   test_vs_sqlite quick    one seed of 3,000
 * Needs SQLite: sh tools/fetch_third_party.sh, then make vs-sqlite. */
#include "dbtest.h"
#include "sqlite3.h"
#include <math.h>

static altsql_db *g_db;
static sqlite3 *g_sq;
static uint32_t g_s;
static unsigned long g_selects, g_writes, g_both_failed, g_rows, g_compared_tables, g_rand_selects, g_rand_writes, g_loads;

/* ---- results as typed rows ---- */
typedef struct val { int type; long long i; double r; char t[64]; } val;
typedef struct res { val *v; int ncol, nrows, cap; } res;
static void res_add(res *r, int ncol, const val *row) {
    if (r->nrows == r->cap) { r->cap = r->cap ? r->cap * 2 : 64; r->v = (val *)realloc(r->v, sizeof(val) * (size_t)r->cap * (size_t)ncol); }
    memcpy(r->v + (size_t)r->nrows * (size_t)ncol, row, sizeof(val) * (size_t)ncol);
    r->ncol = ncol;
    r->nrows++;
}
static int db_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    val row[16];
    int i;
    (void)names;
    memset(row, 0, sizeof row);
    for (i = 0; i < n && i < 16; i++) {
        row[i].type = v[i].type;
        if (v[i].type == ALTSQL_INTEGER) row[i].i = v[i].u.i;
        else if (v[i].type == ALTSQL_REAL) row[i].r = v[i].u.r;
        else if (v[i].type == ALTSQL_TEXT) { int l = v[i].len < 63 ? v[i].len : 63; memcpy(row[i].t, v[i].u.s, (size_t)l); }
    }
    res_add((res *)ctx, n < 16 ? n : 16, row);
    return 0;
}
static int sq_run(const char *sql, res *r) {
    sqlite3_stmt *st;
    int rc, i, n;
    if (sqlite3_prepare_v2(g_sq, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    n = sqlite3_column_count(st);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        val row[16];
        memset(row, 0, sizeof row);
        for (i = 0; i < n && i < 16; i++) {
            switch (sqlite3_column_type(st, i)) {
            case SQLITE_INTEGER: row[i].type = ALTSQL_INTEGER; row[i].i = sqlite3_column_int64(st, i); break;
            case SQLITE_FLOAT: row[i].type = ALTSQL_REAL; row[i].r = sqlite3_column_double(st, i); break;
            case SQLITE_TEXT: row[i].type = ALTSQL_TEXT; snprintf(row[i].t, sizeof row[i].t, "%s", (const char *)sqlite3_column_text(st, i)); break;
            default: row[i].type = ALTSQL_NULL;
            }
        }
        if (r) res_add(r, n < 16 ? n : 16, row);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}
static double num(const val *v) { return v->type == ALTSQL_INTEGER ? (double)v->i : v->r; }
static int val_eq(const val *a, const val *b) {
    int an = a->type == ALTSQL_INTEGER || a->type == ALTSQL_REAL, bn = b->type == ALTSQL_INTEGER || b->type == ALTSQL_REAL;
    if (an && bn) {
        double x = num(a), y = num(b), m = fabs(x) > fabs(y) ? fabs(x) : fabs(y);
        if (a->type == ALTSQL_INTEGER && b->type == ALTSQL_INTEGER) return a->i == b->i;
        return fabs(x - y) <= 1e-9 * (m > 1 ? m : 1);
    }
    if (a->type != b->type) return 0;
    return a->type != ALTSQL_TEXT || !strcmp(a->t, b->t);
}
static void val_str(const val *v, char *out, size_t cap) {
    if (v->type == ALTSQL_INTEGER) snprintf(out, cap, "%lld", v->i);
    else if (v->type == ALTSQL_REAL) snprintf(out, cap, "%.15g", v->r == 0 ? 0.0 : v->r);   /* -0 sorts with 0: equal numbers */
    else if (v->type == ALTSQL_TEXT) snprintf(out, cap, "'%s'", v->t);
    else snprintf(out, cap, "NULL");
}
static int g_ncol_sort;
static int row_cmp(const void *x, const void *y) {
    const val *a = (const val *)x, *b = (const val *)y;
    int i;
    for (i = 0; i < g_ncol_sort; i++) {
        char p[80], q[80];
        int c;
        val_str(&a[i], p, sizeof p);
        val_str(&b[i], q, sizeof q);
        if ((c = strcmp(p, q)) != 0) return c;
    }
    return 0;
}
static void res_sort(res *r) {
    /* rows sorted by their printed form; rows are blocks of ncol values */
    int n = r->nrows, nc = r->ncol, i, j;
    if (n < 2) return;
    g_ncol_sort = nc;
    for (i = 1; i < n; i++)                                    /* insertion sort on blocks: results are small */
        for (j = i; j > 0 && row_cmp(r->v + (size_t)(j - 1) * nc, r->v + (size_t)j * nc) > 0; j--) {
            val t[16];
            memcpy(t, r->v + (size_t)j * nc, sizeof(val) * (size_t)nc);
            memcpy(r->v + (size_t)j * nc, r->v + (size_t)(j - 1) * nc, sizeof(val) * (size_t)nc);
            memcpy(r->v + (size_t)(j - 1) * nc, t, sizeof(val) * (size_t)nc);
        }
}
static int res_same(res *a, res *b, int sorted, const char *sql) {
    int i, k;
    if (!sorted) { res_sort(a); res_sort(b); }
    if (a->nrows != b->nrows) { fprintf(stderr, "  %s\n  AltSql DB %d rows, SQLite %d\n", sql, a->nrows, b->nrows); return 0; }
    if (a->nrows && a->ncol != b->ncol) { fprintf(stderr, "  %s\n  columns %d and %d\n", sql, a->ncol, b->ncol); return 0; }
    for (i = 0; i < a->nrows; i++)
        for (k = 0; k < a->ncol; k++)
            if (!val_eq(&a->v[(size_t)i * a->ncol + k], &b->v[(size_t)i * b->ncol + k])) {
                char p[80], q[80];
                val_str(&a->v[(size_t)i * a->ncol + k], p, sizeof p);
                val_str(&b->v[(size_t)i * b->ncol + k], q, sizeof q);
                fprintf(stderr, "  %s\n  row %d column %d: AltSql DB %s, SQLite %s\n", sql, i, k, p, q);
                {
                    int j, m;
                    for (j = 0; j < a->nrows && j < 8; j++) {
                        fprintf(stderr, "   ");
                        for (m = 0; m < a->ncol; m++) { val_str(&a->v[(size_t)j * a->ncol + m], p, sizeof p); val_str(&b->v[(size_t)j * b->ncol + m], q, sizeof q); fprintf(stderr, " %s/%s", p, q); }
                        fprintf(stderr, "\n");
                    }
                }
                return 0;
            }
    g_rows += (unsigned long)a->nrows;
    return 1;
}

/* ---- the tables ---- */
typedef struct tab { const char *name, *create; const char *cols[4]; int types[4]; int ncols; const char *order; } tab;
enum { TI = 1, TR, TT };
static const tab TABS[3] = {
    { "t1", "CREATE TABLE t1 (a INT, b INT, c REAL, d TEXT, PRIMARY KEY (a, b))", { "a", "b", "c", "d" }, { TI, TI, TR, TT }, 4, "a, b, c, d" },
    { "t2", "CREATE TABLE t2 (k TEXT PRIMARY KEY, v INT, w REAL)", { "k", "v", "w" }, { TT, TI, TR }, 3, "k, v, w" },
    { "t3", "CREATE TABLE t3 (x INT, y INT, z TEXT)", { "x", "y", "z" }, { TI, TI, TT }, 3, "x, y, z" },
};
static uint32_t rnd(uint32_t n) { return xs(&g_s) % n; }
static const char *WORDS[] = { "a", "b", "c", "d", "e", "f", "g", "h", "ab", "ba", "Abc", "", "zz", "it''s", "x y" };

static void lit(char *o, int type) {
    if (type == TI) sprintf(o, "%d", (int)rnd(60) - 10);
    else if (type == TR) sprintf(o, "%.1f", ((double)rnd(1000) - 500.0) / 10.0);
    else sprintf(o, "'%s'", WORDS[rnd(15)]);
}
/* A numeric expression on the table's columns. */
static void numexpr(char *o, const tab *T, int depth, int nodiv) {
    int c = (int)rnd(T->ncols);
    char a[600], b[600];
    switch (depth > 0 ? rnd(7) : rnd(3)) {
    case 0: case 1:
        if (T->types[c] == TT) sprintf(o, "LENGTH(%s)", T->cols[c]);
        else strcpy(o, T->cols[c]);
        break;
    case 2: lit(o, rnd(2) ? TI : TR); break;
    case 3: numexpr(a, T, depth - 1, nodiv); numexpr(b, T, depth - 1, nodiv); sprintf(o, "(%s + %s)", a, b); break;
    case 4: numexpr(a, T, depth - 1, nodiv); numexpr(b, T, depth - 1, nodiv); sprintf(o, "(%s - %s)", a, b); break;
    case 5: numexpr(a, T, depth - 1, nodiv); sprintf(o, "(%s * %d)", a, (int)rnd(5) - 2); break;
    default:
        numexpr(a, T, depth - 1, nodiv);
        if (nodiv) sprintf(o, "ABS(%s)", a);
        else {                                    /* % only on whole numbers: on reals Core keeps the fraction, SQLite drops it */
            int k = (int)rnd(4), ic = (int)rnd(T->ncols);
            while (T->types[ic] != TI) ic = (ic + 1) % T->ncols;
            if (k == 1) sprintf(o, "(%s %% %d)", T->cols[ic], (int)rnd(9) - 4);
            else sprintf(o, k == 0 ? "(%s / 3)" : k == 2 ? "ABS(%s)" : "(%s / 0)", a);
        }
    }
}
/* A condition: comparisons, BETWEEN, IN, NOT IN, IS NULL, AND, OR, NOT. */
static void cond(char *o, const tab *T, int depth) {
    int c = (int)rnd(T->ncols), k, n, i;
    char a[1200], b[1200], l[80], h[80];
    static const char *ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    switch (depth > 0 ? rnd(9) : rnd(6)) {
    case 0: case 1:
        if (T->types[c] == TT) {
            lit(l, TT);
            if (rnd(4)) sprintf(o, "%s %s %s", T->cols[c], ops[rnd(6)], l);
            else sprintf(o, "%s(%s) %s %s", rnd(2) ? "LOWER" : "UPPER", T->cols[c], ops[rnd(6)], l);
        }
        else { numexpr(a, T, 1, 0); lit(l, T->types[c]); sprintf(o, "%s %s %s", rnd(2) ? T->cols[c] : a, ops[rnd(6)], l); }
        break;
    case 2:
        if (T->types[c] == TT) { lit(l, TT); lit(h, TT); }
        else { lit(l, T->types[c]); lit(h, T->types[c]); }
        sprintf(o, "%s %sBETWEEN %s AND %s", T->cols[c], rnd(4) ? "" : "NOT ", l, h);
        break;
    case 3: case 4:
        n = 1 + (int)rnd(5);
        k = sprintf(o, "%s %sIN (", T->cols[c], rnd(4) ? "" : "NOT ");
        for (i = 0; i < n; i++) { lit(l, T->types[c] == TT ? TT : rnd(5) ? TI : TR); k += sprintf(o + k, "%s%s", i ? ", " : "", l); }
        sprintf(o + k, ")");
        break;
    case 5: sprintf(o, "%s IS %sNULL", T->cols[c], rnd(2) ? "NOT " : ""); break;
    case 6: cond(a, T, depth - 1); cond(b, T, depth - 1); sprintf(o, "(%s AND %s)", a, b); break;
    case 7: cond(a, T, depth - 1); cond(b, T, depth - 1); sprintf(o, "(%s OR %s)", a, b); break;
    default: cond(a, T, depth - 1); sprintf(o, "NOT (%s)", a);
    }
}

/* ---- one statement on both ---- */
static int both_select(const char *sql, int ordered) {
    res a, b;
    int r1, r2, ok = 1;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    r1 = altsql_db_exec(g_db, sql, db_cb, &a);
    r2 = sq_run(sql, &b);
    g_selects++;
    if (r1 != ALTSQL_OK || r2 != 0) {
        if (r1 != ALTSQL_OK && r2 != 0) { g_both_failed++; }
        else { fprintf(stderr, "  %s\n  AltSql DB %d (%s), SQLite %s\n", sql, r1, altsql_db_errmsg(g_db), r2 ? sqlite3_errmsg(g_sq) : "ok"); ok = 0; }
    } else ok = res_same(&a, &b, ordered, sql);
    free(a.v); free(b.v);
    return ok;
}
static int same_table(const tab *T) {
    char sql[200];
    sprintf(sql, "SELECT * FROM %s ORDER BY %s", T->name, T->order);
    g_compared_tables++;
    return both_select(sql, 1);
}
static int both_write(const char *sql, const tab *T) {
    int r1 = altsql_db_exec(g_db, sql, NULL, NULL), r2 = sqlite3_exec(g_sq, sql, NULL, NULL, NULL);
    g_writes++;
    if ((r1 == ALTSQL_OK) != (r2 == SQLITE_OK)) {
        fprintf(stderr, "  %s\n  AltSql DB %d (%s), SQLite %d (%s)\n", sql, r1, altsql_db_errmsg(g_db), r2, sqlite3_errmsg(g_sq));
        return 0;
    }
    if (r1 != ALTSQL_OK) g_both_failed++;
    return same_table(T);
}

static int gen_select(const tab *T, char *sql) {
    char w[1500], e[700], g[64];
    int k = 0, n, i, ordered = 0;
    w[0] = 0;
    if (rnd(5)) cond(w, T, 2);
    switch (rnd(3)) {
    case 0: {                                                 /* rows */
        k = sprintf(sql, "SELECT ");
        n = 1 + (int)rnd(3);
        for (i = 0; i < n; i++) {
            if (rnd(2)) strcpy(e, T->cols[rnd(T->ncols)]); else numexpr(e, T, 2, 0);
            k += sprintf(sql + k, "%s%s", i ? ", " : "", e);
        }
        k += sprintf(sql + k, " FROM %s", T->name);
        if (w[0]) k += sprintf(sql + k, " WHERE %s", w);
        if (rnd(2)) {
            char oe[800];
            int ic = (int)rnd(T->ncols);
            while (T->types[ic] != TI) ic = (ic + 1) % T->ncols;
            numexpr(e, T, 1, 0);
            sprintf(oe, "(%s - %s)", e, T->cols[ic]);       /* never a bare number: SQLite reads that as a column position */
            k += sprintf(sql + k, " ORDER BY %s%s, %s", rnd(2) ? T->cols[rnd(T->ncols)] : oe, rnd(2) ? " DESC" : "", T->order);
            ordered = 1;
            if (rnd(2)) k += sprintf(sql + k, " LIMIT %d", (int)rnd(30));
            if (rnd(3) == 0) k += sprintf(sql + k, " OFFSET %d", (int)rnd(10));
        }
        break; }
    case 1: {                                                 /* groups */
        int gc = (int)rnd(T->ncols);
        strcpy(g, T->cols[gc]);
        k = sprintf(sql, "SELECT %s, COUNT(*)", g);
        n = 1 + (int)rnd(3);
        for (i = 0; i < n; i++) {
            int c = (int)rnd(T->ncols);
            static const char *fs[] = { "SUM", "MIN", "MAX", "AVG", "COUNT" };
            const char *f = fs[rnd(5)];
            if (T->types[c] == TT && (f[0] == 'S' || f[0] == 'A')) f = "MAX";
            k += sprintf(sql + k, ", %s(%s)", f, T->cols[c]);
        }
        k += sprintf(sql + k, " FROM %s", T->name);
        if (w[0]) k += sprintf(sql + k, " WHERE %s", w);
        k += sprintf(sql + k, " GROUP BY %s", g);
        if (rnd(3) == 0) k += sprintf(sql + k, " HAVING COUNT(*) %s %d", rnd(2) ? ">" : "<=", (int)rnd(6));
        k += sprintf(sql + k, " ORDER BY %s%s", g, rnd(2) ? " DESC" : "");
        ordered = 1;
        break; }
    default: {                                                /* aggregates over the whole selection */
        int c = (int)rnd(T->ncols), c2 = (int)rnd(T->ncols);
        numexpr(e, T, 1, 0);
        k = sprintf(sql, "SELECT COUNT(*), COUNT(%s), MIN(%s), MAX(%s)%s%s%s FROM %s", T->cols[c], T->cols[c], T->cols[c2],
                    T->types[c2] == TT ? "" : ", SUM(", T->types[c2] == TT ? "" : T->cols[c2], T->types[c2] == TT ? "" : ")", T->name);
        if (w[0]) k += sprintf(sql + k, " WHERE %s", w);
        ordered = 1;
    }
    }
    return ordered;
}
static void gen_row(const tab *T, char *o) {
    int i, k = sprintf(o, "(");
    char l[80];
    for (i = 0; i < T->ncols; i++) {
        if (T == &TABS[1] && i == 0) sprintf(l, "'k%03d'", (int)rnd(120));
        else if (T == &TABS[0] && i == 0) sprintf(l, "%d", (int)rnd(10));
        else if (T == &TABS[0] && i == 1) sprintf(l, "%d", (int)rnd(50));
        else lit(l, T->types[i]);
        k += sprintf(o + k, "%s%s", i ? ", " : "", l);
    }
    sprintf(o + k, ")");
}
static int g_round;
static void gen_write(const tab *T, char *sql) {
    char w[1500], r[300], e[700];
    int k, n, i, c;
    w[0] = 0;
    if (rnd(4)) cond(w, T, 1);
    switch (rnd(6)) {
    case 0: case 1:
        n = 1 + (int)rnd(4);
        k = sprintf(sql, "INSERT %sINTO %s VALUES ", rnd(3) ? "" : "OR REPLACE ", T->name);
        for (i = 0; i < n; i++) { gen_row(T, r); k += sprintf(sql + k, "%s%s", i ? ", " : "", r); }
        break;
    case 2: case 3:                                           /* values, no key column */
        do c = (int)rnd(T->ncols); while ((T == &TABS[0] && c < 2) || (T == &TABS[1] && c == 0) || (T == &TABS[2] && c == 0));
        if (T->types[c] == TT) { if (rnd(2)) lit(e, TT); else sprintf(e, "%s(%s)", rnd(2) ? "LOWER" : "UPPER", T->cols[c]); }
        else if (T->types[c] == TI) sprintf(e, "(%s * 3 + %d) %% 1000", T->cols[c], (int)rnd(7));
        else { numexpr(e, T, 1, 1); }
        k = sprintf(sql, "UPDATE %s SET %s = %s", T->name, T->cols[c], e);
        if (w[0]) sprintf(sql + k, " WHERE %s", w);
        break;
    case 4:                                                   /* keys, to places no row holds */
        g_round++;
        if (T == &TABS[0]) k = sprintf(sql, "UPDATE t1 SET b = b + %d", 1000 * g_round);
        else if (T == &TABS[1]) k = sprintf(sql, "UPDATE t2 SET k = UPPER(k)");
        else k = sprintf(sql, "UPDATE t3 SET x = x + %d", 1000 * g_round);
        if (w[0]) sprintf(sql + k, " WHERE %s", w);
        break;
    default:
        k = sprintf(sql, "DELETE FROM %s", T->name);
        if (w[0]) sprintf(sql + k, " WHERE %s", w);
    }
}

static int run_seed(uint32_t seed, int nstmt) {
    ramfile rf;
    void *mem = malloc(8 << 20);
    char sql[4096];
    int i, t;
    g_s = seed * 2654435761u + 1;
    g_round = 0;
    ram_new(&rf, 1u << 26, 1u << 26);
    CHECK(db_open_ram(&g_db, &rf, mem, 8 << 20, 1024) == ALTSQL_OK, "open");
    CHECK(sqlite3_open(":memory:", &g_sq) == SQLITE_OK, "sqlite open");
    for (t = 0; t < 3; t++) {
        CHECK(altsql_db_exec(g_db, TABS[t].create, NULL, NULL) == ALTSQL_OK, "create %s: %s", TABS[t].name, altsql_db_errmsg(g_db));
        CHECK(sqlite3_exec(g_sq, TABS[t].create, NULL, NULL, NULL) == SQLITE_OK, "sqlite create");
        for (i = 0; i < 300; i++) {                           /* rows, some refused as duplicates on both */
            char r[300];
            gen_row(&TABS[t], r);
            sprintf(sql, "INSERT INTO %s VALUES %s", TABS[t].name, r);
            g_loads++;
            if (!both_write(sql, &TABS[t])) { CHECK(0, "seed %u: loading %s", seed, TABS[t].name); }
        }
    }
    for (i = 0; i < nstmt; i++) {
        const tab *T = &TABS[rnd(3)];
        if (rnd(4)) {
            int ordered = gen_select(T, sql);
            g_rand_selects++;
            if (!both_select(sql, ordered)) { CHECK(0, "seed %u statement %d differs", seed, i); }
        } else {
            gen_write(T, sql);
            g_rand_writes++;
            if (!both_write(sql, T)) { CHECK(0, "seed %u statement %d differs", seed, i); }
        }
        if (i % 500 == 499 && check_slots(g_db, 1)) return 1;
    }
    for (t = 0; t < 3; t++) if (!same_table(&TABS[t])) { CHECK(0, "seed %u: final tables", seed); }
    altsql_db_close(g_db);
    sqlite3_close(g_sq);
    ram_free(&rf);
    free(mem);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick"), nseeds = quick ? 1 : 4, n = quick ? 3000 : 25000, s;
    sqlite3_initialize();
    printf("AltSql DB %s: SQL compared against SQLite %s\n", ALTSQL_DB_VERSION, sqlite3_libversion());
    for (s = 1; s <= nseeds; s++) if (run_seed((uint32_t)s, n)) return 1;
    printf("  %d seeds of %d random statements on three tables (a key of two columns, a text key, no key)\n", nseeds, n);
    printf("  random statements: %lu SELECTs and %lu writes, after %lu INSERTs that loaded the tables\n", g_rand_selects, g_rand_writes, g_loads);
    printf("  %lu SELECTs and %lu writes in all, %lu rows compared, the whole table compared after each write (%lu times)\n",
           g_selects, g_writes, g_rows, g_compared_tables);
    printf("  %lu statements failed on both engines (duplicate keys, a key moved onto another), none on one only\n", g_both_failed);
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
