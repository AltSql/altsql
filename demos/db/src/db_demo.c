/* AltSql DB live demo: twelve devices run AltSql Core, each on its own emulated
 * flash chip, and sync into one AltSql DB file on the gateway. The file sits in
 * memory, where the power can be cut at any write.
 *
 * Every ten seconds of the devices' clock each device saves a reading (its time,
 * the machine it watches, a temperature) and now and then a key-value pair, then
 * hands the gateway a batch of its records with altsql_sync_read, exactly as they
 * sit in its flash. The gateway applies each batch in one transaction with the
 * device's new position (altsql_db_sync_apply) and reports that position back.
 *
 * The page then asks the gateway questions two ways: SQL, through Core's own
 * parser over the tree, and the direct path, with no SQL at all. It shows the
 * bytes the gateway stored beside the bytes the device wrote, and it cuts the
 * gateway's power in the middle of a commit, opens the file again, checks it
 * page by page, lets the devices resend, and counts what was lost.
 *
 * Built for the browser (WebAssembly, the demo_* exports) and natively
 * (-DNATIVE_MAIN, which prints what the page shows).
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0 */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "altsql.h"
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifdef __wasm__
#define API(name) __attribute__((export_name(#name)))
#else
#define API(name)
#endif

#define NDEV   12
#define DSS    4096u                     /* a device's flash sectors              */
#define DSC    48u                       /* 192 KB of flash for each device       */
#define GCAP   (6u << 20)                /* room for the gateway's file           */
#define GLOG   (6u << 20)                /* writes since the last sync            */
#define GMEM   (2u << 20)                /* the gateway's working memory          */
#define BATCH  1200u                     /* the radio's batch size, bytes         */
#define T0     1767225600                /* 2026-01-01 00:00 UTC                  */

static uint8_t DFLASH[NDEV][DSS * DSC], DMEM[NDEV][24 * 1024];
static uint8_t GM[GCAP], GD[GCAP], GL[GLOG], GW[GMEM], CHK[1 << 16], BUF[4096];
static char OUT[1 << 18];
static char IN[8192];
static size_t on;
static const char *const SITES[4] = { "north", "south", "east", "west" };

typedef struct dev {
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql *db;
    int64_t id;
    uint32_t conf;                       /* the position the gateway confirmed    */
    uint32_t readings;
} dev;
static dev D[NDEV];
static altsql_db *G;
static altsql_db_file GF;
static altsql_db_ram GR;
static int64_t now_s;                    /* the devices' clock                    */
static uint64_t batches, cuts;
static uint32_t rs = 2026;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* ---- JSON out ---- */
static void o(const char *fmt, ...) {
    va_list ap;
    int n;
    if (on >= sizeof OUT - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(OUT + on, sizeof OUT - on, fmt, ap);
    va_end(ap);
    if (n > 0) on += (size_t)n < sizeof OUT - on ? (size_t)n : sizeof OUT - on - 1;
}
static void ostr(const char *s, int n) {
    int i;
    o("\"");
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') o("\\%c", c);
        else if (c < 0x20) o("\\u%04x", c);
        else o("%c", c);
    }
    o("\"");
}
static const char *done(void) { OUT[on] = 0; return OUT; }

/* ---- the fleet ---- */
static int dev_open(dev *d, int i) {
    altsql_config c;
    memset(DFLASH[i], 0xFF, sizeof DFLASH[i]);
    altsql_ram_flash_init(&d->fl, &d->ram, DFLASH[i], DSS, DSC, 4);
    d->ram.budget = -1;
    memset(&c, 0, sizeof c);
    c.mem = DMEM[i]; c.mem_size = sizeof DMEM[i]; c.create = 1;
    d->id = 101 + i;
    d->conf = 0;
    d->readings = 0;
    if (altsql_open(&d->db, &d->fl, &c)) return -1;
    if (altsql_ts_create(d->db, "temps", "time:time,machine:int,temp:float")) return -1;
    if (altsql_put(d->db, "site", SITES[i % 4], strlen(SITES[i % 4]))) return -1;
    return altsql_put(d->db, "fw", "1.4.2", 5);
}

