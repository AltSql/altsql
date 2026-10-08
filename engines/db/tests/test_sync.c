/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: sync from AltSql Core devices into the tree (step 5).
 * Real Core devices (Core's flash emulated in RAM) write rows and key-value
 * pairs and hand out their records with altsql_sync_read. The gateway applies
 * the batches. Checked: every row and pair arrives once, the stored payloads
 * are the devices' bytes exactly, resends and stale batches change nothing,
 * damaged and cut batches keep only what came before the damage, a different
 * layout is refused, and a power cut at any write of the gateway loses nothing
 * once the devices resend from the confirmed position.
 *   test_sync          the full run
 *   test_sync quick    fewer cuts */
#include "dbtest.h"

typedef struct dev {
    uint8_t *flash, *mem;
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql_config cfg;
    altsql *db;
    int64_t id;
    uint32_t confirmed;
    uint32_t t;                 /* next reading time */
} dev;

static int dev_open(dev *d, int64_t id, int other_layout) {
    memset(d, 0, sizeof *d);
    d->flash = (uint8_t *)malloc(64 * 4096);
    d->mem = (uint8_t *)malloc(48 * 1024);
    memset(d->flash, 0xFF, 64 * 4096);                  /* an erased chip */
    altsql_ram_flash_init(&d->fl, &d->ram, d->flash, 4096, 64, 4);
    d->ram.budget = -1;
    d->cfg.mem = d->mem;
    d->cfg.mem_size = 48 * 1024;
    d->cfg.create = 1;
    d->id = id;
    d->t = 1700000000u;
    CHECK(altsql_open(&d->db, &d->fl, &d->cfg) == ALTSQL_OK, "device open");
    CHECK(altsql_ts_create(d->db, "temps", other_layout ? "time:time,machine:long,temp:real" : "time:time,machine:int,temp:float") == ALTSQL_OK, "series temps");
    CHECK(altsql_ts_create(d->db, "status", "time:time,code:int,msg:text") == ALTSQL_OK, "series status");
    return 0;
}
static void dev_close(dev *d) { altsql_close(d->db); free(d->flash); free(d->mem); }

/* Some new records: readings, a status line now and then, key-value puts and deletes. */
static int dev_work(dev *d, uint32_t *s, int n) {
    int i;
    for (i = 0; i < n; i++) {
        uint32_t r = xs(s) % 100;
        if (r < 70) {
            double temp = (double)(xs(s) % 4000) / 100.0;               /* draws right to left, in the order gcc on x86-64 made them */
            int mach = (int)(xs(s) % 8);
            CHECK(altsql_append(d->db, "temps", (int64_t)d->t, mach, temp) == ALTSQL_OK,
                  "append: %s", altsql_errmsg(d->db));
            d->t += 1 + xs(s) % 3;
        } else if (r < 80) {
            char msg[40];
            int len = sprintf(msg, "state %u", xs(s) % 1000);
            msg[len] = 0;
            CHECK(altsql_append(d->db, "status", (int64_t)d->t, (int)(xs(s) % 5), msg) == ALTSQL_OK, "append status");
        } else if (r < 95) {
            char key[16], val[32];
            sprintf(key, "k%u", xs(s) % 12);
            sprintf(val, "v%u", xs(s));
            CHECK(altsql_put(d->db, key, val, strlen(val)) == ALTSQL_OK, "put");
        } else {
            char key[16];
            sprintf(key, "k%u", xs(s) % 12);
            altsql_del(d->db, key);
        }
    }
    return 0;
}

/* Hands the device's records to the gateway until caught up. cap: batch size. */
static int dev_sync(dev *d, altsql_db *gw, size_t cap) {
    static uint8_t buf[1 << 16];
    size_t n;
    uint32_t last, conf;
    int rc;
    do {
        rc = altsql_sync_read(d->db, d->confirmed, buf, cap, &n, &last, NULL, NULL);
        CHECK(rc >= 0, "sync_read: %d %s", rc, altsql_errmsg(d->db));
        if (n) {
            int rc2 = altsql_db_sync_apply(gw, d->id, d->confirmed, buf, n, &conf);
            if (rc2) return rc2;
            CHECK(conf == last, "confirmed %u, batch ends at %u", conf, last);
            d->confirmed = conf;
        }
    } while (rc == ALTSQL_OK);
    return 0;
}

/* ---- the gateway against the devices ---- */
typedef struct rows { int64_t t[20000]; int64_t a[20000]; double r[20000]; char txt[20000][24]; int n; } rows;
static rows g_dev, g_gw;

static int dev_row_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    rows *R = (rows *)ctx;
    (void)ncol; (void)names;
    R->t[R->n] = v[0].u.i;
    R->a[R->n] = v[1].u.i;
    if (v[2].type == ALTSQL_TEXT) { memcpy(R->txt[R->n], v[2].u.s, (size_t)v[2].len); R->txt[R->n][v[2].len] = 0; R->r[R->n] = 0; }
    else { R->r[R->n] = v[2].u.r; R->txt[R->n][0] = 0; }
    R->n++;
    return 0;
}

