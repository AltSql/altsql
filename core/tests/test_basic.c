/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Functional tests: key-value, time-series, SQL, text export/import,
 * reclaiming space, rollover and a full store. Each test runs on several
 * flash geometries, write alignments, and with and without map(). */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#define ALTSQL_PORT_FILE
#include <unistd.h>
#include "testutil.h"

/* ---- SQL result capture ---------------------------------------------------- */
static tbuf qb;

static int q_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    tbuf *b = (tbuf *)ctx;
    char t[64];
    int i;
    (void)names;
    for (i = 0; i < n; i++) {
        if (i) tbuf_write(b, "|", 1);
        switch (v[i].type) {
        case ALTSQL_NULL: tbuf_write(b, "NULL", 4); break;
        case ALTSQL_INTEGER: snprintf(t, sizeof t, "%lld", (long long)v[i].u.i); tbuf_write(b, t, strlen(t)); break;
        case ALTSQL_REAL: snprintf(t, sizeof t, "%g", v[i].u.r); tbuf_write(b, t, strlen(t)); break;
        default: tbuf_write(b, v[i].u.s, (size_t)v[i].len);
        }
    }
    tbuf_write(b, "\n", 1);
    return 0;
}

static const char *sql(altsql *db, const char *q) {
    static char err[160];
    int rc;
    qb.n = 0;
    if (qb.p) qb.p[0] = 0;
    rc = altsql_exec(db, q, q_cb, &qb);
    if (rc) {
        snprintf(err, sizeof err, "ERR %d: %s", rc, altsql_errmsg(db));
        return err;
    }
    return qb.p ? qb.p : "";
}

#define CHECK_SQL(db, q, want) do { const char *got_ = sql(db, q); t_checks++; \
    if (strcmp(got_, want) != 0) { t_fail++; \
        fprintf(stderr, "  FAIL %s:%d: %s\n    got:  %s\n    want: %s\n", __FILE__, __LINE__, q, got_, want); } } while (0)

/* ---- Key-value ------------------------------------------------------------------ */
static int count_cb(void *ctx, const char *k, size_t kl, const void *v, size_t vl) {
    (void)k; (void)kl; (void)v; (void)vl;
    (*(int *)ctx)++;
    return 0;
}

static void test_kv(rig *r) {
    altsql *db;
    char buf[300], big[400];
    size_t n;
    int count = 0;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_put(db, "a", "1", 1));
    CHECK_OK(db, altsql_get(db, "a", buf, sizeof buf, &n));
    CHECK(n == 1 && buf[0] == '1');
    CHECK_OK(db, altsql_put(db, "a", "22", 2));
    CHECK_OK(db, altsql_get(db, "a", buf, sizeof buf, &n));
    CHECK(n == 2 && memcmp(buf, "22", 2) == 0);
    CHECK_OK(db, altsql_put(db, "bin", "x\0y", 3));
    CHECK_OK(db, altsql_get(db, "bin", buf, sizeof buf, &n));
    CHECK(n == 3 && memcmp(buf, "x\0y", 3) == 0);
    CHECK_OK(db, altsql_get(db, "bin", buf, 1, &n));      /* short buffer: prefix, full length */
    CHECK(n == 3 && buf[0] == 'x');
    CHECK_OK(db, altsql_put(db, "empty", "", 0));
    CHECK_OK(db, altsql_get(db, "empty", buf, sizeof buf, &n));
    CHECK(n == 0);
    CHECK_RC(db, altsql_get(db, "missing", buf, sizeof buf, &n), ALTSQL_NOTFOUND);
    CHECK_OK(db, altsql_del(db, "a"));
    CHECK_RC(db, altsql_get(db, "a", buf, sizeof buf, &n), ALTSQL_NOTFOUND);
    CHECK_RC(db, altsql_del(db, "a"), ALTSQL_NOTFOUND);
    CHECK_RC(db, altsql_put(db, "", "x", 1), ALTSQL_MISUSE);
    memset(big, 'k', 201);
    big[201] = 0;
    CHECK_RC(db, altsql_put(db, big, "x", 1), ALTSQL_MISUSE);
    CHECK_RC(db, altsql_put(db, "\x01S", "x", 1), ALTSQL_MISUSE);
    memset(big, 'v', sizeof big);
    CHECK_RC(db, altsql_put(db, "toobig", big, sizeof big), ALTSQL_TOOBIG);
    CHECK_OK(db, altsql_kv_each(db, count_cb, &count));
    CHECK(count == 2);                                      /* bin, empty */

    rig_close(r);                                           /* reopen: still there */
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_get(db, "bin", buf, sizeof buf, &n));
    CHECK(n == 3 && memcmp(buf, "x\0y", 3) == 0);
    CHECK_RC(db, altsql_get(db, "a", buf, sizeof buf, &n), ALTSQL_NOTFOUND);
    CHECK_OK(db, altsql_put(db, "a", "3", 1));               /* a deleted key comes back */
    CHECK_OK(db, altsql_get(db, "a", buf, sizeof buf, &n));
    CHECK(n == 1 && buf[0] == '3');
    rig_close(r);
    CHECK_RC(NULL, altsql_put(db, "x", "y", 1), ALTSQL_MISUSE);   /* closed handle refused */
}

