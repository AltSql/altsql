/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Time-series layer
 *
 * A series has a schema declared once ("time:time,machine:int,temp:float")
 * and stored as a key-value record, so it syncs like any other record.
 * Rows are packed little-endian in schema order and appended to the log.
 * ====================================================================== */
#if ALTSQL_ENABLE_TS

typedef void (*as_colname_cb)(void *ctx, int idx, const char *name, size_t n);

static int as_type_of(const char *s, size_t n) {
    if (as_ieq(s, n, "time", 4)) return AS_T_TIME;
    if (as_ieq(s, n, "int", 3)) return AS_T_INT;
    if (as_ieq(s, n, "long", 4)) return AS_T_LONG;
    if (as_ieq(s, n, "float", 5)) return AS_T_FLOAT;
    if (as_ieq(s, n, "real", 4)) return AS_T_REAL;
    if (as_ieq(s, n, "text", 4)) return AS_T_TEXT;
    return 0;
}

AS_FN const char *as_type_name(int t) {
    static const char *const names[] = { "?", "time", "int", "long", "float", "real", "text" };
    return (t >= AS_T_TIME && t <= AS_T_TEXT) ? names[t] : names[0];
}

/* Parses "name:type,..." into types; calls cb for each column name.
 * Returns the number of columns, or -1 on a syntax error. */
static int as_schema_parse(const char *s, size_t n, uint8_t *types, as_colname_cb cb, void *ctx) {
    size_t i = 0;
    int col = 0;
    while (i < n) {
        size_t a, an, b, bn;
        int t;
        while (i < n && s[i] == ' ') i++;
        a = i;
        if (i >= n || !as_is_ident_start((unsigned char)s[i])) return -1;
        while (i < n && as_is_ident_char((unsigned char)s[i])) i++;
        an = i - a;
        while (i < n && s[i] == ' ') i++;
        if (i >= n || s[i] != ':') return -1;
        i++;
        while (i < n && s[i] == ' ') i++;
        b = i;
        while (i < n && as_is_ident_char((unsigned char)s[i])) i++;
        bn = i - b;
        t = as_type_of(s + b, bn);
        if (!t || an > 31 || col >= AS_MAXCOLS) return -1;
        if ((col == 0) != (t == AS_T_TIME)) return -1;   /* first column, and only it, is time */
        types[col] = (uint8_t)t;
        if (cb) cb(ctx, col, s + a, an);
        col++;
        while (i < n && s[i] == ' ') i++;
        if (i < n) {
            if (s[i] != ',') return -1;
            i++;
        }
    }
    return col > 0 ? col : -1;
}

static as_series *as_series_find(altsql *db, const char *name, size_t n) {
    uint32_t i;
    for (i = 0; i < db->max_series; i++) {
        as_series *e = &db->series[i];
        if (e->used && as_ieq(e->name, strlen(e->name), name, n)) return e;
    }
    return NULL;
}

AS_FN as_series *as_series_by_id(altsql *db, uint32_t id) {
    uint32_t i;
    for (i = 0; i < db->max_series; i++)
        if (db->series[i].used && db->series[i].id == id) return &db->series[i];
    return NULL;
}

/* A series name as altsql_ts_create accepts it: 1 to 23 letters, digits or _. */
static int as_series_name_ok(const uint8_t *name, uint32_t nlen) {
    uint32_t i;
    if (nlen == 0 || nlen >= AS_NAMELEN || !as_is_ident_start(name[0])) return 0;
    for (i = 1; i < nlen; i++) if (!as_is_ident_char(name[i])) return 0;
    return 1;
}

/* Registers a series from its definition record. Records that arrive by
 * sync, import or from damaged flash are checked here too: a definition
 * with a bad name, a bad schema or the id of another series is ignored. */
static void as_series_note(altsql *db, const uint8_t *name, uint32_t nlen, const uint8_t *val, uint32_t vlen) {
    uint8_t types[AS_MAXCOLS];
    int nc;
    uint32_t i, id;
    as_series *e;
    if (!as_series_name_ok(name, nlen) || vlen < 2) return;
    nc = as_schema_parse((const char *)val + 2, vlen - 2, types, NULL, NULL);
    if (nc < 1) return;
    id = as_get16(val);
    if (id == 0) return;
    e = as_series_find(db, (const char *)name, nlen);
    for (i = 0; i < db->max_series; i++)
        if (db->series[i].used && db->series[i].id == id && &db->series[i] != e) return;
    for (i = 0; !e && i < db->max_series; i++) if (!db->series[i].used) e = &db->series[i];
    if (!e) return;                       /* series table full: raise max_series */
    memcpy(e->name, name, nlen);
    e->name[nlen] = 0;
    e->id = (uint16_t)id;
    e->ncols = (uint8_t)nc;
    memcpy(e->types, types, (size_t)nc);
    e->used = 1;
}