typedef struct kvs { char k[64][16]; char v[64][40]; int n; } kvs;
static int dev_kv_cb(void *ctx, const char *key, size_t klen, const void *val, size_t vlen) {
    kvs *K = (kvs *)ctx;
    memcpy(K->k[K->n], key, klen); K->k[K->n][klen] = 0;
    memcpy(K->v[K->n], val, vlen); K->v[K->n][vlen] = 0;
    K->n++;
    return 0;
}

/* The gateway holds exactly what the device holds, row for row and pair for pair. */
static int same_as_device(altsql_db *gw, dev *d) {
    static const char *series[2] = { "temps", "status" };
    altsql_db_cursor c;
    altsql_value pre, v[8];
    int si, rc, i;
    for (si = 0; si < 2; si++) {
        g_dev.n = g_gw.n = 0;
        CHECK(altsql_ts_scan(d->db, series[si], INT64_MIN, INT64_MAX, dev_row_cb, &g_dev) == ALTSQL_OK, "device scan");
        pre.type = ALTSQL_INTEGER; pre.u.i = d->id; pre.len = 0;
        rc = altsql_db_row_seek(&c, gw, series[si], &pre, 1);
        while (rc == ALTSQL_OK) {
            CHECK(altsql_db_row_read(&c, v, 8) == ALTSQL_OK, "row_read: %s", altsql_db_errmsg(gw));
            CHECK(v[0].u.i == d->id, "device column");
            g_gw.t[g_gw.n] = v[2].u.i;
            g_gw.a[g_gw.n] = v[3].u.i;
            if (v[4].type == ALTSQL_TEXT) { memcpy(g_gw.txt[g_gw.n], v[4].u.s, (size_t)v[4].len); g_gw.txt[g_gw.n][v[4].len] = 0; g_gw.r[g_gw.n] = 0; }
            else { g_gw.r[g_gw.n] = v[4].u.r; g_gw.txt[g_gw.n][0] = 0; }
            g_gw.n++;
            rc = altsql_db_next(&c);
        }
        CHECK(rc == ALTSQL_NOTFOUND || (rc == ALTSQL_SCHEMA && g_dev.n == 0), "gateway scan: %d %s", rc, altsql_db_errmsg(gw));
        CHECK(g_dev.n == g_gw.n, "device %lld %s: %d rows on the device, %d on the gateway", (long long)d->id, series[si], g_dev.n, g_gw.n);
        for (i = 0; i < g_dev.n; i++)
            CHECK(g_dev.t[i] == g_gw.t[i] && g_dev.a[i] == g_gw.a[i] && g_dev.r[i] == g_gw.r[i] && !strcmp(g_dev.txt[i], g_gw.txt[i]),
                  "device %lld %s row %d differs", (long long)d->id, series[si], i);
    }
    {
        static kvs K;
        int n = 0;
        K.n = 0;
        CHECK(altsql_kv_each(d->db, dev_kv_cb, &K) == ALTSQL_OK, "kv_each");
        pre.type = ALTSQL_INTEGER; pre.u.i = d->id; pre.len = 0;
        rc = altsql_db_row_seek(&c, gw, "kv", &pre, 1);
        while (rc == ALTSQL_OK) {
            int j, found = 0;
            CHECK(altsql_db_row_read(&c, v, 8) == ALTSQL_OK, "kv row_read");
            for (j = 0; j < K.n; j++)
                if ((int)strlen(K.k[j]) == v[1].len && !memcmp(K.k[j], v[1].u.s, (size_t)v[1].len)) {
                    found = 1;
                    CHECK((int)strlen(K.v[j]) == v[2].len && !memcmp(K.v[j], v[2].u.s, (size_t)v[2].len), "kv value differs");
                }
            CHECK(found, "the gateway holds a key the device deleted");
            n++;
            rc = altsql_db_next(&c);
        }
        CHECK(n == K.n, "device %lld: %d keys on the device, %d on the gateway", (long long)d->id, K.n, n);
    }
    return 0;
}

