/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Readable text form
 *
 *   # altsql text export v1
 *   series readings "time:time,machine:int,temp:float"
 *   kv "cfg.interval" "60"
 *   row readings 1700000000 3 21.53
 *
 * Series first (by id), then keys in byte order, then rows oldest first.
 * Two databases with the same content export to the same text, whatever
 * their flash history, so exports can be compared and diffed. Strings are
 * in double quotes with \" \\ \n \r \t and \xHH escapes.
 * ====================================================================== */
#if ALTSQL_ENABLE_TEXT

typedef struct as_wr {
    altsql_write_cb out;
    void *ctx;
    int stopped;
    size_t n;
    char buf[256];
} as_wr;

static void as_wr_flush(as_wr *w) {
    if (w->n && !w->stopped && w->out(w->ctx, w->buf, w->n)) w->stopped = 1;
    w->n = 0;
}

static void as_wr_put(as_wr *w, const char *s, size_t n) {
    while (n) {
        size_t k = sizeof w->buf - w->n;
        if (k > n) k = n;
        memcpy(w->buf + w->n, s, k);
        w->n += k;
        s += k;
        n -= k;
        if (w->n == sizeof w->buf) as_wr_flush(w);
    }
}

static void as_wr_str(as_wr *w, const char *s) { as_wr_put(w, s, strlen(s)); }

static void as_wr_quoted(as_wr *w, const uint8_t *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    as_wr_put(w, "\"", 1);
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        char e[4];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; as_wr_put(w, e, 2); }
        else if (c == '\n') as_wr_put(w, "\\n", 2);
        else if (c == '\r') as_wr_put(w, "\\r", 2);
        else if (c == '\t') as_wr_put(w, "\\t", 2);
        else if (c < 0x20 || c == 0x7F) {
            e[0] = '\\'; e[1] = 'x'; e[2] = hex[c >> 4]; e[3] = hex[c & 15];
            as_wr_put(w, e, 4);
        } else {
            as_wr_put(w, (const char *)&c, 1);
        }
    }
    as_wr_put(w, "\"", 1);
}

/* Key of the record at addr, via a caller buffer of AS_RH + 1 + AS_MAXKEY bytes. */
static const uint8_t *as_key_view(altsql *db, uint32_t addr, uint8_t *b, uint32_t *klen) {
    const uint8_t *p = as_view(db, addr, AS_RH + 1, b);
    if (!p) return NULL;
    *klen = p[AS_RH];
    if (*klen > AS_MAXKEY) *klen = AS_MAXKEY;
    p = as_view(db, addr, AS_RH + 1 + *klen, b);
    return p ? p + AS_RH + 1 : NULL;
}

/* Byte order of the keys of two records; *err is set on a read failure. */
static int as_key_cmp(altsql *db, uint32_t a, uint32_t b, int *err) {
    uint8_t ba[AS_RH + 1 + AS_MAXKEY], bb[AS_RH + 1 + AS_MAXKEY];
    uint32_t la, lb;
    const uint8_t *ka = as_key_view(db, a, ba, &la), *kb;
    int c;
    if (!ka) { *err = 1; return 0; }
    kb = as_key_view(db, b, bb, &lb);      /* separate buffer: ka stays valid */
    if (!kb) { *err = 1; return 0; }
    c = memcmp(ka, kb, la < lb ? la : lb);
    return c ? c : (la > lb) - (la < lb);
}

/* In-place heapsort of record addresses by key: no extra memory. */
static void as_sift(altsql *db, uint32_t *a, size_t i, size_t n, int *err) {
    for (;;) {
        size_t c = 2 * i + 1;
        uint32_t t;
        if (c >= n) return;
        if (c + 1 < n && as_key_cmp(db, a[c + 1], a[c], err) > 0) c++;
        if (as_key_cmp(db, a[c], a[i], err) <= 0) return;
        t = a[i]; a[i] = a[c]; a[c] = t;
        i = c;
    }
}

