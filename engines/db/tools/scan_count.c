/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* The direct path's ordered scan on its own, for instruction counts: n entries keyed and sized as
 * in Gate 1, loaded into a file in RAM, then scanned twice. tools/gate1_versions.py builds it on
 * each version's header and runs it under valgrind --tool=callgrind; scan() is what it counts.
 *   scan_count [entries] */
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long scan(altsql_db *db, uint32_t bk, uint64_t *sum) {
    altsql_db_cursor c;
    long n = 0;
    uint8_t v[64];
    size_t vn;
    int rc = altsql_db_seek(&c, db, bk, NULL, 0);
    while (rc == ALTSQL_OK) {
        if (altsql_db_value(&c, v, sizeof v, &vn)) return -1;
        *sum += v[2] + v[10] + vn;
        n++;
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND ? n : -1;
}

int main(int argc, char **argv) {
    uint32_t n = argc > 1 ? (uint32_t)atol(argv[1]) : 100000, i;
    uint64_t cap = 1u << 28, sum = 0;
    uint8_t *fm = (uint8_t *)malloc(cap), *fd = (uint8_t *)malloc(cap), *lg = (uint8_t *)malloc(64u << 20);
    size_t memsz = 256u << 20;
    void *mem = malloc(memsz);
    altsql_db_file f;
    altsql_db_ram r;
    altsql_db_config cfg;
    altsql_db *db = NULL;
    uint32_t bk;
    long a, b;
    altsql_db_ram_init(&f, &r, fm, fd, cap, lg, 64u << 20);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = memsz; cfg.create = 1;
    if (altsql_db_open(&db, &f, &cfg) || altsql_db_bucket(db, "readings", 1, &bk)) return 1;
    for (i = 0; i < n; i++) {                                /* keys as Gate 1's: device, then time */
        uint8_t k[12], v[14];
        uint32_t dev = i % 1000;
        uint64_t t = 1700000000ull + i / 1000;
        int j;
        k[0] = (uint8_t)(dev >> 24); k[1] = (uint8_t)(dev >> 16); k[2] = (uint8_t)(dev >> 8); k[3] = (uint8_t)dev;
        for (j = 0; j < 8; j++) k[4 + j] = (uint8_t)(t >> (56 - 8 * j));
        memset(v, (int)(i & 0xFF), sizeof v);
        if (i % 10000 == 0 && altsql_db_begin(db, 1)) return 2;
        if (altsql_db_put(db, bk, k, 12, v, 14)) return 3;
        if ((i % 10000 == 9999 || i == n - 1) && altsql_db_commit(db)) return 4;
    }
    a = scan(db, bk, &sum);
    b = scan(db, bk, &sum);
    printf("%s: scanned %ld and %ld entries, checksum %llu\n", ALTSQL_DB_VERSION, a, b, (unsigned long long)sum);
    altsql_db_close(db);
    free(fm); free(fd); free(lg); free(mem);
    return a == (long)n && b == (long)n ? 0 : 5;
}
