/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: SQL on the tree (steps 6 and 9).
 * The same tables and rows in AltSql Core's gateway SQL and in AltSql DB, the
 * same statements on both, and the answers compared line for line. Then the
 * plans: EXPLAIN names the plan WHERE allows, the plan reads only the rows it
 * should, and every plan returns what a full scan returns. Then INSERT's
 * rules and SQL over tables that sync fills.
 *   test_sql           the full run
 *   test_sql quick     fewer random queries */
#include "dbtest.h"

/* ---- answers as text, to compare ---- */
typedef struct out { char *buf; size_t n, cap; int rows; } out;
static void out_add(out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) { o->cap = (o->n + n + 1) * 2 + 4096; o->buf = (char *)realloc(o->buf, o->cap); }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
    o->buf[o->n] = 0;
}
static int collect(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    out *o = (out *)ctx;
    char b[64];
    int i;
    (void)names;
    for (i = 0; i < ncol; i++) {
        int n;
        if (i) out_add(o, "|", 1);
        switch (v[i].type) {
        case ALTSQL_INTEGER: n = sprintf(b, "%lld", (long long)v[i].u.i); out_add(o, b, (size_t)n); break;
        case ALTSQL_REAL: n = sprintf(b, "%.15g", v[i].u.r); out_add(o, b, (size_t)n); break;
        case ALTSQL_TEXT: out_add(o, "'", 1); out_add(o, v[i].u.s, (size_t)v[i].len); out_add(o, "'", 1); break;
        default: out_add(o, "NULL", 4);
        }
    }
    out_add(o, "\n", 1);
    o->rows++;
    return 0;
}
static int names_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    out *o = (out *)ctx;
    int i;
    if (!o->rows) {
        for (i = 0; i < ncol; i++) { if (i) out_add(o, ",", 1); out_add(o, names[i], strlen(names[i])); }
        out_add(o, "\n", 1);
    }
    return collect(ctx, ncol, v, names);
}

/* ---- a Core gateway database to compare with ---- */
typedef struct core { uint8_t *flash, *mem; altsql_ram_flash ram; altsql_flash fl; altsql_config cfg; altsql *db; } core;
static int core_open(core *c) {
    memset(c, 0, sizeof *c);
    c->flash = (uint8_t *)malloc(1024 * 4096);
    c->mem = (uint8_t *)malloc(4 << 20);
    memset(c->flash, 0xFF, 1024 * 4096);                /* an erased chip */
    altsql_ram_flash_init(&c->fl, &c->ram, c->flash, 4096, 1024, 4);
    c->ram.budget = -1;
    c->cfg.mem = c->mem; c->cfg.mem_size = 4 << 20; c->cfg.create = 1; c->cfg.max_series = 8;
    CHECK(altsql_open(&c->db, &c->fl, &c->cfg) == ALTSQL_OK, "core open");
    return 0;
}

static altsql_db *g_db;
static ramfile g_rf;
static void *g_mem;
static size_t g_msize = 8 << 20;

static int both(core *c, const char *sql, int must_ok) {
    out a, b;
    int ra, rb;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    ra = altsql_exec(c->db, sql, names_cb, &a);
    rb = altsql_db_exec(g_db, sql, names_cb, &b);
    CHECK(ra == rb, "%s\n  Core %d (%s), AltSql DB %d (%s)", sql, ra, altsql_errmsg(c->db), rb, altsql_db_errmsg(g_db));
    CHECK(!must_ok || ra == ALTSQL_OK, "%s: %d %s", sql, ra, altsql_errmsg(c->db));
    CHECK((!a.buf && !b.buf) || (a.buf && b.buf && !strcmp(a.buf, b.buf)), "%s\n--- Core:\n%s--- AltSql DB:\n%s", sql, a.buf ? a.buf : "", b.buf ? b.buf : "");
    free(a.buf); free(b.buf);
    return 0;
}