/* Copies the schema text of a series into buf (NUL-terminated). */
static int as_series_schema(altsql *db, const as_series *S, char *buf, size_t cap) {
    uint8_t key[2 + AS_NAMELEN], hb[AS_RH];
    size_t nlen = strlen(S->name);
    uint32_t addr, len, slen, klen = (uint32_t)(2 + nlen);
    uint8_t type;
    const uint8_t *h;
    int rc;
    key[0] = 0x01;
    key[1] = 'S';
    memcpy(key + 2, S->name, nlen);
    rc = as_kv_latest(db, key, klen, &addr, &type);
    if (rc) return rc;
    if (!addr || type != AS_R_PUT) return as_err(db, ALTSQL_CORRUPT, "series schema missing");
    h = as_view(db, addr, AS_RH, hb);
    if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
    len = as_get16(h + 2);
    if (len < 1 + klen + 2) return as_err(db, ALTSQL_CORRUPT, "series schema damaged");
    slen = len - 1 - klen - 2;
    if (slen + 1 > cap) return as_err(db, ALTSQL_TOOBIG, "schema too long");
    if (db->fl.map) {
        const uint8_t *p = db->fl.map(db->fl.ctx, addr + AS_RH + 1 + klen + 2, slen);
        if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
        memcpy(buf, p, slen);
    } else if (slen && db->fl.read(db->fl.ctx, addr + AS_RH + 1 + klen + 2, buf, slen) != 0) {
        return as_err(db, ALTSQL_IOERR, "flash read failed");
    }
    buf[slen] = 0;
    return ALTSQL_OK;
}

int altsql_ts_create(altsql *db, const char *series, const char *schema) {
    uint8_t types[AS_MAXCOLS], key[2 + AS_NAMELEN], val[2 + AS_MAXSCHEMA];
    size_t nlen, slen, i;
    uint32_t id = 0;
    int rc;
    if (!db || !series || !schema) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    nlen = strlen(series);
    slen = strlen(schema);
    if (nlen == 0 || nlen >= AS_NAMELEN || !as_is_ident_start((unsigned char)series[0]))
        return as_err(db, ALTSQL_MISUSE, "series name must be 1 to 23 letters, digits or _");
    for (i = 0; i < nlen; i++)
        if (!as_is_ident_char((unsigned char)series[i]))
            return as_err(db, ALTSQL_MISUSE, "series name must be 1 to 23 letters, digits or _");
    if (as_ieq(series, nlen, "kv", 2)) return as_err(db, ALTSQL_MISUSE, "\"kv\" is reserved for the key-value table");
    if (slen > AS_MAXSCHEMA || as_schema_parse(schema, slen, types, NULL, NULL) < 1)
        return as_err(db, ALTSQL_SYNTAX, "bad schema; expected \"time:time,name:type,...\"");
    if (as_series_find(db, series, nlen)) return as_err(db, ALTSQL_EXISTS, "series already exists");
    for (i = 0; i < db->max_series && db->series[i].used; i++) {}
    if (i == db->max_series) return as_err(db, ALTSQL_NOMEM, "series table full; raise max_series");
    for (i = 0; i < db->max_series; i++)
        if (db->series[i].used && db->series[i].id > id) id = db->series[i].id;
    id++;
    key[0] = 0x01;
    key[1] = 'S';
    memcpy(key + 2, series, nlen);
    as_put16(val, id);
    memcpy(val + 2, schema, slen);
    rc = as_kv_write(db, AS_R_PUT, key, (uint32_t)(2 + nlen), val, 2 + slen);
    if (rc) return rc;
    return as_series_find(db, series, nlen) ? ALTSQL_OK : as_err(db, ALTSQL_ERROR, "series registration failed");
}