/* ---- Time-series ------------------------------------------------------------------- */
typedef struct scan_acc { int n; int64_t first, last; double sum; int ordered; } scan_acc;

static int scan_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    scan_acc *a = (scan_acc *)ctx;
    (void)names;
    if (a->n && v[0].u.i != a->last + 1) a->ordered = 0;
    if (!a->n) a->first = v[0].u.i;
    a->last = v[0].u.i;
    if (ncol > 2) a->sum += v[2].u.r;
    a->n++;
    return 0;
}

static int series_cb(void *ctx, const char *name, const char *schema) {
    tbuf *b = (tbuf *)ctx;
    tbuf_write(b, name, strlen(name));
    tbuf_write(b, "=", 1);
    tbuf_write(b, schema, strlen(schema));
    tbuf_write(b, ";", 1);
    return 0;
}

static void test_ts(rig *r) {
    altsql *db;
    altsql_stats st;
    altsql_value v[4];
    scan_acc a;
    tbuf sb = {0, 0, 0};
    int i;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_ts_create(db, "readings", "time:time,machine:int,temp:float,note:text"));
    CHECK_RC(db, altsql_ts_create(db, "readings", "time:time,x:int"), ALTSQL_EXISTS);
    CHECK_RC(db, altsql_ts_create(db, "bad", "x:int,time:time"), ALTSQL_SYNTAX);
    CHECK_RC(db, altsql_ts_create(db, "bad", "time:time,x:blob"), ALTSQL_SYNTAX);
    CHECK_RC(db, altsql_ts_create(db, "kv", "time:time"), ALTSQL_MISUSE);
    CHECK_RC(db, altsql_ts_create(db, "a_name_that_is_too_long_x", "time:time"), ALTSQL_MISUSE);
    for (i = 1; i <= 100; i++)
        CHECK_OK(db, altsql_append(db, "readings", (int64_t)i, i % 3, (double)i, i % 10 ? "ok" : "check"));
    memset(&a, 0, sizeof a);
    a.ordered = 1;
    CHECK_OK(db, altsql_ts_scan(db, "readings", 11, 20, scan_cb, &a));
    CHECK(a.n == 10 && a.first == 11 && a.last == 20 && a.ordered && a.sum == 155.0);
    CHECK_OK(db, altsql_ts_window(db, "readings", "temp", 51, &st));
    CHECK(st.count == 50 && st.min == 51.0 && st.max == 100.0 && st.avg == 75.5 && st.first == 51 && st.last == 100);
    CHECK_RC(db, altsql_ts_window(db, "readings", "nope", 0, &st), ALTSQL_SCHEMA);
    CHECK_RC(db, altsql_ts_window(db, "readings", "note", 0, &st), ALTSQL_SCHEMA);
    CHECK_RC(db, altsql_ts_window(db, "nope", "temp", 0, &st), ALTSQL_NOTFOUND);
    /* typed append with conversion: integer into a float column */
    v[0].type = ALTSQL_INTEGER; v[0].u.i = 101;
    v[1].type = ALTSQL_INTEGER; v[1].u.i = 7;
    v[2].type = ALTSQL_INTEGER; v[2].u.i = 42;
    v[3].type = ALTSQL_TEXT; v[3].u.s = "typed"; v[3].len = 5;
    CHECK_OK(db, altsql_ts_append(db, "readings", v, 4));
    CHECK_RC(db, altsql_ts_append(db, "readings", v, 3), ALTSQL_SCHEMA);
    v[1].u.i = 5000000000LL;                                /* does not fit int */
    CHECK_RC(db, altsql_ts_append(db, "readings", v, 4), ALTSQL_SCHEMA);
    v[1].u.i = 7;
    v[3].type = ALTSQL_INTEGER;
    CHECK_RC(db, altsql_ts_append(db, "readings", v, 4), ALTSQL_SCHEMA);
    CHECK_RC(db, altsql_append(db, "nope", (int64_t)1), ALTSQL_NOTFOUND);
    CHECK_OK(db, altsql_ts_create(db, "second", "time:time,value:real"));
    CHECK_OK(db, altsql_append(db, "second", (int64_t)5, 2.5));
    CHECK_OK(db, altsql_series_each(db, series_cb, &sb));
    CHECK(sb.p && strcmp(sb.p, "readings=time:time,machine:int,temp:float,note:text;second=time:time,value:real;") == 0);
    tbuf_free(&sb);

    rig_close(r);
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    memset(&a, 0, sizeof a);
    a.ordered = 1;
    CHECK_OK(db, altsql_ts_scan(db, "readings", AS_TIME_NONE, INT64_MAX, scan_cb, &a));
    CHECK(a.n == 101 && a.first == 1 && a.last == 101 && a.ordered);
    CHECK_OK(db, altsql_ts_window(db, "second", "value", 0, &st));
    CHECK(st.count == 1 && st.sum == 2.5);
    rig_close(r);
}

