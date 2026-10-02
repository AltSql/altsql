/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Key-value layer
 *
 * A small hash index in RAM maps each key to its newest record (PUT or
 * DEL). It is rebuilt by scanning the log at open. If more keys arrive
 * than the index has room for, lookups fall back to scanning the log.
 * ====================================================================== */

#if ALTSQL_ENABLE_TS
static void as_series_note(altsql *db, const uint8_t *name, uint32_t nlen, const uint8_t *val, uint32_t vlen);
#endif

/* Compares the key of the record at addr with key. Uses a stack buffer,
 * never db->rbuf, so a record the caller holds in rbuf stays intact. */
static int as_key_at(altsql *db, uint32_t addr, const uint8_t *key, uint32_t klen, uint8_t *type, int *match) {
    uint8_t b[AS_RH + 1 + AS_MAXKEY + 2];
    const uint8_t *p;
    *match = 0;
    *type = 0;
    if (klen > AS_MAXKEY) return ALTSQL_OK;
    p = as_view(db, addr, AS_RH + 1, b);
    if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
    *type = p[1];
    if (p[AS_RH] != klen) return ALTSQL_OK;
    p = as_view(db, addr, AS_RH + 1 + klen, b);
    if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
    *match = memcmp(p + AS_RH + 1, key, klen) == 0;
    return ALTSQL_OK;
}

/* 1 = found (*slot), 0 = not found (*slot = where to insert, or AS_ALL
 * when the index is full), <0 = error. A tombstone (AS_TOMB) stands for a
 * key whose delete record was reclaimed: it keeps probe chains intact
 * and matches no key. */
static int as_kv_lookup(altsql *db, const uint8_t *key, uint32_t klen, uint32_t h, uint32_t *slot) {
    uint32_t i = h % db->kv_slots, n, tomb = AS_ALL;
    for (n = 0; n < db->kv_slots; n++, i = (i + 1) % db->kv_slots) {
        as_slot *s = &db->kv[i];
        if (s->addr == 0) { *slot = tomb != AS_ALL ? tomb : i; return 0; }
        if (s->addr == AS_TOMB) { if (tomb == AS_ALL) tomb = i; continue; }
        if (s->hash == h) {
            uint8_t type;
            int match, rc = as_key_at(db, s->addr, key, klen, &type, &match);
            if (rc) return rc;
            if (match) { *slot = i; return 1; }
        }
    }
    *slot = tomb;
    return 0;
}

static int as_kv_index(altsql *db, const uint8_t *key, uint32_t klen, uint32_t addr) {
    uint32_t h = as_hash(key, klen), slot;
    int rc = as_kv_lookup(db, key, klen, h, &slot);
    if (rc < 0) return rc;
    if (rc == 1) { db->kv[slot].addr = addr; return ALTSQL_OK; }
    if (slot != AS_ALL && db->kv[slot].addr == AS_TOMB) {     /* reuse a tombstone */
        db->kv[slot].hash = h;
        db->kv[slot].addr = addr;
        return ALTSQL_OK;
    }
    if (slot == AS_ALL || (db->kv_used + 1u) * 4u > db->kv_slots * 3u) {
        db->kv_complete = 0;              /* index full: fall back to scans */
        return ALTSQL_OK;
    }
    db->kv[slot].hash = h;
    db->kv[slot].addr = addr;
    db->kv_used++;
    return ALTSQL_OK;
}

/* Index slot currently pointing at addr, or AS_ALL. No flash reads. */
static uint32_t as_slot_of(const altsql *db, uint32_t h, uint32_t addr) {
    uint32_t i = h % db->kv_slots, n;
    for (n = 0; n < db->kv_slots; n++, i = (i + 1) % db->kv_slots) {
        if (db->kv[i].addr == 0) break;
        if (db->kv[i].hash == h && db->kv[i].addr == addr) return i;
    }
    return AS_ALL;
}

/* The record at r->addr moved to new_addr (or was reclaimed: AS_TOMB). */
static void as_kv_repoint(altsql *db, const as_rec *r, uint32_t new_addr) {
    uint32_t klen, s;
    if (r->len < 1) return;
    klen = r->p[0];
    if (1u + klen > r->len) return;
    s = as_slot_of(db, as_hash(r->p + 1, klen), r->addr);
    if (s != AS_ALL) db->kv[s].addr = new_addr;
}