/* Every record the devices sent, by (device, seq): the payload bytes, to compare byte for byte. */
#define NSEEN 200000
static struct seen { int64_t dev; uint32_t seq, len, off; } g_seen[NSEEN];
static uint8_t g_seenbuf[8 << 20];
static uint32_t g_nseen, g_seenused;
static void note_batch(int64_t device, const uint8_t *b, size_t n) {
    size_t off = 0;
    while (off + 12 <= n && g_nseen < NSEEN) {
        uint32_t plen = as_get16(b + off + 2);
        if (b[off + 1] == 3 && g_seenused + plen <= sizeof g_seenbuf) {
            g_seen[g_nseen].dev = device;
            g_seen[g_nseen].seq = as_get32(b + off + 4);
            g_seen[g_nseen].len = plen;
            g_seen[g_nseen].off = g_seenused;
            memcpy(g_seenbuf + g_seenused, b + off + 12, plen);
            g_seenused += plen;
            g_nseen++;
        }
        off += 12 + plen;
    }
}
static int bytes_as_sent(altsql_db *gw, const char *table) {
    altsql_db_cursor c;
    altsql_value v[8];
    uint8_t val[512];
    size_t vn;
    int rc = altsql_db_row_seek(&c, gw, table, NULL, 0), checked = 0;
    (void)checked;
    while (rc == ALTSQL_OK) {
        uint32_t i;
        int found = 0;
        CHECK(altsql_db_row_read(&c, v, 8) == ALTSQL_OK, "row_read");
        CHECK(altsql_db_value(&c, val, sizeof val, &vn) == ALTSQL_OK, "value");
        for (i = g_nseen; i-- > 0; )
            if (g_seen[i].dev == v[0].u.i && g_seen[i].seq == (uint32_t)v[1].u.i) {
                found = 1;
                CHECK(g_seen[i].len == vn && !memcmp(g_seenbuf + g_seen[i].off, val, vn), "stored payload differs from the device's bytes");
                break;
            }
        CHECK(found, "a stored row was never sent");
        checked++;
        (void)checked;
        rc = altsql_db_next(&c);
    }
    return 0;
}