/* ---- SQL --------------------------------------------------------------------------------- */
/* Hostile expressions must give an error, never a stack overflow or a query
 * that runs for ever: deep brackets, long chains, and chains of aliases. */
static void test_sql_limits(altsql *db) {
    static char q[320000];
    size_t n;
    int i;
    n = (size_t)snprintf(q, sizeof q, "SELECT ");
    for (i = 0; i < 100000; i++) q[n++] = '(';
    q[n++] = '1';
    for (i = 0; i < 100000; i++) q[n++] = ')';
    q[n] = 0;
    CHECK_SQL(db, q, "ERR -8: expression nested too deeply");
    n = (size_t)snprintf(q, sizeof q, "SELECT 1");
    for (i = 0; i < 500; i++) n += (size_t)snprintf(q + n, sizeof q - n, "+1");
    CHECK_SQL(db, q, "ERR -8: expression nested too deeply");
    n = (size_t)snprintf(q, sizeof q, "SELECT ");
    for (i = 0; i < 300; i++) n += (size_t)snprintf(q + n, sizeof q - n, "NOT ");
    snprintf(q + n, sizeof q - n, "1");
    CHECK_SQL(db, q, "ERR -8: expression nested too deeply");
    n = (size_t)snprintf(q, sizeof q, "SELECT ");
    for (i = 0; i < 300; i++) n += (size_t)snprintf(q + n, sizeof q - n, "- ");
    snprintf(q + n, sizeof q - n, "1");
    CHECK_SQL(db, q, "ERR -8: expression nested too deeply");
    /* a long but reasonable chain still works */
    n = (size_t)snprintf(q, sizeof q, "SELECT COUNT(*) FROM t WHERE machine = 1");
    for (i = 0; i < 150; i++) n += (size_t)snprintf(q + n, sizeof q - n, " OR machine = 1");
    CHECK_SQL(db, q, "2\n");
    CHECK_SQL(db, "SELECT time AS a, a + a AS b, b + b AS c FROM t WHERE time = 3", "3|6|12\n");
    /* each alias doubles the expanded expression: 2^40 nodes */
    n = (size_t)snprintf(q, sizeof q, "SELECT time AS a0");
    for (i = 1; i <= 40; i++) n += (size_t)snprintf(q + n, sizeof q - n, ", a%d + a%d AS a%d", i - 1, i - 1, i);
    snprintf(q + n, sizeof q - n, " FROM t");
    CHECK_SQL(db, q, "ERR -8: expression too large once aliases are expanded");
    n = (size_t)snprintf(q, sizeof q, "SELECT time AS a0");
    for (i = 1; i <= 40; i++) n += (size_t)snprintf(q + n, sizeof q - n, ", a%d + 1 AS a%d", i - 1, i);
    snprintf(q + n, sizeof q - n, " FROM t WHERE time = 1");
    {   /* a chain of 40 aliases that each add 1 stays small: fine */
        const char *got = sql(db, q);
        CHECK(strncmp(got, "1|2|3|", 6) == 0 && strstr(got, "|41\n") != NULL);
    }
}