static int as_row_size(const as_series *S, const altsql_value *v, uint32_t *len) {
    uint32_t n = 2, i;
    for (i = 0; i < S->ncols; i++) {
        switch (S->types[i]) {
        case AS_T_TIME: case AS_T_LONG: case AS_T_REAL: n += 8; break;
        case AS_T_INT: case AS_T_FLOAT: n += 4; break;
        default:
            if (v[i].len < 0 || v[i].len > 255) return -1;
            n += 1u + (uint32_t)v[i].len;
        }
    }
    *len = n;
    return 0;
}

/* Packs values (already of the right kind) into dst. */
static void as_row_pack(const as_series *S, const altsql_value *v, uint8_t *dst) {
    uint32_t i, off = 2;
    as_put16(dst, S->id);
    for (i = 0; i < S->ncols; i++) {
        switch (S->types[i]) {
        case AS_T_TIME: case AS_T_LONG:
            as_put64(dst + off, (uint64_t)v[i].u.i); off += 8; break;
        case AS_T_INT:
            as_put32(dst + off, (uint32_t)(int32_t)v[i].u.i); off += 4; break;
        case AS_T_FLOAT: {
            float f = (float)v[i].u.r;
            uint32_t b;
            memcpy(&b, &f, 4);
            as_put32(dst + off, b); off += 4; break;
        }
        case AS_T_REAL: {
            uint64_t b;
            memcpy(&b, &v[i].u.r, 8);
            as_put64(dst + off, b); off += 8; break;
        }
        default:
            dst[off++] = (uint8_t)v[i].len;
            if (v[i].len) memcpy(dst + off, v[i].u.s, (size_t)v[i].len);
            off += (uint32_t)v[i].len;
        }
    }
}

static const double as_p10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

/* A float column value as the shortest decimal that gives back the same
 * float, so 21.53 reads back as 21.53 rather than 21.530000686645508. */
static double as_nice_float(float f) {
    double d = (double)f, a = d < 0 ? -d : d;
    int k = 0, p;
    if (!(a >= 1e-7 && a < 1e22)) return d;          /* zero, tiny, huge, NaN: as is */
    if (a >= 1.0) { while (k < 22 && as_p10[k + 1] <= a) k++; }
    else { while (a * as_p10[-k] < 1.0) k--; }        /* 10^k <= a < 10^(k+1) */
    for (p = 6; p <= 9; p++) {
        int n = p - 1 - k;
        double y, r;
        y = n >= 0 ? a * as_p10[n] : a / as_p10[-n];
        y = (double)(int64_t)(y + 0.5);
        r = n >= 0 ? y / as_p10[n] : y * as_p10[-n];
        if ((float)r == (float)a) return d < 0 ? -r : r;
    }
    return d;
}

/* Unpacks a row payload. Text values point into the payload. */
static int as_row_decode(const as_series *S, const uint8_t *p, uint32_t len, altsql_value *v) {
    uint32_t off = 2, i;
    for (i = 0; i < S->ncols; i++) {
        uint8_t t = S->types[i];
        uint32_t w = (t == AS_T_INT || t == AS_T_FLOAT) ? 4u : (t == AS_T_TEXT ? 1u : 8u);
        if (off + w > len) return -1;
        v[i].len = 0;
        switch (t) {
        case AS_T_TIME: case AS_T_LONG:
            v[i].type = ALTSQL_INTEGER; v[i].u.i = (int64_t)as_get64(p + off); break;
        case AS_T_INT:
            v[i].type = ALTSQL_INTEGER; v[i].u.i = (int32_t)as_get32(p + off); break;
        case AS_T_FLOAT: {
            uint32_t b = as_get32(p + off);
            float f;
            memcpy(&f, &b, 4);
            v[i].type = ALTSQL_REAL; v[i].u.r = as_nice_float(f); break;
        }
        case AS_T_REAL: {
            uint64_t b = as_get64(p + off);
            v[i].type = ALTSQL_REAL;
            memcpy(&v[i].u.r, &b, 8); break;
        }
        default:
            w = 1u + p[off];
            if (off + w > len) return -1;
            v[i].type = ALTSQL_TEXT; v[i].u.s = (const char *)p + off + 1; v[i].len = p[off];
        }
        off += w;
    }
    return 0;
}