/* ---- Core's SQL and AltSql DB's give the same answers ---- */
static int t_same_as_core(int quick) {
    static const char *queries[] = {
        "SELECT * FROM r",
        "SELECT time, machine, temp FROM r WHERE machine = 3",
        "SELECT * FROM r WHERE time >= 1700000100 AND time < 1700000200",
        "SELECT * FROM r WHERE time BETWEEN 1700000050 AND 1700000060",
        "SELECT * FROM r WHERE time NOT BETWEEN 1700000050 AND 1700000990",
        "SELECT machine, COUNT(*), MIN(temp), MAX(temp), AVG(temp), SUM(temp) FROM r GROUP BY machine",
        "SELECT machine, COUNT(*) AS n FROM r GROUP BY machine HAVING n > 120 ORDER BY n DESC, machine",
        "SELECT note, COUNT(*) FROM r GROUP BY note ORDER BY note",
        "SELECT * FROM r ORDER BY temp DESC, time LIMIT 15",
        "SELECT * FROM r ORDER BY machine, time DESC LIMIT 10 OFFSET 20",
        "SELECT * FROM r LIMIT 7",
        "SELECT * FROM r LIMIT 5 OFFSET 990",
        "SELECT time - 1700000000 AS t, temp * 2 + 1, ABS(temp - 20), ROUND(temp, 1) FROM r WHERE machine = 1 LIMIT 20",
        "SELECT UPPER(note), LOWER(note), LENGTH(note) FROM r WHERE note LIKE 'a%' LIMIT 10",
        "SELECT * FROM r WHERE note LIKE '%b_' OR machine = 7",
        "SELECT * FROM r WHERE NOT (machine < 6) AND temp > 25.5",
        "SELECT * FROM r WHERE machine IS NULL",
        "SELECT COUNT(*) FROM r WHERE temp > 100",
        "SELECT MAX(time), MIN(time) FROM r",
        "SELECT machine % 3 AS g, SUM(machine) FROM r GROUP BY g ORDER BY g",
        "SELECT 1 + 2, 'text', 7 / 2, 7.0 / 2",
        "SELECT * FROM r WHERE time = 1700000500",
        "SELECT * FROM r WHERE 1700000500 = time OR 1700000501 = time",
        "SELECT * FROM r WHERE temp = 21.5",
        "SELECT time, temp FROM r WHERE machine = 2 AND time > 1700000900 ORDER BY time DESC",
        "SELECT COUNT(*), SUM(time) FROM r WHERE machine >= 2 AND machine <= 4",
        "SELECT * FROM nosuch",
        "SELECT nosuch FROM r",
        "SELECT * FROM r WHERE",
        "SELECT machine, COUNT(*) FROM r GROUP BY machine HAVING COUNT(*) > 125",
    };
    core c;
    unsigned i;
    uint32_t s = 7;
    char sql[512];
    if (core_open(&c)) return 1;
    CHECK(altsql_exec(c.db, "CREATE TABLE r (time TIME, machine INT, temp FLOAT, note TEXT)", NULL, NULL) == ALTSQL_OK, "core create");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE r (time TIME, machine INT, temp FLOAT, note TEXT)", NULL, NULL) == ALTSQL_OK, "create: %s", altsql_db_errmsg(g_db));
    for (i = 0; i < 1000; i++) {
        static const char *notes[] = { "alpha", "beta", "ab", "abc", "", "zeta" };
        int t = 1700000000 + (int)i;                                 /* in time order: Core reads a series in arrival order */
        const char *note = notes[xs(&s) % 6];                           /* draws right to left, in the order gcc on x86-64 made them */
        double temp = (double)(xs(&s) % 4000) / 100.0;
        unsigned mach = xs(&s) % 8;
        sprintf(sql, "INSERT INTO r VALUES (%d, %u, %.2f, '%s')", t, mach, temp, note);
        CHECK(altsql_exec(c.db, sql, NULL, NULL) == ALTSQL_OK, "core insert");
        CHECK(altsql_db_exec(g_db, sql, NULL, NULL) == ALTSQL_OK, "insert: %s", altsql_db_errmsg(g_db));
    }
    for (i = 0; i < sizeof queries / sizeof queries[0]; i++) if (both(&c, queries[i], 0)) return 1;
    {   /* rows that arrive out of time order: AltSql DB returns them by time, then by arrival */
        out o;
        memset(&o, 0, sizeof o);
        CHECK(altsql_db_exec(g_db, "CREATE TABLE late (time TIME, v INT); INSERT INTO late VALUES (5, 1), (3, 2), (5, 3), (1, 4); "
                                   "SELECT * FROM late", collect, &o) == ALTSQL_OK && o.buf && !strcmp(o.buf, "1|4\n3|2\n5|1\n5|3\n"),
              "late rows: %s", o.buf ? o.buf : "");
        free(o.buf);
    }
    /* random WHERE clauses */
    for (i = 0; i < (quick ? 100u : 1000u); i++) {
        static const char *cols[] = { "time", "machine", "temp" };
        static const char *ops[] = { "=", "<", "<=", ">", ">=", "<>" };
        int a = (int)(xs(&s) % 3), b = (int)(xs(&s) % 3);
        long va = a == 0 ? 1700000000 + (long)(xs(&s) % 1000) : a == 1 ? (long)(xs(&s) % 9) : (long)(xs(&s) % 40);
        long vb = b == 0 ? 1700000000 + (long)(xs(&s) % 1000) : b == 1 ? (long)(xs(&s) % 9) : (long)(xs(&s) % 40);
        {
            const char *opb = ops[xs(&s) % 6], *conj = xs(&s) % 3 ? "AND" : "OR", *opa = ops[xs(&s) % 6];   /* draws right to left, in the order gcc on x86-64 made them */
            sprintf(sql, "SELECT * FROM r WHERE %s %s %ld %s %s %s %ld ORDER BY time, machine, temp LIMIT 50",
                    cols[a], opa, va, conj, cols[b], opb, vb);
        }
        if (both(&c, sql, 1)) return 1;
    }
    altsql_close(c.db);
    free(c.flash); free(c.mem);
    printf("  Core's SQL and AltSql DB's: %u fixed statements and %u random ones, the same answers: ok\n",
           (unsigned)(sizeof queries / sizeof queries[0]), quick ? 100u : 1000u);
    return 0;
}

static uint64_t rows_read(void) { altsql_db_info i; altsql_db_info_get(g_db, &i); return i.sql_rows; }
static int plan_is(const char *sql, const char *plan) {
    out o;
    int rc;
    char ex[600];
    memset(&o, 0, sizeof o);
    sprintf(ex, "EXPLAIN %s", sql);
    rc = altsql_db_exec(g_db, ex, collect, &o);
    CHECK(rc == ALTSQL_OK && o.buf && !strncmp(o.buf + 1, plan, strlen(plan)), "%s: plan %s, want %s", sql, o.buf ? o.buf : "-", plan);
    free(o.buf);
    return 0;
}
/* The same rows by the plan and by a full scan (an OR that no plan uses). */
static int same_rows(const char *where, const char *table) {
    out a, b;
    char q1[512], q2[512];
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    sprintf(q1, "SELECT * FROM %s WHERE %s", table, where);
    sprintf(q2, "SELECT * FROM %s WHERE (%s) OR 0", table, where);
    CHECK(altsql_db_exec(g_db, q1, collect, &a) == ALTSQL_OK, "%s: %s", q1, altsql_db_errmsg(g_db));
    CHECK(altsql_db_exec(g_db, q2, collect, &b) == ALTSQL_OK, "%s", q2);
    CHECK((!a.buf && !b.buf) || (a.buf && b.buf && !strcmp(a.buf, b.buf)), "%s: the plan's rows differ from a full scan's", q1);
    free(a.buf); free(b.buf);
    return 0;
}