static void test_sql(rig *r) {
    altsql *db;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_SQL(db, "CREATE TABLE t (time TIME, machine INT, temp FLOAT, name TEXT)", "");
    CHECK_SQL(db, "CREATE TABLE IF NOT EXISTS t (time TIME, machine INT)", "");
    CHECK_SQL(db, "INSERT INTO t VALUES (1, 1, 20.5, 'alpha'), (2, 2, 30.0, 'beta'), (3, 1, 21.5, 'Gamma');"
                  "INSERT INTO t (name, time, machine, temp) VALUES ('delta', 4, 2, 32), ('epsilon', 5, 3, 50.0)", "");
    CHECK_SQL(db, "SELECT COUNT(*) FROM t", "5\n");
    CHECK_SQL(db, "SELECT * FROM t WHERE time >= 4", "4|2|32|delta\n5|3|50|epsilon\n");
    CHECK_SQL(db, "SELECT machine, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM t GROUP BY machine ORDER BY machine",
              "1|2|21|20.5|21.5\n2|2|31|30|32\n3|1|50|50|50\n");
    CHECK_SQL(db, "SELECT name FROM t WHERE temp > 25 AND machine = 2", "beta\ndelta\n");
    CHECK_SQL(db, "SELECT machine, SUM(temp) AS s FROM t GROUP BY machine HAVING s > 45 ORDER BY s DESC", "2|62\n3|50\n");
    CHECK_SQL(db, "SELECT name FROM t ORDER BY temp DESC LIMIT 2 OFFSET 1", "delta\nbeta\n");
    CHECK_SQL(db, "SELECT name FROM t LIMIT 2", "alpha\nbeta\n");
    CHECK_SQL(db, "SELECT name FROM t ORDER BY name LIMIT 3", "Gamma\nalpha\nbeta\n");
    CHECK_SQL(db, "SELECT name FROM t ORDER BY name DESC LIMIT 1 OFFSET 1", "delta\n");
    CHECK_SQL(db, "SELECT name FROM t ORDER BY time LIMIT 0", "");
    CHECK_SQL(db, "SELECT UPPER(name), LOWER(name), LENGTH(name) FROM t WHERE name LIKE 'g%'", "GAMMA|gamma|5\n");
    CHECK_SQL(db, "SELECT ABS(-3), ROUND(2.567, 2), 7/2, 7/2.0, 7%3, 1/0, -(2+3)*2", "3|2.57|3|3.5|1|NULL|-10\n");
    CHECK_SQL(db, "SELECT time FROM t WHERE temp BETWEEN 21 AND 31", "2\n3\n");
    CHECK_SQL(db, "SELECT time FROM t WHERE temp NOT BETWEEN 21 AND 31 AND name NOT LIKE '%a'", "5\n");
    CHECK_SQL(db, "SELECT COUNT(*) FROM t WHERE name IS NULL", "0\n");
    CHECK_SQL(db, "SELECT COUNT(name) FROM t WHERE name IS NOT NULL", "5\n");
    CHECK_SQL(db, "SELECT NULL AND 0, NULL OR 1, NULL = NULL, NOT NULL, 1 <> 2, 'b' > 'a'", "0|1|NULL|NULL|1|1\n");
    CHECK_SQL(db, "SELECT machine % 2 AS odd, COUNT(*) FROM t GROUP BY 1 ORDER BY 1", "0|2\n1|3\n");
    CHECK_SQL(db, "SELECT time * 10 AS x, x + 1 FROM t WHERE x > 30 ORDER BY x DESC", "50|51\n40|41\n");
    CHECK_SQL(db, "SELECT name, temp FROM t ORDER BY machine DESC, temp", "epsilon|50\nbeta|30\ndelta|32\nalpha|20.5\nGamma|21.5\n");
    CHECK_SQL(db, "SELECT COUNT(*), SUM(temp) FROM t WHERE time > 100", "0|NULL\n");
    CHECK_SQL(db, "SELECT MIN(name), MAX(name) FROM t", "Gamma|epsilon\n");
    CHECK_SQL(db, "SELECT name, COUNT(*) FROM t WHERE machine = 3", "epsilon|1\n");
    CHECK_SQL(db, "SELECT 9223372036854775807 + 1 > 0, 99999999999999999999 > 1, -(-9223372036854775807 - 1) > 0", "1|1|1\n");
    CHECK_SQL(db, "SELECT 'it''s', LENGTH(12.5), LENGTH(-7)", "it's|4|2\n");
    CHECK_SQL(db, "-- a comment\nSELECT 1; SELECT 2;", "1\n2\n");
    /* float columns read back as the decimal that was stored */
    CHECK_SQL(db, "CREATE TABLE f (time TIME, x FLOAT, y REAL)", "");
    CHECK_SQL(db, "INSERT INTO f VALUES (1, 20.1, 20.1), (2, 0.3, 0.1 + 0.2), (3, -1234.567, 1e300)", "");
    CHECK_SQL(db, "SELECT time FROM f WHERE x = 20.1 AND y = 20.1", "1\n");
    CHECK_SQL(db, "SELECT time FROM f WHERE x = 0.3 AND y <> 0.3", "2\n");     /* real keeps full precision */
    CHECK_SQL(db, "SELECT x, y > 1e299 FROM f WHERE time = 3", "-1234.57|1\n");
    CHECK_SQL(db, "SELECT COUNT(*) FROM f WHERE x = -1234.567", "1\n");
    /* the key-value store as a table */
    CHECK_SQL(db, "INSERT INTO kv VALUES ('b', 42), ('a', 'x'), ('c', 1.5)", "");
    CHECK_SQL(db, "SELECT key, value FROM kv ORDER BY key", "a|x\nb|42\nc|1.5\n");
    CHECK_SQL(db, "SELECT value FROM kv WHERE key = 'b'", "42\n");
    /* errors */
    CHECK_SQL(db, "SELEC 1", "ERR -8: expected SELECT, CREATE or INSERT near SELEC");
    CHECK_SQL(db, "SELECT nope FROM t", "ERR -9: no such column: nope");
    CHECK_SQL(db, "SELECT * FROM nope", "ERR -9: no such table: nope");
    CHECK_SQL(db, "SELECT COUNT(*) FROM t WHERE COUNT(*) > 1", "ERR -8: aggregate functions are not allowed here");
    CHECK_SQL(db, "SELECT COUNT(*) AS c FROM t GROUP BY c", "ERR -8: GROUP BY cannot use an aggregate");
    CHECK_SQL(db, "SELECT 1 +", "ERR -8: syntax error near end");
    CHECK_SQL(db, "SELECT 'abc", "ERR -8: unterminated string near end");
    CHECK_SQL(db, "INSERT INTO t VALUES (6, 1, 2.0)", "ERR -9: wrong number of values");
    CHECK_SQL(db, "INSERT INTO t VALUES (6, machine, 2.0, 'x')", "ERR -9: values must be constants: machine");
    CHECK_SQL(db, "CREATE TABLE u (a INT, time TIME)", "ERR -9: the first column must be TIME: it holds each row's timestamp");
    CHECK_SQL(db, "SELECT * FROM t WHERE", "ERR -8: syntax error near end");
    /* found by fuzzing: an error in the token after a statement */
    CHECK_SQL(db, "CREATE TABLE IF NOT EXISTS t (time TIME, a INT)\x95", "ERR -8: unexpected character \x95");
    CHECK_SQL(db, "CREATE TABLE IF NOT EXISTS t (time TIME, a INT)' SELECT 1", "ERR -8: unterminated string near end");
    CHECK_SQL(db, "INSERT INTO t VALUES (7, 1, 1.0, 'z') bogus", "ERR -8: syntax error near bogus");
    CHECK_SQL(db, "SELECT ROUND(2.567, 1e308), ROUND(2.567, -1e30), ROUND(2.567, 1)", "2.567|3|2.6\n");
    CHECK(strncmp(sql(db, "SELECT ROUND(1e308 * 10 - 1e308 * 10, 2), ROUND(1e308 * 10)"), "ERR", 3) != 0);   /* NaN, infinity */
    CHECK_SQL(db, "SELECT COUNT(*) FROM t", "5\n");                         /* nothing half-applied */
    test_sql_limits(db);
    rig_close(r);
}

