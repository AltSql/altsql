/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql demo: one engine, set up for three different devices.
 *
 * Runs without hardware: each device's flash is emulated in RAM exactly as
 * the engine sees real NOR flash, and each network link is simulated with
 * lost messages. The engine code is the same in all three; what changes is
 * the set-up: flash layout, memory, data layout, the rule the device
 * applies by itself, what it keeps and what it sends over which link.
 *
 *   machine    a machine sensor: a reading a second for an hour, overheat
 *              rule, Wi-Fi that drops out for 15 minutes
 *   coldchain  a refrigerated-truck logger: a reading every 30 seconds for
 *              48 hours, mobile coverage only at depots
 *   farm       a soil sensor: a reading every 15 minutes for 30 days, runs
 *              the irrigation valve, one 51-byte radio message a day
 *
 * Usage: demo [machine|coldchain|farm]   (default: all three)            */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "altsql.h"

/* ---- devices and gateways --------------------------------------------------------- */
typedef struct node {
    const char *name;
    uint8_t *flash;
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql_config cfg;
    altsql *db;
    uint32_t sector_size, sectors;
} node;

static void check(altsql *db, int rc, const char *what) {
    if (rc < 0) { fprintf(stderr, "%s: %s\n", what, altsql_errmsg(db)); exit(1); }
}

static void node_open(node *n, const char *name, uint32_t sector_size, uint32_t sectors, int replica,
                      size_t mem, uint32_t kv_slots, uint16_t max_series, uint16_t max_record) {
    memset(n, 0, sizeof *n);
    n->name = name;
    n->sector_size = sector_size;
    n->sectors = sectors;
    n->flash = (uint8_t *)malloc((size_t)sectors * sector_size);
    memset(n->flash, 0xFF, (size_t)sectors * sector_size);
    altsql_ram_flash_init(&n->fl, &n->ram, n->flash, sector_size, sectors, 4);
    n->cfg.mem = malloc(mem);
    n->cfg.mem_size = mem;
    n->cfg.create = 1;
    n->cfg.replica = (uint8_t)replica;
    n->cfg.kv_slots = kv_slots;
    n->cfg.max_series = max_series;
    n->cfg.max_record = max_record;
    if (altsql_open(&n->db, &n->fl, &n->cfg)) { fprintf(stderr, "%s: %s\n", name, altsql_errmsg(n->db)); exit(1); }
}

static void node_free(node *n) {
    altsql_close(n->db);
    free(n->flash);
    free(n->cfg.mem);
}

/* Engine RAM on a 32-bit microcontroller, from the Cortex-M4 structure sizes
 * that "make size" measures: 232 bytes + 16 per sector + 8 per key slot +
 * 44 per series + the record buffer (at least 64 bytes). */
static unsigned mcu_ram(const node *n) {
    unsigned rec = n->cfg.max_record ? n->cfg.max_record : 256;
    if (rec < 64) rec = 64;
    return 232u + 16u * n->sectors + 8u * (n->cfg.kv_slots ? n->cfg.kv_slots : 64)
         + 44u * (n->cfg.max_series ? n->cfg.max_series : 8) + rec;
}

/* ---- links ---------------------------------------------------------------------------- */
static uint32_t rng = 2026;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct link {
    node *gw;
    uint32_t cursor;                 /* newest record the gateway has confirmed */
    altsql_sync_filter filter;
    void *fctx;
    size_t cap;                      /* largest message the link carries */
    int loss;                        /* percent of messages lost */
    long bytes, msgs;
} link;

/* Sends what the gateway does not have yet, in at most max_msgs messages
 * (0 = as many as needed). A lost message is sent again next time. */
