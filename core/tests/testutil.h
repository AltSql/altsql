/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Small helpers shared by the tests. */
#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "altsql.h"

static int t_fail, t_checks;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_RC(db, expr, want) do { int rc_ = (expr); t_checks++; if (rc_ != (want)) { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s returned %d, want %d (%s)\n", __FILE__, __LINE__, #expr, rc_, (want), \
            (db) ? altsql_errmsg(db) : "-"); } } while (0)

#define CHECK_OK(db, expr) CHECK_RC(db, expr, ALTSQL_OK)

/* A database on emulated flash in RAM. */
typedef struct rig {
    uint8_t *flash;
    altsql_ram_flash ram;
    altsql_flash fl;
    uint8_t *mem;
    size_t mem_size;
    altsql_config cfg;
    altsql *db;
    uint32_t ss, sc, align;
    int nomap;
} rig;

static inline void rig_init(rig *r, uint32_t ss, uint32_t sc, uint32_t align, size_t mem_size, int nomap) {
    memset(r, 0, sizeof *r);
    r->ss = ss;
    r->sc = sc;
    r->align = align;
    r->nomap = nomap;
    r->flash = (uint8_t *)malloc((size_t)ss * sc);
    memset(r->flash, 0xFF, (size_t)ss * sc);
    r->mem_size = mem_size;
    r->mem = (uint8_t *)malloc(mem_size);
    r->cfg.mem = r->mem;
    r->cfg.mem_size = mem_size;
    r->cfg.create = 1;
}

/* (Re)boots the emulated chip and opens the database on it. */
static inline int rig_open(rig *r) {
    altsql_ram_flash_init(&r->fl, &r->ram, r->flash, r->ss, r->sc, r->align);
    if (r->nomap) r->fl.map = NULL;
    r->db = NULL;
    return altsql_open(&r->db, &r->fl, &r->cfg);
}

static inline void rig_close(rig *r) {
    altsql_close(r->db);
    r->db = NULL;
}

static inline void rig_free(rig *r) {
    rig_close(r);
    free(r->flash);
    free(r->mem);
}

/* Growing text buffer, used as an export writer. */
typedef struct tbuf { char *p; size_t n, cap; } tbuf;

static inline int tbuf_write(void *ctx, const char *data, size_t len) {
    tbuf *b = (tbuf *)ctx;
    if (b->n + len + 1 > b->cap) {
        b->cap = (b->n + len + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
    return 0;
}

static inline void tbuf_free(tbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static inline int t_report(const char *name) {
    printf("%s: %d checks, %d failed\n", name, t_checks, t_fail);
    return t_fail ? 1 : 0;
}

#endif