/* Newest record for key: *addr = 0 if none; *type = AS_R_PUT or AS_R_DEL. */
static int as_kv_latest(altsql *db, const uint8_t *key, uint32_t klen, uint32_t *addr, uint8_t *type) {
    uint32_t slot;
    int rc;
    *addr = 0;
    *type = 0;
    rc = as_kv_lookup(db, key, klen, as_hash(key, klen), &slot);
    if (rc < 0) return rc;
    if (rc == 1) {
        int match;
        uint32_t a = db->kv[slot].addr;
        rc = as_key_at(db, a, key, klen, type, &match);
        if (rc) return rc;
        *addr = a;
        return ALTSQL_OK;
    }
    if (db->kv_complete) return ALTSQL_OK;
    {   /* the index overflowed at some point: scan the whole log */
        as_iter it;
        as_rec r;
        uint8_t k[AS_MAXKEY + 2];
        memcpy(k, key, klen);
        as_iter_start(&it, 0, AS_ALL);
        while ((rc = as_next(db, &it, &r)) == 1) {
            if ((r.type == AS_R_PUT || r.type == AS_R_DEL) && r.len >= 1u + klen &&
                r.p[0] == klen && memcmp(r.p + 1, k, klen) == 0) {
                *addr = r.addr;
                *type = r.type;
            }
        }
        return rc < 0 ? rc : ALTSQL_OK;
    }
}

/* Is this PUT or DEL the newest record of its key? May re-read *r into rbuf. */
static int as_rec_is_live(altsql *db, as_rec *r, int *live) {
    uint32_t klen, h, addr;
    uint8_t k[AS_MAXKEY + 2], type;
    int rc;
    *live = 0;
    if (r->len < 1) return ALTSQL_OK;
    klen = r->p[0];
    if (klen == 0 || klen > AS_MAXKEY || 1u + klen > r->len) return ALTSQL_OK;
    h = as_hash(r->p + 1, klen);
    if (as_slot_of(db, h, r->addr) != AS_ALL) { *live = 1; return ALTSQL_OK; }
    if (db->kv_complete) return ALTSQL_OK;
    memcpy(k, r->p + 1, klen);
    rc = as_kv_latest(db, k, klen, &addr, &type);
    if (rc) return rc;
    *live = addr == r->addr;
    if (!db->fl.map) {       /* the scan reused rbuf: read this record back */
        if (db->fl.read(db->fl.ctx, r->addr, db->rbuf, AS_RH + r->len) != 0)
            return as_err(db, ALTSQL_IOERR, "flash read failed");
        r->p = db->rbuf + AS_RH;
    }
    return ALTSQL_OK;
}

/* Bookkeeping for every record that enters the log (open, write, sync). */
static int as_note(altsql *db, const as_rec *r) {
    as_sector *s = &db->sec[r->sector];
    if (r->seq >= db->next_rseq) db->next_rseq = r->seq + 1;
    if (!s->first_rseq) s->first_rseq = r->seq;
    if (r->type == AS_R_ROW) {
        if (r->len >= 10) {
            int64_t t = (int64_t)as_get64(r->p + 2);
            if (t > s->max_time) s->max_time = t;
        }
        return ALTSQL_OK;
    }
    if ((r->type == AS_R_PUT || r->type == AS_R_DEL) && r->len >= 1) {
        uint32_t klen = r->p[0];
        int rc;
        if (klen == 0 || klen > AS_MAXKEY || 1u + klen > r->len) return ALTSQL_OK;
        rc = as_kv_index(db, r->p + 1, klen, r->addr);
        if (rc) return rc;
#if ALTSQL_ENABLE_TS
        if (r->type == AS_R_PUT && klen >= 3 && r->p[1] == 0x01 && r->p[2] == 'S')
            as_series_note(db, r->p + 3, klen - 2, r->p + 1 + klen, r->len - 1 - klen);
#endif
    }
    return ALTSQL_OK;
}