static long sync_now(node *dev, link *l, int max_msgs) {
    uint8_t buf[1024];
    long sent = 0;
    int n = 0;
    for (;;) {
        size_t len;
        uint32_t last;
        int rc = altsql_sync_read(dev->db, l->cursor, buf, l->cap, &len, &last, l->filter, l->fctx);
        check(dev->db, rc, "sync_read");
        if (len) {
            if (max_msgs && n >= max_msgs) break;
            n++;
            l->msgs++;
            if ((int)(rnd() % 100) < l->loss) break;              /* lost on the way */
            check(l->gw->db, altsql_sync_apply(l->gw->db, buf, len, NULL), "sync_apply");
            sent += (long)len;
        }
        l->cursor = last;                                         /* confirmed */
        if (rc == ALTSQL_DONE) break;
    }
    l->bytes += sent;
    return sent;
}

static void link_init(link *l, node *gw, size_t cap, int loss, altsql_sync_filter f, void *ctx) {
    memset(l, 0, sizeof *l);
    l->gw = gw;
    l->cap = cap;
    l->loss = loss;
    l->filter = f;
    l->fctx = ctx;
}

/* ---- printing -------------------------------------------------------------------------- */
static int print_row(void *ctx, int n, const altsql_value *v, const char *const *names) {
    int i, *first = (int *)ctx;
    if (*first) {
        printf("      ");
        for (i = 0; i < n; i++) printf("%-12s", names[i]);
        printf("\n");
        *first = 0;
    }
    printf("      ");
    for (i = 0; i < n; i++) {
        char b[64];
        if (v[i].type == ALTSQL_INTEGER) snprintf(b, sizeof b, "%lld", (long long)v[i].u.i);
        else if (v[i].type == ALTSQL_REAL) snprintf(b, sizeof b, "%.1f", v[i].u.r);
        else if (v[i].type == ALTSQL_TEXT) snprintf(b, sizeof b, "%.*s", v[i].len, v[i].u.s);
        else snprintf(b, sizeof b, "NULL");
        printf("%-12s", b);
    }
    printf("\n");
    return 0;
}

static void show(node *gw, const char *sql) {
    int first = 1;
    printf("    %s> %s\n", gw->name, sql);
    check(gw->db, altsql_exec(gw->db, sql, print_row, &first), sql);
}

/* One line per device for the closing comparison. */
typedef struct result {
    const char *device, *flash, *reading, *decision, *link;
    unsigned ram;
    long readings, decisions, sent, every, json;
    uint64_t written, erases;        /* flash wear on the device during the run */
    uint32_t sectors;
} result;

/* Flash work on the device after its set-up (format and series definitions). */
static void wear_start(node *n, uint64_t *w, uint64_t *e) { *w = n->ram.bytes_written; *e = n->ram.erases; }
static void wear_end(result *res, node *n, uint64_t w, uint64_t e) {
    res->written = n->ram.bytes_written - w;
    res->erases = n->ram.erases - e;
    res->sectors = n->sectors;
}

/* =========================================================================================
 * 1. Machine sensor: a reading a second for an hour, Wi-Fi out from minute 20 to 35
 * ========================================================================================= */
static const char *mmss(int t) {
    static char b[16];
    snprintf(b, sizeof b, "%02d:%02d", t / 60, t % 60);
    return b;
}

static double machine_temp(int t) {
    double base = 44.0 + 3.0 * ((t / 450) % 2), noise = (double)(rnd() % 200) / 100.0 - 1.0;
    static const int hot[3][2] = { { 5 * 60 + 10, 60 }, { 26 * 60 + 40, 90 }, { 48 * 60, 45 } };
    int i;
    for (i = 0; i < 3; i++) {
        int d = t - hot[i][0];
        if (d >= 0 && d < hot[i][1]) base += 20.0 * (d < 15 ? d / 15.0 : 1.0);
    }
    return base + noise;
}

static int summaries_only(void *ctx, int kind, int series_id) {
    return kind == ALTSQL_REC_KV || kind == ALTSQL_REC_SCHEMA || series_id == *(int *)ctx;
}