static int t_fleet(void) {
    ramfile rf;
    altsql_db *gw;
    void *mem = malloc(1 << 21);
    dev d[6];
    uint32_t s = 4242, round, i;
    uint8_t buf[1 << 14];
    size_t n;
    uint32_t last, conf, before;
    int rc;
    ram_new(&rf, 1u << 26, 1u << 26);
    CHECK(db_open_ram(&gw, &rf, mem, 1 << 21, 4096) == ALTSQL_OK, "gateway open");
    for (i = 0; i < 6; i++) if (dev_open(&d[i], 1000 + i * 7, 0)) return 1;
    g_nseen = g_seenused = 0;
    for (round = 0; round < 40; round++) {
        for (i = 0; i < 6; i++) {
            size_t cap = 64 + xs(&s) % 3000;
            if (dev_work(&d[i], &s, (int)(xs(&s) % 60))) return 1;
            /* the batch is noted before it is applied, so the bytes can be compared afterwards */
            rc = altsql_sync_read(d[i].db, d[i].confirmed, buf, cap, &n, &last, NULL, NULL);
            CHECK(rc >= 0, "sync_read");
            note_batch(d[i].id, buf, n);
            if (n) {
                CHECK(altsql_db_sync_apply(gw, d[i].id, d[i].confirmed, buf, n, &conf) == ALTSQL_OK, "apply: %s", altsql_db_errmsg(gw));
                CHECK(conf == last, "confirmed position");
                /* the same batch again changes nothing */
                CHECK(altsql_db_sync_apply(gw, d[i].id, d[i].confirmed, buf, n, &conf) == ALTSQL_OK && conf == last, "resend");
                d[i].confirmed = conf;
            }
        }
    }
    for (i = 0; i < 6; i++) {
        uint8_t big[1 << 16];
        do {
            rc = altsql_sync_read(d[i].db, d[i].confirmed, big, sizeof big, &n, &last, NULL, NULL);
            note_batch(d[i].id, big, n);
            if (n) { CHECK(altsql_db_sync_apply(gw, d[i].id, d[i].confirmed, big, n, &conf) == ALTSQL_OK, "apply"); d[i].confirmed = conf; }
        } while (rc == ALTSQL_OK);
        if (same_as_device(gw, &d[i])) return 1;
    }
    if (bytes_as_sent(gw, "temps") || bytes_as_sent(gw, "status")) return 1;
    /* a stale batch, from before the confirmed position, is skipped */
    before = d[0].confirmed;
    rc = altsql_sync_read(d[0].db, 0, buf, 2000, &n, &last, NULL, NULL);
    CHECK(altsql_db_sync_apply(gw, d[0].id, 0, buf, n, &conf) == ALTSQL_OK && conf == before, "a stale batch moves nothing");
    if (same_as_device(gw, &d[0])) return 1;
    /* a cut batch keeps what came before the cut */
    if (dev_work(&d[1], &s, 200)) return 1;
    rc = altsql_sync_read(d[1].db, d[1].confirmed, buf, sizeof buf, &n, &last, NULL, NULL);
    note_batch(d[1].id, buf, n);
    rc = altsql_db_sync_apply(gw, d[1].id, d[1].confirmed, buf, n / 2, &conf);
    CHECK(rc == ALTSQL_CORRUPT && conf > d[1].confirmed && conf < last, "a cut batch: %d, confirmed %u of %u", rc, conf, last);
    d[1].confirmed = conf;
    if (dev_sync(&d[1], gw, 5000) || same_as_device(gw, &d[1])) return 1;
    /* damaged batches: any flipped byte, never a crash, never a wrong row */
    if (dev_work(&d[2], &s, 300)) return 1;
    rc = altsql_sync_read(d[2].db, d[2].confirmed, buf, sizeof buf, &n, &last, NULL, NULL);
    note_batch(d[2].id, buf, n);
    for (i = 0; i < 300; i++) {
        uint8_t copy[1 << 14];
        uint32_t pos = xs(&s) % (uint32_t)n;
        memcpy(copy, buf, n);
        copy[pos] ^= (uint8_t)(1 + xs(&s) % 255);
        rc = altsql_db_begin(gw, 1);
        rc = altsql_db_sync_apply(gw, d[2].id, d[2].confirmed, copy, n, &conf);
        CHECK(rc == ALTSQL_OK || rc == ALTSQL_CORRUPT, "flipped byte %u: %d %s", pos, rc, altsql_db_errmsg(gw));
        CHECK(altsql_db_rollback(gw) == ALTSQL_OK, "rollback");
    }
    if (dev_sync(&d[2], gw, 5000) || same_as_device(gw, &d[2])) return 1;
    /* a device whose series has another layout is refused, and the message names the series */
    {
        dev x;
        if (dev_open(&x, 77, 1) || dev_work(&x, &s, 30)) return 1;
        rc = altsql_sync_read(x.db, 0, buf, sizeof buf, &n, &last, NULL, NULL);
        rc = altsql_db_sync_apply(gw, 77, 0, buf, n, &conf);
        CHECK(rc == ALTSQL_SCHEMA && strstr(altsql_db_errmsg(gw), "temps"), "a different layout: %d %s", rc, altsql_db_errmsg(gw));
        CHECK(altsql_db_sync_state(gw, 77, &conf) == ALTSQL_OK && conf == 0, "a refused batch moves nothing");
        dev_close(&x);
    }
    {
        altsql_db_tableinfo ti;
        CHECK(altsql_db_table_info(gw, "temps", &ti) == ALTSQL_OK && ti.kind == ALTSQL_DB_SYNCED && ti.ncols == 5 &&
              !strcmp(ti.names[0], "device") && !strcmp(ti.names[1], "seq") && !strcmp(ti.names[2], "time"), "synced table layout");
        altsql_value r[5];
        r[0].type = ALTSQL_INTEGER; r[0].u.i = 1; r[0].len = 0;
        r[1] = r[0]; r[2] = r[0]; r[3] = r[0]; r[4].type = ALTSQL_REAL; r[4].u.r = 1; r[4].len = 0;
        CHECK(altsql_db_row_put(gw, "temps", r, 5) == ALTSQL_MISUSE, "a synced table takes rows only from sync");
    }
    if (check_slots(gw, 1)) return 1;
    printf("  six devices, 40 rounds, batches of 64 to 3,064 bytes: %u rows stored as sent; resends, stale, cut and damaged batches: ok\n", g_nseen);
    for (i = 0; i < 6; i++) dev_close(&d[i]);
    altsql_db_close(gw);
    ram_free(&rf);
    free(mem);
    return 0;
}

