/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* gzip reading for AltSql Restore: CRC-32, a streaming inflater (RFC 1951) and the gzip
 * wrapper (RFC 1952), written for this program. Every member's CRC-32 and length are
 * checked, so a damaged or cut-off .gz file is an error, never a short read.
 *
 * stream_file() hands a file's contents to a callback in pieces: unpacked when the file is
 * gzip, as they are otherwise. The callback returns 0 to go on, 1 to stop early (not an
 * error), or -1 for an error it has set with seterr(). */

static uint32_t crc_table[256];
static void crc_init(void) {
    uint32_t i, j, c;
    if (crc_table[1]) return;
    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32_add(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef int (*chunk_cb)(void *ctx, const uint8_t *p, size_t n);

typedef struct inflater {
    FILE    *f;
    const char *name;
    uint32_t bitbuf;
    int      bitcnt;
    uint64_t total;            /* bytes out in this member              */
    uint32_t crc;
    uint8_t  win[32768];
    uint8_t  out[65536];
    size_t   on;
    chunk_cb cb;
    void    *ctx;
    int      stop;             /* the callback asked to stop             */
    int      cberr;            /* the callback failed                    */
} inflater;

static int inf_byte(inflater *s) {
    int c = getc(s->f);
    if (c == EOF) return seterr("%s: the gzip data ends early (truncated)", s->name);
    return c;
}
static int inf_bits(inflater *s, int need, uint32_t *v) {
    while (s->bitcnt < need) {
        int c = inf_byte(s);
        if (c < 0) return -1;
        s->bitbuf |= (uint32_t)c << s->bitcnt;
        s->bitcnt += 8;
    }
    *v = s->bitbuf & (need ? ((1u << need) - 1) : 0);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return 0;
}
static int inf_flush(inflater *s) {
    if (s->on && !s->stop) {
        int r;
        s->crc = crc32_add(s->crc, s->out, s->on);
        r = s->cb(s->ctx, s->out, s->on);
        s->on = 0;
        if (r < 0) { s->cberr = 1; return -1; }
        if (r > 0) { s->stop = 1; return -1; }   /* not an error: stream_file sees stop */
        return 0;
    }
    s->on = 0;
    return 0;
}
static int inf_put(inflater *s, uint8_t b) {
    s->win[s->total & 32767] = b;
    s->total++;
    s->out[s->on++] = b;
    if (s->on == sizeof s->out) return inf_flush(s);
    return 0;
}

typedef struct huff { short count[16]; short symbol[320]; } huff;

/* Builds a canonical Huffman decoder from code lengths. Returns <0 when over-subscribed,
 * 0 when complete, >0 when incomplete. */
static int huff_build(huff *h, const short *length, int n) {
    short offs[16];
    int len, sym, left;
    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static int huff_decode(inflater *s, const huff *h) {
    int code = 0, first = 0, index = 0, len, count;
    uint32_t b;
    for (len = 1; len < 16; len++) {
        if (inf_bits(s, 1, &b)) return -1;
        code |= (int)b;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return seterr("%s: damaged gzip data (bad Huffman code)", s->name);
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inf_codes(inflater *s, const huff *lc, const huff *dc) {
    int sym;
    for (;;) {
        uint32_t e;
        if ((sym = huff_decode(s, lc)) < 0) return -1;
        if (sym < 256) {
            if (inf_put(s, (uint8_t)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dist;
            sym -= 257;
            if (sym >= 29) return seterr("%s: damaged gzip data (bad length code)", s->name);
            if (inf_bits(s, lext[sym], &e)) return -1;
            len = lbase[sym] + (int)e;
            if ((sym = huff_decode(s, dc)) < 0) return -1;
            if (sym >= 30) return seterr("%s: damaged gzip data (bad distance code)", s->name);
            if (inf_bits(s, dext[sym], &e)) return -1;
            dist = dbase[sym] + (int)e;
            if ((uint64_t)dist > s->total) return seterr("%s: damaged gzip data (distance too far back)", s->name);
            while (len--) if (inf_put(s, s->win[(s->total - (uint64_t)dist) & 32767])) return -1;
        }
    }
}

static int inf_stored(inflater *s) {
    int a, b, c, d;
    unsigned len, nlen;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if ((a = inf_byte(s)) < 0 || (b = inf_byte(s)) < 0 || (c = inf_byte(s)) < 0 || (d = inf_byte(s)) < 0) return -1;
    len = (unsigned)a | (unsigned)b << 8;
    nlen = (unsigned)c | (unsigned)d << 8;
    if (len != (~nlen & 0xFFFF)) return seterr("%s: damaged gzip data (stored block length)", s->name);
    while (len--) {
        int x = inf_byte(s);
        if (x < 0 || inf_put(s, (uint8_t)x)) return -1;
    }
    return 0;
}

static int inf_fixed(inflater *s) {
    static huff lc, dc;
    static int built;
    if (!built) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dc, l, 30);
        built = 1;
    }
    return inf_codes(s, &lc, &dc);
}

static int inf_dynamic(inflater *s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    huff lc, dc;
    uint32_t v;
    int nlen, ndist, ncode, i, err;
    if (inf_bits(s, 5, &v)) return -1;
    nlen = (int)v + 257;
    if (inf_bits(s, 5, &v)) return -1;
    ndist = (int)v + 1;
    if (inf_bits(s, 4, &v)) return -1;
    ncode = (int)v + 4;
    if (nlen > 286 || ndist > 30) return seterr("%s: damaged gzip data (bad code counts)", s->name);
    for (i = 0; i < ncode; i++) { if (inf_bits(s, 3, &v)) return -1; lengths[order[i]] = (short)v; }
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (huff_build(&lc, lengths, 19) != 0) return seterr("%s: damaged gzip data (bad code lengths)", s->name);
    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(s, &lc), len = 0, rep;
        if (sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (short)sym; continue; }
        if (sym == 16) {
            if (i == 0) return seterr("%s: damaged gzip data (repeat with no length)", s->name);
            len = lengths[i - 1];
            if (inf_bits(s, 2, &v)) return -1;
            rep = 3 + (int)v;
        } else if (sym == 17) {
            if (inf_bits(s, 3, &v)) return -1;
            rep = 3 + (int)v;
        } else {
            if (inf_bits(s, 7, &v)) return -1;
            rep = 11 + (int)v;
        }
        if (i + rep > nlen + ndist) return seterr("%s: damaged gzip data (too many lengths)", s->name);
        while (rep--) lengths[i++] = (short)len;
    }
    if (lengths[256] == 0) return seterr("%s: damaged gzip data (no end-of-block code)", s->name);
    err = huff_build(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return seterr("%s: damaged gzip data (bad literal codes)", s->name);
    err = huff_build(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return seterr("%s: damaged gzip data (bad distance codes)", s->name);
    return inf_codes(s, &lc, &dc);
}

/* One gzip member, its header already checked up to the flags byte. */
static int gz_member(inflater *s) {
    int flg, i, c;
    uint32_t last, type, crc, isize;
    uint8_t t[8];
    if ((flg = inf_byte(s)) < 0) return -1;
    for (i = 0; i < 6; i++) if (inf_byte(s) < 0) return -1;          /* mtime, xfl, os */
    if (flg & 4) {                                                   /* FEXTRA */
        int a = inf_byte(s), b = inf_byte(s), n;
        if (a < 0 || b < 0) return -1;
        for (n = a | b << 8; n > 0; n--) if (inf_byte(s) < 0) return -1;
    }
    if (flg & 8) do { if ((c = inf_byte(s)) < 0) return -1; } while (c);     /* FNAME */
    if (flg & 16) do { if ((c = inf_byte(s)) < 0) return -1; } while (c);    /* FCOMMENT */
    if (flg & 2) { if (inf_byte(s) < 0 || inf_byte(s) < 0) return -1; }      /* FHCRC */
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->total = 0;
    s->crc = 0;
    do {
        if (inf_bits(s, 1, &last) || inf_bits(s, 2, &type)) return -1;
        if (type == 0) { if (inf_stored(s)) return -1; }
        else if (type == 1) { if (inf_fixed(s)) return -1; }
        else if (type == 2) { if (inf_dynamic(s)) return -1; }
        else return seterr("%s: damaged gzip data (bad block type)", s->name);
    } while (!last);
    if (inf_flush(s)) return -1;
    for (i = 0; i < 8; i++) { if ((c = inf_byte(s)) < 0) return -1; t[i] = (uint8_t)c; }
    crc = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
    isize = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
    if (crc != s->crc) return seterr("%s: damaged gzip data (CRC-32 mismatch)", s->name);
    if (isize != (uint32_t)s->total) return seterr("%s: damaged gzip data (length mismatch)", s->name);
    return 0;
}

static int stream_file(const char *path, chunk_cb cb, void *ctx, int *was_gz) {
    FILE *f = fopen(path, "rb");
    int a, b;
    if (was_gz) *was_gz = 0;
    if (!f) return seterr("cannot read %s: %s", path, strerror(errno));
    a = getc(f);
    b = getc(f);
    if (a == 0x1f && b == 0x8b) {
        inflater *s = (inflater *)xmalloc(sizeof *s);
        int rc = 0;
        crc_init();
        memset(s, 0, sizeof *s);
        s->f = f; s->name = base_name(path); s->cb = cb; s->ctx = ctx;
        if (was_gz) *was_gz = 1;
        for (;;) {
            int m;
            if (getc(f) != 8) { rc = seterr("%s: unknown gzip compression method", s->name); break; }
            if (gz_member(s)) { rc = s->stop ? 0 : -1; break; }
            /* another member, or zero padding, or the end */
            if ((m = getc(f)) == EOF) break;
            if (m == 0x1f) {
                if (getc(f) != 0x8b) { rc = seterr("%s: data after the gzip stream", s->name); break; }
                continue;
            }
            if (m == 0) {
                while ((m = getc(f)) == 0) {}
                if (m == EOF) break;
            }
            rc = seterr("%s: data after the gzip stream", s->name);
            break;
        }
        if (rc == 0 && ferror(f)) rc = seterr("cannot read %s: %s", path, strerror(errno));
        free(s);
        fclose(f);
        return rc;
    } else {
        uint8_t *tmp = (uint8_t *)xmalloc(65536);
        size_t k;
        int rc = 0;
        rewind(f);
        while ((k = fread(tmp, 1, 65536, f)) > 0) {
            int r = cb(ctx, tmp, k);
            if (r < 0) { rc = -1; break; }
            if (r > 0) break;
        }
        if (rc == 0 && ferror(f)) rc = seterr("cannot read %s: %s", path, strerror(errno));
        free(tmp);
        fclose(f);
        return rc;
    }
}

/* ---- copying a backup into the sandbox, unpacked; keeps the first and last bytes ---- */
typedef struct copier {
    FILE    *out;
    const char *outpath;
    uint64_t n;
    uint8_t  head[4096];
    size_t   hn;
    uint8_t  tail[8192];       /* ring of the last bytes */
    uint64_t tn;
} copier;
static int copier_cb(void *ctx, const uint8_t *p, size_t n) {
    copier *c = (copier *)ctx;
    size_t i;
    if (c->out && fwrite(p, 1, n, c->out) != n) return seterr("cannot write %s: %s", c->outpath, strerror(errno));
    for (i = 0; i < n && c->hn < sizeof c->head; i++) c->head[c->hn++] = p[i];
    for (i = 0; i < n; i++) c->tail[(c->tn + i) % sizeof c->tail] = p[i];
    c->tn += n;
    c->n += n;
    return 0;
}
/* The last up to 8 KB, in order, NUL-terminated, into out (8193 bytes). */
static size_t copier_tail(const copier *c, char *out) {
    size_t k = c->tn < sizeof c->tail ? (size_t)c->tn : sizeof c->tail, i;
    uint64_t start = c->tn - k;
    for (i = 0; i < k; i++) out[i] = (char)c->tail[(start + i) % sizeof c->tail];
    out[k] = 0;
    for (i = 0; i < k; i++) if (!out[i]) out[i] = ' ';
    return k;
}

/* The first bytes of a file, unpacked when it is gzip. */
typedef struct peeker { uint8_t b[4096]; size_t n; } peeker;
static int peek_cb(void *ctx, const uint8_t *p, size_t n) {
    peeker *k = (peeker *)ctx;
    size_t m = sizeof k->b - k->n;
    if (m > n) m = n;
    memcpy(k->b + k->n, p, m);
    k->n += m;
    return k->n == sizeof k->b ? 1 : 0;
}
