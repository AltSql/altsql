/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Shared test helpers for AltSql DB: a plain in-memory model to compare
 * against, deterministic values, and the checks every test uses.
 * The engine is compiled into each test, so tests may look at its insides
 * (pins, frames) to catch leaks. */
#ifndef DBTEST_H
#define DBTEST_H

#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_RAM
#include "altsql.h"
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_PORT_FILE
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#if defined(__GNUC__) || defined(__clang__)
#define TFN static __attribute__((unused))
#else
#define TFN static
#endif
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; return 1; } } while (0)

/* ---- deterministic values ---- */
static uint32_t xs(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static void fill(uint8_t *out, uint32_t vs, uint32_t vn) {
    uint32_t s = vs * 2654435761u + 7, i;
    for (i = 0; i < vn; i++) out[i] = (uint8_t)(xs(&s) >> 7);
}

/* ---- the model: sorted (bucket, key) -> (value seed, length) ---- */
typedef struct ment { uint32_t b, kn, vn, vs; uint8_t k[64]; } ment;
typedef struct model { ment *e; uint32_t n, cap; } model;

TFN int mcmp(const ment *a, uint32_t b, const uint8_t *k, uint32_t kn) {
    uint32_t n;
    int c;
    if (a->b != b) return a->b < b ? -1 : 1;
    n = a->kn < kn ? a->kn : kn;
    c = n ? memcmp(a->k, k, n) : 0;
    if (c) return c;
    return a->kn < kn ? -1 : (a->kn > kn ? 1 : 0);
}
TFN uint32_t mfind(const model *m, uint32_t b, const uint8_t *k, uint32_t kn, int *found) {
    uint32_t lo = 0, hi = m->n;
    *found = 0;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = mcmp(&m->e[mid], b, k, kn);
        if (c < 0) lo = mid + 1; else { hi = mid; if (!c) *found = 1; }
    }
    return lo;
}
TFN void mput(model *m, uint32_t b, const uint8_t *k, uint32_t kn, uint32_t vs, uint32_t vn) {
    int f;
    uint32_t i = mfind(m, b, k, kn, &f);
    if (!f) {
        if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 256; m->e = (ment *)realloc(m->e, m->cap * sizeof(ment)); }
        memmove(&m->e[i + 1], &m->e[i], (m->n - i) * sizeof(ment));
        m->n++;
        m->e[i].b = b; m->e[i].kn = kn; memcpy(m->e[i].k, k, kn);
    }
    m->e[i].vs = vs; m->e[i].vn = vn;
}
TFN int mdel(model *m, uint32_t b, const uint8_t *k, uint32_t kn) {
    int f;
    uint32_t i = mfind(m, b, k, kn, &f);
    if (!f) return 0;
    memmove(&m->e[i], &m->e[i + 1], (m->n - i - 1) * sizeof(ment));
    m->n--;
    return 1;
}
TFN void mcopy(model *d, const model *s) {
    if (d->cap < s->n) { d->cap = s->n + 16; d->e = (ment *)realloc(d->e, d->cap * sizeof(ment)); }
    if (s->n) memcpy(d->e, s->e, s->n * sizeof(ment));
    d->n = s->n;
}
TFN void mfree(model *m) { free(m->e); m->e = 0; m->n = m->cap = 0; }

static uint8_t g_vbuf[1 << 20], g_vexp[1 << 20];

/* No frame may stay pinned between calls. */
TFN int pins_clear(altsql_db *db) {
    uint32_t i;
    for (i = 0; i < db->nfr; i++) if (db->fr[i].pin) return 0;
    return 1;
}

/* The whole bucket, forwards and backwards, against the model. */
TFN int verify_bucket(altsql_db *db, const model *m, uint32_t b) {
    altsql_db_cursor c;
    uint32_t i, first, last;
    int f, rc;
    size_t vn = 0, kn = 0;
    const void *k = 0;
    first = mfind(m, b, (const uint8_t *)"", 0, &f);
    for (last = first; last < m->n && m->e[last].b == b; last++) {}
    rc = altsql_db_seek(&c, db, b, NULL, 0);
    for (i = first; i < last; i++) {
        CHECK(rc == ALTSQL_OK, "scan: expected entry %u of bucket %u, got %d (%s)", i - first, b, rc, altsql_db_errmsg(db));
        altsql_db_key(&c, &k, &kn);
        CHECK(kn == m->e[i].kn && !memcmp(k, m->e[i].k, kn), "scan: key %u of bucket %u differs", i - first, b);
        rc = altsql_db_value(&c, g_vbuf, sizeof g_vbuf, &vn);
        CHECK(rc == ALTSQL_OK && vn == m->e[i].vn, "scan: value %u: rc %d len %zu want %u", i - first, rc, vn, m->e[i].vn);
        fill(g_vexp, m->e[i].vs, m->e[i].vn);
        CHECK(!memcmp(g_vbuf, g_vexp, vn), "scan: value %u of bucket %u differs", i - first, b);
        rc = altsql_db_next(&c);
    }
    CHECK(rc == ALTSQL_NOTFOUND, "scan: bucket %u has more entries than the model (%d)", b, rc);
    rc = altsql_db_last(&c, db, b);
    for (i = last; i-- > first; ) {
        CHECK(rc == ALTSQL_OK, "reverse scan: expected entry %u, got %d (%s)", i - first, rc, altsql_db_errmsg(db));
        altsql_db_key(&c, &k, &kn);
        CHECK(kn == m->e[i].kn && !memcmp(k, m->e[i].k, kn), "reverse scan: key %u differs", i - first);
        rc = altsql_db_prev(&c);
    }
    CHECK(rc == ALTSQL_NOTFOUND, "reverse scan: more entries than the model (%d)", rc);
    CHECK(pins_clear(db), "pins left after a scan");
    return 0;
}

static uint8_t *g_chk;
static size_t g_chksize = 1 << 22;

TFN int check_slots(altsql_db *db, int need_both) {
    altsql_db_check_report r;
    int s, rc;
    if (!g_chk) g_chk = (uint8_t *)malloc(g_chksize);
    rc = altsql_db_check(db, -1, g_chk, g_chksize, &r);
    CHECK(rc == ALTSQL_OK, "check of the current header: %d %s", rc, altsql_db_errmsg(db));
    for (s = 0; s < 2; s++) {
        rc = altsql_db_check(db, s, g_chk, g_chksize, &r);
        CHECK(rc == ALTSQL_OK || (rc == ALTSQL_NOTFOUND && !need_both), "check of slot %d: %d %s", s, rc, altsql_db_errmsg(db));
    }
    CHECK(pins_clear(db), "pins left after a check");
    return 0;
}

/* A RAM file with its three buffers. */
typedef struct ramfile { altsql_db_file f; altsql_db_ram r; uint8_t *mem, *disk, *log; } ramfile;
static void ram_new(ramfile *rf, uint64_t cap, uint64_t logcap) {
    rf->mem = (uint8_t *)malloc(cap);
    rf->disk = (uint8_t *)malloc(cap);
    rf->log = (uint8_t *)malloc(logcap);
    altsql_db_ram_init(&rf->f, &rf->r, rf->mem, rf->disk, cap, rf->log, logcap);
}
TFN void ram_free(ramfile *rf) { free(rf->mem); free(rf->disk); free(rf->log); }

TFN int db_open_ram(altsql_db **db, ramfile *rf, void *mem, size_t msize, uint32_t ps) {
    altsql_db_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem;
    cfg.mem_size = msize;
    cfg.page_size = ps;
    cfg.create = 1;
    return altsql_db_open(db, &rf->f, &cfg);
}

#endif