/* One reading from each device, then a batch from each to the gateway. */
static int tick(void) {
    int i, rc;
    now_s += 10;
    for (i = 0; i < NDEV; i++) {
        dev *d = &D[i];
        double temp = 19.0 + (i % 6) * 1.5 + (double)((now_s / 10 + i * 7) % 40) / 10.0 + (double)(rnd() % 10) / 100.0;
        if (altsql_append(d->db, "temps", (int64_t)now_s, i % 3, temp)) return -1;
        d->readings++;
        if (rnd() % 50 == 0) {
            char v[16];
            int n = snprintf(v, sizeof v, "%s", rnd() % 2 ? "running" : "idle");
            altsql_put(d->db, "state", v, (size_t)n);
        }
    }
    for (i = 0; i < NDEV; i++) {
        dev *d = &D[i];
        size_t n;
        uint32_t last;
        rc = altsql_sync_read(d->db, d->conf, BUF, BATCH, &n, &last, NULL, NULL);
        if (rc < 0) return rc;
        if (!n) continue;
        rc = altsql_db_sync_apply(G, d->id, d->conf, BUF, n, &d->conf);
        if (rc) return rc;
        batches++;
    }
    return 0;
}

/* The gateway's writes pass through here, so a cut can be aimed at a commit header:
 * the 64-byte write at the start of page 0 or page 1. */
static int header_armed;
static int gw_write(void *ctx, uint64_t off, const void *buf, size_t n) {
    if (header_armed && n == 64 && (off == 0 || off == 4096)) { header_armed = 0; GR.cut = 1; }
    return asd_ram_write(ctx, off, buf, n);
}

static int gw_open(int fresh) {
    altsql_db_config cfg;
    if (fresh) altsql_db_ram_init(&GF, &GR, GM, GD, GCAP, GL, GLOG);
    GF.write = gw_write;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = GW; cfg.mem_size = sizeof GW; cfg.create = 1; cfg.page_size = 4096;
    return altsql_db_open(&G, &GF, &cfg);
}

API(demo_version) const char *demo_version(void) { return ALTSQL_DB_VERSION; }
API(demo_in) char *demo_in(void) { return IN; }

API(demo_init) const char *demo_init(void) {
    int i, k;
    on = 0;
    now_s = T0;
    batches = cuts = 0;
    rs = 2026;
    if (gw_open(1)) { o("{\"ok\":false,\"error\":\"gateway: %s\"}", altsql_db_errmsg(G)); return done(); }
    for (i = 0; i < NDEV; i++) if (dev_open(&D[i], i)) { o("{\"ok\":false,\"error\":\"device %d\"}", i); return done(); }
    for (k = 0; k < 360; k++) if (tick()) { o("{\"ok\":false,\"error\":\"%s\"}", altsql_db_errmsg(G)); return done(); }
    o("{\"ok\":true,\"devices\":%d}", NDEV);
    return done();
}

static uint64_t gw_rows(void) {
    altsql_db_cursor c;
    uint64_t n = 0;
    int rc = altsql_db_row_seek(&c, G, "temps", NULL, 0);
    while (rc == ALTSQL_OK) { n++; rc = altsql_db_next(&c); }
    return n;
}