/* ---- power cuts on the gateway during sync ---- */
static int session(altsql_db *gw, dev *d, int nd, uint32_t seed) {
    uint32_t s = seed, round;
    int i, rc;
    for (round = 0; round < 6; round++)
        for (i = 0; i < nd; i++) {
            size_t cap = 200 + xs(&s) % 1500;
            if ((rc = dev_sync(&d[i], gw, cap)) != 0) return rc;
        }
    return 0;
}

static int t_powercut(int quick) {
    ramfile rf;
    altsql_db *gw;
    void *mem = malloc(1 << 20);
    dev d[3];
    uint32_t s = 99, k, total, i;
    unsigned long cuts = 0;
    ram_new(&rf, 1u << 24, 1u << 24);
    for (i = 0; i < 3; i++) { if (dev_open(&d[i], 50 + i, 0) || dev_work(&d[i], &s, 400)) return 1; }
    /* a session without a cut: how many writes the gateway makes */
    altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
    CHECK(db_open_ram(&gw, &rf, mem, 1 << 20, 1024) == ALTSQL_OK, "open");
    total = (uint32_t)rf.r.writes;
    CHECK(session(gw, d, 3, 7) == 0, "dry session: %s", altsql_db_errmsg(gw));
    total = (uint32_t)rf.r.writes - total;
    altsql_db_close(gw);
    for (k = 1; k <= total; k += quick ? 7 : 1) {
        for (i = 0; i < 3; i++) d[i].confirmed = 0;
        altsql_db_ram_init(&rf.f, &rf.r, rf.mem, rf.disk, rf.r.cap, rf.log, rf.r.logcap);
        CHECK(db_open_ram(&gw, &rf, mem, 1 << 20, 1024) == ALTSQL_OK, "open");
        rf.r.cut = k;
        session(gw, d, 3, 7);                        /* runs until the power goes */
        altsql_db_close(gw);
        altsql_db_ram_powercut(&rf.r, k);
        cuts++;
        CHECK(db_open_ram(&gw, &rf, mem, 1 << 20, 1024) == ALTSQL_OK, "cut %u: open after the cut: %s", k, altsql_db_errmsg(gw));
        /* each device resumes from what the gateway says it holds */
        for (i = 0; i < 3; i++) CHECK(altsql_db_sync_state(gw, d[i].id, &d[i].confirmed) == ALTSQL_OK, "state");
        CHECK(session(gw, d, 3, 8) == 0, "cut %u: the session after the cut failed: %s", k, altsql_db_errmsg(gw));
        for (i = 0; i < 3; i++) if (same_as_device(gw, &d[i])) { printf("  after cut %u\n", k); return 1; }
        if (check_slots(gw, 0)) return 1;
        altsql_db_close(gw);
    }
    printf("  power cuts on the gateway during sync: %lu cuts, %u writes in the session; every device complete after resending: ok\n", cuts, total);
    for (i = 0; i < 3; i++) dev_close(&d[i]);
    ram_free(&rf);
    free(mem);
    return 0;
}

