/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Power-cut test.
 *
 * A random workload of puts, deletes and appends runs against emulated
 * flash whose power is cut at a random byte, over and over: in the middle
 * of a record, of reclaiming a sector, of an erase. After every cut the
 * chip "reboots" and the database is reopened and checked against a model:
 *
 *   - every acknowledged put and delete is there, unchanged;
 *   - the operation that was in flight is either fully there or not at all;
 *   - rows form one unbroken run ending at the newest acknowledged row
 *     (or the in-flight one); older rows may only be gone through rollover,
 *     oldest first, and never come back.
 *
 * Usage: test_powercut [cuts [seed]]   (default 10000; "quick" = 1000)     */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "testutil.h"

#define NKEYS 12

typedef struct model {
    int     has[NKEYS];
    char    val[NKEYS][64];
    int64_t next_time;       /* time of the next row to append */
    int64_t acked;           /* newest acknowledged row, 0 = none yet */
    int64_t first_seen;      /* oldest row seen at the last check */
} model;

typedef struct pending { int kind, key; char val[64]; int64_t time; } pending;   /* 1 put 2 del 3 row */

typedef struct stats {
    long cuts, cut_erase, applied, not_applied, ops, reopen_fail;
    uint64_t gc, dropped;
} stats;

static uint32_t rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void keyname(char *b, int k) { snprintf(b, 16, "key%02d", k); }

/* Keys written once before the first cut and never again: they are only
 * ever moved by reclaiming, so they catch a stale key index. */
#define NCOLD 4
static void coldname(char *b, int k) { snprintf(b, 16, "cold%d", k); }
static void coldval(char *b, int k) { snprintf(b, 32, "cold-value-%d-xyz", k); }

static int cold_check(altsql *db) {
    char key[16], want[32], buf[64];
    size_t n;
    int k, rc;
    for (k = 0; k < NCOLD; k++) {
        coldname(key, k);
        coldval(want, k);
        rc = altsql_get(db, key, buf, sizeof buf, &n);
        if (rc == ALTSQL_IOERR) return rc;
        CHECK(rc == ALTSQL_OK && n == strlen(want) && memcmp(buf, want, n) == 0);
    }
    return 0;
}

/* While powered: every key reads back as the model says. */
static int live_check(altsql *db, const model *m) {
    char key[16], buf[128];
    size_t n;
    int k;
    for (k = 0; k < NKEYS; k++) {
        int rc;
        keyname(key, k);
        rc = altsql_get(db, key, buf, sizeof buf, &n);
        if (rc == ALTSQL_IOERR) return rc;                     /* reads fail once the power is gone */
        CHECK(rc == (m->has[k] ? ALTSQL_OK : ALTSQL_NOTFOUND));
        if (rc == ALTSQL_OK && m->has[k]) CHECK(n == strlen(m->val[k]) && memcmp(buf, m->val[k], n) == 0);
    }
    return cold_check(db);
}

/* Runs random operations until the power goes. Returns the failing code. */
static int run_life(altsql *db, model *m, pending *p, uint32_t *rng, stats *st) {
    char key[16];
    for (;;) {
        uint32_t x = rnd(rng) % 100;
        int rc;
        p->kind = 0;
        st->ops++;
        if (st->ops % 64 == 0 && (rc = live_check(db, m)) != 0) return rc;
        if (x < 40) {
            int k = (int)(rnd(rng) % NKEYS), pad = (int)(rnd(rng) % 40), n;
            keyname(key, k);
            n = snprintf(p->val, sizeof p->val, "v%ld-", st->ops);
            while (pad-- > 0 && n < (int)sizeof p->val - 1) p->val[n++] = (char)('a' + pad % 26);
            p->val[n] = 0;
            p->kind = 1;
            p->key = k;
            rc = altsql_put(db, key, p->val, strlen(p->val));
            if (rc) return rc;
            m->has[k] = 1;
            strcpy(m->val[k], p->val);
        } else if (x < 50) {
            int k = (int)(rnd(rng) % NKEYS);
            keyname(key, k);
            p->kind = 2;
            p->key = k;
            rc = altsql_del(db, key);
            if (rc == ALTSQL_NOTFOUND) { CHECK(!m->has[k]); continue; }
            if (rc) return rc;
            CHECK(m->has[k]);
            m->has[k] = 0;
        } else {
            p->kind = 3;
            p->time = m->next_time;
            rc = altsql_append(db, "r", (int64_t)p->time, (int)(p->time % 100000) * 7);
            if (rc) return rc;
            m->acked = p->time;
            m->next_time++;
        }
    }
}

typedef struct rowcheck { int n, ok; int64_t first, last; } rowcheck;

static int row_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    rowcheck *c = (rowcheck *)ctx;
    (void)ncol; (void)names;
    if (c->n && v[0].u.i != c->last + 1) c->ok = 0;            /* unbroken run */
    if (v[1].u.i != (v[0].u.i % 100000) * 7) c->ok = 0;         /* row intact */
    if (!c->n) c->first = v[0].u.i;
    c->last = v[0].u.i;
    c->n++;
    return 0;
}

static int kv_count_cb(void *ctx, const char *k, size_t kl, const void *v, size_t vl) {
    (void)k; (void)kl; (void)v; (void)vl;
    (*(int *)ctx)++;
    return 0;
}