API(demo_status) const char *demo_status(void) {
    altsql_db_info info;
    uint64_t rows = gw_rows(), total = 0;
    int i;
    on = 0;
    altsql_db_info_get(G, &info);
    o("{\"clock\":%lld,\"devices\":[", (long long)(now_s - T0));
    for (i = 0; i < NDEV; i++) {
        dev *d = &D[i];
        altsql_info di;
        altsql_info_get(d->db, &di);
        total += d->readings;
        o("%s{\"id\":%lld,\"site\":\"%s\",\"readings\":%u,\"confirmed\":%u,\"pending\":%u}", i ? "," : "",
          (long long)d->id, SITES[i % 4], d->readings, d->conf, di.last_seq > d->conf ? di.last_seq - d->conf : 0);
    }
    o("],\"gw\":{\"txn\":%llu,\"pages\":%u,\"bytes\":%llu,\"free\":%u,\"rows\":%llu,\"readings\":%llu,\"perReading\":%.1f,\"batches\":%llu,\"cuts\":%llu}}",
      (unsigned long long)info.txn, info.pages, (unsigned long long)info.pages * info.page_size, info.free_pages,
      (unsigned long long)rows, (unsigned long long)total, rows ? (double)info.pages * info.page_size / (double)rows : 0.0,
      (unsigned long long)batches, (unsigned long long)cuts);
    return done();
}

API(demo_tick) const char *demo_tick(int n) {
    int k;
    for (k = 0; k < n; k++)
        if (tick()) { on = 0; o("{\"ok\":false,\"error\":"); ostr(altsql_db_errmsg(G), (int)strlen(altsql_db_errmsg(G))); o("}"); return done(); }
    return demo_status();
}

/* ---- SQL ---- */
static int sql_rows, sql_shown, sql_ncol;
static int sql_cb(void *ctx, int ncol, const altsql_value *v, const char *const *names) {
    int i;
    (void)ctx;
    if (!sql_rows) {
        o("\"cols\":[");
        for (i = 0; i < ncol; i++) { if (i) o(","); ostr(names[i], (int)strlen(names[i])); }
        o("],\"rows\":[");
        sql_ncol = ncol;
    }
    sql_rows++;
    if (sql_shown >= 50) return 0;
    o("%s[", sql_shown ? "," : "");
    for (i = 0; i < ncol; i++) {
        if (i) o(",");
        if (v[i].type == ALTSQL_INTEGER) o("%lld", (long long)v[i].u.i);
        else if (v[i].type == ALTSQL_REAL) o("%.10g", v[i].u.r);
        else if (v[i].type == ALTSQL_TEXT) ostr(v[i].u.s, v[i].len);
        else o("null");
    }
    o("]");
    sql_shown++;
    return 0;
}
static char PLAN[200];
static int plan_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    (void)ctx; (void)n; (void)names;
    snprintf(PLAN, sizeof PLAN, "%.*s|%.*s", v[0].len, v[0].u.s, v[1].len, v[1].u.s);
    return 0;
}

/* Runs the SQL the page wrote at demo_in(); reps > 1 runs it again, for timing. */
API(demo_sql) const char *demo_sql(int len, int reps) {
    altsql_db_info info;
    int rc, k;
    char ex[8300];
    if (len < 0 || len >= (int)sizeof IN) len = 0;
    IN[len] = 0;
    on = 0;
    PLAN[0] = 0;
    snprintf(ex, sizeof ex, "EXPLAIN %s", IN);
    if (altsql_db_exec(G, ex, plan_cb, NULL) != ALTSQL_OK) PLAN[0] = 0;
    for (k = 1; k < reps; k++) { size_t keep = on; sql_rows = sql_shown = 0; altsql_db_exec(G, IN, NULL, NULL); on = keep; }
    o("{");
    sql_rows = sql_shown = sql_ncol = 0;
    rc = altsql_db_exec(G, IN, sql_cb, NULL);
    if (sql_rows) o("],");
    altsql_db_info_get(G, &info);
    o("\"ok\":%s,\"count\":%d,\"read\":%llu,\"plan\":", rc ? "false" : "true", sql_rows, (unsigned long long)info.sql_rows);
    ostr(PLAN, (int)strlen(PLAN));
    if (rc) { o(",\"error\":"); ostr(altsql_db_errmsg(G), (int)strlen(altsql_db_errmsg(G))); }
    o("}");
    return done();
}