/* ---- sync in order (0.2): devices that send several batches ahead over a link that loses,
 * repeats and reorders them. Each batch carries the position it was read from; the gateway
 * applies it only when nothing is missing before it, and otherwise answers ALTSQL_DB_GAP with
 * its own position, from which the device sends again. Every record must arrive exactly once.
 * The same run with every batch claiming position 0, which is what 0.1 assumed, must lose
 * records: that shows the link is hard enough to find the gap. */
typedef struct inflight { int dev; uint32_t after, last; uint32_t off, n; } inflight;
#define NFLY 4096
static inflight g_fly[NFLY];
static uint8_t g_flybuf[16 << 20];
static int count_rows(altsql_db *gw, dev *d) {
    static const char *series[2] = { "temps", "status" };
    altsql_db_cursor c;
    altsql_value pre;
    int si, rc, n = 0;
    for (si = 0; si < 2; si++) {
        pre.type = ALTSQL_INTEGER; pre.u.i = d->id; pre.len = 0;
        rc = altsql_db_row_seek(&c, gw, series[si], &pre, 1);
        while (rc == ALTSQL_OK) { n++; rc = altsql_db_next(&c); }
    }
    return n;
}
static int device_rows(dev *d) {
    int n = 0;
    g_dev.n = 0;
    if (altsql_ts_scan(d->db, "temps", INT64_MIN, INT64_MAX, dev_row_cb, &g_dev) == ALTSQL_OK) n += g_dev.n;
    g_dev.n = 0;
    if (altsql_ts_scan(d->db, "status", INT64_MIN, INT64_MAX, dev_row_cb, &g_dev) == ALTSQL_OK) n += g_dev.n;
    return n;
}