/* ---- Text export and import --------------------------------------------------------------- */
static int imp(altsql *db, const char *text) { return altsql_import(db, text, strlen(text)); }

static void test_text(rig *r) {
    rig r2;
    tbuf a = {0, 0, 0}, b = {0, 0, 0};
    altsql *db;
    char buf[64];
    size_t n;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_put(db, "zeta", "last", 4));
    CHECK_OK(db, altsql_put(db, "alpha", "quote \" back \\ nl \n tab \t", 25));
    CHECK_OK(db, altsql_put(db, "bin", "\x00\x01\xff\x7f", 4));
    CHECK_OK(db, altsql_put(db, "mid", "old", 3));
    CHECK_OK(db, altsql_put(db, "mid", "new", 3));
    CHECK_OK(db, altsql_ts_create(db, "m", "time:time,v:float,d:real,i:int,l:long,s:text"));
    CHECK_OK(db, altsql_append(db, "m", (int64_t)-5, 21.53, 0.1, -7, (int64_t)1 << 40, "a \"b\""));
    CHECK_OK(db, altsql_append(db, "m", (int64_t)7, 1e-7, 1.0 / 3.0, 0, (int64_t)0, ""));
    CHECK_OK(db, altsql_export(db, tbuf_write, &a));
    CHECK(a.p && strcmp(a.p,
        "# altsql text export v1\n"
        "series m \"time:time,v:float,d:real,i:int,l:long,s:text\"\n"
        "kv \"alpha\" \"quote \\\" back \\\\ nl \\n tab \\t\"\n"
        "kv \"bin\" \"\\x00\\x01\xff\\x7f\"\n"
        "kv \"mid\" \"new\"\n"
        "kv \"zeta\" \"last\"\n"
        "row m -5 21.53 0.1 -7 1099511627776 \"a \\\"b\\\"\"\n"
        "row m 7 1e-07 0.33333333333333331 0 0 \"\"\n") == 0);

    rig_init(&r2, r->ss, r->sc, r->align, r->mem_size, r->nomap);
    CHECK_OK(r2.db, rig_open(&r2));
    CHECK_OK(r2.db, altsql_import(r2.db, a.p, a.n));
    CHECK_OK(r2.db, altsql_export(r2.db, tbuf_write, &b));
    CHECK(a.p && b.p && strcmp(a.p, b.p) == 0);             /* round trip is exact */
    CHECK_OK(r2.db, altsql_get(r2.db, "bin", buf, sizeof buf, &n));
    CHECK(n == 4 && memcmp(buf, "\x00\x01\xff\x7f", 4) == 0);
    CHECK_OK(r2.db, imp(r2.db, "series m \"time:time,v:float,d:real,i:int,l:long,s:text\"\n"));
    CHECK_RC(r2.db, imp(r2.db, "series m \"time:time\"\n"), ALTSQL_SCHEMA);
    CHECK_RC(r2.db, imp(r2.db, "\nrow m 1 2\n"), ALTSQL_SYNTAX);
    CHECK(strcmp(altsql_errmsg(r2.db), "import line 2: expected a number") == 0);
    CHECK_RC(r2.db, imp(r2.db, "kv \"a\" \"b\" \n bogus\n"), ALTSQL_SYNTAX);
    CHECK_RC(r2.db, imp(r2.db, "row nope 1\n"), ALTSQL_SCHEMA);
    CHECK_RC(r2.db, imp(r2.db, "kv \"a\" \"unterminated\n"), ALTSQL_SYNTAX);
    /* found by fuzzing: a series name with a zero byte in it, given twice */
    {
        static const char bad[] = "series mD\0x \"time:time,x:float\"\nseries mD\0x \"time:time,x:float\"\n";
        CHECK_RC(r2.db, altsql_import(r2.db, bad, sizeof bad - 1), ALTSQL_SYNTAX);
        CHECK_RC(r2.db, imp(r2.db, "series 9lives \"time:time\"\n"), ALTSQL_SYNTAX);
    }
    rig_free(&r2);
    tbuf_free(&a);
    tbuf_free(&b);
    rig_close(r);
}