static void run_machine(result *res) {
    node dev, gwa, gwb;
    link la, lb;
    altsql_info info;
    long json = 0, decisions = 0;
    uint64_t w0, e0;
    int t, minute_id, fan = 0, up = 1;
    const int T = 3600, down_from = 20 * 60, down_to = 35 * 60;

    rng = 2026;                                   /* each set-up gives the same numbers run alone or together */
    node_open(&dev, "sensor", 4096, 16, 0, 4096, 32, 4, 128);          /* 64 KB flash */
    node_open(&gwa, "gateway-A", 4096, 256, 1, 1 << 20, 0, 0, 0);
    node_open(&gwb, "gateway-B", 4096, 64, 1, 1 << 20, 0, 0, 0);
    check(dev.db, altsql_ts_create(dev.db, "raw", "time:time,temp:float"), "create");
    check(dev.db, altsql_ts_create(dev.db, "minute", "time:time,min:float,avg:float,max:float"), "create");
    check(dev.db, altsql_put(dev.db, "fan", "off", 3), "put");
    minute_id = altsql_series_id(dev.db, "minute");
    link_init(&la, &gwa, 1024, 3, NULL, NULL);
    link_init(&lb, &gwb, 1024, 3, summaries_only, &minute_id);
    wear_start(&dev, &w0, &e0);

    printf("1. MACHINE SENSOR  64 KB flash, about %u bytes of engine RAM on a 32-bit chip\n", mcu_ram(&dev));
    printf("   A reading a second for an hour. Wi-Fi drops out from minute 20 to 35.\n");
    printf("   Gateway A gets every record; gateway B only per-minute summaries and the state.\n\n");

    for (t = 0; t < T; t++) {
        double temp = machine_temp(t);
        altsql_stats hot;
        char js[128];
        check(dev.db, altsql_append(dev.db, "raw", (int64_t)t, temp), "append");
        json += snprintf(js, sizeof js, "{\"device\":\"sensor-17\",\"time\":%d,\"temp\":%.2f}", 1767225600 + t, temp);
        /* the sensor decides by itself: above 60 C for 10 s -> alarm and fan */
        check(dev.db, altsql_ts_window(dev.db, "raw", "temp", t - 9, &hot), "window");
        if (!fan && hot.count == 10 && hot.min > 60.0) {
            char msg[48];
            fan = 1;
            decisions++;
            snprintf(msg, sizeof msg, "overheat at %s", mmss(t));
            check(dev.db, altsql_put(dev.db, "alarm", msg, strlen(msg)), "put");
            check(dev.db, altsql_put(dev.db, "fan", "on", 2), "put");
            printf("   %s  above 60 C for 10 s: alarm raised, fan on%s\n", mmss(t),
                   up ? "" : "  (Wi-Fi down: decided on the device)");
        } else if (fan && hot.count == 10 && hot.max < 55.0) {
            fan = 0;
            decisions++;
            check(dev.db, altsql_del(dev.db, "alarm"), "del");
            check(dev.db, altsql_put(dev.db, "fan", "off", 3), "put");
            printf("   %s  back to normal: alarm cleared, fan off\n", mmss(t));
        }
        if (t % 60 == 59) {                                /* one summary row a minute */
            altsql_stats m;
            check(dev.db, altsql_ts_window(dev.db, "raw", "temp", t - 59, &m), "window");
            check(dev.db, altsql_append(dev.db, "minute", (int64_t)(t / 60), m.min, m.avg, m.max), "append");
        }
        if (t == down_from) { up = 0; printf("   %s  Wi-Fi down\n", mmss(t)); }
        if (t == down_to) {
            long a, b;
            up = 1;
            a = sync_now(&dev, &la, 0);
            b = sync_now(&dev, &lb, 0);
            printf("   %s  Wi-Fi back: sensor catches up, %ld bytes to gateway A, %ld to gateway B\n", mmss(t), a, b);
        }
        if (up && t % 10 == 9) { sync_now(&dev, &la, 0); sync_now(&dev, &lb, 0); }
    }
    altsql_info_get(dev.db, &info);
    while (la.cursor != info.last_seq) sync_now(&dev, &la, 0);
    while (lb.cursor != info.last_seq) sync_now(&dev, &lb, 0);

    printf("\n   On the gateways:\n");
    show(&gwa, "SELECT COUNT(*) AS readings, AVG(temp) AS avg, MAX(temp) AS max FROM raw");
    show(&gwb, "SELECT time AS minute, max FROM minute WHERE max > 60");
    show(&gwb, "SELECT key, value FROM kv");
    printf("   Gateway A holds all %d readings, including the %d taken while Wi-Fi was down.\n\n", T, down_to - down_from);

    res->device = "Machine sensor"; res->flash = "64 KB"; res->reading = "1 per second, 1 hour";
    res->decision = "overheat: alarm, fan"; res->link = "Wi-Fi, 15-min outage";
    res->ram = mcu_ram(&dev); res->readings = T; res->decisions = decisions;
    res->sent = lb.bytes; res->every = la.bytes; res->json = json;
    wear_end(res, &dev, w0, e0);
    node_free(&dev); node_free(&gwa); node_free(&gwb);
}