/* Converts user values to the column types. Returns 0 or an error code. */
static int as_row_coerce(altsql *db, const as_series *S, const altsql_value *in, altsql_value *out) {
    uint32_t i;
    for (i = 0; i < S->ncols; i++) {
        uint8_t t = S->types[i];
        out[i] = in[i];
        if (in[i].type == ALTSQL_NULL) return as_err(db, ALTSQL_SCHEMA, "NULL values are not supported in time-series rows");
        if (t == AS_T_TEXT) {
            if (in[i].type != ALTSQL_TEXT) return as_err(db, ALTSQL_SCHEMA, "text column needs a text value");
            if (in[i].len > 255) return as_err(db, ALTSQL_TOOBIG, "text value longer than 255 bytes");
        } else if (in[i].type == ALTSQL_TEXT) {
            return as_err(db, ALTSQL_SCHEMA, "numeric column needs a number");
        } else if (t == AS_T_FLOAT || t == AS_T_REAL) {
            out[i].type = ALTSQL_REAL;
            out[i].u.r = in[i].type == ALTSQL_INTEGER ? (double)in[i].u.i : in[i].u.r;
        } else {
            if (in[i].type == ALTSQL_REAL && !(in[i].u.r > -9.2e18 && in[i].u.r < 9.2e18))
                return as_err(db, ALTSQL_SCHEMA, "value out of range for an integer column");
            out[i].type = ALTSQL_INTEGER;
            out[i].u.i = in[i].type == ALTSQL_REAL ? (int64_t)in[i].u.r : in[i].u.i;
            if (t == AS_T_INT && (out[i].u.i < INT32_MIN || out[i].u.i > INT32_MAX))
                return as_err(db, ALTSQL_SCHEMA, "value out of range for an int column (32-bit)");
        }
    }
    return ALTSQL_OK;
}

static int as_row_write(altsql *db, const as_series *S, const altsql_value *v) {
    uint32_t len, addr, seq;
    as_rec r;
    int rc;
    if (as_row_size(S, v, &len)) return as_err(db, ALTSQL_TOOBIG, "text value longer than 255 bytes");
    rc = as_reserve(db, len, AS_RES_GC);
    if (rc) return rc;
    as_row_pack(S, v, db->rbuf + AS_RH);
    seq = db->next_rseq;
    rc = as_commit(db, AS_R_ROW, seq, len, &addr);
    if (rc) return rc;
    as_rec_fresh(db, &r, AS_R_ROW, seq, len, addr);
    return as_note(db, &r);
}

static as_series *as_series_for_write(altsql *db, const char *series) {
    as_series *S;
    if (db->replica) { as_err(db, ALTSQL_MISUSE, "a replica only changes through sync"); return NULL; }
    S = as_series_find(db, series, strlen(series));
    if (!S) as_err2(db, ALTSQL_NOTFOUND, "no such series: ", series, strlen(series));
    return S;
}

int altsql_ts_append(altsql *db, const char *series, const altsql_value *vals, int n) {
    altsql_value v[AS_MAXCOLS];
    as_series *S;
    int rc;
    if (!db || !series || !vals) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    S = as_series_for_write(db, series);
    if (!S) return db->replica ? ALTSQL_MISUSE : ALTSQL_NOTFOUND;
    if (n != S->ncols) return as_err(db, ALTSQL_SCHEMA, "wrong number of values for this series");
    rc = as_row_coerce(db, S, vals, v);
    if (rc) return rc;
    return as_row_write(db, S, v);
}

int altsql_append(altsql *db, const char *series, ...) {
    altsql_value v[AS_MAXCOLS];
    as_series *S;
    va_list ap;
    uint32_t i;
    int rc;
    if (!db || !series) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    S = as_series_for_write(db, series);
    if (!S) return db->replica ? ALTSQL_MISUSE : ALTSQL_NOTFOUND;
    va_start(ap, series);
    for (i = 0; i < S->ncols; i++) {
        v[i].len = 0;
        switch (S->types[i]) {
        case AS_T_TIME: case AS_T_LONG: v[i].type = ALTSQL_INTEGER; v[i].u.i = va_arg(ap, int64_t); break;
        case AS_T_INT: v[i].type = ALTSQL_INTEGER; v[i].u.i = va_arg(ap, int); break;
        case AS_T_FLOAT: case AS_T_REAL: v[i].type = ALTSQL_REAL; v[i].u.r = va_arg(ap, double); break;
        default: {
            const char *s = va_arg(ap, const char *);
            size_t n = s ? strlen(s) : 0;
            v[i].type = ALTSQL_TEXT;
            v[i].u.s = s ? s : "";
            v[i].len = n > 255 ? 256 : (int)n;
        }
        }
    }
    va_end(ap);
    return as_row_write(db, S, v);
}

