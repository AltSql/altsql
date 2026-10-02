/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Storage base: a log of records spread over flash sectors.
 *
 * The log is a circular run of sectors, oldest (tail) to newest (head).
 * Records are only ever appended at the head. When the head fills, the
 * next free sector becomes the head. When free sectors run low, the
 * tail sector is reclaimed: live key-value records are copied to the
 * head, time-series rows are dropped (rollover), and the sector is
 * retired, to be erased when it is next used. Every sector takes its
 * turn, so wear spreads evenly.
 *
 * Power-cut safety: a record only counts once its checksum is complete.
 * A write torn by a power cut leaves damaged bytes that the scanner steps
 * over, and new records go after them. Nothing older is ever rewritten.
 * ====================================================================== */

static int as_note(altsql *db, const as_rec *r);                 /* key-value layer */
static int as_rec_is_live(altsql *db, as_rec *r, int *live);
static void as_kv_repoint(altsql *db, const as_rec *r, uint32_t new_addr);

/* ---- Flash access ---------------------------------------------------- */
static const uint8_t *as_view(altsql *db, uint32_t addr, uint32_t len, uint8_t *dst) {
    if (db->fl.map) return db->fl.map(db->fl.ctx, addr, len);
    return db->fl.read(db->fl.ctx, addr, dst, len) == 0 ? dst : NULL;
}

static int as_write(altsql *db, uint32_t addr, const void *buf, uint32_t len) {
    if (db->fl.write(db->fl.ctx, addr, buf, len) != 0) {
        db->broken = 1;
        return as_err(db, ALTSQL_IOERR, "flash write failed");
    }
    return ALTSQL_OK;
}

static int as_erase(altsql *db, uint32_t s) {
    if (db->fl.erase(db->fl.ctx, s) != 0) {
        db->broken = 1;
        return as_err(db, ALTSQL_IOERR, "flash erase failed");
    }
    return ALTSQL_OK;
}

static int as_flash_ok(const altsql_flash *f) {
    uint32_t a;
    if (!f || !f->read || !f->write || !f->erase) return 0;
    if (f->sector_size < 256u || (f->sector_size & (f->sector_size - 1u))) return 0;
    if (f->sector_count < AS_RESERVE + 2u) return 0;
    if ((uint64_t)f->sector_size * f->sector_count > 0xFFFFFFFFull) return 0;
    a = f->write_align;
    return a == 1 || a == 2 || a == 4 || a == 8 || a == 16;
}

/* Entry check for public calls. */
static int as_ready(altsql *db) {
    if (!db) return ALTSQL_MISUSE;
    if (db->broken) return as_err(db, ALTSQL_MISUSE, "database is closed or must be reopened after an error");
    return ALTSQL_OK;
}

/* ---- Sector headers ---------------------------------------------------- */
static void as_hdr_make(uint8_t *h, uint32_t seq, uint32_t ss, uint32_t sc, uint32_t align, uint32_t gc_of) {
    memset(h, 0, AS_SH);
    as_put32(h, AS_MAGIC);
    h[4] = AS_VERSION;
    h[5] = (uint8_t)align;
    as_put32(h + 8, seq);
    as_put32(h + 12, ss);
    as_put32(h + 16, sc);
    as_put32(h + 20, gc_of);
    as_put32(h + 28, as_crc32(0, h, 28));
}

static int as_hdr_valid(const uint8_t *h) {
    return as_get32(h) == AS_MAGIC && h[4] == AS_VERSION && as_get32(h + 28) == as_crc32(0, h, 28);
}

static int as_hdr_check(const uint8_t *h, uint32_t ss, uint32_t sc, uint32_t *seq) {
    if (!as_hdr_valid(h)) return 0;
    if (as_get32(h + 12) != ss || as_get32(h + 16) != sc) return 0;
    *seq = as_get32(h + 8);
    return *seq != 0;
}

/* ---- The run of sectors ------------------------------------------------ */
static uint32_t as_run_sector(const altsql *db, uint32_t k) { return (db->tail + k) % db->sc; }
static int as_in_run(const altsql *db, uint32_t s) { return ((s + db->sc - db->tail) % db->sc) < db->used; }