static void as_sort_keys(altsql *db, uint32_t *a, size_t n, int *err) {
    size_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) as_sift(db, a, i, n, err);
    for (i = n - 1; i > 0; i--) {
        uint32_t t = a[0]; a[0] = a[i]; a[i] = t;
        as_sift(db, a, 0, i, err);
    }
}

int altsql_export(altsql *db, altsql_write_cb out, void *ctx) {
    as_wr w;
    as_iter it;
    as_rec r;
    uint32_t *keys = NULL;
    size_t nkeys = 0, maxkeys, i;
    int rc = 0, err = 0;
    if (!db || !out) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
    w.out = out;
    w.ctx = ctx;
    w.stopped = 0;
    w.n = 0;
    as_wr_str(&w, "# altsql text export v1\n");

#if ALTSQL_ENABLE_TS
    {   /* series, by id */
        uint32_t last = 0;
        for (;;) {
            as_series *S = NULL;
            char schema[AS_MAXSCHEMA + 1];
            for (i = 0; i < db->max_series; i++) {
                as_series *e = &db->series[i];
                if (e->used && e->id > last && (!S || e->id < S->id)) S = e;
            }
            if (!S) break;
            last = S->id;
            rc = as_series_schema(db, S, schema, sizeof schema);
            if (rc) return rc;
            as_wr_str(&w, "series ");
            as_wr_str(&w, S->name);
            as_wr_str(&w, " ");
            as_wr_quoted(&w, (const uint8_t *)schema, strlen(schema));
            as_wr_str(&w, "\n");
        }
    }
#endif

    /* keys, in byte order: the addresses are gathered in working memory */
    maxkeys = db->work ? db->work_size / sizeof(uint32_t) : 0;
    keys = (uint32_t *)db->work;
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        uint32_t klen;
        if (r.type != AS_R_PUT || r.len < 1) continue;
        klen = r.p[0];
        if (!klen || klen > AS_MAXKEY || 1u + klen > r.len || r.p[1] == 0x01) continue;
        if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
        if (nkeys == maxkeys) return as_err(db, ALTSQL_NOMEM, "export needs 4 bytes of working memory per key");
        keys[nkeys++] = r.addr;
    }
    if (rc < 0) return rc;
    as_sort_keys(db, keys, nkeys, &err);
    if (err) return as_err(db, ALTSQL_IOERR, "flash read failed");
    for (i = 0; i < nkeys && !w.stopped; i++) {
        uint8_t hb[AS_RH];
        const uint8_t *h = as_view(db, keys[i], AS_RH, hb), *p;
        uint32_t len, klen;
        if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
        len = as_get16(h + 2);
        p = as_view(db, keys[i], AS_RH + len, db->rbuf);
        if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
        p += AS_RH;
        klen = p[0];
        as_wr_str(&w, "kv ");
        as_wr_quoted(&w, p + 1, klen);
        as_wr_str(&w, " ");
        as_wr_quoted(&w, p + 1 + klen, len - 1 - klen);
        as_wr_str(&w, "\n");
    }

#if ALTSQL_ENABLE_TS
    /* rows, oldest first */
    as_iter_start(&it, 0, AS_ALL);
    while (!w.stopped && (rc = as_next(db, &it, &r)) == 1) {
        altsql_value v[AS_MAXCOLS];
        as_series *S;
        uint32_t c;
        if (r.type != AS_R_ROW || r.len < 2) continue;
        S = as_series_by_id(db, as_get16(r.p));
        if (!S || as_row_decode(S, r.p, r.len, v)) continue;
        as_wr_str(&w, "row ");
        as_wr_str(&w, S->name);
        for (c = 0; c < S->ncols; c++) {
            char nb[40];
            as_wr_put(&w, " ", 1);
            if (v[c].type == ALTSQL_TEXT) {
                as_wr_quoted(&w, (const uint8_t *)v[c].u.s, (size_t)v[c].len);
            } else if (v[c].type == ALTSQL_REAL) {
                as_wr_put(&w, nb, as_fmt_real(v[c].u.r, nb, sizeof nb, S->types[c] == AS_T_FLOAT ? 'f' : 'd'));
            } else {
                as_wr_put(&w, nb, as_fmt_value(&v[c], nb, sizeof nb));
            }
        }
        as_wr_str(&w, "\n");
    }
    if (rc < 0) return rc;