/* ---- Reclaiming space, rollover, a full store --------------------------------------------- */
static void test_gc(rig *r) {
    altsql *db;
    altsql_info info;
    scan_acc a;
    char key[16], val[64], buf[64];
    size_t n;
    int i, k, rows = 3000;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_ts_create(db, "r", "time:time,v:int"));
    for (i = 1; i <= rows; i++) {
        CHECK_OK(db, altsql_append(db, "r", (int64_t)i, i));
        if (i % 50 == 0) {                                  /* keys keep changing under rollover */
            k = (i / 50) % 10;
            snprintf(key, sizeof key, "key%d", k);
            snprintf(val, sizeof val, "value-%d-%d", k, i);
            CHECK_OK(db, altsql_put(db, key, val, strlen(val)));
        }
    }
    CHECK_OK(db, altsql_info_get(db, &info));
    CHECK(info.gc_runs > 0 && info.rows_dropped > 0);
    for (k = 0; k < 2; k++) {                               /* check, reopen, check again */
        memset(&a, 0, sizeof a);
        a.ordered = 1;
        CHECK_OK(db, altsql_ts_scan(db, "r", AS_TIME_NONE, INT64_MAX, scan_cb, &a));
        CHECK(a.n > 0 && a.last == rows && a.ordered && a.first > 1);
        for (i = 0; i < 10; i++) {
            int last = 0, j;
            for (j = 50; j <= rows; j += 50) if ((j / 50) % 10 == i) last = j;
            snprintf(key, sizeof key, "key%d", i);
            snprintf(val, sizeof val, "value-%d-%d", i, last);
            CHECK_OK(db, altsql_get(db, key, buf, sizeof buf, &n));
            CHECK(n == strlen(val) && memcmp(buf, val, n) == 0);
        }
        {   /* SQL time pushdown agrees with a plain scan */
            char q[96], want[32];
            int64_t from = a.first + (a.last - a.first) / 2;
            snprintf(q, sizeof q, "SELECT COUNT(*) FROM r WHERE time >= %lld", (long long)from);
            snprintf(want, sizeof want, "%lld\n", (long long)(a.last - from + 1));
            CHECK_SQL(db, q, want);
            snprintf(want, sizeof want, "%d\n%d\n%d\n", rows - 2, rows - 3, rows - 4);
            CHECK_SQL(db, "SELECT v FROM r ORDER BY v DESC LIMIT 3 OFFSET 2", want);    /* top-N */
            snprintf(want, sizeof want, "%lld\n%lld\n", (long long)a.first, (long long)a.first + 1);
            CHECK_SQL(db, "SELECT time FROM r ORDER BY v LIMIT 2", want);
        }
        rig_close(r);
        CHECK_OK(r->db, rig_open(r));
        db = r->db;
    }
    /* one key rewritten many times: old versions are reclaimed */
    for (i = 0; i < 5000; i++) {
        snprintf(val, sizeof val, "v%d", i);
        CHECK_OK(db, altsql_put(db, "hot", val, strlen(val)));
    }
    CHECK_OK(db, altsql_get(db, "hot", buf, sizeof buf, &n));
    CHECK(n == 5 && memcmp(buf, "v4999", 5) == 0);
    rig_close(r);
}

static void test_full(rig *r) {
    altsql *db;
    char key[32], val[64], buf[64];
    size_t n;
    int i, rc = 0, stored = 0;
    r->cfg.kv_slots = 4096;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    for (i = 0; i < 100000; i++) {                          /* unique live keys until full */
        snprintf(key, sizeof key, "key-%06d", i);
        snprintf(val, sizeof val, "value-%06d-0123456789", i);
        rc = altsql_put(db, key, val, strlen(val));
        if (rc) break;
        stored++;
    }
    CHECK(rc == ALTSQL_FULL);
    {   /* a second attempt fails fast, without erasing anything */
        uint64_t erases = r->ram.erases, written = r->ram.bytes_written;
        CHECK_RC(db, altsql_put(db, "key-999999", "value-999999-0123456789", 23), ALTSQL_FULL);
        CHECK(r->ram.erases == erases && r->ram.bytes_written == written);
    }
    rig_close(r);
    CHECK_OK(r->db, rig_open(r));                           /* everything acknowledged is there */
    db = r->db;
    for (i = 0; i < stored; i++) {
        snprintf(key, sizeof key, "key-%06d", i);
        snprintf(val, sizeof val, "value-%06d-0123456789", i);
        rc = altsql_get(db, key, buf, sizeof buf, &n);
        if (rc || n != strlen(val) || memcmp(buf, val, n) != 0) break;
    }
    CHECK(i == stored);
    for (i = 0; i < stored / 2; i++) {                      /* deleting frees space again */
        snprintf(key, sizeof key, "key-%06d", i);
        CHECK_OK(db, altsql_del(db, key));
    }
    for (i = 0; i < stored / 4; i++) {
        snprintf(key, sizeof key, "new-%06d", i);
        CHECK_OK(db, altsql_put(db, key, "fresh", 5));
    }
    snprintf(key, sizeof key, "key-%06d", stored - 1);
    CHECK_OK(db, altsql_get(db, key, buf, sizeof buf, &n));
    rig_close(r);
    r->cfg.kv_slots = 0;
}