/* 1 if [addr, addr+len) is all 0xFF, 0 if not, <0 on read failure. */
static int as_all_ff(altsql *db, uint32_t addr, uint32_t len) {
    while (len) {
        uint32_t n = len < db->rbuf_size ? len : db->rbuf_size, i;
        const uint8_t *p = as_view(db, addr, n, db->rbuf);
        if (!p) return -1;
        for (i = 0; i < n; i++) if (p[i] != 0xFF) return 0;
        addr += n;
        len -= n;
    }
    return 1;
}

/* ---- Reading records ----------------------------------------------------- */
#define AS_ALL 0xFFFFFFFFu

static void as_iter_start(as_iter *it, uint32_t k, uint32_t only) {
    it->k = k;
    it->off = AS_SH;
    it->only = only;
}

/* Next good record in log order: 1 = record in *r, 0 = end, <0 = error.
 * r->p stays valid until the next read from flash. When the flash is not
 * memory-mapped, the record is read into db->rbuf (header, then payload). */
static int as_next(altsql *db, as_iter *it, as_rec *r) {
    while (it->k < db->used && (it->only == AS_ALL || it->k <= it->only)) {
        uint32_t s = as_run_sector(db, it->k), base = s * db->ss;
        uint32_t end = (s == db->head) ? db->head_off : db->ss;
        while (it->off + AS_RH <= end) {
            uint32_t a = base + it->off, len;
            const uint8_t *h = as_view(db, a, AS_RH, db->rbuf), *p;
            if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
            if (h[0] != AS_MARK) {
                if (h[0] == 0xFF) {
                    int ff = as_all_ff(db, a, end - it->off);
                    if (ff < 0) return as_err(db, ALTSQL_IOERR, "flash read failed");
                    if (ff) break;                  /* nothing more in this sector */
                }
                it->off += db->align;               /* step over damaged bytes */
                continue;
            }
            len = as_get16(h + 2);
            if (it->off + AS_RH + len > end || AS_RH + len > db->rbuf_size) {
                it->off += db->align;
                continue;
            }
            if (db->fl.map) p = db->fl.map(db->fl.ctx, a, AS_RH + len);
            else p = db->fl.read(db->fl.ctx, a + AS_RH, db->rbuf + AS_RH, len) == 0 ? db->rbuf : NULL;
            if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
            if (as_crc32(as_crc32(0, p + 1, 7), p + AS_RH, len) != as_get32(p + 8)) {
                it->off += db->align;
                continue;
            }
            r->type = p[1];
            r->len = len;
            r->seq = as_get32(p + 4);
            r->addr = a;
            r->sector = s;
            r->p = p + AS_RH;
            it->off += as_alignup(db, AS_RH + len);
            return 1;
        }
        it->k++;
        it->off = AS_SH;
    }
    return 0;
}

/* ---- Writing records ------------------------------------------------------ */
static int as_activate(altsql *db) {
    uint32_t s = (db->head + 1) % db->sc;
    uint8_t h[AS_SH];
    int rc;
    if (db->used >= db->sc) return as_err(db, ALTSQL_FULL, "no free sector");
    rc = as_erase(db, s);
    if (rc) return rc;
    as_hdr_make(h, db->next_seq, db->ss, db->sc, db->align, db->gc_of);
    rc = as_write(db, s * db->ss, h, AS_SH);
    if (rc) return rc;
    db->sec[s].seq = db->next_seq++;
    db->sec[s].first_rseq = 0;
    db->sec[s].max_time = AS_TIME_NONE;
    db->head = s;
    db->head_off = AS_SH;
    db->used++;
    return ALTSQL_OK;
}

static int as_gc(altsql *db);

/* How a write may find room (as_reserve). */
enum {
    AS_RES_NOGC = 0,   /* reclaiming itself: any free sector, never reclaim */
    AS_RES_GC   = 1,   /* normal write: keep AS_RESERVE sectors free, reclaim */
    AS_RES_DEL  = 2    /* delete: as normal, then one reserve sector if full */
};

/* Makes room at the head for a record with a payload of len bytes.
 * Call before filling db->rbuf: reclaiming space reads through rbuf.
 * When a full turn of reclaiming frees nothing, the database remembers
 * it is full and fails fast (no pointless erasing) until a key-value
 * write succeeds. A delete may still use one reserve sector, so space
 * can always be freed. */
