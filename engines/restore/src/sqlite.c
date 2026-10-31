/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* SQLite database files for AltSql Restore, read without SQLite: a read-only walker of the
 * file format (https://www.sqlite.org/fileformat2.html), written for this program.
 *
 * It reads the schema table, then walks every table's b-tree (rowid tables and WITHOUT
 * ROWID tables) and every index's, page by page, with their overflow chains, and the
 * free list. For each table it counts the rows and takes a SHA-256 over every row's rowid
 * and record bytes, in key order.
 *
 * What fails the restore: a file shorter than the header's page count (truncated), a size
 * that isn't a whole number of pages, a page of the wrong kind where a b-tree page should
 * be, cell pointers or payloads out of the page, a page reached twice (a loop, or two
 * owners), page numbers past the end, too deep a tree, an overflow chain that ends early,
 * and schema records that don't decode. Then, as SQLite's PRAGMA integrity_check does: rowids
 * that don't rise, an index whose entries don't match its table's rows (a partial index may
 * hold fewer, never more), and a page that no table, index or the free list uses. A file
 * copied in the middle of a write transaction fails these (0.5.1: it passed before). SQLite
 * keeps no page checksums, so a change that leaves the structure valid shows up only in the
 * row counts and checksums. */

typedef struct sqlf {
    FILE    *f;
    const char *name;
    uint32_t psize, usable, npages;
    uint8_t *page;              /* one page buffer per tree level, and one for overflow */
    uint8_t *seen;              /* one bit per page                                     */
    uint64_t pages_read;
} sqlf;

#define SQ_MAXDEPTH 40

static int sq_read(sqlf *s, uint32_t pg, uint8_t *out) {
    if (pg < 1 || pg > s->npages) return seterr("%s: damaged SQLite file (page %u is past the end, %u pages)", s->name, pg, s->npages);
    if (fseeko(s->f, (off_t)(pg - 1) * s->psize, SEEK_SET) || fread(out, 1, s->psize, s->f) != s->psize)
        return seterr("%s: cannot read page %u", s->name, pg);
    s->pages_read++;
    return 0;
}
static int sq_mark(sqlf *s, uint32_t pg) {
    if (pg < 1 || pg > s->npages) return seterr("%s: damaged SQLite file (page %u is past the end, %u pages)", s->name, pg, s->npages);
    if (s->seen[(pg - 1) >> 3] & (1u << ((pg - 1) & 7)))
        return seterr("%s: damaged SQLite file (page %u is used twice)", s->name, pg);
    s->seen[(pg - 1) >> 3] |= (uint8_t)(1u << ((pg - 1) & 7));
    return 0;
}
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A varint; 0 bytes when it runs past end. */
static int sq_varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    uint64_t x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (p + i >= end) return 0;
        x = x << 7 | (p[i] & 0x7F);
        if (!(p[i] & 0x80)) { *v = x; return i + 1; }
    }
    if (p + 8 >= end) return 0;
    *v = x << 8 | p[8];
    return 9;
}