/* ---- a device's newest reading: the direct path, no SQL ---- */
static int newest(int i, altsql_value *v, uint8_t *val, size_t *vn) {
    altsql_db_cursor c;
    altsql_value pre;
    int rc;
    pre.type = ALTSQL_INTEGER; pre.len = 0; pre.u.i = D[i].id;
    if ((rc = altsql_db_row_last(&c, G, "temps", &pre, 1)) != 0) return rc;
    if ((rc = altsql_db_row_read(&c, v, 8)) != 0) return rc;
    return val ? altsql_db_value(&c, val, 64, vn) : 0;
}

API(demo_direct) const char *demo_direct(int i, int reps) {
    altsql_value v[8];
    int k, rc = 0;
    on = 0;
    if (i < 0 || i >= NDEV) i = 0;
    for (k = 0; k < reps && !rc; k++) rc = newest(i, v, NULL, NULL);
    if (rc) { o("{\"ok\":false}"); return done(); }
    o("{\"ok\":true,\"device\":%lld,\"seq\":%lld,\"time\":%lld,\"machine\":%lld,\"temp\":%.10g}",
      (long long)v[0].u.i, (long long)v[1].u.i, (long long)v[2].u.i, (long long)v[3].u.i, v[4].u.r);
    return done();
}

/* The newest reading's bytes, on the gateway and on the device. */
API(demo_bytes) const char *demo_bytes(int i) {
    altsql_value v[8];
    uint8_t val[64];
    size_t vn = 0, n = 0, k;
    uint32_t last, seq;
    const uint8_t *p = BUF + 12;
    on = 0;
    if (i < 0 || i >= NDEV) i = 0;
    if (newest(i, v, val, &vn)) { o("{\"ok\":false}"); return done(); }
    seq = (uint32_t)v[1].u.i;
    if (altsql_sync_read(D[i].db, seq - 1, BUF, 64, &n, &last, NULL, NULL) < 0 || n < 12 || as_get32(BUF + 4) != seq) {
        o("{\"ok\":false}");
        return done();
    }
    o("{\"ok\":true,\"device\":%lld,\"seq\":%u,\"header\":\"", (long long)D[i].id, seq);
    for (k = 0; k < 12; k++) o("%02x", BUF[k]);
    o("\",\"onDevice\":\"");
    for (k = 0; k < n - 12; k++) o("%02x", p[k]);
    o("\",\"onGateway\":\"");
    for (k = 0; k < vn; k++) o("%02x", val[k]);
    o("\",\"same\":%s}", (n - 12 == vn && !memcmp(p, val, vn)) ? "true" : "false");
    return done();
}

/* ---- the power goes in the middle of a commit ---- */
static int devices_whole(uint64_t *rows) {
    int i, bad = 0;
    *rows = 0;
    for (i = 0; i < NDEV; i++) {
        altsql_db_cursor c;
        altsql_value pre;
        uint32_t n = 0;
        int rc;
        pre.type = ALTSQL_INTEGER; pre.len = 0; pre.u.i = D[i].id;
        rc = altsql_db_row_seek(&c, G, "temps", &pre, 1);
        while (rc == ALTSQL_OK) { n++; rc = altsql_db_next(&c); }
        *rows += n;
        if (n != D[i].readings) bad++;
    }
    return bad;
}