static int as_reserve(altsql *db, uint32_t len, int mode) {
    uint32_t total, guard = 0;
    int rc;
    if (db->broken) return as_err(db, ALTSQL_IOERR, "reopen the database after an I/O error");
    if (len > 0xFFFFu) return as_err(db, ALTSQL_TOOBIG, "record too large");
    total = as_alignup(db, AS_RH + len);
    if (total > db->rbuf_size || total > db->cap) return as_err(db, ALTSQL_TOOBIG, "record too large");
    for (;;) {
        uint32_t freec = db->sc - db->used;
        if (db->head_off + total <= db->ss) return ALTSQL_OK;
        if (freec > AS_RESERVE || (mode == AS_RES_NOGC && freec > 0)) return as_activate(db);
        if (mode == AS_RES_NOGC) return as_err(db, ALTSQL_FULL, "no free sector");
        rc = (db->full || ++guard > db->sc) ? ALTSQL_FULL : as_gc(db);
        if (rc == ALTSQL_FULL) {
            db->full = 1;
            if (mode == AS_RES_DEL && db->sc - db->used > 1) return as_activate(db);
            return as_err(db, ALTSQL_FULL, "storage is full of live key-value data");
        }
        if (rc) return rc;
    }
}

/* Writes the record whose payload is already in db->rbuf + AS_RH. */
static int as_commit(altsql *db, uint8_t type, uint32_t seq, uint32_t len, uint32_t *out_addr) {
    uint8_t *h = db->rbuf;
    uint32_t total = as_alignup(db, AS_RH + len), addr;
    int rc;
    h[0] = AS_MARK;
    h[1] = type;
    as_put16(h + 2, len);
    as_put32(h + 4, seq);
    as_put32(h + 8, as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, len));
    memset(h + AS_RH + len, 0, total - AS_RH - len);
    addr = db->head * db->ss + db->head_off;
    rc = as_write(db, addr, h, total);
    if (rc) return rc;
    if (!db->sec[db->head].first_rseq) db->sec[db->head].first_rseq = seq;
    db->head_off += total;
    if (seq >= db->next_rseq) db->next_rseq = seq + 1;
    if (out_addr) *out_addr = addr;
    return ALTSQL_OK;
}

/* Record as it sits in rbuf right after as_commit, for as_note(). */
static void as_rec_fresh(altsql *db, as_rec *r, uint8_t type, uint32_t seq, uint32_t len, uint32_t addr) {
    r->type = type;
    r->seq = seq;
    r->len = len;
    r->addr = addr;
    r->sector = addr / db->ss;
    r->p = db->rbuf + AS_RH;
}

/* ---- Reclaiming the oldest sector ------------------------------------------ */

/* A delete the gateway has not confirmed yet is carried forward, so a
 * gateway that was offline for a while still learns about it. */
static int as_keep_del(const altsql *db, const as_rec *r) {
#if ALTSQL_ENABLE_SYNC
    return !db->replica && r->seq > db->synced;
#else
    (void)db;
    (void)r;
    return 0;
#endif
}

static int as_gc(altsql *db) {
    as_iter it;
    as_rec r;
    uint32_t t = db->tail, addr;
    uint8_t z[16];
    int rc, live;
    if (db->used < 2) return as_err(db, ALTSQL_FULL, "storage full");
    as_iter_start(&it, 0, 0);
    while ((rc = as_next(db, &it, &r)) == 1) {
        if (r.type == AS_R_ROW) { db->rows_dropped++; continue; }
        if (r.type == AS_R_DEL && !as_keep_del(db, &r)) { as_kv_repoint(db, &r, AS_TOMB); continue; }
        if (r.type != AS_R_PUT && r.type != AS_R_DEL) continue;
        rc = as_rec_is_live(db, &r, &live);
        if (rc) return rc;
        if (!live) continue;
        db->gc_of = db->sec[t].seq;        /* a sector opened now is marked as ours */
        rc = as_reserve(db, r.len, AS_RES_NOGC);
        db->gc_of = 0;
        if (rc) return rc;
        if (r.p != db->rbuf + AS_RH) memmove(db->rbuf + AS_RH, r.p, r.len);
        rc = as_commit(db, r.type, db->replica ? r.seq : db->next_rseq, r.len, &addr);
        if (rc) return rc;
        as_kv_repoint(db, &r, addr);
    }
    if (rc < 0) return rc;
    /* Retire the sector by clearing its magic number. It is erased only
     * when it next becomes the head, so an erase cut short by a power loss
     * can never leave a half-erased sector inside the log. */
    memset(z, 0, sizeof z);
    rc = as_write(db, t * db->ss, z, db->align > 4u ? db->align : 4u);
    if (rc) return rc;
    db->sec[t].seq = 0;
    db->sec[t].first_rseq = 0;
    db->sec[t].max_time = AS_TIME_NONE;
    db->tail = (t + 1) % db->sc;
    db->used--;
    db->gc_runs++;
    return ALTSQL_OK;
}