/* PUT or DEL. key and val must not point into db->rbuf. */
static int as_kv_write(altsql *db, uint8_t type, const uint8_t *key, uint32_t klen, const void *val, size_t vlen) {
    uint32_t len, addr, seq;
    as_rec r;
    int rc;
    if (vlen > 0xFFFFu) return as_err(db, ALTSQL_TOOBIG, "value too large");
    len = 1u + klen + (uint32_t)vlen;
    rc = as_reserve(db, len, type == AS_R_DEL ? AS_RES_DEL : AS_RES_GC);
    if (rc) return rc;
    db->rbuf[AS_RH] = (uint8_t)klen;
    memcpy(db->rbuf + AS_RH + 1, key, klen);
    if (vlen) memcpy(db->rbuf + AS_RH + 1 + klen, val, vlen);
    seq = db->next_rseq;
    rc = as_commit(db, type, seq, len, &addr);
    if (rc) return rc;
    db->full = 0;                  /* this may have made old records reclaimable */
    as_rec_fresh(db, &r, type, seq, len, addr);
    return as_note(db, &r);
}

static int as_user_key(altsql *db, const char *key, size_t *klen) {
    int rc = as_ready(db);
    if (rc) return rc;
    *klen = key ? strlen(key) : 0;
    if (*klen == 0 || *klen > AS_MAXKEY) return as_err(db, ALTSQL_MISUSE, "key must be 1 to 200 bytes");
    if ((unsigned char)key[0] == 0x01) return as_err(db, ALTSQL_MISUSE, "keys starting with byte 0x01 are reserved");
    return ALTSQL_OK;
}

int altsql_put(altsql *db, const char *key, const void *val, size_t len) {
    size_t klen;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    if (len && !val) return as_err(db, ALTSQL_MISUSE, "value is NULL");
    return as_kv_write(db, AS_R_PUT, (const uint8_t *)key, (uint32_t)klen, val, len);
}

int altsql_get(altsql *db, const char *key, void *buf, size_t cap, size_t *outlen) {
    size_t klen;
    uint32_t addr, len, vlen, n;
    uint8_t type, hb[AS_RH];
    const uint8_t *h;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    rc = as_kv_latest(db, (const uint8_t *)key, (uint32_t)klen, &addr, &type);
    if (rc) return rc;
    if (!addr || type != AS_R_PUT) return ALTSQL_NOTFOUND;
    h = as_view(db, addr, AS_RH, hb);
    if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
    len = as_get16(h + 2);
    if (len < 1u + (uint32_t)klen) return as_err(db, ALTSQL_CORRUPT, "damaged record");
    vlen = len - 1u - (uint32_t)klen;
    if (outlen) *outlen = vlen;
    n = vlen < cap ? vlen : (uint32_t)cap;
    if (n && buf) {
        if (db->fl.map) {
            const uint8_t *p = db->fl.map(db->fl.ctx, addr + AS_RH + 1 + (uint32_t)klen, n);
            if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
            memcpy(buf, p, n);
        } else if (db->fl.read(db->fl.ctx, addr + AS_RH + 1 + (uint32_t)klen, buf, n) != 0) {
            return as_err(db, ALTSQL_IOERR, "flash read failed");
        }
    }
    return ALTSQL_OK;
}

int altsql_del(altsql *db, const char *key) {
    size_t klen;
    uint32_t addr;
    uint8_t type;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    rc = as_kv_latest(db, (const uint8_t *)key, (uint32_t)klen, &addr, &type);
    if (rc) return rc;
    if (!addr || type != AS_R_PUT) return ALTSQL_NOTFOUND;
    return as_kv_write(db, AS_R_DEL, (const uint8_t *)key, (uint32_t)klen, NULL, 0);
}

/* Live user keys in log order. Needs a complete index. */
int altsql_kv_each(altsql *db, altsql_kv_cb cb, void *ctx) {
    as_iter it;
    as_rec r;
    int rc;
    if (!db || !cb) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        uint32_t klen;
        if (r.type != AS_R_PUT || r.len < 1) continue;
        klen = r.p[0];
        if (!klen || 1u + klen > r.len || r.p[1] == 0x01) continue;
        if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
        if (cb(ctx, (const char *)r.p + 1, klen, r.p + 1 + klen, r.len - 1 - klen)) break;
    }
    return rc < 0 ? rc : ALTSQL_OK;
}