#endif

    as_wr_flush(&w);
    return w.stopped ? as_err(db, ALTSQL_ERROR, "export stopped by the writer") : ALTSQL_OK;
}

/* ---- Import ------------------------------------------------------------------ */
typedef struct as_rd {
    const char *p, *end;
    char *buf;                 /* unescaped strings go here, reset per line */
    size_t cap, used;
} as_rd;

static void as_rd_ws(as_rd *r) { while (r->p < r->end && (*r->p == ' ' || *r->p == '\t')) r->p++; }

/* A bare word (name or number) up to the next space. */
static int as_rd_word(as_rd *r, const char **s, size_t *n) {
    as_rd_ws(r);
    *s = r->p;
    while (r->p < r->end && *r->p != ' ' && *r->p != '\t') r->p++;
    *n = (size_t)(r->p - *s);
    return *n > 0;
}

static int as_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A quoted string, unescaped into the line buffer. 0 = ok, 1 = bad syntax, 2 = no room. */
static int as_rd_quoted(as_rd *r, const char **s, size_t *n) {
    char *d = r->buf + r->used;
    size_t k = 0, room = r->cap - r->used;
    as_rd_ws(r);
    if (r->p >= r->end || *r->p != '"') return 1;
    r->p++;
    for (;;) {
        int c;
        if (r->p >= r->end) return 1;
        c = (unsigned char)*r->p++;
        if (c == '"') break;
        if (c == '\\') {
            if (r->p >= r->end) return 1;
            c = (unsigned char)*r->p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'x') {
                int hi, lo;
                if (r->end - r->p < 2) return 1;
                hi = as_hexval((unsigned char)r->p[0]);
                lo = as_hexval((unsigned char)r->p[1]);
                if (hi < 0 || lo < 0) return 1;
                c = hi * 16 + lo;
                r->p += 2;
            } else if (c != '"' && c != '\\') {
                return 1;
            }
        }
        if (k == room) return 2;
        d[k++] = (char)c;
    }
    *s = d;
    *n = k;
    r->used += k;
    return 0;
}

static int as_imp_err(altsql *db, int rc, unsigned long line, const char *msg) {
    char m[96];
    snprintf(m, sizeof m, "import line %lu: %s", line, msg);
    return as_err(db, rc, m);
}

