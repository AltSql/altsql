/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Sync test: a sensor streams to two gateway replicas over a lossy link.
 *
 * One replica takes everything; the other takes only per-minute summaries
 * (and the key-value state). The link loses batches and acknowledgements,
 * and goes down for 25 minutes, long enough for the sensor to roll over
 * its oldest raw rows. A key deleted during the outage must still be
 * deleted on the gateway afterwards. */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "testutil.h"

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

typedef struct link {
    rig *dev, *gw;
    uint32_t cursor;                 /* newest seq the gateway has confirmed */
    altsql_sync_filter filter;
    void *fctx;
    long batches, lost, lost_acks, bytes;
} link;

/* One sync round: batches until the device is caught up or a batch is lost. */
static int sync_round(link *l, int loss_pct) {
    uint8_t buf[512];
    for (;;) {
        size_t len;
        uint32_t last;
        int rc = altsql_sync_read(l->dev->db, l->cursor, buf, sizeof buf, &len, &last, l->filter, l->fctx);
        if (rc < 0) return rc;
        if (len) {
            l->batches++;
            if ((int)(rnd() % 100) < loss_pct) { l->lost++; return 1; }          /* batch lost */
            l->bytes += (long)len;
            CHECK_OK(l->gw->db, altsql_sync_apply(l->gw->db, buf, len, NULL));
            if ((int)(rnd() % 100) < loss_pct) { l->lost_acks++; return 1; }     /* ack lost: resent later */
        }
        l->cursor = last;
        if (rc == ALTSQL_DONE) return 0;
    }
}

static int rows_of(void *ctx, int kind, int series_id) {
    return kind == ALTSQL_REC_ROW && series_id == *(int *)ctx;
}

static int count_one(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)n; (void)names;
    *(int *)ctx = (int)v[0].u.i;
    return 0;
}

static int summary_only(void *ctx, int kind, int series_id) {
    return kind == ALTSQL_REC_KV || kind == ALTSQL_REC_SCHEMA || series_id == *(int *)ctx;
}

/* ---- Collecting rows and keys for comparison ------------------------------------- */
typedef struct rows { int n; int64_t t[20000]; double a[20000], b[20000]; } rows;

static int rows_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    rows *r = (rows *)ctx;
    (void)names;
    if (r->n < 20000) {
        r->t[r->n] = v[0].u.i;
        r->a[r->n] = v[1].u.r;
        r->b[r->n] = ncol > 2 ? v[2].u.r : 0.0;
        r->n++;
    }
    return 0;
}

static void get_rows(altsql *db, const char *series, rows *r) {
    r->n = 0;
    CHECK_OK(db, altsql_ts_scan(db, series, INT64_MIN, INT64_MAX, rows_cb, r));
}

/* Is every row of a present in b (same time, same values)? Both are in time order. */
static int subset(const rows *a, const rows *b) {
    int i, j = 0;
    for (i = 0; i < a->n; i++) {
        while (j < b->n && b->t[j] < a->t[i]) j++;
        if (j == b->n || b->t[j] != a->t[i] || b->a[j] != a->a[i] || b->b[j] != a->b[i]) return 0;
    }
    return 1;
}

static int kv_text_cb(void *ctx, const char *k, size_t kl, const void *v, size_t vl) {
    tbuf *b = (tbuf *)ctx;
    tbuf_write(b, k, kl);
    tbuf_write(b, "=", 1);
    tbuf_write(b, (const char *)v, vl);
    tbuf_write(b, ";", 1);
    return 0;
}

static int same_kv(altsql *x, altsql *y) {
    tbuf a = {0, 0, 0}, b = {0, 0, 0};
    int same;
    altsql_kv_each(x, kv_text_cb, &a);
    altsql_kv_each(y, kv_text_cb, &b);
    same = (!a.p && !b.p) || (a.p && b.p && a.n == b.n && memcmp(a.p, b.p, a.n) == 0);
    if (!same) fprintf(stderr, "  device kv:  %s\n  gateway kv: %s\n", a.p ? a.p : "", b.p ? b.p : "");
    tbuf_free(&a);
    tbuf_free(&b);
    return same;
}

static int tbuf_write_null(void *ctx, const char *d, size_t n) { (void)ctx; (void)d; (void)n; return 0; }