/* A cell's whole payload into out, following its overflow pages. */
static int sq_payload(sqlf *s, const uint8_t *page, const uint8_t *p, uint64_t plen, int index_tree, buf *out, uint32_t pg) {
    uint64_t U = s->usable, X, M, K, local;
    const uint8_t *end = page + s->usable;
    X = index_tree ? ((U - 12) * 64 / 255) - 23 : U - 35;
    M = ((U - 12) * 32 / 255) - 23;
    if (plen <= X) local = plen;
    else {
        K = M + ((plen - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if (plen > (1u << 30)) return seterr("%s: damaged SQLite file (record of %llu bytes on page %u)", s->name, (unsigned long long)plen, pg);
    if (p + local > end || (local < plen && p + local + 4 > end))
        return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, pg);
    out->n = 0;
    buf_add(out, p, (size_t)local);
    if (local < plen) {
        uint32_t next = be32(p + local);
        uint8_t *ov = s->page + (size_t)SQ_MAXDEPTH * s->psize;
        while (out->n < plen) {
            size_t take;
            if (!next) return seterr("%s: damaged SQLite file (an overflow chain from page %u ends early)", s->name, pg);
            if (sq_mark(s, next) || sq_read(s, next, ov)) return -1;
            take = (size_t)(plen - out->n);
            if (take > U - 4) take = (size_t)(U - 4);
            buf_add(out, ov + 4, take);
            next = be32(ov);
        }
    }
    return 0;
}

typedef int (*sq_row_cb)(void *ctx, uint64_t rowid, const uint8_t *rec, size_t n);

/* Walks the b-tree at root: every leaf cell goes to cb, in key order. */
static int sq_walk(sqlf *s, uint32_t root, int depth, int *family, sq_row_cb cb, void *ctx, buf *pay) {
    uint8_t *page = s->page + (size_t)depth * s->psize;
    uint32_t hdr, ncells, i, type;
    int interior, idx;
    if (depth >= SQ_MAXDEPTH) return seterr("%s: damaged SQLite file (a b-tree deeper than %d levels)", s->name, SQ_MAXDEPTH);
    if (sq_mark(s, root) || sq_read(s, root, page)) return -1;
    hdr = root == 1 ? 100 : 0;
    type = page[hdr];
    if (type == 13 || type == 5) { idx = 0; interior = type == 5; }
    else if (type == 10 || type == 2) { idx = 1; interior = type == 2; }
    else return seterr("%s: damaged SQLite file (page %u is not a b-tree page, type %u)", s->name, root, type);
    if (*family < 0) *family = idx;
    else if (*family != idx) return seterr("%s: damaged SQLite file (page %u is of the wrong kind of b-tree)", s->name, root);
    ncells = be16(page + hdr + 3);
    if (hdr + (interior ? 12u : 8u) + 2u * ncells > s->usable) return seterr("%s: damaged SQLite file (page %u lists more cells than fit)", s->name, root);
    for (i = 0; i < ncells; i++) {
        uint32_t off = be16(page + hdr + (interior ? 12 : 8) + 2 * i);
        const uint8_t *c = page + off, *end = page + s->usable;
        uint64_t plen, rowid = 0;
        int k;
        if (off < hdr + (interior ? 12u : 8u) + 2u * ncells || off >= s->usable)
            return seterr("%s: damaged SQLite file (a cell pointer out of page %u)", s->name, root);
        if (interior) {
            uint32_t child;
            if (c + 4 > end) return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, root);
            child = be32(c);
            if (sq_walk(s, child, depth + 1, family, cb, ctx, pay)) return -1;
            if (idx) {                                  /* an index's interior cells are entries too */
                c += 4;
                if (!(k = sq_varint(c, end, &plen))) return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, root);
                c += k;
                if (sq_payload(s, page, c, plen, 1, pay, root)) return -1;
                if (cb && cb(ctx, 0, pay->p, pay->n)) return -1;
            }
        } else if (!idx) {
            if (!(k = sq_varint(c, end, &plen))) return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, root);
            c += k;
            if (!(k = sq_varint(c, end, &rowid))) return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, root);
            c += k;
            if (sq_payload(s, page, c, plen, 0, pay, root)) return -1;
            if (cb && cb(ctx, rowid, pay->p, pay->n)) return -1;
        } else {
            if (!(k = sq_varint(c, end, &plen))) return seterr("%s: damaged SQLite file (a cell runs past page %u)", s->name, root);
            c += k;
            if (sq_payload(s, page, c, plen, 1, pay, root)) return -1;
            if (cb && cb(ctx, 0, pay->p, pay->n)) return -1;
        }
    }
    if (interior) {
        uint32_t right = be32(page + hdr + 8);
        if (sq_walk(s, right, depth + 1, family, cb, ctx, pay)) return -1;
    }
    return 0;
}

/* Column i of a record as text or a number; type: 0 null, 1 int, 3 text/blob. */
static int sq_column(const uint8_t *rec, size_t n, int col, int *type, int64_t *iv, const uint8_t **tp, size_t *tn) {
    uint64_t hlen, st;
    const uint8_t *h, *end = rec + n, *d;
    int k, i;
    if (!(k = sq_varint(rec, end, &hlen)) || hlen > n) return -1;
    h = rec + k;
    d = rec + hlen;
    for (i = 0; i <= col; i++) {
        uint64_t len;
        if (h >= rec + hlen) { *type = 0; return 0; }          /* fewer columns: NULL */
        if (!(k = sq_varint(h, rec + hlen, &st))) return -1;
        h += k;
        if (st == 0 || st == 8 || st == 9) len = 0;
        else if (st <= 4) len = st;
        else if (st == 5) len = 6;
        else if (st == 6 || st == 7) len = 8;
        else if (st >= 12) len = (st - 12) / 2;
        else return -1;
        if (d + len > end) return -1;
        if (i == col) {
            if (st == 0) *type = 0;
            else if (st == 8 || st == 9) { *type = 1; *iv = (int64_t)st - 8; }
            else if (st <= 6) {
                int64_t v = (d[0] & 0x80) ? -1 : 0;
                uint64_t j;
                for (j = 0; j < len; j++) v = (int64_t)((uint64_t)v << 8 | d[j]);
                *type = 1; *iv = v;
            } else if (st == 7) { *type = 2; }
            else { *type = 3; *tp = d; *tn = (size_t)len; }
            return 0;
        }
        d += len;
    }
    return -1;
}