int altsql_ts_scan(altsql *db, const char *series, int64_t from, int64_t to, altsql_row_cb cb, void *ctx) {
    altsql_value v[AS_MAXCOLS];
    as_series *S;
    as_iter it;
    as_rec r;
    uint32_t k;
    int rc = 0;
    if (!db || !series || !cb) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    S = as_series_find(db, series, strlen(series));
    if (!S) return as_err2(db, ALTSQL_NOTFOUND, "no such series: ", series, strlen(series));
    for (k = 0; k < db->used; k++) {
        if (db->sec[as_run_sector(db, k)].max_time < from) continue;
        as_iter_start(&it, k, k);
        while ((rc = as_next(db, &it, &r)) == 1) {
            if (r.type != AS_R_ROW || r.len < 2 || as_get16(r.p) != S->id) continue;
            if (as_row_decode(S, r.p, r.len, v)) continue;
            if (v[0].u.i < from || v[0].u.i > to) continue;
            if (cb(ctx, S->ncols, v, NULL)) return ALTSQL_OK;
        }
        if (rc < 0) return rc;
    }
    return ALTSQL_OK;
}

typedef struct { const char *want; size_t n; int idx; } as_colfind;
static void as_colfind_cb(void *ctx, int idx, const char *name, size_t n) {
    as_colfind *f = (as_colfind *)ctx;
    if (f->idx < 0 && as_ieq(name, n, f->want, f->n)) f->idx = idx;
}

int altsql_ts_window(altsql *db, const char *series, const char *column, int64_t since, altsql_stats *out) {
    char schema[AS_MAXSCHEMA + 1];
    uint8_t types[AS_MAXCOLS];
    altsql_value v[AS_MAXCOLS];
    as_colfind f;
    as_series *S;
    as_iter it;
    as_rec r;
    uint32_t k;
    int rc = 0;
    if (!db || !series || !column || !out) return ALTSQL_MISUSE;
    memset(out, 0, sizeof *out);
    if ((rc = as_ready(db)) != 0) return rc;
    S = as_series_find(db, series, strlen(series));
    if (!S) return as_err2(db, ALTSQL_NOTFOUND, "no such series: ", series, strlen(series));
    rc = as_series_schema(db, S, schema, sizeof schema);
    if (rc) return rc;
    f.want = column;
    f.n = strlen(column);
    f.idx = -1;
    as_schema_parse(schema, strlen(schema), types, as_colfind_cb, &f);
    if (f.idx < 0) return as_err2(db, ALTSQL_SCHEMA, "no such column: ", column, f.n);
    if (S->types[f.idx] == AS_T_TEXT) return as_err(db, ALTSQL_SCHEMA, "window needs a numeric column");
    for (k = 0; k < db->used; k++) {
        if (db->sec[as_run_sector(db, k)].max_time < since) continue;
        as_iter_start(&it, k, k);
        while ((rc = as_next(db, &it, &r)) == 1) {
            double x;
            if (r.type != AS_R_ROW || r.len < 2 || as_get16(r.p) != S->id) continue;
            if (as_row_decode(S, r.p, r.len, v) || v[0].u.i < since) continue;
            x = v[f.idx].type == ALTSQL_REAL ? v[f.idx].u.r : (double)v[f.idx].u.i;
            if (!out->count || x < out->min) out->min = x;
            if (!out->count || x > out->max) out->max = x;
            if (!out->count) out->first = v[0].u.i;
            out->last = v[0].u.i;
            out->sum += x;
            out->count++;
        }
        if (rc < 0) return rc;
    }
    out->avg = out->count ? out->sum / out->count : 0.0;
    return ALTSQL_OK;
}

int altsql_series_each(altsql *db, altsql_series_cb cb, void *ctx) {
    char schema[AS_MAXSCHEMA + 1];
    uint32_t i;
    int rc;
    if (!db || !cb) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    for (i = 0; i < db->max_series; i++) {
        if (!db->series[i].used) continue;
        rc = as_series_schema(db, &db->series[i], schema, sizeof schema);
        if (rc) return rc;
        if (cb(ctx, db->series[i].name, schema)) break;
    }
    return ALTSQL_OK;
}

#endif /* ALTSQL_ENABLE_TS */
