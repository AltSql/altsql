/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Sync: device to gateway, record for record.
 *
 * The device hands out its records exactly as they sit in flash. The
 * gateway replica checks each one (marker, length, checksum, layout) and
 * appends it to its own log unchanged. Both ends hold the same bytes, so
 * there is no translation step in between.
 *
 * Sequence numbers make it safe to resend: the device resumes after the
 * newest record the gateway confirmed, and the replica skips anything it
 * already has.
 * ====================================================================== */
#if ALTSQL_ENABLE_SYNC

/* Checks the layout of a payload that arrives from outside. */
static int as_payload_ok(uint8_t type, const uint8_t *p, uint32_t len) {
    if (type == AS_R_PUT || type == AS_R_DEL) {
        uint32_t klen;
        if (len < 1) return 0;
        klen = p[0];
        if (!(klen >= 1 && klen <= AS_MAXKEY && 1u + klen <= len)) return 0;
#if ALTSQL_ENABLE_TS
        if (klen >= 2 && p[1] == 0x01 && p[2] == 'S') {         /* a series definition */
            uint8_t types[AS_MAXCOLS];
            if (type == AS_R_DEL) return 0;                     /* series are never deleted */
            if (!as_series_name_ok(p + 3, klen - 2) || len < 1u + klen + 2u) return 0;
            if (as_get16(p + 1 + klen) == 0) return 0;
            return as_schema_parse((const char *)p + 1 + klen + 2, len - 1 - klen - 2, types, NULL, NULL) >= 1;
        }
#endif
        return 1;
    }
    return type == AS_R_ROW && len >= 10;           /* series id + time */
}

/* What a record is, for the sync filter; *sid is the series id, or 0. */
static int as_rec_kind(const as_rec *r, int *sid) {
    uint32_t klen;
    *sid = 0;
    if (r->type == AS_R_ROW) {
        if (r->len >= 2) *sid = (int)as_get16(r->p);
        return ALTSQL_REC_ROW;
    }
    klen = r->len >= 1 ? r->p[0] : 0;
    if (r->type == AS_R_PUT && klen >= 3 && r->p[1] == 0x01 && r->p[2] == 'S') {   /* series definition */
        if (r->len >= 1u + klen + 2u) *sid = (int)as_get16(r->p + 1 + klen);
        return ALTSQL_REC_SCHEMA;
    }
    return ALTSQL_REC_KV;
}

int altsql_sync_read(altsql *db, uint32_t after_seq, void *buf, size_t cap, size_t *len,
                     uint32_t *last_seq, altsql_sync_filter filter, void *fctx) {
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;
    uint32_t k, last = after_seq;
    as_iter it;
    as_rec r;
    int rc = 0;
    if (!db || !buf || !len || !last_seq) return ALTSQL_MISUSE;
    *len = 0;
    *last_seq = after_seq;
    if ((rc = as_ready(db)) != 0) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "sync_read runs on the device, not on a replica");
    /* The gateway has everything up to after_seq: deletes older than that
     * no longer need to be carried forward. */
    if (after_seq < db->next_rseq && after_seq > db->synced) db->synced = after_seq;

    for (k = 0; k < db->used; k++) {
        /* Sequence numbers grow along the log, so a whole sector can be
         * skipped when the next one starts at or before after_seq + 1. */
        if (k + 1 < db->used) {
            uint32_t nf = db->sec[as_run_sector(db, k + 1)].first_rseq;
            if (nf && nf <= after_seq + 1u) continue;
        }
        as_iter_start(&it, k, k);
        while ((rc = as_next(db, &it, &r)) == 1) {
            uint32_t need;
            if (r.seq <= after_seq) continue;
            if (filter) {
                int sid, kind = as_rec_kind(&r, &sid);
                if (!filter(fctx, kind, sid)) { last = r.seq; continue; }
            }
            need = AS_RH + r.len;
            if (n + need > cap) {
                if (n == 0 && last == after_seq)
                    return as_err(db, ALTSQL_TOOBIG, "sync buffer is smaller than one record");
                *len = n;
                *last_seq = last;
                return ALTSQL_OK;                   /* more records are waiting */
            }
            memcpy(out + n, r.p - AS_RH, need);     /* header and payload, as stored */
            n += need;
            last = r.seq;
        }
        if (rc < 0) return rc;
    }
    *len = n;
    *last_seq = last;
    return ALTSQL_DONE;
}

int altsql_sync_apply(altsql *db, const void *buf, size_t len, uint32_t *last_seq) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int rc;
    if (!db || (!buf && len)) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->replica) return as_err(db, ALTSQL_MISUSE, "sync_apply needs a database opened with replica = 1");
    while (off < len) {
        const uint8_t *h = p + off;
        uint32_t plen, seq, addr;
        uint8_t type;
        as_rec r;
        if (len - off < AS_RH || h[0] != AS_MARK) return as_err(db, ALTSQL_CORRUPT, "sync data is damaged");
        type = h[1];
        plen = as_get16(h + 2);
        seq = as_get32(h + 4);
        if (len - off - AS_RH < plen) return as_err(db, ALTSQL_CORRUPT, "sync data is cut short");
        if (as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, plen) != as_get32(h + 8))
            return as_err(db, ALTSQL_CORRUPT, "sync data failed its checksum");
        if (!as_payload_ok(type, h + AS_RH, plen)) return as_err(db, ALTSQL_CORRUPT, "sync data holds an unknown record");
        if (seq >= db->next_rseq) {                 /* new to this replica */
            rc = as_reserve(db, plen, AS_RES_GC);
            if (rc) return rc;
            memcpy(db->rbuf + AS_RH, h + AS_RH, plen);
            rc = as_commit(db, type, seq, plen, &addr);
            if (rc) return rc;
            as_rec_fresh(db, &r, type, seq, plen, addr);
            if (type != AS_R_ROW) db->full = 0;
            rc = as_note(db, &r);
            if (rc) return rc;
        }
        off += AS_RH + plen;
    }
    if (last_seq) *last_seq = db->next_rseq - 1;
    return ALTSQL_OK;
}

int altsql_series_id(altsql *db, const char *series) {
#if ALTSQL_ENABLE_TS
    as_series *S;
    if (!db || !series) return 0;
    S = as_series_find(db, series, strlen(series));
    return S ? (int)S->id : 0;
#else
    (void)db;
    (void)series;
    return 0;
#endif
}

#endif /* ALTSQL_ENABLE_SYNC */