typedef struct sq_schema { strs types, names, tbls; uint32_t *roots; int *partial; int64_t *count; int n, cap; int bad; } sq_schema;

static int sq_idc(int c) { return c == '_' || c == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80; }
/* Does an index's CREATE text have a WHERE clause (a partial index)? Outside quotes. */
static int sq_partial(const uint8_t *p, size_t n) {
    size_t i;
    int q = 0;
    for (i = 0; i < n; i++) {
        int c = p[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '\'' || c == '"' || c == '`' || c == '[') { q = c == '[' ? ']' : c; continue; }
        if (i + 5 <= n && (i == 0 || !sq_idc(p[i - 1])) && (i + 5 == n || !sq_idc(p[i + 5]))) {
            char w[6];
            int k;
            for (k = 0; k < 5; k++) w[k] = (char)(p[i + k] | 0x20);
            w[5] = 0;
            if (!strcmp(w, "where")) return 1;
        }
    }
    return 0;
}
static int sq_schema_cb(void *ctx, uint64_t rowid, const uint8_t *rec, size_t n) {
    sq_schema *S = (sq_schema *)ctx;
    int t0, t1, t2, t3, t4;
    int64_t root = 0, iv;
    const uint8_t *tp, *np, *bp, *sp;
    size_t tn, nn, bn, sn;
    char ty[16], nm[1024], tb[1024];
    (void)rowid;
    if (sq_column(rec, n, 0, &t0, &iv, &tp, &tn) || t0 != 3 ||
        sq_column(rec, n, 1, &t1, &iv, &np, &nn) || t1 != 3 ||
        sq_column(rec, n, 2, &t2, &iv, &bp, &bn) ||
        sq_column(rec, n, 3, &t3, &root, &tp, &tn) ||
        sq_column(rec, n, 4, &t4, &iv, &sp, &sn)) { S->bad = 1; return 0; }
    sq_column(rec, n, 0, &t0, &iv, &tp, &tn);
    if (t2 != 3) bn = 0;
    snprintf(tb, sizeof tb, "%.*s", (int)(bn < 1023 ? bn : 1023), t2 == 3 ? (const char *)bp : "");
    snprintf(ty, sizeof ty, "%.*s", (int)(tn < 15 ? tn : 15), (const char *)tp);
    snprintf(nm, sizeof nm, "%.*s", (int)(nn < 1023 ? nn : 1023), (const char *)np);
    if (t3 != 1) root = 0;
    if (S->n == S->cap) {
        S->cap = S->cap ? S->cap * 2 : 16;
        S->roots = (uint32_t *)xrealloc(S->roots, (size_t)S->cap * sizeof *S->roots);
        S->partial = (int *)xrealloc(S->partial, (size_t)S->cap * sizeof *S->partial);
        S->count = (int64_t *)xrealloc(S->count, (size_t)S->cap * sizeof *S->count);
    }
    strs_add(&S->types, ty);
    strs_add(&S->names, nm);
    strs_add(&S->tbls, tb);
    S->partial[S->n] = t4 == 3 && sq_partial(sp, sn);
    S->count[S->n] = -1;
    S->roots[S->n++] = root > 0 && root <= 0xFFFFFFFFLL ? (uint32_t)root : 0;
    return 0;
}

typedef struct sq_tally { uint64_t rows; sha256 sha; int rowids; int64_t last; const char *name, *file; } sq_tally;
static int sq_tally_cb(void *ctx, uint64_t rowid, const uint8_t *rec, size_t n) {
    sq_tally *t = (sq_tally *)ctx;
    uint8_t r[12];
    int i;
    if (t->rowids) {                                   /* a rowid table's keys rise strictly */
        if (t->rows && (int64_t)rowid <= t->last)
            return seterr("%s: damaged SQLite file (rowids out of order in table %s)", t->file, t->name);
        t->last = (int64_t)rowid;
    }
    for (i = 0; i < 8; i++) r[i] = (uint8_t)(rowid >> (56 - 8 * i));
    r[8] = (uint8_t)(n >> 24); r[9] = (uint8_t)(n >> 16); r[10] = (uint8_t)(n >> 8); r[11] = (uint8_t)n;
    sha256_add(&t->sha, r, 12);
    sha256_add(&t->sha, rec, n);
    t->rows++;
    return 0;
}

