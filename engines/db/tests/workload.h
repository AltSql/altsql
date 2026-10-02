/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* The workload the crash matrix and fault injection share: transactions of
 * puts and deletes on one bucket, some values big enough for overflow pages.
 * Deterministic by seed, so any state can be rebuilt in the model. */
#ifndef WORKLOAD_H
#define WORKLOAD_H
#include "dbtest.h"

typedef struct wl { uint32_t seed, ntx, ops, ps; } wl;

static void wl_op(const wl *w, uint32_t t, uint32_t i, int *put, uint8_t *k, uint32_t *kn, uint32_t *vs, uint32_t *vn) {
    uint32_t s = (w->seed * 1000003u) ^ (t * 7919u + 1) ^ (i * 104729u) ^ 0x9E3779B9u, r;
    xs(&s); xs(&s); xs(&s);
    *put = xs(&s) % 100 < 75;
    *kn = (uint32_t)sprintf((char *)k, "key%u", xs(&s) % 300);
    *vs = xs(&s);
    r = xs(&s) % 100;
    *vn = r < 80 ? xs(&s) % 50 : (r < 95 ? xs(&s) % (w->ps / 3) : w->ps / 2 + xs(&s) % (2 * w->ps));
}

static void wl_model(const wl *w, model *m, uint32_t b, uint32_t t) {
    uint32_t i, kn, vs, vn;
    uint8_t k[32];
    int put;
    for (i = 0; i < w->ops; i++) {
        wl_op(w, t, i, &put, k, &kn, &vs, &vn);
        if (put) mput(m, b, k, kn, vs, vn); else mdel(m, b, k, kn);
    }
}

/* One transaction on the database. 0, or the first error. */
static int wl_txn(const wl *w, altsql_db *db, uint32_t b, uint32_t t, int *in_commit) {
    uint32_t i, kn, vs, vn;
    uint8_t k[32];
    int put, rc;
    *in_commit = 0;
    if ((rc = altsql_db_begin(db, 1)) != 0) return rc;
    for (i = 0; i < w->ops; i++) {
        wl_op(w, t, i, &put, k, &kn, &vs, &vn);
        if (put) { fill(g_vbuf, vs, vn); rc = altsql_db_put(db, b, k, kn, g_vbuf, vn); }
        else rc = altsql_db_del(db, b, k, kn);
        if (rc < 0) { altsql_db_rollback(db); return rc; }
    }
    *in_commit = 1;
    return altsql_db_commit(db);
}

/* 1 when the bucket holds exactly the model's entries. Quiet. */
static int same(altsql_db *db, const model *m, uint32_t b) {
    altsql_db_cursor c;
    uint32_t i;
    int rc = altsql_db_seek(&c, db, b, NULL, 0);
    for (i = 0; i < m->n; i++) {
        size_t kn = 0, vn = 0;
        const void *k = 0;
        if (m->e[i].b != b) continue;
        if (rc != ALTSQL_OK) return 0;
        altsql_db_key(&c, &k, &kn);
        if (kn != m->e[i].kn || memcmp(k, m->e[i].k, kn)) return 0;
        if (altsql_db_value(&c, g_vbuf, sizeof g_vbuf, &vn) != ALTSQL_OK || vn != m->e[i].vn) return 0;
        fill(g_vexp, m->e[i].vs, m->e[i].vn);
        if (memcmp(g_vbuf, g_vexp, vn)) return 0;
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND;
}
#endif