/* ---- A small key index: lookups fall back to scanning ------------------------------------ */
static void test_small_index(rig *r) {
    altsql *db;
    char key[16], buf[16];
    size_t n;
    int i;
    r->cfg.kv_slots = 8;
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    for (i = 0; i < 40; i++) {
        snprintf(key, sizeof key, "k%d", i);
        CHECK_OK(db, altsql_put(db, key, key, strlen(key)));
    }
    CHECK_OK(db, altsql_del(db, "k3"));
    CHECK_OK(db, altsql_ts_create(db, "r", "time:time,v:int"));
    for (i = 0; i < 2000; i++) CHECK_OK(db, altsql_append(db, "r", (int64_t)i, i));
    rig_close(r);
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    for (i = 0; i < 40; i++) {
        snprintf(key, sizeof key, "k%d", i);
        if (i == 3) { CHECK_RC(db, altsql_get(db, key, buf, sizeof buf, &n), ALTSQL_NOTFOUND); continue; }
        CHECK_OK(db, altsql_get(db, key, buf, sizeof buf, &n));
        CHECK(n == strlen(key) && memcmp(buf, key, n) == 0);
    }
    CHECK_RC(db, altsql_kv_each(db, count_cb, &i), ALTSQL_NOMEM);   /* listing needs the full index */
    rig_close(r);
    r->cfg.kv_slots = 0;
}

/* ---- Found by the mutation check: paths the other tests did not reach --------------------- */
static int count_rows_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)n; (void)v; (void)names;
    (*(long *)ctx)++;
    return 0;
}
static int one_int_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)n; (void)names;
    *(long *)ctx = (long)v[0].u.i;
    return 0;
}

/* Time-range lookups at every row, including rows that end a sector. */
static void test_ts_boundaries(rig *r) {
    altsql *db;
    altsql_stats st;
    long got, oldest = -1;
    int t;
    const int N = 600;
    char q[96];
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    CHECK_OK(db, altsql_ts_create(db, "b", "time:time,v:int"));
    for (t = 0; t < N; t++) CHECK_OK(db, altsql_append(db, "b", (int64_t)t, t));
    CHECK_OK(db, altsql_exec(db, "SELECT MIN(time) FROM b", one_int_cb, &oldest));   /* rollover may have dropped some */
    CHECK(oldest >= 0 && oldest < N);
    for (t = (int)oldest; t < N; t++) {
        got = 0;
        CHECK_OK(db, altsql_ts_scan(db, "b", t, t, count_rows_cb, &got));
        CHECK(got == 1);
        CHECK_OK(db, altsql_ts_window(db, "b", "v", t, &st));
        CHECK(st.count == (uint32_t)(N - t) && st.first == t);
        snprintf(q, sizeof q, "SELECT COUNT(*) FROM b WHERE time >= %d", t);
        got = -1;
        CHECK_OK(db, altsql_exec(db, q, one_int_cb, &got));
        CHECK(got == N - t);
    }
    rig_close(r);
}

/* Deletes the gateway has confirmed are dropped when their sector is
 * reclaimed; the key index keeps a marker in their place so other keys
 * stay reachable. */
static void test_tombstones(rig *r) {
    altsql *db;
    altsql_info info;
    char key[16], buf[16];
    uint8_t batch[256];
    size_t n, len;
    uint32_t last;
    int i, rc;
    r->cfg.kv_slots = 16;                            /* 11 keys and a series: the index stays complete */
    CHECK_OK(r->db, rig_open(r));
    db = r->db;
    for (i = 0; i < 11; i++) {
        snprintf(key, sizeof key, "t%d", i);
        CHECK_OK(db, altsql_put(db, key, key, strlen(key)));
    }
    for (i = 0; i < 6; i++) {
        snprintf(key, sizeof key, "t%d", i);
        CHECK_OK(db, altsql_del(db, key));
    }
    CHECK_OK(db, altsql_info_get(db, &info));
    rc = altsql_sync_read(db, info.last_seq, batch, sizeof batch, &len, &last, NULL, NULL);   /* gateway has it all */
    CHECK(rc == ALTSQL_DONE && len == 0);
    CHECK_OK(db, altsql_ts_create(db, "fill", "time:time,v:int"));
    for (i = 0; i < 4 * (int)(r->ss * r->sc / 28); i++) CHECK_OK(db, altsql_append(db, "fill", (int64_t)i, i));
    CHECK(db->kv_complete == 1);                     /* lookups use the index, not a scan */
    for (i = 0; i < 2; i++) {
        int k;
        for (k = 0; k < 11; k++) {
            snprintf(key, sizeof key, "t%d", k);
            if (k < 6) { CHECK_RC(db, altsql_get(db, key, buf, sizeof buf, &n), ALTSQL_NOTFOUND); continue; }
            CHECK_OK(db, altsql_get(db, key, buf, sizeof buf, &n));
            CHECK(n == strlen(key) && memcmp(buf, key, n) == 0);
        }
        rig_close(r);                               /* and again after the index is rebuilt */
        CHECK_OK(r->db, rig_open(r));
        db = r->db;
    }
    for (i = 0; i < 6; i++) {                        /* the freed places are reused */
        snprintf(key, sizeof key, "u%d", i);
        CHECK_OK(db, altsql_put(db, key, key, strlen(key)));
        CHECK_OK(db, altsql_get(db, key, buf, sizeof buf, &n));
        CHECK(n == strlen(key) && memcmp(buf, key, n) == 0);
    }
    rig_close(r);
    r->cfg.kv_slots = 0;
}