/* Verifies the SQLite file at path (already copied into the sandbox). notes gets what was
 * checked, for the report. */
static int sq_count_cb(void *ctx, uint64_t rowid, const uint8_t *rec, size_t n) {
    (void)rowid; (void)rec; (void)n;
    (*(int64_t *)ctx)++;
    return 0;
}
static int sq_name_eq(const char *a, const char *b) {   /* SQLite names: ASCII case doesn't matter */
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static int sqlite_verify(const char *path, items *out, strs *notes) {
    sqlf s;
    uint8_t h[100];
    struct stat st;
    sq_schema S;
    buf pay = { 0 };
    int fam = -1, i, rc = -1, nidx = 0, skipped = 0, npartial = 0;
    uint32_t hdr_pages, trunk, nfree, freecount = 0;
    memset(&s, 0, sizeof s);
    memset(&S, 0, sizeof S);
    s.name = base_name(path);
    if (!(s.f = fopen(path, "rb"))) return seterr("cannot read %s: %s", path, strerror(errno));
    if (fread(h, 1, 100, s.f) != 100 || memcmp(h, "SQLite format 3", 16)) { fclose(s.f); return seterr("%s: not a SQLite database file (or cut short)", s.name); }
    s.psize = be16(h + 16);
    if (s.psize == 1) s.psize = 65536;
    if (s.psize < 512 || s.psize > 65536 || (s.psize & (s.psize - 1))) { fclose(s.f); return seterr("%s: damaged SQLite header (page size %u)", s.name, s.psize); }
    if (h[20] > s.psize - 480) { fclose(s.f); return seterr("%s: damaged SQLite header (reserved space)", s.name); }
    s.usable = s.psize - h[20];
    if (fstat(fileno(s.f), &st)) { fclose(s.f); return seterr("cannot read %s", path); }
    hdr_pages = be32(h + 28);
    if (hdr_pages && be32(h + 24) == be32(h + 92)) {       /* the header's page count is valid */
        if ((uint64_t)st.st_size < (uint64_t)hdr_pages * s.psize) {
            fclose(s.f);
            return seterr("%s: truncated: the header says %u pages of %u bytes (%llu bytes), the file has %llu bytes",
                          s.name, hdr_pages, s.psize, (unsigned long long)hdr_pages * s.psize, (unsigned long long)st.st_size);
        }
        s.npages = hdr_pages;
    } else {
        s.npages = (uint32_t)((uint64_t)st.st_size / s.psize);
    }
    if ((uint64_t)st.st_size % s.psize) { fclose(s.f); return seterr("%s: truncated or damaged: %llu bytes is not a whole number of %u-byte pages", s.name, (unsigned long long)st.st_size, s.psize); }
    if (!s.npages) { fclose(s.f); return seterr("%s: empty SQLite file", s.name); }
    s.page = (uint8_t *)xmalloc((size_t)(SQ_MAXDEPTH + 1) * s.psize);
    s.seen = (uint8_t *)xmalloc(s.npages / 8 + 1);
    memset(s.seen, 0, s.npages / 8 + 1);
    if (sq_walk(&s, 1, 0, &fam, sq_schema_cb, &S, &pay)) goto done;
    if (fam != 0) { seterr("%s: damaged SQLite file (the schema is not a table b-tree)", s.name); goto done; }
    if (S.bad) { seterr("%s: damaged SQLite file (a schema record doesn't decode)", s.name); goto done; }
    for (i = 0; i < S.n; i++) {
        const char *ty = S.types.v[i], *nm = S.names.v[i];
        int f = -1;
        if (!strcmp(ty, "table") || !strcmp(ty, "index")) {
            if (!S.roots[i]) { if (!strcmp(ty, "table")) skipped++; continue; }    /* a virtual table */
            if (!strcmp(ty, "table")) {
                sq_tally t;
                char hex[65];
                memset(&t, 0, sizeof t);
                sha256_init(&t.sha);
                t.name = nm; t.file = s.name;
                t.rowids = 1;                                    /* cleared below for WITHOUT ROWID */
                {
                    uint8_t *pg = s.page;                        /* the root's kind: table or index b-tree */
                    if (sq_read(&s, S.roots[i], pg)) goto done;
                    t.rowids = pg[S.roots[i] == 1 ? 100 : 0] == 13 || pg[S.roots[i] == 1 ? 100 : 0] == 5;
                }
                if (sq_walk(&s, S.roots[i], 0, &f, sq_tally_cb, &t, &pay)) goto done;
                S.count[i] = (int64_t)t.rows;
                sha256_hex_done(&t.sha, hex);
                if (strncmp(nm, "sqlite_", 7)) items_add(out, nm, (int64_t)t.rows, hex);
            } else {
                int64_t entries = 0;
                if (sq_walk(&s, S.roots[i], 0, &f, sq_count_cb, &entries, &pay)) goto done;
                S.count[i] = entries;
                nidx++;
            }
        }
    }
    /* the free list: trunk pages, each with leaf page numbers */
    trunk = be32(h + 32);
    nfree = be32(h + 36);
    while (trunk) {
        uint32_t nleaf, j;
        uint8_t *pg = s.page;
        if (sq_mark(&s, trunk) || sq_read(&s, trunk, pg)) goto done;
        freecount++;
        nleaf = be32(pg + 4);
        if (nleaf > s.usable / 4 - 2) { seterr("%s: damaged SQLite file (free-list page %u)", s.name, trunk); goto done; }
        for (j = 0; j < nleaf; j++) {
            if (sq_mark(&s, be32(pg + 8 + 4 * j))) goto done;
            freecount++;
        }
        trunk = be32(pg);
        if (freecount > s.npages) { seterr("%s: damaged SQLite file (the free list loops)", s.name); goto done; }
    }
    if (freecount != nfree) { seterr("%s: damaged SQLite file (the free list has %u pages, the header says %u)", s.name, freecount, nfree); goto done; }
    /* every index holds one entry per row of its table (a partial index: no more) */
    for (i = 0; i < S.n; i++) {
        int j;
        if (strcmp(S.types.v[i], "index") || S.count[i] < 0) continue;
        for (j = 0; j < S.n; j++) if (!strcmp(S.types.v[j], "table") && S.count[j] >= 0 && sq_name_eq(S.names.v[j], S.tbls.v[i])) break;
        if (j == S.n) { seterr("%s: damaged SQLite file (index %s belongs to no table in the file)", s.name, S.names.v[i]); goto done; }
        if (S.partial[i]) {
            npartial++;
            if (S.count[i] > S.count[j]) {
                seterr("%s: damaged SQLite file (partial index %s has %lld entries, more than the %lld rows of %s)", s.name,
                       S.names.v[i], (long long)S.count[i], (long long)S.count[j], S.names.v[j]);
                goto done;
            }
        } else if (S.count[i] != S.count[j]) {
            seterr("%s: damaged SQLite file (table %s has %lld rows, its index %s has %lld entries: copied in the middle of a write?)",
                   s.name, S.names.v[j], (long long)S.count[j], S.names.v[i], (long long)S.count[i]);
            goto done;
        }
    }
    /* every page is used: by a b-tree, the free list, a pointer-map page (auto-vacuum), or
     * it is the lock-byte page at 1 GiB */
    {
        uint32_t pg, unused = 0, first = 0;
        if (be32(h + 52)) {                                  /* auto-vacuum: pointer maps at 2, then every usable/5 + 1 pages */
            uint64_t step = s.usable / 5 + 1, q;
            for (q = 2; q <= s.npages; q += step) s.seen[(q - 1) >> 3] |= (uint8_t)(1u << ((q - 1) & 7));
        }
        pg = (uint32_t)(1073741824u / s.psize) + 1;
        if (pg <= s.npages) s.seen[(pg - 1) >> 3] |= (uint8_t)(1u << ((pg - 1) & 7));
        for (pg = 1; pg <= s.npages; pg++) if (!(s.seen[(pg - 1) >> 3] & (1u << ((pg - 1) & 7)))) { if (!unused++) first = pg; }
        if (unused) {
            seterr("%s: damaged SQLite file (%u page%s used by no table, index or the free list, the first %u)", s.name,
                   unused, unused == 1 ? "" : "s", first);
            goto done;
        }
    }
    strs_addf(notes, "SQLite file: %s pages of %u bytes, %d tables, %d indexes walked page by page, %u free pages",
              commas(s.npages), s.psize, out->n, nidx, freecount);
    strs_addf(notes, "every index matches its table's rows%s, rowids rise, every page is used", npartial ? " (partial indexes: no more entries than rows)" : "");
    if (skipped) strs_addf(notes, "%d virtual tables have no rows in the file and were skipped", skipped);
    if (h[18] == 2) strs_add(notes, "the file is in WAL mode: a separate -wal file is not part of this backup");
    rc = 0;
done:
    fclose(s.f);
    free(s.page);
    free(s.seen);
    free(S.roots);
    free(S.partial);
    free(S.count);
    strs_free(&S.types);
    strs_free(&S.names);
    strs_free(&S.tbls);
    buf_free(&pay);
    return rc;
}