/* =========================================================================================
 * 2. Cold-chain logger: a reading every 30 s for 48 hours, coverage only at depots
 * ========================================================================================= */
static const char *hhmm(int t) {
    static char b[16];
    snprintf(b, sizeof b, "%02dh%02d", t / 3600, (t / 60) % 60);
    return b;
}

static int in_coverage(int t) {                      /* depots at 0h, 16h, 32h and arrival at 47h30 */
    static const int from[4] = { 0, 16 * 3600, 32 * 3600, 47 * 3600 + 1800 };
    int i;
    for (i = 0; i < 4; i++) if (t >= from[i] && t < from[i] + 1800) return 1;
    return 0;
}

static double truck_temp(int t, int *door) {
    double temp = 4.0 + (double)(rnd() % 60) / 100.0 - 0.3;
    static const int doors[4] = { 600, 16 * 3600 + 600, 32 * 3600 + 600, 47 * 3600 + 2400 };
    int i, d;
    *door = 0;
    for (i = 0; i < 4; i++) {                        /* unloading: door open 4 minutes, a short warm spell */
        d = t - doors[i];
        if (d >= 0 && d < 240) *door = 1;
        if (d >= 0 && d < 900) temp += 4.8 * (d < 240 ? d / 240.0 : 1.0 - (d - 240) / 660.0);
    }
    d = t - 22 * 3600;                               /* compressor fault at 22h, fixed after 55 minutes */
    if (d >= 0 && d < 55 * 60) temp += 0.15 * d / 60.0;
    else if (d >= 55 * 60 && d < 55 * 60 + 2750) temp += 8.25 - 0.18 * (d - 55 * 60) / 60.0;
    return temp;
}

static int coldchain_filter(void *ctx, int kind, int series_id) {
    const int *ids = (const int *)ctx;
    return kind == ALTSQL_REC_KV || kind == ALTSQL_REC_SCHEMA || series_id == ids[0] || series_id == ids[1];
}