/* ---- Mounting ------------------------------------------------------------------ */
static int as_mount(altsql *db) {
    uint32_t i, best = 0, hs = 0, valid_end = AS_SH, last = 0, off, align = 0, gc_of = 0;
    int foreign = 0;
    as_iter it;
    as_rec r;
    int rc;
    for (i = 0; i < db->sc; i++) {
        uint32_t seq = 0;
        const uint8_t *h = as_view(db, i * db->ss, AS_SH, db->rbuf);
        if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
        db->sec[i].seq = as_hdr_check(h, db->ss, db->sc, &seq) ? seq : 0;
        db->sec[i].first_rseq = 0;
        db->sec[i].max_time = AS_TIME_NONE;
        if (db->sec[i].seq > best) { best = db->sec[i].seq; hs = i; align = h[5]; gc_of = as_get32(h + 20); }
        else if (!db->sec[i].seq && as_hdr_valid(h))
            foreign = 1;               /* a valid header, but for another geometry */
    }
    if (!best && foreign)
        return as_err(db, ALTSQL_MISUSE, "flash holds a database with another sector size or count");
    if (!best) return as_err(db, ALTSQL_NOTFOUND, "no database on this flash");
    if (align != db->align) return as_err(db, ALTSQL_MISUSE, "flash was formatted with another write_align");

    /* The log is the run of sectors ending at the head whose sequence
     * numbers count down by one. Anything else is free space. */
    db->head = hs;
    db->used = 1;
    while (db->used < db->sc && db->sec[(hs + db->sc - db->used) % db->sc].seq == best - db->used)
        db->used++;
    db->tail = (hs + db->sc + 1 - db->used) % db->sc;
    /* A reclaim cut short after it opened a new sector: the tail it was
     * copying is still here, and the new sector holds only copies of it.
     * Drop that sector; the reclaim simply runs again. Without this, each
     * such power cut would cost a spare sector for good. */
    db->next_seq = best + 1;
    if (gc_of && db->used > 1 && gc_of == best - db->used + 1) {
        db->used--;
        db->head = (hs + db->sc - 1) % db->sc;
        db->next_seq = best;          /* its number is used again when that sector is */
    }
    for (i = 0; i < db->sc; i++) if (!as_in_run(db, i)) db->sec[i].seq = 0;
    db->next_rseq = 1;

    db->head_off = db->ss;                    /* scan the whole head sector */
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        if (r.sector == db->head) valid_end = it.off;
        rc = as_note(db, &r);
        if (rc) return rc;
    }
    if (rc < 0) return rc;

    /* Resume after the last byte ever programmed in the head sector, so a
     * torn write is never written over. */
    off = db->ss;
    while (off > AS_SH && !last) {
        uint32_t n = off - AS_SH < db->rbuf_size ? off - AS_SH : db->rbuf_size, j;
        const uint8_t *p = as_view(db, db->head * db->ss + off - n, n, db->rbuf);
        if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
        for (j = n; j > 0; j--) if (p[j - 1] != 0xFF) { last = off - n + j; break; }
        off -= n;
    }
    if (last) last = as_alignup(db, last);
    db->head_off = valid_end > last ? valid_end : last;
    if (db->head_off < AS_SH) db->head_off = AS_SH;
    if (db->head_off > db->ss) db->head_off = db->ss;
    return ALTSQL_OK;
}

/* ---- Public: format, open, close, info ------------------------------------------ */
int altsql_format(const altsql_flash *f) {
    uint8_t h[AS_SH];
    uint32_t i;
    if (!as_flash_ok(f)) return ALTSQL_MISUSE;
    for (i = 0; i < f->sector_count; i++) if (f->erase(f->ctx, i) != 0) return ALTSQL_IOERR;
    as_hdr_make(h, 1, f->sector_size, f->sector_count, f->write_align, 0);
    return f->write(f->ctx, 0, h, AS_SH) != 0 ? ALTSQL_IOERR : ALTSQL_OK;
}