int altsql_import(altsql *db, const char *text, size_t len) {
    const char *p = text, *end = text + len;
    unsigned long line = 0;
    int rc;
    if (!db || (!text && len)) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    if (!db->work || db->work_size < 512) return as_err(db, ALTSQL_NOMEM, "import needs working memory (mem_size)");
    while (p < end) {
        const char *eol = p, *w;
        size_t wn;
        as_rd r;
        int q;
        while (eol < end && *eol != '\n') eol++;
        line++;
        r.p = p;
        r.end = (eol > p && eol[-1] == '\r') ? eol - 1 : eol;
        r.buf = (char *)db->work;
        r.cap = db->work_size;
        r.used = 0;
        p = eol < end ? eol + 1 : end;
        as_rd_ws(&r);
        if (r.p >= r.end || *r.p == '#') continue;          /* blank line or comment */
        if (!as_rd_word(&r, &w, &wn)) continue;

        if (as_ieq(w, wn, "kv", 2)) {
            const char *k, *v;
            size_t kn, vn;
            char key[AS_MAXKEY + 1];
            if ((q = as_rd_quoted(&r, &k, &kn)) != 0 || (q = as_rd_quoted(&r, &v, &vn)) != 0)
                return as_imp_err(db, q == 2 ? ALTSQL_NOMEM : ALTSQL_SYNTAX, line, q == 2 ? "line too long for working memory" : "expected kv \"key\" \"value\"");
            if (kn == 0 || kn > AS_MAXKEY || memchr(k, 0, kn))
                return as_imp_err(db, ALTSQL_SYNTAX, line, "key must be 1 to 200 bytes, without zero bytes");
            memcpy(key, k, kn);
            key[kn] = 0;
            rc = altsql_put(db, key, v, vn);
            if (rc) return rc;
#if ALTSQL_ENABLE_TS
        } else if (as_ieq(w, wn, "series", 6)) {
            const char *nm, *sc;
            size_t nn, sn;
            char name[AS_NAMELEN], schema[AS_MAXSCHEMA + 1];
            if (!as_rd_word(&r, &nm, &nn) || nn >= AS_NAMELEN || (q = as_rd_quoted(&r, &sc, &sn)) != 0 || sn > AS_MAXSCHEMA)
                return as_imp_err(db, ALTSQL_SYNTAX, line, "expected series name \"schema\"");
            if (!as_series_name_ok((const uint8_t *)nm, (uint32_t)nn) || memchr(sc, 0, sn))
                return as_imp_err(db, ALTSQL_SYNTAX, line, "bad series name or schema");
            memcpy(name, nm, nn);
            name[nn] = 0;
            memcpy(schema, sc, sn);
            schema[sn] = 0;
            rc = altsql_ts_create(db, name, schema);
            if (rc == ALTSQL_EXISTS) {                         /* fine if it is the same */
                char have[AS_MAXSCHEMA + 1];
                const as_series *S = as_series_find(db, name, nn);
                if (!S) return as_imp_err(db, ALTSQL_SCHEMA, line, "series name not found");
                rc = as_series_schema(db, S, have, sizeof have);
                if (rc) return rc;
                if (strcmp(have, schema) != 0)
                    return as_imp_err(db, ALTSQL_SCHEMA, line, "series exists with a different schema");
            } else if (rc) {
                return rc;
            }
        } else if (as_ieq(w, wn, "row", 3)) {
            altsql_value v[AS_MAXCOLS];
            const char *nm;
            size_t nn;
            as_series *S;
            uint32_t c;
            if (!as_rd_word(&r, &nm, &nn) || !(S = as_series_find(db, nm, nn)))
                return as_imp_err(db, ALTSQL_SCHEMA, line, "row for an unknown series");
            for (c = 0; c < S->ncols; c++) {
                v[c].len = 0;
                if (S->types[c] == AS_T_TEXT) {
                    const char *s;
                    size_t sn;
                    if ((q = as_rd_quoted(&r, &s, &sn)) != 0)
                        return as_imp_err(db, q == 2 ? ALTSQL_NOMEM : ALTSQL_SYNTAX, line, q == 2 ? "line too long for working memory" : "expected a quoted text value");
                    v[c].type = ALTSQL_TEXT;
                    v[c].u.s = s;
                    v[c].len = sn > 255 ? 256 : (int)sn;
                } else {
                    char nb[64], *e;
                    const char *s;
                    size_t sn;
                    if (!as_rd_word(&r, &s, &sn) || sn >= sizeof nb)
                        return as_imp_err(db, ALTSQL_SYNTAX, line, "expected a number");
                    memcpy(nb, s, sn);
                    nb[sn] = 0;
                    errno = 0;
                    if (S->types[c] == AS_T_FLOAT || S->types[c] == AS_T_REAL) {
                        v[c].type = ALTSQL_REAL;
                        v[c].u.r = strtod(nb, &e);
                    } else {
                        v[c].type = ALTSQL_INTEGER;
                        v[c].u.i = strtoll(nb, &e, 10);
                        if (errno == ERANGE) e = nb;
                    }
                    if (e != nb + sn) return as_imp_err(db, ALTSQL_SYNTAX, line, "bad number");
                }
            }
            as_rd_ws(&r);
            if (r.p != r.end) return as_imp_err(db, ALTSQL_SCHEMA, line, "too many values for this series");
            rc = altsql_ts_append(db, S->name, v, (int)S->ncols);
            if (rc) return rc;
#endif
        } else {
            return as_imp_err(db, ALTSQL_SYNTAX, line, "expected series, kv or row");
        }
    }
    return ALTSQL_OK;
}

#endif /* ALTSQL_ENABLE_TEXT */