static void run_coldchain(result *res) {
    node dev, cloud, dock;
    link lc, ld;
    long json = 0, decisions = 0;
    uint64_t w0, e0;
    int t, ids[2], excursion = 0, start = -1, was_covered = 1;
    const int T = 48 * 3600, STEP = 30;

    rng = 2027;
    node_open(&dev, "logger", 65536, 8, 0, 4096, 32, 4, 128);          /* 512 KB flash in 64 KB blocks */
    node_open(&cloud, "cloud", 4096, 64, 1, 1 << 20, 0, 0, 0);
    node_open(&dock, "dock", 4096, 256, 1, 1 << 20, 0, 0, 0);
    check(dev.db, altsql_ts_create(dev.db, "raw", "time:time,temp:float,door:int"), "create");
    check(dev.db, altsql_ts_create(dev.db, "quarter", "time:time,min:float,avg:float,max:float"), "create");
    check(dev.db, altsql_ts_create(dev.db, "excursion", "time:time,minutes:int,peak:float"), "create");
    check(dev.db, altsql_put(dev.db, "status", "ok", 2), "put");
    ids[0] = altsql_series_id(dev.db, "quarter");
    ids[1] = altsql_series_id(dev.db, "excursion");
    link_init(&lc, &cloud, 1024, 2, coldchain_filter, ids);            /* mobile network */
    link_init(&ld, &dock, 1024, 0, NULL, NULL);                        /* local read-out on arrival */
    wear_start(&dev, &w0, &e0);

    printf("2. COLD-CHAIN LOGGER  512 KB flash in 64 KB blocks, about %u bytes of engine RAM\n", mcu_ram(&dev));
    printf("   A reading every 30 s for a 48-hour run. Mobile coverage only at depots.\n");
    printf("   Over the air: 15-minute summaries and excursion records. Full log read out at the dock.\n\n");

    for (t = 0; t < T; t += STEP) {
        int door, covered = in_coverage(t);
        double temp = truck_temp(t, &door);
        altsql_stats w;
        char js[128];
        check(dev.db, altsql_append(dev.db, "raw", (int64_t)t, temp, door), "append");
        json += snprintf(js, sizeof js, "{\"truck\":\"TR-204\",\"time\":%d,\"temp\":%.2f,\"door\":%d}", 1767225600 + t, temp, door);
        /* the logger decides by itself: above 8 C for 15 minutes is an excursion */
        check(dev.db, altsql_ts_window(dev.db, "raw", "temp", t - 900 + STEP, &w), "window");
        if (!excursion && w.count == 900 / STEP && w.min > 8.0) {
            char msg[64];
            excursion = 1;
            start = t - 900 + STEP;
            decisions++;
            snprintf(msg, sizeof msg, "excursion since %s", hhmm(start));
            check(dev.db, altsql_put(dev.db, "status", msg, strlen(msg)), "put");
            printf("   %s  above 8 C for 15 min: excursion recorded, driver alerted%s\n", hhmm(t),
                   covered ? "" : "  (no coverage: decided on the logger)");
        } else if (excursion && temp <= 8.0) {
            altsql_stats e;
            excursion = 0;
            check(dev.db, altsql_ts_window(dev.db, "raw", "temp", start, &e), "window");
            check(dev.db, altsql_append(dev.db, "excursion", (int64_t)start, (t - start) / 60, e.max), "append");
            check(dev.db, altsql_put(dev.db, "status", "ok", 2), "put");
            printf("   %s  back below 8 C: excursion of %d min closed, peak %.1f C\n", hhmm(t), (t - start) / 60, e.max);
        } else if (!excursion && temp > 8.0 && door) {
            /* door open at a depot: short warm spells are expected, no excursion */
        }
        if (t % 900 == 900 - STEP) {                     /* one summary row every 15 minutes */
            altsql_stats q;
            check(dev.db, altsql_ts_window(dev.db, "raw", "temp", t - 900 + STEP, &q), "window");
            check(dev.db, altsql_append(dev.db, "quarter", (int64_t)(t - 900 + STEP), q.min, q.avg, q.max), "append");
        }
        if (covered && !was_covered) {
            long b = sync_now(&dev, &lc, 0);
            printf("   %s  depot: coverage back, %ld bytes sent over the mobile network\n", hhmm(t), b);
        } else if (covered && t % 300 == 0) {
            sync_now(&dev, &lc, 0);
        }
        was_covered = covered;
    }
    {
        altsql_info info;
        altsql_info_get(dev.db, &info);
        while (lc.cursor != info.last_seq) sync_now(&dev, &lc, 0);
        while (ld.cursor != info.last_seq) sync_now(&dev, &ld, 0);
    }

    printf("\n   In the cloud, from the mobile link:\n");
    show(&cloud, "SELECT time / 3600 AS start_hour, minutes, peak FROM excursion");
    show(&cloud, "SELECT COUNT(*) AS quarters, MAX(max) AS warmest FROM quarter WHERE max > 8");
    printf("   At the dock, from the full read-out:\n");
    show(&dock, "SELECT COUNT(*) AS readings, SUM(door) AS door_open, MIN(temp) AS coldest, MAX(temp) AS warmest FROM raw");
    printf("   Door openings warmed the load past 8 C for a few minutes without counting as excursions.\n\n");

    res->device = "Cold-chain logger"; res->flash = "512 KB"; res->reading = "1 per 30 s, 48 hours";
    res->decision = "excursion record"; res->link = "mobile, at depots only";
    res->ram = mcu_ram(&dev); res->readings = T / STEP; res->decisions = decisions;
    res->sent = lc.bytes; res->every = ld.bytes; res->json = json;
    wear_end(res, &dev, w0, e0);
    node_free(&dev); node_free(&cloud); node_free(&dock);
}