/* ---- Opening flash with the wrong geometry must not format it ---------------------------- */
static void test_geometry_guard(rig *r) {
    altsql_flash f;
    altsql_ram_flash ram;
    altsql *db = NULL;
    CHECK_OK(r->db, rig_open(r));
    CHECK_OK(r->db, altsql_put(r->db, "keep", "me", 2));
    rig_close(r);
    altsql_ram_flash_init(&f, &ram, r->flash, r->ss * 2, r->sc / 2, r->align);
    CHECK_RC(db, altsql_open(&db, &f, &r->cfg), ALTSQL_MISUSE);
    CHECK(db && strstr(altsql_errmsg(db), "another sector size") != NULL);
    CHECK_OK(r->db, rig_open(r));
    {
        char buf[8];
        size_t n;
        CHECK_OK(r->db, altsql_get(r->db, "keep", buf, sizeof buf, &n));
    }
    rig_close(r);
}

/* ---- The file port: create, reopen with detected geometry, refuse a mismatch ----------- */
static void test_file_port(void) {
    const char *path = "build/test_file_port.db";
    altsql_flash f;
    altsql_config cfg;
    altsql *db = NULL;
    static uint8_t mem[64 * 1024];
    char buf[16];
    size_t n;
    unlink(path);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem;
    cfg.mem_size = sizeof mem;
    cfg.create = 1;
    CHECK(altsql_file_flash_open(&f, path, 0, 0, 0, 0) != 0);           /* no file, no create */
    CHECK(altsql_file_flash_open(&f, path, 1024, 8, 8, 1) == 0);
    CHECK_OK(db, altsql_open(&db, &f, &cfg));
    CHECK_OK(db, altsql_put(db, "k", "file", 4));
    altsql_close(db);
    altsql_file_flash_close(&f);
    cfg.create = 0;
    CHECK(altsql_file_flash_open(&f, path, 0, 0, 0, 0) == 0);           /* geometry read from the file */
    CHECK(f.sector_size == 1024 && f.sector_count == 8 && f.write_align == 8);
    CHECK_OK(db, altsql_open(&db, &f, &cfg));
    CHECK_OK(db, altsql_get(db, "k", buf, sizeof buf, &n));
    CHECK(n == 4 && memcmp(buf, "file", 4) == 0);
    altsql_close(db);
    altsql_file_flash_close(&f);
    CHECK(altsql_file_flash_open(&f, path, 2048, 4, 8, 0) != 0);        /* asked for another geometry */
    unlink(path);
}

static void reset(rig *r) { memset(r->flash, 0xFF, (size_t)r->ss * r->sc); }

int main(void) {
    static const struct { uint32_t ss, sc, align; int nomap; } v[] = {
        { 4096, 16, 4, 0 }, { 4096, 16, 4, 1 }, { 512, 16, 1, 0 }, { 512, 16, 8, 1 },
        { 1024, 8, 16, 0 }, { 256, 32, 2, 1 },
    };
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        rig r;
        int before = t_fail;
        rig_init(&r, v[i].ss, v[i].sc, v[i].align, 256 * 1024, v[i].nomap);
        reset(&r); test_kv(&r);
        reset(&r); test_ts(&r);
        reset(&r); test_sql(&r);
        reset(&r); test_text(&r);
        reset(&r); test_gc(&r);
        reset(&r); test_full(&r);
        reset(&r); test_small_index(&r);
        reset(&r); test_ts_boundaries(&r);
        reset(&r); test_tombstones(&r);
        reset(&r); test_geometry_guard(&r);
        printf("  sectors %2u x %4u B, align %2u, %s: %s\n", v[i].sc, v[i].ss, v[i].align,
               v[i].nomap ? "read()" : "map() ", t_fail == before ? "ok" : "FAILED");
        rig_free(&r);
    }
    test_file_port();
    printf("  file port: %s\n", t_fail ? "see above" : "ok");
    tbuf_free(&qb);
    return t_report("test_basic");
}