/* Checks the reopened database against the model and settles the in-flight op. */
static void verify(altsql *db, model *m, pending *p, stats *st) {
    char key[16], buf[128];
    size_t n;
    int k, present, listed = 0, want = 0, applied = -1;
    rowcheck c;
    for (k = 0; k < NKEYS; k++) {
        int rc;
        keyname(key, k);
        rc = altsql_get(db, key, buf, sizeof buf, &n);
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_NOTFOUND);
        present = rc == ALTSQL_OK;
        if (p->kind == 1 && p->key == k && present && n == strlen(p->val) && memcmp(buf, p->val, n) == 0) {
            m->has[k] = 1;
            strcpy(m->val[k], p->val);
            applied = 1;
            continue;
        }
        if (p->kind == 2 && p->key == k && !present && m->has[k]) {
            m->has[k] = 0;
            applied = 1;
            continue;
        }
        if (p->kind && p->kind != 3 && p->key == k) applied = 0;
        CHECK(present == m->has[k]);
        if (present && m->has[k]) CHECK(n == strlen(m->val[k]) && memcmp(buf, m->val[k], n) == 0);
    }
    for (k = 0; k < NKEYS; k++) want += m->has[k];
    CHECK_OK(db, altsql_kv_each(db, kv_count_cb, &listed));
    CHECK(listed == want + NCOLD);
    cold_check(db);

    memset(&c, 0, sizeof c);
    c.ok = 1;
    CHECK_OK(db, altsql_ts_scan(db, "r", INT64_MIN, INT64_MAX, row_cb, &c));
    CHECK(c.ok);
    if (p->kind == 3) {
        applied = c.n && c.last == p->time;
        if (applied) { m->acked = p->time; m->next_time = p->time + 1; }
    }
    if (c.n) {
        CHECK(c.last == m->acked);
        CHECK(c.first >= m->first_seen);                        /* nothing comes back */
        m->first_seen = c.first;
    }
    if (applied == 1) st->applied++;
    else if (applied == 0) st->not_applied++;
    p->kind = 0;
}

static void run_config(uint32_t ss, uint32_t sc, uint32_t align, int nomap, long cuts, uint32_t seed, stats *st) {
    rig r;
    model m;
    pending p;
    uint32_t rng = seed;
    long i;
    altsql_info info;
    int before = t_fail;
    rig_init(&r, ss, sc, align, 64 * 1024, nomap);
    memset(&m, 0, sizeof m);
    memset(&p, 0, sizeof p);
    m.next_time = 1;
    CHECK_OK(r.db, rig_open(&r));
    CHECK_OK(r.db, altsql_ts_create(r.db, "r", "time:time,v:int"));
    for (i = 0; i < NCOLD; i++) {
        char key[16], val[32];
        coldname(key, (int)i);
        coldval(val, (int)i);
        CHECK_OK(r.db, altsql_put(r.db, key, val, strlen(val)));
    }
    r.cfg.create = 0;                     /* from here on the database must always be found */
    for (i = 0; i < cuts && t_fail == before; i++) {
        int rc;
        /* mostly short lives; one in ten runs through the whole flash a few times */
        r.ram.budget = (int64_t)(rnd(&rng) % (i % 10 == 9 ? ss * sc * 3 : ss * 3));
        r.ram.rng = rnd(&rng) | 1u;
        rc = run_life(r.db, &m, &p, &rng, st);
        CHECK(rc == ALTSQL_IOERR && r.ram.dead);
        if (rc != ALTSQL_IOERR) fprintf(stderr, "  unexpected result %d: %s\n", rc, altsql_errmsg(r.db));
        st->cuts++;
        if (r.ram.dead == 2) st->cut_erase++;
        altsql_info_get(r.db, &info);                          /* counters of this life */
        st->gc += info.gc_runs;
        st->dropped += info.rows_dropped;
        rc = rig_open(&r);                                     /* reboot */
        if (rc != ALTSQL_OK) {
            st->reopen_fail++;
            CHECK_OK(r.db, rc);
            break;
        }
        verify(r.db, &m, &p, st);
    }
    printf("  %2u sectors x %4u B, align %2u, %s: %s after %ld cuts\n", sc, ss, align,
           nomap ? "read()" : "map() ", t_fail == before ? "ok" : "FAILED", i);
    rig_free(&r);
}

int main(int argc, char **argv) {
    static const struct { uint32_t ss, sc, align; int nomap; } v[] = {
        { 1024, 8, 4, 0 }, { 512, 16, 1, 1 }, { 1024, 8, 8, 1 }, { 2048, 6, 16, 0 },
    };
    long cuts = 10000;
    uint32_t seed = 0x1234567u;
    size_t i;
    stats st;
    if (argc > 1) cuts = strcmp(argv[1], "quick") == 0 ? 1000 : atol(argv[1]);
    if (argc > 2) seed = (uint32_t)strtoul(argv[2], NULL, 0);
    memset(&st, 0, sizeof st);
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        run_config(v[i].ss, v[i].sc, v[i].align, v[i].nomap, cuts / 4, seed + (uint32_t)i * 7919u, &st);
    printf("  %ld power cuts (%ld during an erase), %ld operations; in-flight op kept %ld, dropped %ld;\n"
           "  %llu sectors reclaimed, %llu rows rolled over; reopen failures: %ld\n",
           st.cuts, st.cut_erase, st.ops, st.applied, st.not_applied,
           (unsigned long long)st.gc, (unsigned long long)st.dropped, st.reopen_fail);
    return t_report("test_powercut");
}
