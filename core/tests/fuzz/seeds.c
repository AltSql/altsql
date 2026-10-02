/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Writes starting inputs for the fuzz targets into build/fuzz/seeds/<target>/:
 * real SQL, real sync batches, a real text export and real flash images,
 * so the fuzzer starts from data the engine actually produces.            */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "altsql.h"

static const char *dir;

static void save(const char *target, int i, const void *p, size_t n, int flag) {
    char path[256];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s/seed%02d", dir, target, i);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (flag >= 0) fputc(flag, f);
    fwrite(p, 1, n, f);
    fclose(f);
}

static char text[65536];
static size_t text_n;
static int to_text(void *ctx, const char *d, size_t n) {
    (void)ctx;
    if (text_n + n < sizeof text) { memcpy(text + text_n, d, n); text_n += n; }
    return 0;
}

int main(int argc, char **argv) {
    static const char *const sql[] = {
        "CREATE TABLE t2 (time TIME, a INT, b FLOAT, c TEXT)",
        "INSERT INTO t VALUES (9, 2, 3.5, 'x', 0.5, 1), (10, -4, 1e3, 'it''s', 2.0, 3)",
        "SELECT a, COUNT(*), AVG(b) FROM t WHERE time >= 1 AND c LIKE '%x%' GROUP BY a HAVING COUNT(*) > 0 ORDER BY 2 DESC LIMIT 5 OFFSET 1",
        "SELECT UPPER(c), LOWER(c), LENGTH(c), ABS(a), ROUND(b, 1), a / 0, a % 3 FROM t WHERE b BETWEEN 1 AND 2000 OR c IS NULL",
        "SELECT key, value FROM kv WHERE key > 'a' ORDER BY key",
        "SELECT time * 2 AS x, x + 1 FROM t WHERE NOT (a <> 2) ORDER BY x",
        "INSERT INTO kv (key, value) VALUES ('k', 'v'), ('n', 42)",
        "SELECT MIN(c), MAX(d), SUM(e), COUNT(c) FROM t WHERE time BETWEEN 2 AND 3",
        "SELECT time / 2 AS h, COUNT(*) FROM t GROUP BY h ORDER BY h DESC LIMIT 1",
        "CREATE TABLE IF NOT EXISTS t (time TIME, a INT); SELECT * FROM t; SELECT 1",
    };
    static uint8_t flash[8 * 256], big[16 * 1024], mem[64 * 1024], batch[2048];
    altsql_ram_flash ram;
    altsql_flash fl;
    altsql_config cfg;
    altsql *db;
    size_t i, len;
    uint32_t last;
    int n = 0, rc;

    dir = argc > 1 ? argv[1] : "build/fuzz/seeds";
    for (i = 0; i < sizeof sql / sizeof sql[0]; i++) save("sql", (int)i, sql[i], strlen(sql[i]), -1);

    /* a device database with a series, rows and keys: sync batches, export */
    memset(big, 0xFF, sizeof big);
    altsql_ram_flash_init(&fl, &ram, big, 1024, 16, 4);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = sizeof mem; cfg.create = 1;
    if (altsql_open(&db, &fl, &cfg)) return 1;
    altsql_ts_create(db, "raw", "time:time,temp:float,door:int,note:text,v:real,big:long");
    for (i = 0; i < 40; i++) altsql_append(db, "raw", (int64_t)i, 20.5 + (double)i, (int)(i & 1), "ok", 0.25 * (double)i, (int64_t)i << 40);
    altsql_put(db, "fan", "on", 2);
    altsql_put(db, "cfg.interval", "60", 2);
    altsql_del(db, "fan");
    last = 0;
    for (;;) {
        rc = altsql_sync_read(db, last, batch, sizeof batch, &len, &last, NULL, NULL);
        if (rc < 0) return 1;
        if (len) { save("sync", n, batch, len, 0); n++; save("sync", n, batch, len, 3); n++; }
        if (rc == ALTSQL_DONE) break;
    }
    text_n = 0;
    altsql_export(db, to_text, NULL);
    save("import", 0, text, text_n, -1);
    {
        static const char small[] = "# altsql text export v1\nseries m \"time:time,x:float\"\nrow m 1 2.5\nkv \"a\" \"b\\x01\"\n";
        save("import", 1, small, strlen(small), -1);
    }
    altsql_close(db);

    /* small flash images like the mount target uses, before and after reclaiming */
    for (n = 0; n < 4; n++) {
        memset(flash, 0xFF, sizeof flash);
        altsql_ram_flash_init(&fl, &ram, flash, 256, 8, 4);
        if (altsql_open(&db, &fl, &cfg)) return 1;
        altsql_ts_create(db, "r", "time:time,v:int");
        for (i = 0; i < (size_t)(10 + n * 60); i++) {
            char k[16];
            altsql_append(db, "r", (int64_t)i, (int)i);
            snprintf(k, sizeof k, "key%u", (unsigned)(i % 5));
            if (i % 7 == 0) altsql_put(db, k, "value", 5);
            if (i % 11 == 0) altsql_del(db, k);
        }
        altsql_close(db);
        save("mount", n * 2, flash, sizeof flash, 0);
        save("mount", n * 2 + 1, flash, sizeof flash, 3);
    }
    printf("seeds written to %s\n", dir);
    return 0;
}