static int t_plans(int quick) {
    uint32_t s = 11, i;
    char sql[512];
    CHECK(altsql_db_exec(g_db, "CREATE TABLE m (machine INT, time TIME, temp FLOAT, PRIMARY KEY (machine, time))", NULL, NULL) == ALTSQL_OK,
          "create m: %s", altsql_db_errmsg(g_db));
    CHECK(altsql_db_begin(g_db, 1) == ALTSQL_OK, "begin");
    for (i = 0; i < 20000; i++) {
        sprintf(sql, "INSERT INTO m VALUES (%u, %u, %.1f)", i % 50, 1700000000u + i / 50, (double)(xs(&s) % 400) / 10.0);
        CHECK(altsql_db_exec(g_db, sql, NULL, NULL) == ALTSQL_OK, "insert m: %s", altsql_db_errmsg(g_db));
    }
    CHECK(altsql_db_commit(g_db) == ALTSQL_OK, "commit");
    if (plan_is("SELECT * FROM m WHERE machine = 7 AND time = 1700000100", "point lookup")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine = 7 AND time = 1700000100", NULL, NULL) == ALTSQL_OK && rows_read() == 1,
          "a point lookup reads one row, read %llu", (unsigned long long)rows_read());
    if (plan_is("SELECT * FROM m WHERE machine = 7", "range scan")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine = 7", NULL, NULL) == ALTSQL_OK && rows_read() == 400, "machine 7: 400 rows read, read %llu", (unsigned long long)rows_read());
    if (plan_is("SELECT * FROM m WHERE machine = 7 AND time BETWEEN 1700000010 AND 1700000019", "range scan")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine = 7 AND time BETWEEN 1700000010 AND 1700000019", NULL, NULL) == ALTSQL_OK && rows_read() == 10,
          "ten rows read, read %llu", (unsigned long long)rows_read());
    if (plan_is("SELECT * FROM m WHERE machine >= 48", "range scan")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine >= 48", NULL, NULL) == ALTSQL_OK && rows_read() == 800, "800 rows read, read %llu", (unsigned long long)rows_read());
    if (plan_is("SELECT * FROM m WHERE time = 1700000010", "full scan")) return 1;
    if (plan_is("SELECT * FROM m WHERE machine = 3 OR machine = 4", "key list")) return 1;
    if (plan_is("SELECT * FROM m", "full scan")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT COUNT(*) FROM m", NULL, NULL) == ALTSQL_OK && rows_read() == 20000, "a full scan reads every row");
    for (i = 0; i < (quick ? 150u : 1500u); i++) {               /* random narrowings: same rows as a full scan */
        static const char *ops[] = { "=", "<", "<=", ">", ">=" };
        uint32_t m = xs(&s) % 52, t = 1700000000u + xs(&s) % 410;
        switch (xs(&s) % 5) {
        case 0: sprintf(sql, "machine %s %u", ops[xs(&s) % 5], m); break;
        case 1: sprintf(sql, "machine = %u AND time %s %u", m, ops[xs(&s) % 5], t); break;
        case 2: sprintf(sql, "machine = %u AND time BETWEEN %u AND %u", m, t, t + xs(&s) % 20); break;
        case 3: { unsigned v = xs(&s) % 40; const char *op = ops[xs(&s) % 5];   /* draws right to left, in the order gcc on x86-64 made them */
                  sprintf(sql, "%u %s machine AND temp > %u", m, op, v); break; }
        default: sprintf(sql, "machine = %u.5 OR machine = %u", m, m); break;
        }
        if (same_rows(sql, "m")) return 1;
    }
    /* reals, negatives and text in keys narrow correctly too */
    CHECK(altsql_db_exec(g_db, "CREATE TABLE k (name TEXT PRIMARY KEY, x REAL); "
                               "INSERT INTO k VALUES ('a', -2.5), ('b', 0), ('c', 3.25), ('d', -100), ('e', 1e300)", NULL, NULL) == ALTSQL_OK,
          "create k: %s", altsql_db_errmsg(g_db));
    if (same_rows("name >= 'b' AND name < 'd'", "k") || same_rows("name = 'c'", "k") || same_rows("name > 'aa'", "k")) return 1;
    CHECK(altsql_db_exec(g_db, "CREATE TABLE n (x REAL, v INT, PRIMARY KEY (x)); "
                               "INSERT INTO n VALUES (-2.5, 1), (-0.0, 2), (3.25, 3), (-1e9, 4), (7, 5)", NULL, NULL) == ALTSQL_OK,
          "create n: %s", altsql_db_errmsg(g_db));
    if (same_rows("x > -3", "n") || same_rows("x <= 0", "n") || same_rows("x = 0", "n") || same_rows("x BETWEEN -2.5 AND 3.25", "n")) return 1;
    printf("  plans: point lookup reads 1 row, range scans only their range, %u random narrowings return a full scan's rows: ok\n",
           quick ? 150u : 1500u);
    return 0;
}

static int t_insert_rules(void) {
    out o;
    CHECK(altsql_db_exec(g_db, "CREATE TABLE p (id INT PRIMARY KEY, v TEXT)", NULL, NULL) == ALTSQL_OK, "create p");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE IF NOT EXISTS p (id INT PRIMARY KEY, v TEXT)", NULL, NULL) == ALTSQL_OK, "if not exists");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE p (a INT)", NULL, NULL) == ALTSQL_EXISTS, "name taken");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (1, 'one'), (2, 'two')", NULL, NULL) == ALTSQL_OK, "insert");
    /* a key already there: nothing of the statement is written */
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (3, 'three'), (1, 'again')", NULL, NULL) == ALTSQL_EXISTS, "duplicate key");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (4, 'four'), (4, 'twice')", NULL, NULL) == ALTSQL_EXISTS, "key twice in one statement");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (5, 'five'), (6, 7)", NULL, NULL) == ALTSQL_SCHEMA, "a wrong type in the last row");
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT * FROM p", collect, &o) == ALTSQL_OK && o.rows == 2, "all or nothing: %d rows", o.rows);
    free(o.buf);
    CHECK(altsql_db_exec(g_db, "INSERT OR REPLACE INTO p VALUES (1, 'uno'), (3, 'tres')", NULL, NULL) == ALTSQL_OK, "or replace");
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT * FROM p", collect, &o) == ALTSQL_OK && o.buf && !strcmp(o.buf, "1|'uno'\n2|'two'\n3|'tres'\n"), "replaced: %s", o.buf ? o.buf : "");
    free(o.buf);
    /* inside a transaction, an error found before anything changed leaves it usable */
    CHECK(altsql_db_begin(g_db, 1) == ALTSQL_OK, "begin");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (2, 'dup')", NULL, NULL) == ALTSQL_EXISTS, "dup in txn");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (9, 'nine')", NULL, NULL) == ALTSQL_OK, "the transaction goes on: %s", altsql_db_errmsg(g_db));
    CHECK(altsql_db_commit(g_db) == ALTSQL_OK, "commit");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p (v, id) VALUES ('ten', 10)", NULL, NULL) == ALTSQL_OK, "named columns");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (11)", NULL, NULL) == ALTSQL_SCHEMA, "too few values");
    CHECK(altsql_db_exec(g_db, "INSERT INTO p VALUES (NULL, 'x')", NULL, NULL) == ALTSQL_SCHEMA, "NULL refused");
    CHECK(altsql_db_exec(g_db, "INSERT INTO nosuch VALUES (1)", NULL, NULL) == ALTSQL_SCHEMA && strstr(altsql_db_errmsg(g_db), "nosuch"), "unknown table");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE q (a INT, PRIMARY KEY (b))", NULL, NULL) == ALTSQL_SCHEMA, "key not a column");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE q (a BLOB)", NULL, NULL) == ALTSQL_SYNTAX, "unknown type");
    CHECK(altsql_db_exec(g_db, "DROP TABLE p x", NULL, NULL) == ALTSQL_SYNTAX, "DROP takes one name");
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT COUNT(*) FROM p; SELECT MAX(id) FROM p", collect, &o) == ALTSQL_OK && o.buf && !strcmp(o.buf, "5\n10\n"),
          "two statements: %s", o.buf ? o.buf : "");
    free(o.buf);
    printf("  INSERT: all or nothing, duplicate keys refused, OR REPLACE, named columns; errors keep a transaction usable: ok\n");
    return 0;
}