/* mode 0: the power goes while a commit writes its pages; 1: while it writes its header. */
API(demo_cut) const char *demo_cut(int mode) {
    altsql_db_check_report r0, r1;
    uint64_t before, after, rows;
    uint32_t k = mode ? 0 : 1 + rnd() % 3, resent = 0, twice = 0;
    int ticks = 0, rc = 0, s0, s1, i, bad;
    on = 0;
    before = G->com.txn;
    if (mode) header_armed = 1;                   /* the next commit header write */
    else GR.cut = k;                              /* the k-th write from now: a page */
    while (!GR.dead && ticks < 40) { rc = tick(); ticks++; if (rc && !GR.dead) break; }
    if (!GR.dead) { GR.cut = 0; o("{\"ok\":false,\"error\":\"no cut\"}"); return done(); }
    cuts++;
    {                                              /* which commit was under way */
        uint64_t inflight = G->tx == 2 ? G->cur : G->com.txn + 1;
        altsql_db_close(G);
        altsql_db_ram_powercut(&GR, rnd());
        if (gw_open(0)) { o("{\"ok\":false,\"error\":"); ostr(altsql_db_errmsg(G), (int)strlen(altsql_db_errmsg(G))); o("}"); return done(); }
        after = G->com.txn;
        s0 = altsql_db_check(G, 0, CHK, sizeof CHK, &r0);
        s1 = altsql_db_check(G, 1, CHK, sizeof CHK, &r1);
        o("{\"ok\":true,\"mode\":\"%s\",\"atWrite\":%u,\"ticks\":%d,\"before\":%llu,\"inflight\":%llu,\"after\":%llu,\"kept\":\"%s\",",
          mode ? "header" : "pages", k, ticks, (unsigned long long)before, (unsigned long long)inflight, (unsigned long long)after,
          after >= inflight ? "the commit under way" : "the last commit");
        o("\"slot0\":\"%s\",\"slot1\":\"%s\",\"txn0\":%llu,\"txn1\":%llu,",
          s0 == ALTSQL_OK ? "complete" : s0 == ALTSQL_NOTFOUND ? "torn" : "damaged",
          s1 == ALTSQL_OK ? "complete" : s1 == ALTSQL_NOTFOUND ? "torn" : "damaged",
          (unsigned long long)r0.txn, (unsigned long long)r1.txn);
    }
    /* each device resends from the position the gateway now holds for it */
    for (i = 0; i < NDEV; i++) {
        uint32_t held;
        altsql_db_sync_state(G, D[i].id, &held);
        if (held < D[i].conf) resent += D[i].conf - held;
        if (held > D[i].conf) twice += held - D[i].conf;   /* kept, though the device never heard: sent again, skipped */
        D[i].conf = held;
        do {
            size_t n;
            uint32_t last;
            rc = altsql_sync_read(D[i].db, D[i].conf, BUF, BATCH, &n, &last, NULL, NULL);
            if (n && altsql_db_sync_apply(G, D[i].id, D[i].conf, BUF, n, &D[i].conf)) break;
            if (n) batches++;
        } while (rc == ALTSQL_OK);
    }
    bad = devices_whole(&rows);
    o("\"resent\":%u,\"twice\":%u,\"rows\":%llu,\"devicesShort\":%d}", resent, twice, (unsigned long long)rows, bad);
    return done();
}

#ifdef NATIVE_MAIN
static double ms(void) { return 0; }
int main(void) {
    int i;
    (void)ms;
    printf("%s\n", demo_init());
    printf("%s\n", demo_tick(60));
    strcpy(IN, "SELECT device, COUNT(*), AVG(temp) FROM temps WHERE time >= 1767228600 GROUP BY device");
    printf("%s\n", demo_sql((int)strlen(IN), 1));
    strcpy(IN, "SELECT * FROM temps WHERE device = 105 AND time > 1767229200");
    printf("%s\n", demo_sql((int)strlen(IN), 1));
    strcpy(IN, "SELECT key, value, COUNT(*) FROM kv GROUP BY key, value");
    printf("%s\n", demo_sql((int)strlen(IN), 1));
    printf("%s\n", demo_direct(4, 1));
    printf("%s\n", demo_bytes(4));
    for (i = 0; i < 20; i++) printf("%s\n", demo_cut(i % 2));
    printf("%s\n", demo_status());
    return 0;
}
#endif