int main(void) {
    rig dev, raw, sum;
    link lraw, lsum;
    rows *dr = (rows *)malloc(sizeof(rows)), *gr = (rows *)malloc(sizeof(rows));
    altsql_info info;
    int minute_id, t, rc;
    int64_t pre_max = 0, resume_first = 0;
    char buf[32];
    size_t n;

    rig_init(&dev, 1024, 16, 4, 64 * 1024, 0);          /* 16 KB sensor flash */
    rig_init(&raw, 1024, 256, 4, 64 * 1024, 0);         /* 256 KB gateway copy */
    rig_init(&sum, 1024, 16, 4, 64 * 1024, 1);
    raw.cfg.replica = sum.cfg.replica = 1;
    CHECK_OK(dev.db, rig_open(&dev));
    CHECK_OK(raw.db, rig_open(&raw));
    CHECK_OK(sum.db, rig_open(&sum));

    CHECK_OK(dev.db, altsql_ts_create(dev.db, "raw", "time:time,temp:float"));
    CHECK_OK(dev.db, altsql_ts_create(dev.db, "minute", "time:time,avg:float,max:float"));
    CHECK_OK(dev.db, altsql_put(dev.db, "cfg.rate", "1", 1));
    minute_id = altsql_series_id(dev.db, "minute");
    CHECK(minute_id > 0 && altsql_series_id(dev.db, "nope") == 0);

    memset(&lraw, 0, sizeof lraw);
    memset(&lsum, 0, sizeof lsum);
    lraw.dev = lsum.dev = &dev;
    lraw.gw = &raw;
    lsum.gw = &sum;
    lsum.filter = summary_only;
    lsum.fctx = &minute_id;

    for (t = 1; t <= 6000; t++) {
        double temp = 40.0 + 25.0 * ((t / 300) % 2) + (double)(rnd() % 100) / 10.0;
        CHECK_OK(dev.db, altsql_append(dev.db, "raw", (int64_t)t, temp));
        if (t % 60 == 0) {
            altsql_stats st;
            CHECK_OK(dev.db, altsql_ts_window(dev.db, "raw", "temp", t - 59, &st));
            CHECK(st.count == 60);
            CHECK_OK(dev.db, altsql_append(dev.db, "minute", (int64_t)t, st.avg, st.max));
            CHECK_OK(dev.db, altsql_put(dev.db, "state", st.max > 60.0 ? "hot" : "ok", st.max > 60.0 ? 3 : 2));
        }
        if (t == 1000) CHECK_OK(dev.db, altsql_put(dev.db, "tmp", "x", 1));
        if (t == 2100) CHECK_OK(dev.db, altsql_del(dev.db, "tmp"));      /* during the outage */
        if (t == 2000) {                        /* outage starts: what does the gateway have? */
            get_rows(raw.db, "raw", gr);
            pre_max = gr->n ? gr->t[gr->n - 1] : 0;
        }
        if (t == 3500) {                        /* link back: what does the sensor still have? */
            get_rows(dev.db, "raw", dr);
            resume_first = dr->t[0];
        }
        if (t % 10 == 0 && (t < 2000 || t >= 3500)) {                    /* link down 2000..3499 */
            CHECK((rc = sync_round(&lraw, 10)) >= 0);
            CHECK((rc = sync_round(&lsum, 10)) >= 0);
        }
    }
    while (sync_round(&lraw, 0) != 0) {}
    while (sync_round(&lsum, 0) != 0) {}

    /* the gateway saw the delete made while it was offline */
    CHECK_RC(raw.db, altsql_get(raw.db, "tmp", buf, sizeof buf, &n), ALTSQL_NOTFOUND);
    CHECK_RC(sum.db, altsql_get(sum.db, "tmp", buf, sizeof buf, &n), ALTSQL_NOTFOUND);
    CHECK(same_kv(dev.db, raw.db));
    CHECK(same_kv(dev.db, sum.db));

    /* raw replica: everything the sensor still has, plus older rows it rolled over */
    altsql_info_get(dev.db, &info);
    CHECK(info.rows_dropped > 0);
    get_rows(dev.db, "raw", dr);
    get_rows(raw.db, "raw", gr);
    CHECK(dr->n > 0 && gr->n > dr->n && subset(dr, gr));
    CHECK(gr->t[gr->n - 1] == 6000);
    {   /* exactly: everything before the outage, then all the sensor kept from then on */
        int i, ok = gr->n == (int)(pre_max + (6000 - resume_first + 1));
        for (i = 0; ok && i < gr->n; i++)
            ok = gr->t[i] == (i < pre_max ? i + 1 : resume_first + (i - pre_max));
        CHECK(ok);
        printf("  outage: gateway had rows 1..%lld, sensor still had %lld.. when the link came back\n",
               (long long)pre_max, (long long)resume_first);
    }
    printf("  sensor keeps %d raw rows (rolled over %u); gateway has %d\n", dr->n, info.rows_dropped, gr->n);
    get_rows(dev.db, "minute", dr);
    get_rows(raw.db, "minute", gr);
    CHECK(subset(dr, gr));

    /* summary replica: minute rows only, and every one the sensor still has */
    get_rows(sum.db, "raw", gr);
    CHECK(gr->n == 0);
    get_rows(sum.db, "minute", gr);
    CHECK(gr->n > 0 && subset(dr, gr));
    printf("  summary gateway has %d minute rows, no raw rows\n", gr->n);
    printf("  link to raw gateway: %ld bytes in %ld batches (%ld lost, %ld acks lost)\n",
           lraw.bytes, lraw.batches, lraw.lost, lraw.lost_acks);
    printf("  link to summary gateway: %ld bytes in %ld batches (%ld lost, %ld acks lost)\n",
           lsum.bytes, lsum.batches, lsum.lost, lsum.lost_acks);
    CHECK(lsum.bytes * 10 < lraw.bytes);

    /* SQL on the gateway works on the synced rows */
    {
        tbuf out = {0, 0, 0};
        CHECK_OK(raw.db, altsql_exec(raw.db, "SELECT COUNT(*) FROM minute WHERE max > 60", NULL, NULL));
        (void)out;
    }

    /* resending is harmless; damaged data is refused; a replica refuses local writes */
    {
        uint8_t b[512];
        size_t len;
        uint32_t last, before, after;
        altsql_info i1, i2;
        CHECK(altsql_sync_read(dev.db, 0, b, sizeof b, &len, &last, NULL, NULL) == ALTSQL_OK && len > 0);
        altsql_info_get(raw.db, &i1);
        before = i1.last_seq;
        CHECK_OK(raw.db, altsql_sync_apply(raw.db, b, len, &after));
        altsql_info_get(raw.db, &i2);
        CHECK(after == before && i2.used_sectors == i1.used_sectors);
        b[len / 2] ^= 0x40;
        CHECK_RC(raw.db, altsql_sync_apply(raw.db, b, len, NULL), ALTSQL_CORRUPT);
        CHECK_RC(raw.db, altsql_sync_apply(raw.db, b, 5, NULL), ALTSQL_CORRUPT);
        CHECK_RC(dev.db, altsql_sync_read(dev.db, 0, b, 8, &len, &last, NULL, NULL), ALTSQL_TOOBIG);
        CHECK_RC(dev.db, altsql_sync_apply(dev.db, b, len, NULL), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_sync_read(raw.db, 0, b, sizeof b, &len, &last, NULL, NULL), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_put(raw.db, "k", "v", 1), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_del(raw.db, "state"), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_ts_create(raw.db, "x", "time:time"), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_append(raw.db, "raw", (int64_t)1, 1.0), ALTSQL_MISUSE);
        CHECK_RC(raw.db, altsql_exec(raw.db, "INSERT INTO raw VALUES (1, 2.0)", NULL, NULL), ALTSQL_MISUSE);
    }

    /* the gateway restarts and carries on without duplicates */
    rig_close(&raw);
    CHECK_OK(raw.db, rig_open(&raw));
    get_rows(raw.db, "raw", gr);
    {
        int before = gr->n, i, ordered = 1;
        CHECK_OK(dev.db, altsql_append(dev.db, "raw", (int64_t)6001, 1.0));
        lraw.cursor = 0;                                     /* worst case: device resends it all */
        while (sync_round(&lraw, 0) != 0) {}
        get_rows(raw.db, "raw", gr);
        CHECK(gr->n == before + 1);
        for (i = 1; i < gr->n; i++) if (gr->t[i] <= gr->t[i - 1]) ordered = 0;
        CHECK(ordered);
    }
    CHECK(same_kv(dev.db, raw.db));

    /* A tiny radio link: 51-byte messages carrying one series only. The
     * gateway got the definitions at setup, so the filter leaves them out,
     * even after reclaiming copies them forward with new numbers. */
    {
        rig d2, g2;
        link lt;
        int day_id, day, k, got;
        rig_init(&d2, 512, 8, 4, 16 * 1024, 0);
        rig_init(&g2, 1024, 32, 4, 64 * 1024, 0);
        g2.cfg.replica = 1;
        CHECK_OK(d2.db, rig_open(&d2));
        CHECK_OK(g2.db, rig_open(&g2));
        CHECK_OK(d2.db, altsql_ts_create(d2.db, "day", "time:time,min:float,avg:float,max:float,irrigation:int"));
        CHECK_OK(d2.db, altsql_put(d2.db, "valve", "closed", 6));
        memset(&lt, 0, sizeof lt);
        lt.dev = &d2;
        lt.gw = &g2;
        while (sync_round(&lt, 0) != 0) {}                       /* setup, over a local link */
        day_id = altsql_series_id(d2.db, "day");
        for (day = 0; day < 40; day++) {
            uint8_t b[51];
            size_t len;
            uint32_t last;
            for (k = 0; k < 30; k++)                               /* churn: forces reclaiming */
                CHECK_OK(d2.db, altsql_put(d2.db, "valve", k % 2 ? "open" : "closed", k % 2 ? 4 : 6));
            CHECK_OK(d2.db, altsql_append(d2.db, "day", (int64_t)day * 86400, 20.0, 25.0, 30.0, day));
            for (;;) {
                int r2 = altsql_sync_read(d2.db, lt.cursor, b, sizeof b, &len, &last, rows_of, &day_id);
                CHECK(r2 == ALTSQL_OK || r2 == ALTSQL_DONE);
                if (r2 < 0) break;
                if (len) CHECK_OK(g2.db, altsql_sync_apply(g2.db, b, len, NULL));
                CHECK(len <= 51);
                lt.cursor = last;
                if (r2 == ALTSQL_DONE) break;
            }
        }
        {
            altsql_info in;
            altsql_info_get(d2.db, &in);
            CHECK(in.gc_runs > 0);
        }
        got = 0;
        CHECK_OK(g2.db, altsql_exec(g2.db, "SELECT COUNT(*) FROM day", count_one, &got));
        CHECK(got == 40);
        rig_free(&d2);
        rig_free(&g2);
    }

    /* Found by fuzzing: a replica must refuse a series definition it could
     * not use later (a bad name, id 0, or a delete of a definition). */
    {
        rig g3;
        static const struct { const char *key; uint32_t id; int type; int want; } c[] = {
            { "\x01Sraw", 1, 1, ALTSQL_OK },
            { "\x01Sra w", 2, 1, ALTSQL_CORRUPT },
            { "\x01Sraw\x01", 3, 1, ALTSQL_CORRUPT },
            { "\x01Sokay", 0, 1, ALTSQL_CORRUPT },
            { "\x01Sraw", 1, 2, ALTSQL_CORRUPT },
        };
        size_t i;
        rig_init(&g3, 1024, 16, 4, 64 * 1024, 0);
        g3.cfg.replica = 1;
        CHECK_OK(g3.db, rig_open(&g3));
        for (i = 0; i < sizeof c / sizeof c[0]; i++) {
            uint8_t rec[128];
            const char *schema = "time:time,v:float";
            uint32_t klen = (uint32_t)strlen(c[i].key), len;
            len = 1 + klen + (c[i].type == 1 ? 2 + (uint32_t)strlen(schema) : 0);
            rec[0] = 0xA5;
            rec[1] = (uint8_t)c[i].type;
            as_put16(rec + 2, len);
            as_put32(rec + 4, (uint32_t)(10 + i));
            rec[12] = (uint8_t)klen;
            memcpy(rec + 13, c[i].key, klen);
            if (c[i].type == 1) {
                as_put16(rec + 13 + klen, c[i].id);
                memcpy(rec + 15 + klen, schema, strlen(schema));
            }
            as_put32(rec + 8, as_crc32(as_crc32(0, rec + 1, 7), rec + 12, len));
            CHECK_RC(g3.db, altsql_sync_apply(g3.db, rec, 12 + len, NULL), c[i].want);
        }
        CHECK_OK(g3.db, altsql_export(g3.db, tbuf_write_null, NULL));
        rig_close(&g3);
        CHECK_OK(g3.db, rig_open(&g3));
        CHECK(altsql_series_id(g3.db, "raw") == 1);
        rig_free(&g3);
    }

    rig_free(&dev);
    rig_free(&raw);
    rig_free(&sum);
    free(dr);
    free(gr);
    return t_report("test_sync");
}