static void *as_take(uint8_t *base, size_t size, size_t *off, size_t n) {
    void *p;
    size_t o = (*off + 7u) & ~(size_t)7u;
    if (o > size || n > size - o) return NULL;
    p = base + o;
    *off = o + n;
    return p;
}

int altsql_open(altsql **out, const altsql_flash *f, const altsql_config *cfg) {
    uint8_t *m;
    size_t off = 0, o;
    altsql *db;
    uint32_t rec;
    int rc;
    if (!out) return ALTSQL_MISUSE;
    *out = NULL;
    if (!cfg || !cfg->mem || !as_flash_ok(f)) return ALTSQL_MISUSE;
    m = (uint8_t *)cfg->mem;
    db = (altsql *)as_take(m, cfg->mem_size, &off, sizeof *db);
    if (!db) return ALTSQL_NOMEM;
    memset(db, 0, sizeof *db);
    db->fl = *f;
    db->ss = f->sector_size;
    db->sc = f->sector_count;
    db->align = f->write_align;
    db->cap = db->ss - AS_SH;
    db->replica = cfg->replica ? 1 : 0;
    db->kv_complete = 1;
    db->kv_slots = cfg->kv_slots ? cfg->kv_slots : 64;
    db->max_series = cfg->max_series ? cfg->max_series : 8;
    rec = cfg->max_record ? cfg->max_record : 256;
    if (rec < 64) rec = 64;
    rec = as_alignup(db, rec);
    if (rec > db->cap) rec = db->cap & ~(db->align - 1);
    if (rec > 0xFFFFu + AS_RH) rec = 0xFFFFu + AS_RH;
    db->rbuf_size = rec;
    db->sec = (as_sector *)as_take(m, cfg->mem_size, &off, sizeof(as_sector) * db->sc);
    db->kv = (as_slot *)as_take(m, cfg->mem_size, &off, sizeof(as_slot) * db->kv_slots);
    db->series = (as_series *)as_take(m, cfg->mem_size, &off, sizeof(as_series) * db->max_series);
    db->rbuf = (uint8_t *)as_take(m, cfg->mem_size, &off, db->rbuf_size);
    if (!db->sec || !db->kv || !db->series || !db->rbuf) return ALTSQL_NOMEM;
    memset(db->kv, 0, sizeof(as_slot) * db->kv_slots);
    memset(db->series, 0, sizeof(as_series) * db->max_series);
    db->mem_used = off;
    o = (off + 7u) & ~(size_t)7u;
    if (o < cfg->mem_size) {
        db->work = m + o;
        db->work_size = cfg->mem_size - o;
    }
    rc = as_mount(db);
    if (rc == ALTSQL_NOTFOUND && cfg->create) {
        rc = altsql_format(f);
        if (rc == ALTSQL_OK) {
            memset(db->kv, 0, sizeof(as_slot) * db->kv_slots);
            memset(db->series, 0, sizeof(as_series) * db->max_series);
            db->kv_used = 0;
            db->kv_complete = 1;
            rc = as_mount(db);
        } else {
            as_err(db, rc, "formatting the flash failed");
        }
    }
    *out = db;                 /* also on failure, so altsql_errmsg() can explain */
    if (rc) db->broken = 1;
    return rc;
}

void altsql_close(altsql *db) {
    if (db) db->broken = 1;
}

const char *altsql_errmsg(const altsql *db) {
    return db ? db->err : "no database";
}

int altsql_info_get(altsql *db, altsql_info *o) {
    if (!db || !o) return ALTSQL_MISUSE;
    memset(o, 0, sizeof *o);
    o->sector_size = db->ss;
    o->sectors = db->sc;
    o->used_sectors = db->used;
    o->free_sectors = db->sc - db->used;
    o->last_seq = db->next_rseq - 1;
    o->kv_keys = db->kv_used;
    o->gc_runs = db->gc_runs;
    o->rows_dropped = db->rows_dropped;
    o->mem_used = (uint32_t)db->mem_used;
    return ALTSQL_OK;
}