static int order_run(int old_way, uint32_t seed, long *lost_out, long stats[6]) {
    static uint8_t mem[1 << 21];
    ramfile rf;
    altsql_db *gw;
    dev d[4];
    uint32_t s = seed, sendfrom[4];
    int nfly = 0, i, round;
    uint32_t used = 0;
    long sent = 0, delivered = 0, dropped = 0, repeated = 0, gaps = 0, reordered = 0, lost = 0;
    ram_new(&rf, 1u << 26, 1u << 26);
    CHECK(db_open_ram(&gw, &rf, mem, sizeof mem, 4096) == ALTSQL_OK, "gateway open");
    for (i = 0; i < 4; i++) { if (dev_open(&d[i], 3000 + i, 0)) return 1; sendfrom[i] = 0; }
    for (round = 0; round < 150; round++) {
        for (i = 0; i < 4; i++) {                       /* new records, then up to four batches sent ahead */
            int k, nb = 1 + (int)(xs(&s) % 4);
            if (dev_work(&d[i], &s, (int)(xs(&s) % 12))) return 1;
            for (k = 0; k < nb && nfly < NFLY; k++) {
                size_t n;
                uint32_t last;
                int rc = altsql_sync_read(d[i].db, sendfrom[i], g_flybuf + used, 64 + xs(&s) % 1500, &n, &last, NULL, NULL);
                CHECK(rc >= 0, "sync_read: %d", rc);
                if (!n) break;
                g_fly[nfly].dev = i; g_fly[nfly].after = sendfrom[i]; g_fly[nfly].last = last;
                g_fly[nfly].off = used; g_fly[nfly].n = (uint32_t)n;
                nfly++; used += (uint32_t)n; sent++;
                sendfrom[i] = last;                     /* the next batch goes on from here, without waiting */
                CHECK(used < sizeof g_flybuf - 4096, "the link's buffer is full");
            }
        }
        while (nfly > 0 && xs(&s) % 4 != 0) {           /* the link delivers some batches, in any order */
            int j = (int)(xs(&s) % (uint32_t)nfly), r = (int)(xs(&s) % 100), rc;
            inflight b = g_fly[j];
            uint32_t pos = 0;
            if (j != 0) reordered++;
            if (r < 15) {                               /* lost */
                g_fly[j] = g_fly[--nfly];
                dropped++;
                continue;
            }
            if (r >= 85) repeated++;                    /* delivered, and kept to arrive again later */
            else g_fly[j] = g_fly[--nfly];
            rc = altsql_db_sync_apply(gw, d[b.dev].id, old_way ? 0 : b.after, g_flybuf + b.off, b.n, &pos);
            delivered++;
            if (rc == ALTSQL_DB_GAP) gaps++;
            else if (old_way && rc == ALTSQL_SCHEMA) continue;   /* 0.1's way: rows whose series definition was lost */
            else CHECK(rc == ALTSQL_OK, "apply: %d %s", rc, altsql_db_errmsg(gw));
            if (xs(&s) % 5 == 0) continue;              /* the answer is lost on the way back */
            if (rc == ALTSQL_DB_GAP || pos < sendfrom[b.dev]) {
                if (rc == ALTSQL_DB_GAP || xs(&s) % 3 == 0) sendfrom[b.dev] = pos;   /* send again from the gateway's position */
            }
            if (pos > d[b.dev].confirmed) d[b.dev].confirmed = pos;
        }
    }
    while (nfly > 0) {                                  /* what is still on the link arrives late, in any order */
        int j = (int)(xs(&s) % (uint32_t)nfly), rc;
        inflight b = g_fly[j];
        uint32_t pos = 0;
        g_fly[j] = g_fly[--nfly];
        rc = altsql_db_sync_apply(gw, d[b.dev].id, old_way ? 0 : b.after, g_flybuf + b.off, b.n, &pos);
        delivered++;
        if (j != 0) reordered++;
        if (rc == ALTSQL_DB_GAP) gaps++;
        else CHECK(rc == ALTSQL_OK || (old_way && rc == ALTSQL_SCHEMA), "late apply: %d %s", rc, altsql_db_errmsg(gw));
    }
    /* the link settles: the devices learn the gateway's position and send the rest in order */
    for (i = 0; i < 4; i++) {
        uint32_t pos;
        CHECK(altsql_db_sync_state(gw, d[i].id, &pos) == ALTSQL_OK, "state");
        d[i].confirmed = pos;
        if (dev_sync(&d[i], gw, 4000) && !old_way) return 1;   /* 0.1's way can end refused: a lost series definition */
        lost += device_rows(&d[i]) - count_rows(gw, &d[i]);
        if (!old_way && same_as_device(gw, &d[i])) return 1;
        dev_close(&d[i]);
    }
    altsql_db_close(gw);
    ram_free(&rf);
    *lost_out = lost;
    stats[0] = sent; stats[1] = delivered; stats[2] = dropped; stats[3] = repeated; stats[4] = gaps; stats[5] = reordered;
    return 0;
}

static int t_order(void) {
    long lost = 0, lost_old = 0, st[6], st_old[6], tot[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t seed;
    int i;
    for (seed = 1; seed <= 8; seed++) {
        if (order_run(0, seed * 7919u, &lost, st)) return 1;
        CHECK(lost == 0, "seed %u: %ld records missing with sync in order", seed, lost);
        for (i = 0; i < 6; i++) tot[i] += st[i];
    }
    printf("  sync in order: 8 runs, 4 devices each sending up to 4 batches ahead: %ld batches sent, %ld delivered, "
           "%ld lost on the link, %ld delivered twice, %ld out of order, %ld refused as gaps; every record applied exactly once\n",
           tot[0], tot[1], tot[2], tot[3], tot[5], tot[4]);
    for (seed = 1; seed <= 8; seed++) {
        long l;
        if (order_run(1, seed * 7919u, &l, st_old)) return 1;
        lost_old += l;
    }
    CHECK(lost_old > 0, "the same link without positions lost nothing: the test is too easy");
    printf("  the same 8 runs with every batch claiming position 0, as 0.1 assumed: %ld records lost\n", lost_old);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    printf("AltSql DB %s: sync from AltSql Core devices\n", ALTSQL_DB_VERSION);
    if (t_fleet()) return 1;
    if (t_powercut(quick)) return 1;
    if (t_order()) return 1;
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