/* ---- SQL over synced tables ---- */
static int t_synced(void) {
    static uint8_t flash[64 * 4096], mem[48 * 1024], buf[1 << 16];
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql_config cfg;
    altsql *dev;
    int d, i, rc;
    size_t n;
    uint32_t last, conf;
    out o;
    for (d = 1; d <= 20; d++) {
        memset(flash, 0xFF, sizeof flash);                          /* a fresh, erased chip for each device */
        altsql_ram_flash_init(&fl, &ram, flash, 4096, 64, 4);
        ram.budget = -1;
        memset(&cfg, 0, sizeof cfg);
        cfg.mem = mem; cfg.mem_size = sizeof mem; cfg.create = 1;
        CHECK(altsql_open(&dev, &fl, &cfg) == ALTSQL_OK, "device");
        CHECK(altsql_ts_create(dev, "temps", "time:time,machine:int,temp:float") == ALTSQL_OK, "series");
        for (i = 0; i < 300; i++) CHECK(altsql_append(dev, "temps", (int64_t)(1700000000 + i * 10), d % 4, (double)(d * 10 + i % 7)) == ALTSQL_OK, "append");
        CHECK(altsql_put(dev, "site", d % 2 ? "north" : "south", 5) == ALTSQL_OK, "put");
        do {
            rc = altsql_sync_read(dev, 0, buf, sizeof buf, &n, &last, NULL, NULL);
            CHECK(altsql_db_sync_apply(g_db, 1000 + d, 0, buf, n, &conf) == ALTSQL_OK, "apply: %s", altsql_db_errmsg(g_db));
        } while (rc == ALTSQL_OK);
        altsql_close(dev);
    }
    if (plan_is("SELECT * FROM temps WHERE time BETWEEN 1700000100 AND 1700000150", "range per device")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM temps WHERE time BETWEEN 1700000100 AND 1700000150", NULL, NULL) == ALTSQL_OK && rows_read() == 20 * 6,
          "range per device reads 6 rows from each of 20 devices, read %llu", (unsigned long long)rows_read());
    if (same_rows("time BETWEEN 1700000100 AND 1700000150", "temps") || same_rows("time >= 1700002950", "temps") ||
        same_rows("time < 1700000020 AND machine = 2", "temps")) return 1;
    if (plan_is("SELECT * FROM temps WHERE device = 1005 AND time > 1700002900", "range scan")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM temps WHERE device = 1005 AND time > 1700002900", NULL, NULL) == ALTSQL_OK && rows_read() == 10,
          "nine rows and the bound read, read %llu", (unsigned long long)rows_read());   /* a range starts at its bound, WHERE drops it */
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT device, COUNT(*), AVG(temp) FROM temps WHERE device <= 1003 GROUP BY device", collect, &o) == ALTSQL_OK &&
          o.buf && !strcmp(o.buf, "1001|300|12.99\n1002|300|22.99\n1003|300|32.99\n"), "group by device: %s", o.buf ? o.buf : "");
    free(o.buf);
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT value, COUNT(*) FROM kv WHERE key = 'site' GROUP BY value", collect, &o) == ALTSQL_OK &&
          o.buf && !strcmp(o.buf, "'north'|10\n'south'|10\n"), "kv over SQL: %s", o.buf ? o.buf : "");
    free(o.buf);
    memset(&o, 0, sizeof o);
    CHECK(altsql_db_exec(g_db, "SELECT * FROM temps LIMIT 1", names_cb, &o) == ALTSQL_OK && o.buf && !strncmp(o.buf, "device,seq,time,machine,temp\n", 29),
          "SELECT * starts with device and seq: %s", o.buf ? o.buf : "");
    free(o.buf);
    CHECK(altsql_db_exec(g_db, "INSERT INTO temps VALUES (1, 1, 1, 1, 1)", NULL, NULL) == ALTSQL_MISUSE, "a synced table takes rows only from sync");
    printf("  synced tables: range per device reads only its ranges; GROUP BY device; kv over SQL: ok\n");
    return 0;
}