/* =========================================================================================
 * 3. Soil sensor: a reading every 15 min for 30 days, one 51-byte radio message a day
 * ========================================================================================= */
static int days_only(void *ctx, int kind, int series_id) {
    return kind == ALTSQL_REC_ROW && series_id == *(int *)ctx;   /* definitions were given at setup */
}

static void run_farm(result *res) {
    node dev, office;
    link setup, radio;
    long json = 0, decisions = 0, every = 0;
    uint64_t w0, e0;
    int t, day_id, valve = 0, irrigation = 0;
    double moisture = 31.0;
    const int T = 30 * 86400, STEP = 900;

    rng = 2028;
    node_open(&dev, "soil", 4096, 8, 0, 2048, 16, 2, 96);             /* 32 KB flash, a small chip */
    node_open(&office, "farm-office", 4096, 64, 1, 1 << 20, 0, 0, 0);
    check(dev.db, altsql_ts_create(dev.db, "raw", "time:time,moisture:float,soil_temp:float"), "create");
    check(dev.db, altsql_ts_create(dev.db, "day", "time:time,min:float,avg:float,max:float,irrigation:int"), "create");
    check(dev.db, altsql_put(dev.db, "valve", "closed", 6), "put");
    day_id = altsql_series_id(dev.db, "day");
    link_init(&setup, &office, 1024, 0, NULL, NULL);
    sync_now(&dev, &setup, 0);                                        /* at installation, over a local link */
    link_init(&radio, &office, 51, 5, days_only, &day_id);            /* then one small radio message a day */
    radio.cursor = setup.cursor;
    wear_start(&dev, &w0, &e0);

    printf("3. SOIL SENSOR  32 KB flash, about %u bytes of engine RAM\n", mcu_ram(&dev));
    printf("   A reading every 15 minutes for 30 days. The sensor runs the irrigation valve itself.\n");
    printf("   Radio: one message a day of at most 51 bytes, carrying the daily summary.\n\n");

    for (t = 0; t < T; t += STEP) {
        int day = t / 86400, hour = (t % 86400) / 3600;
        double soil_temp = 12.0 + 6.0 * (hour >= 6 && hour < 18 ? (double)(hour - 6) / 12.0 : 0.0) + (double)(rnd() % 50) / 100.0;
        altsql_stats h;
        char js[128];
        moisture -= (day >= 8 && day < 16) ? 0.045 : 0.021;          /* a hot spell dries the soil faster */
        if (day == 4 && hour >= 2 && hour < 5) moisture += 1.3;       /* rain */
        if (day == 21 && hour >= 13 && hour < 16) moisture += 1.1;
        if (valve) { moisture += 1.4; irrigation += 15; }
        moisture += (double)(rnd() % 40) / 100.0 - 0.2;
        check(dev.db, altsql_append(dev.db, "raw", (int64_t)t, moisture, soil_temp), "append");
        json += snprintf(js, sizeof js, "{\"sensor\":\"soil-3\",\"time\":%d,\"moisture\":%.2f,\"soil_temp\":%.2f}",
                         1767225600 + t, moisture, soil_temp);
        every += 30;                                                  /* record size of a raw reading */
        /* the sensor decides by itself, on the last hour's average */
        check(dev.db, altsql_ts_window(dev.db, "raw", "moisture", t - 3 * STEP, &h), "window");
        if (!valve && h.count == 4 && h.avg < 20.0) {
            valve = 1;
            decisions++;
            check(dev.db, altsql_put(dev.db, "valve", "open", 4), "put");
            printf("   day %2d  moisture %.1f%% for an hour: valve opened\n", day + 1, h.avg);
        } else if (valve && h.avg >= 35.0) {
            valve = 0;
            decisions++;
            check(dev.db, altsql_put(dev.db, "valve", "closed", 6), "put");
            printf("   day %2d  moisture back to %.1f%%: valve closed\n", day + 1, h.avg);
        }
        if (t % 86400 == 86400 - STEP) {                             /* end of the day: summary and radio */
            altsql_stats d;
            check(dev.db, altsql_ts_window(dev.db, "raw", "moisture", t - 86400 + STEP, &d), "window");
            check(dev.db, altsql_append(dev.db, "day", (int64_t)(t - 86400 + STEP), d.min, d.avg, d.max, irrigation), "append");
            irrigation = 0;
            sync_now(&dev, &radio, 3);                                /* up to 3 messages when behind */
        }
    }

    printf("\n   At the farm office, from %ld radio messages:\n", radio.msgs);
    show(&office, "SELECT COUNT(*) AS days, MIN(min) AS driest, SUM(irrigation) AS irrigation_minutes FROM day");
    show(&office, "SELECT time / 86400 + 1 AS day, avg, irrigation FROM day WHERE irrigation > 0");
    {
        altsql_info info;
        altsql_info_get(dev.db, &info);
        printf("   The sensor keeps the most recent days of 15-minute detail; its 32 KB rolled over %u older readings.\n\n",
               info.rows_dropped);
    }

    res->device = "Soil sensor"; res->flash = "32 KB"; res->reading = "1 per 15 min, 30 days";
    res->decision = "irrigation valve"; res->link = "radio, 51 bytes a day";
    res->ram = mcu_ram(&dev); res->readings = T / STEP; res->decisions = decisions;
    res->sent = radio.bytes; res->every = every; res->json = json;
    wear_end(res, &dev, w0, e0);
    node_free(&dev); node_free(&office);
}