/* ---- step 9: IN lists, UPDATE, DELETE, DROP TABLE, prepared statements ---- */
static char *run_text(const char *sql, int *rc) {
    out o;
    memset(&o, 0, sizeof o);
    *rc = altsql_db_exec(g_db, sql, collect, &o);
    if (!o.buf) { o.buf = (char *)malloc(1); o.buf[0] = 0; }
    return o.buf;
}
static long long one_number(const char *sql) {
    int rc;
    char *t = run_text(sql, &rc);
    long long v = rc == ALTSQL_OK ? atoll(t) : -999999;
    free(t);
    return v;
}
static uint64_t changed(void) { altsql_db_info i; altsql_db_info_get(g_db, &i); return i.sql_changed; }
/* Every row of a prepared statement, as collect writes rows. */
static char *step_all(altsql_db_stmt *st, int *rc, int *rows) {
    out o;
    altsql_value v[64];
    int n, i;
    memset(&o, 0, sizeof o);
    *rows = 0;
    while ((*rc = altsql_db_step(st)) == ALTSQL_OK) {
        n = altsql_db_column_count(st);
        for (i = 0; i < n && i < 64; i++) if (altsql_db_column(st, i, &v[i]) != ALTSQL_OK) { *rc = -100; break; }
        collect(&o, n, v, NULL);
        (*rows)++;
    }
    if (*rc == ALTSQL_DONE) *rc = ALTSQL_OK;
    if (!o.buf) { o.buf = (char *)malloc(1); o.buf[0] = 0; }
    return o.buf;
}
static int same_prepared(const char *prep, const char *plain, const altsql_value *par, int np) {
    altsql_db_stmt *st;
    int rc1, rc2, rows, i;
    char *a, *b;
    CHECK(altsql_db_prepare(g_db, prep, &st) == ALTSQL_OK, "prepare %s: %s", prep, altsql_db_errmsg(g_db));
    for (i = 0; i < np; i++) CHECK(altsql_db_bind(st, i + 1, &par[i]) == ALTSQL_OK, "bind %d", i + 1);
    a = step_all(st, &rc1, &rows);
    b = run_text(plain, &rc2);
    CHECK(rc1 == ALTSQL_OK && rc2 == ALTSQL_OK && !strcmp(a, b), "%s: prepared and plain differ (%d rows; rc %d %d: %s)",
          prep, rows, rc1, rc2, altsql_db_errmsg(g_db));
    free(a); free(b);
    altsql_db_finalize(st);
    return 0;
}

static int t_in_lists(void) {
    int rc;
    char *t;
    if (plan_is("SELECT * FROM m WHERE machine IN (3, 4)", "key list")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine IN (3, 4, 3)", NULL, NULL) == ALTSQL_OK && rows_read() == 800,
          "IN (3, 4, 3): one walk per value, 800 rows read, read %llu", (unsigned long long)rows_read());
    if (plan_is("SELECT * FROM m WHERE machine = 5 AND time IN (1700000005, 1700000300)", "key list")) return 1;
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine = 5 AND time IN (1700000005, 1700000300, 1800000000)", NULL, NULL) == ALTSQL_OK &&
          rows_read() == 2, "whole keys: a point lookup per value, read %llu", (unsigned long long)rows_read());
    if (same_rows("machine IN (49, 0, 17, 17)", "m") || same_rows("machine NOT IN (1, 2, 3, 4, 5)", "m") ||
        same_rows("machine IN (1) AND time IN (1700000001, 1700000002)", "m") || same_rows("machine IN (-1, 1e10, 2.5, 3)", "m") ||
        same_rows("name IN ('a', 'c', 'zz', 'it''s')", "k") || same_rows("x IN (-2.5, 7, 0, -0.0)", "n") ||
        same_rows("machine in(1,2) and temp>10", "m") || same_rows("machine IN (NULL, 4)", "m")) return 1;
    t = run_text("SELECT machine IN (1, 2), machine NOT IN (1, 2) FROM m WHERE machine = 1 LIMIT 1", &rc);
    CHECK(rc == ALTSQL_OK && !strcmp(t, "1|0\n"), "IN as a value: %s", t);
    free(t);
    t = run_text("SELECT name FROM k WHERE name IN ('x in (1)', 'a') ORDER BY name", &rc);
    CHECK(rc == ALTSQL_OK && !strcmp(t, "'a'\n"), "IN inside a string is text: %s", t);
    free(t);
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine IN (SELECT 1)", NULL, NULL) == ALTSQL_SYNTAX, "IN takes values only");
    CHECK(altsql_db_exec(g_db, "SELECT * FROM m WHERE machine = ?", NULL, NULL) == ALTSQL_MISUSE, "? needs a prepared statement");
    printf("  IN lists: key list plan, one walk per value; NOT IN; text, reals, NULL: the same rows as a full scan: ok\n");
    return 0;
}

static int t_update_delete_drop(void) {
    int rc;
    char *t;
    long long before = one_number("SELECT COUNT(*) FROM m");
    CHECK(altsql_db_exec(g_db, "UPDATE m SET temp = temp + 100 WHERE machine = 7", NULL, NULL) == ALTSQL_OK && changed() == 400,
          "update 400 rows: %s, changed %llu", altsql_db_errmsg(g_db), (unsigned long long)changed());
    CHECK(one_number("SELECT COUNT(*) FROM m WHERE machine = 7 AND temp >= 100") == 400 && one_number("SELECT COUNT(*) FROM m WHERE temp >= 100") == 400,
          "the update reached machine 7's rows and no others");
    CHECK(altsql_db_exec(g_db, "UPDATE m SET temp = temp - 100 WHERE machine IN (7) AND temp >= 100", NULL, NULL) == ALTSQL_OK && changed() == 400, "update back");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE u (id INT PRIMARY KEY, v TEXT, w FLOAT); "
                               "INSERT INTO u VALUES (1,'a',1),(2,'b',2),(3,'c',3),(4,'d',4),(5,'e',5),(6,'f',6),(7,'g',7),(8,'h',8)", NULL, NULL) == ALTSQL_OK,
          "create u: %s", altsql_db_errmsg(g_db));
    CHECK(altsql_db_exec(g_db, "UPDATE u SET id = id + 1", NULL, NULL) == ALTSQL_OK && changed() == 8, "every key moves up one: %s", altsql_db_errmsg(g_db));
    t = run_text("SELECT id, v FROM u", &rc);
    CHECK(rc == ALTSQL_OK && !strcmp(t, "2|'a'\n3|'b'\n4|'c'\n5|'d'\n6|'e'\n7|'f'\n8|'g'\n9|'h'\n"), "keys moved, each row once: %s", t);
    free(t);
    CHECK(altsql_db_exec(g_db, "UPDATE u SET id = 9 WHERE id = 2", NULL, NULL) == ALTSQL_EXISTS, "a key another row has: %s", altsql_db_errmsg(g_db));
    CHECK(altsql_db_exec(g_db, "UPDATE u SET id = 'x' WHERE id = 2", NULL, NULL) == ALTSQL_SCHEMA, "a text key into an int column");
    CHECK(altsql_db_exec(g_db, "UPDATE u SET nosuch = 1", NULL, NULL) == ALTSQL_SCHEMA, "no such column");
    CHECK(altsql_db_exec(g_db, "UPDATE u SET v = 'z', v = 'y'", NULL, NULL) == ALTSQL_SCHEMA, "a column set twice");
    CHECK(altsql_db_exec(g_db, "UPDATE u SET w = COUNT(*)", NULL, NULL) < 0, "no aggregates in SET");
    t = run_text("SELECT id, v FROM u WHERE id <= 3", &rc);
    CHECK(rc == ALTSQL_OK && !strcmp(t, "2|'a'\n3|'b'\n"), "a failed UPDATE leaves the table as it was: %s", t);
    free(t);
    CHECK(altsql_db_begin(g_db, 1) == ALTSQL_OK, "begin");
    CHECK(altsql_db_exec(g_db, "UPDATE u SET v = v, w = w * 2 WHERE id > 4; DELETE FROM u WHERE id = 2", NULL, NULL) == ALTSQL_OK, "in a transaction");
    CHECK(altsql_db_rollback(g_db) == ALTSQL_OK && one_number("SELECT SUM(w) FROM u") == 36 && one_number("SELECT COUNT(*) FROM u") == 8, "rolled back");
    CHECK(altsql_db_exec(g_db, "UPDATE temps SET temp = 0", NULL, NULL) == ALTSQL_MISUSE, "a synced table takes no UPDATE");
    CHECK(altsql_db_exec(g_db, "DELETE FROM m WHERE machine = 49", NULL, NULL) == ALTSQL_OK && changed() == 400, "delete 400 rows");
    CHECK(altsql_db_exec(g_db, "DELETE FROM m WHERE machine IN (47, 48) AND time >= 1700000200", NULL, NULL) == ALTSQL_OK && changed() == 400, "delete by a key list");
    CHECK(one_number("SELECT COUNT(*) FROM m") == before - 800, "rows left after DELETE");
    CHECK(one_number("SELECT COUNT(*) FROM temps") == 6000, "temps before retention");
    CHECK(altsql_db_exec(g_db, "DELETE FROM temps WHERE time < 1700000500", NULL, NULL) == ALTSQL_OK && changed() == 1000,
          "retention on a synced table: %s, changed %llu", altsql_db_errmsg(g_db), (unsigned long long)changed());
    CHECK(altsql_db_exec(g_db, "DROP TABLE temps", NULL, NULL) == ALTSQL_MISUSE, "a synced table is not dropped");
    CHECK(altsql_db_exec(g_db, "DELETE FROM u", NULL, NULL) == ALTSQL_OK && changed() == 8 && one_number("SELECT COUNT(*) FROM u") == 0, "delete every row");
    CHECK(altsql_db_exec(g_db, "INSERT INTO u VALUES (1, 'a', 1); DROP TABLE u", NULL, NULL) == ALTSQL_OK && changed() == 1, "drop: %s", altsql_db_errmsg(g_db));
    CHECK(altsql_db_exec(g_db, "SELECT * FROM u", NULL, NULL) == ALTSQL_SCHEMA, "a dropped table is gone");
    CHECK(altsql_db_exec(g_db, "DROP TABLE u", NULL, NULL) == ALTSQL_SCHEMA && altsql_db_exec(g_db, "DROP TABLE IF EXISTS u", NULL, NULL) == ALTSQL_OK, "IF EXISTS");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE u (id INT PRIMARY KEY, v TEXT, w FLOAT)", NULL, NULL) == ALTSQL_OK && one_number("SELECT COUNT(*) FROM u") == 0,
          "the name again, empty");
    CHECK(altsql_db_exec(g_db, "CREATE TABLE big (time TIME, v INT)", NULL, NULL) == ALTSQL_OK, "create big");
    {
        int i;
        CHECK(altsql_db_begin(g_db, 1) == ALTSQL_OK, "begin");
        for (i = 0; i < 3000; i++) {
            char sql[96];
            sprintf(sql, "INSERT INTO big VALUES (%d, %d)", 1700000000 + i % 97, i);
            CHECK(altsql_db_exec(g_db, sql, NULL, NULL) == ALTSQL_OK, "insert big");
        }
        CHECK(altsql_db_commit(g_db) == ALTSQL_OK, "commit");
    }
    CHECK(altsql_db_exec(g_db, "UPDATE big SET time = time + 1000 WHERE v < 1500", NULL, NULL) == ALTSQL_OK && changed() == 1500,
          "a key change on a table without a key: %s", altsql_db_errmsg(g_db));
    CHECK(one_number("SELECT COUNT(*) FROM big WHERE time >= 1700001000") == 1500 && one_number("SELECT SUM(v) FROM big") == 4498500, "every row once");
    CHECK(altsql_db_exec(g_db, "DROP TABLE big", NULL, NULL) == ALTSQL_OK && changed() == 3000, "drop 3000 rows");
    if (check_slots(g_db, 1)) return 1;
    printf("  UPDATE (in place and of keys), DELETE (tables and synced retention), DROP TABLE: rules, rollback, no pages lost: ok\n");
    return 0;
}