/* ========================================================================================= */
int main(int argc, char **argv) {
    result r[3];
    int n = 0, i;
    const char *only = argc > 1 ? argv[1] : NULL;
    printf("AltSql demo: one engine, set up for three devices\n\n");
    if (!only || !strcmp(only, "machine")) run_machine(&r[n++]);
    if (!only || !strcmp(only, "coldchain")) run_coldchain(&r[n++]);
    if (!only || !strcmp(only, "farm")) run_farm(&r[n++]);
    if (!n) { fprintf(stderr, "usage: demo [machine|coldchain|farm]\n"); return 2; }

    printf("Same engine, three set-ups\n");
    printf("  %-26s", "");
    for (i = 0; i < n; i++) printf("%-24s", r[i].device);
    printf("\n  %-26s", "flash on the device");
    for (i = 0; i < n; i++) printf("%-24s", r[i].flash);
    printf("\n  %-26s", "engine RAM (32-bit chip)");
    for (i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "about %u bytes", r[i].ram); printf("%-24s", b); }
    printf("\n  %-26s", "readings");
    for (i = 0; i < n; i++) printf("%-24s", r[i].reading);
    printf("\n  %-26s", "decided on the device");
    for (i = 0; i < n; i++) printf("%-24s", r[i].decision);
    printf("\n  %-26s", "link");
    for (i = 0; i < n; i++) printf("%-24s", r[i].link);
    printf("\n  %-26s", "sent upstream");
    for (i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "%ld bytes", r[i].sent); printf("%-24s", b); }
    printf("\n  %-26s", "every reading as records");
    for (i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "%ld bytes", r[i].every); printf("%-24s", b); }
    printf("\n  %-26s", "every reading as JSON");
    for (i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "%ld bytes", r[i].json); printf("%-24s", b); }
    printf("\n  %-26s", "flash written on device");
    for (i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "%llu bytes", (unsigned long long)r[i].written); printf("%-24s", b); }
    printf("\n  %-26s", "sector erases");
    for (i = 0; i < n; i++) {
        char b[48];
        snprintf(b, sizeof b, "%llu (%.1f per sector)", (unsigned long long)r[i].erases, (double)r[i].erases / r[i].sectors);
        printf("%-24s", b);
    }
    printf("\n");
    return 0;
}