static int t_prepared(void) {
    altsql_db_stmt *st, *a, *b, *x[4], *y;
    altsql_value v[3], c;
    int rc, rows, i, ra = 0, rb = 0;
    char *t;
    v[0].type = ALTSQL_INTEGER; v[0].u.i = 7;
    v[1].type = ALTSQL_INTEGER; v[1].u.i = 1700000100;
    if (same_prepared("SELECT * FROM m WHERE machine = ? AND time = ?", "SELECT * FROM m WHERE machine = 7 AND time = 1700000100", v, 2)) return 1;
    v[0].u.i = 0;
    if (same_prepared("SELECT * FROM m WHERE machine >= ?", "SELECT * FROM m WHERE machine >= 0", v, 1)) return 1;      /* many batches, by key */
    if (same_prepared("SELECT * FROM m WHERE machine >= ? LIMIT 5000 OFFSET 7", "SELECT * FROM m WHERE machine >= 0 LIMIT 5000 OFFSET 7", v, 1)) return 1;
    if (same_prepared("SELECT machine, time FROM m WHERE machine < 9 ORDER BY temp DESC, machine, time",
                      "SELECT machine, time FROM m WHERE machine < 9 ORDER BY temp DESC, machine, time", NULL, 0)) return 1;   /* batches by skipping */
    if (same_prepared("SELECT machine, COUNT(*), AVG(temp) FROM m GROUP BY machine", "SELECT machine, COUNT(*), AVG(temp) FROM m GROUP BY machine", NULL, 0)) return 1;
    v[0].u.i = 3; v[1].u.i = 30;
    if (same_prepared("SELECT * FROM m WHERE machine IN (?, ?) AND time < 1700000020", "SELECT * FROM m WHERE machine IN (3, 30) AND time < 1700000020", v, 2)) return 1;
    v[0].u.i = 0;
    if (same_prepared("EXPLAIN SELECT * FROM m WHERE machine IN (?, 5)", "EXPLAIN SELECT * FROM m WHERE machine IN (0, 5)", v, 1)) return 1;
    /* INSERT, UPDATE and DELETE with parameters, text with quotes */
    CHECK(altsql_db_prepare(g_db, "INSERT INTO u VALUES (?, ?, ?)", &st) == ALTSQL_OK, "prepare insert: %s", altsql_db_errmsg(g_db));
    for (i = 0; i < 200; i++) {
        char txt[32];
        sprintf(txt, "it's %d", i);
        v[0].type = ALTSQL_INTEGER; v[0].u.i = i;
        v[1].type = ALTSQL_TEXT; v[1].u.s = txt; v[1].len = (int)strlen(txt);
        v[2].type = ALTSQL_REAL; v[2].u.r = -0.1 * i;
        CHECK(altsql_db_bind(st, 1, &v[0]) == ALTSQL_OK && altsql_db_bind(st, 2, &v[1]) == ALTSQL_OK && altsql_db_bind(st, 3, &v[2]) == ALTSQL_OK, "bind");
        CHECK(altsql_db_step(st) == ALTSQL_DONE && changed() == 1, "insert %d: %s", i, altsql_db_errmsg(g_db));
    }
    altsql_db_finalize(st);
    t = run_text("SELECT v, w FROM u WHERE id = 42", &rc);
    CHECK(rc == ALTSQL_OK && !strcmp(t, "'it's 42'|-4.2\n"), "text with a quote and a negative real: %s", t);
    free(t);
    CHECK(altsql_db_prepare(g_db, "UPDATE u SET w = ? WHERE id IN (?, ?)", &st) == ALTSQL_OK, "prepare update: %s", altsql_db_errmsg(g_db));
    v[0].type = ALTSQL_INTEGER; v[0].u.i = 5; v[1].type = ALTSQL_INTEGER; v[1].u.i = 10; v[2].type = ALTSQL_INTEGER; v[2].u.i = 11;
    CHECK(altsql_db_bind(st, 1, &v[0]) == ALTSQL_OK && altsql_db_bind(st, 2, &v[1]) == ALTSQL_OK && altsql_db_bind(st, 3, &v[2]) == ALTSQL_OK &&
          altsql_db_step(st) == ALTSQL_DONE && changed() == 2, "prepared update: %s", altsql_db_errmsg(g_db));
    altsql_db_finalize(st);
    CHECK(altsql_db_prepare(g_db, "DELETE FROM u WHERE id >= ?", &st) == ALTSQL_OK, "prepare delete");
    v[0].u.i = 150;
    CHECK(altsql_db_bind(st, 1, &v[0]) == ALTSQL_OK && altsql_db_step(st) == ALTSQL_DONE && changed() == 50, "prepared delete");
    altsql_db_finalize(st);
    CHECK(one_number("SELECT COUNT(*) FROM u") == 150 && one_number("SELECT SUM(w) FROM u WHERE id IN (10, 11)") == 10, "after prepared changes");
    /* two statements stepped in turn */
    CHECK(altsql_db_prepare(g_db, "SELECT id FROM u WHERE id < 100", &a) == ALTSQL_OK && altsql_db_prepare(g_db, "SELECT machine FROM m WHERE machine < 3", &b) == ALTSQL_OK, "two");
    for (;;) {
        int r1 = altsql_db_step(a), r2 = altsql_db_step(b);
        if (r1 == ALTSQL_OK) { CHECK(altsql_db_column(a, 0, &c) == ALTSQL_OK && c.u.i == ra, "a's row %d", ra); ra++; }
        if (r2 == ALTSQL_OK) { CHECK(altsql_db_column(b, 0, &c) == ALTSQL_OK && c.u.i == rb / 400, "b's row %d", rb); rb++; }
        if (r1 != ALTSQL_OK && r2 != ALTSQL_OK) break;
    }
    CHECK(ra == 100 && rb == 1200, "interleaved: %d and %d rows", ra, rb);
    CHECK(altsql_db_column_name(b, 0) && !strcmp(altsql_db_column_name(b, 0), "machine"), "column name");
    CHECK(altsql_db_reset(a) == ALTSQL_OK && altsql_db_step(a) == ALTSQL_OK && altsql_db_column(a, 0, &c) == ALTSQL_OK && c.u.i == 0, "reset runs again");
    altsql_db_finalize(a);
    altsql_db_finalize(b);
    /* rules */
    CHECK(altsql_db_prepare(g_db, "SELECT * FROM nosuch WHERE x = ?", &st) == ALTSQL_SCHEMA && !st, "an unknown table at prepare");
    CHECK(altsql_db_prepare(g_db, "SELEC 1", &st) == ALTSQL_SYNTAX, "a syntax error at prepare");
    CHECK(altsql_db_prepare(g_db, "SELECT 1; SELECT 2", &st) == ALTSQL_MISUSE, "one statement");
    for (i = 0; i < 4; i++) CHECK(altsql_db_prepare(g_db, "SELECT ?", &x[i]) == ALTSQL_OK, "statement %d", i);
    CHECK(altsql_db_prepare(g_db, "SELECT 1", &y) == ALTSQL_NOMEM, "four at a time");
    v[0].type = ALTSQL_TEXT; v[0].u.s = "x"; v[0].len = 300;
    CHECK(altsql_db_bind(x[0], 2, &v[1]) == ALTSQL_MISUSE && altsql_db_bind(x[0], 1, &v[0]) == ALTSQL_TOOBIG, "bind rules");
    v[0].type = ALTSQL_REAL; v[0].u.r = 0.0 / 0.0;
    CHECK(altsql_db_bind(x[0], 1, &v[0]) == ALTSQL_OK && altsql_db_step(x[0]) == ALTSQL_MISUSE, "NaN is refused");
    CHECK(altsql_db_bind(x[1], 1, NULL) == ALTSQL_OK && altsql_db_step(x[1]) == ALTSQL_OK && altsql_db_column(x[1], 0, &c) == ALTSQL_OK &&
          c.type == ALTSQL_NULL && altsql_db_step(x[1]) == ALTSQL_DONE, "NULL bound");
    for (i = 0; i < 4; i++) altsql_db_finalize(x[i]);
    t = step_all(NULL, &rc, &rows);
    free(t);
    CHECK(rc == ALTSQL_MISUSE, "no statement");
    printf("  prepared statements: parameters, batches by key and by skipping, INSERT, UPDATE and DELETE, two at once, rules: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    printf("AltSql DB %s: SQL on the tree\n", ALTSQL_DB_VERSION);
    g_mem = malloc(g_msize);
    ram_new(&g_rf, 1u << 27, 1u << 27);
    CHECK(db_open_ram(&g_db, &g_rf, g_mem, g_msize, 4096) == ALTSQL_OK, "open");
    if (t_same_as_core(quick) || t_plans(quick) || t_insert_rules() || t_synced()) return 1;
    if (t_in_lists() || t_update_delete_drop() || t_prepared()) return 1;
    if (check_slots(g_db, 1)) return 1;
    altsql_db_close(g_db);
    CHECK(db_open_ram(&g_db, &g_rf, g_mem, g_msize, 4096) == ALTSQL_OK, "reopen");
    {
        out o;
        memset(&o, 0, sizeof o);
        CHECK(altsql_db_exec(g_db, "SELECT COUNT(*) FROM m; SELECT COUNT(*) FROM r; SELECT COUNT(*) FROM temps; SELECT COUNT(*) FROM u", collect, &o) == ALTSQL_OK &&
              o.buf && !strcmp(o.buf, "19200\n1000\n5000\n150\n"), "after reopening: %s", o.buf ? o.buf : "");
        free(o.buf);
    }
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
