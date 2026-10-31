/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* File archives for AltSql Restore: a streaming tar reader (ustar, GNU long names, pax
 * path records) that unpacks into the sandbox, and a folder walker that copies a folder
 * there. Both hash every file with SHA-256 as it goes.
 *
 * What gets checked: each header's checksum, sizes, the end-of-archive blocks (an archive
 * cut short fails), and paths (absolute paths and ".." are refused, so nothing is written
 * outside the sandbox). Symbolic and hard links are recorded with their targets, not made.
 *
 * The result is one item per top-level entry: its files (directories don't count) and a
 * checksum over each file's path, size and SHA-256; and an item "(total bytes)". */

typedef struct fentry { char *path; int64_t size; char sha[65]; char type; long seq; } fentry;
typedef struct fentries { fentry *v; long n, cap; } fentries;

static void fent_add(fentries *L, const char *path, int64_t size, const char *sha, char type) {
    fentry *e;
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->v = (fentry *)xrealloc(L->v, (size_t)L->cap * sizeof *L->v); }
    e = &L->v[L->n];
    e->path = xstrdup(path);
    e->size = size;
    snprintf(e->sha, sizeof e->sha, "%s", sha);
    e->type = type;
    e->seq = L->n++;
}
static void fent_free(fentries *L) {
    long i;
    for (i = 0; i < L->n; i++) free(L->v[i].path);
    free(L->v);
    L->v = NULL; L->n = L->cap = 0;
}
static int fent_cmp(const void *a, const void *b) {
    const fentry *x = (const fentry *)a, *y = (const fentry *)b;
    int c = strcmp(x->path, y->path);
    return c ? c : (x->seq < y->seq ? -1 : x->seq > y->seq);
}

/* Items from the files: sorted, the last of duplicate paths kept (tar appends updates). */
static void fent_items(fentries *L, items *out) {
    long i;
    sha256 all, cur;
    char top[AR_PATH] = "", hex[65];
    int64_t files = 0, bytes = 0;
    int open = 0;
    int level = 1;
    qsort(L->v, (size_t)L->n, sizeof *L->v, fent_cmp);
    /* everything under one top folder (tar czf x.tgz site/): items are the level below it */
    if (L->n) {
        const char *f = L->v[0].path, *sl = strchr(f, '/');
        size_t k = sl ? (size_t)(sl - f) : strlen(f);
        int deeper = 0;
        level = 2;
        for (i = 0; i < L->n; i++) {
            const char *p = L->v[i].path;
            if (strncmp(p, f, k) || (p[k] != '/' && p[k] != 0)) { level = 1; break; }
            if (p[k] == '/') deeper = 1;
        }
        if (!deeper) level = 1;
    }
    sha256_init(&all);
    for (i = 0; i < L->n; i++) {
        fentry *e = &L->v[i];
        const char *slash;
        char t[AR_PATH], line[AR_PATH + 128];
        size_t tl;
        int ln;
        if (i + 1 < L->n && !strcmp(L->v[i + 1].path, e->path)) continue;   /* a later copy wins */
        slash = strchr(e->path, '/');
        if (level == 2 && !slash && e->type == 'd') continue;    /* the top folder itself */
        if (level == 2 && slash) slash = strchr(slash + 1, '/');
        tl = slash ? (size_t)(slash - e->path) : strlen(e->path);
        if (tl >= sizeof t) tl = sizeof t - 1;
        memcpy(t, e->path, tl);
        t[tl] = 0;
        if (!open || strcmp(t, top)) {
            if (open) { sha256_hex_done(&cur, hex); items_add(out, top, files, hex); }
            snprintf(top, sizeof top, "%s", t);
            sha256_init(&cur);
            files = 0;
            open = 1;
        }
        if (e->type == 'd') continue;
        ln = snprintf(line, sizeof line, "%c\t%s\t%lld\t%s\n", e->type, e->path, (long long)e->size, e->sha);
        if (ln > 0) { sha256_add(&cur, line, (size_t)ln); sha256_add(&all, line, (size_t)ln); }
        files++;
        bytes += e->size;
    }
    if (open) { sha256_hex_done(&cur, hex); items_add(out, top, files, hex); }
    sha256_hex_done(&all, hex);
    items_add(out, "(total bytes)", bytes, hex);
}

/* A path from an archive, made relative and safe: no leading '/', no "..", no empty name. */
static int safe_path(const char *in, char *out, size_t cap) {
    const char *p = in;
    size_t n = 0;
    if (*p == '/') return seterr("the archive holds an absolute path: %s", in);
    while (*p) {
        const char *s = p;
        size_t k;
        while (*p && *p != '/') p++;
        k = (size_t)(p - s);
        if (k == 2 && s[0] == '.' && s[1] == '.') return seterr("the archive holds a path with \"..\": %s", in);
        if (k && !(k == 1 && s[0] == '.')) {
            if (n + k + 2 > cap) return seterr("path too long in the archive: %s", in);
            if (n) out[n++] = '/';
            memcpy(out + n, s, k);
            n += k;
        }
        while (*p == '/') p++;
    }
    out[n] = 0;
    return 0;
}

typedef struct tarstate {
    const char *root;           /* the sandbox folder files go to              */
    const char *name;           /* the backup's name, for messages             */
    fentries   *files;
    uint8_t     hdr[512];
    size_t      hn;
    int         phase;          /* 0 header, 1 data, 2 padding                 */
    uint64_t    left, pad;
    int         zeros, ended;
    char        type;
    char        path[AR_PATH];
    int64_t     size;
    FILE       *out;
    sha256      sha;
    buf         meta;           /* the data of an 'L' or 'x' record            */
    char        longname[AR_PATH];
    char        paxpath[AR_PATH];
    long        headers;
} tarstate;

static uint64_t tar_num(const uint8_t *p, int n) {
    uint64_t v = 0;
    int i;
    if (p[0] & 0x80) {                                  /* base-256, for large sizes */
        v = p[0] & 0x7F;
        for (i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    for (i = 0; i < n && (p[i] == ' ' || p[i] == 0); i++) {}
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

/* The path in a pax extended header: records "LEN path=VALUE\n", LEN counting the whole
 * record. Every record must lie inside the header's data; the digits of LEN must too. */
static void pax_path(const buf *m, char *out, size_t cap) {
    size_t i = 0;
    while (i < m->n) {
        size_t len = 0, j = i;
        while (j < m->n && j - i < 9 && m->p[j] >= '0' && m->p[j] <= '9') len = len * 10 + (size_t)(m->p[j++] - '0');
        if (!len || len > m->n - i || j >= i + len) return;
        if (m->p[j] == ' ' && i + len - (j + 1) >= 6 && !memcmp(m->p + j + 1, "path=", 5)) {
            size_t vs = j + 6, vn = i + len - 1 - vs;
            if (vn < cap) { memcpy(out, m->p + vs, vn); out[vn] = 0; }
        }
        i += len;
    }
}

static int tar_header(tarstate *t) {
    uint64_t sum = 0, want;
    int i, zero = 1;
    char raw[AR_PATH];
    for (i = 0; i < 512; i++) if (t->hdr[i]) { zero = 0; break; }
    if (zero) {
        if (++t->zeros >= 2) t->ended = 1;
        return 0;
    }
    if (t->zeros) return seterr("%s: damaged tar archive (data after an end block)", t->name);
    for (i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : t->hdr[i];
    want = tar_num(t->hdr + 148, 8);
    if (sum != want) return seterr("%s: damaged tar archive (header %ld fails its checksum)", t->name, t->headers + 1);
    t->headers++;
    t->type = (char)t->hdr[156];
    t->size = (int64_t)tar_num(t->hdr + 124, 12);
    if (t->size < 0) return seterr("%s: damaged tar archive (bad size)", t->name);
    t->left = (uint64_t)t->size;
    t->pad = (512 - t->left % 512) % 512;
    t->phase = t->left ? 1 : 2;
    if (t->type == 'L' || t->type == 'x' || t->type == 'g' || t->type == 'K') {
        t->meta.n = 0;
        if (t->left > (1u << 20)) return seterr("%s: damaged tar archive (huge extended header)", t->name);
        if (!t->left) t->phase = 2;
        return 0;
    }
    if (t->longname[0]) snprintf(raw, sizeof raw, "%s", t->longname);
    else if (t->paxpath[0]) snprintf(raw, sizeof raw, "%s", t->paxpath);
    else if (!memcmp(t->hdr + 257, "ustar", 5) && t->hdr[345])
        snprintf(raw, sizeof raw, "%.155s/%.100s", (const char *)t->hdr + 345, (const char *)t->hdr);
    else snprintf(raw, sizeof raw, "%.100s", (const char *)t->hdr);
    t->longname[0] = t->paxpath[0] = 0;
    if (safe_path(raw, t->path, sizeof t->path)) return -1;
    if (!t->path[0]) { t->type = 'd'; return 0; }        /* "./" itself */
    if (t->type == '5') {
        char full[AR_PATH];
        snprintf(full, sizeof full, "%s/%s", t->root, t->path);
        if (mkdirs(full, 0755)) return -1;
        fent_add(t->files, t->path, 0, "", 'd');
        t->phase = t->left ? 1 : 2;
        return 0;
    }
    if (t->type == '2' || t->type == '1') {             /* links: recorded with their targets */
        char target[101], hex[65];
        snprintf(target, sizeof target, "%.100s", (const char *)t->hdr + 157);
        sha256_hex(target, strlen(target), hex);
        fent_add(t->files, t->path, 0, hex, t->type == '2' ? 's' : 'h');
        return 0;
    }
    if (t->type == '0' || t->type == 0 || t->type == '7') {
        char full[AR_PATH], *slash;
        if (snprintf(full, sizeof full, "%s/%s", t->root, t->path) >= (int)sizeof full) return seterr("path too long: %s", t->path);
        slash = strrchr(full, '/');
        *slash = 0;
        if (mkdirs(full, 0755)) return -1;
        *slash = '/';
        if (!(t->out = fopen(full, "wb"))) return seterr("cannot write %s: %s", full, strerror(errno));
        sha256_init(&t->sha);
        if (!t->left) {                                 /* an empty file */
            char hex[65];
            fclose(t->out);
            t->out = NULL;
            sha256_hex_done(&t->sha, hex);
            fent_add(t->files, t->path, 0, hex, 'f');
        }
        return 0;
    }
    return 0;                                           /* devices, fifos: skipped */
}

static int tar_cb(void *ctx, const uint8_t *p, size_t n) {
    tarstate *t = (tarstate *)ctx;
    while (n) {
        if (t->ended) return 0;                         /* padding after the end blocks */
        if (t->phase == 0) {
            size_t k = 512 - t->hn;
            if (k > n) k = n;
            memcpy(t->hdr + t->hn, p, k);
            t->hn += k; p += k; n -= k;
            if (t->hn == 512) { t->hn = 0; if (tar_header(t)) return -1; }
        } else if (t->phase == 1) {
            size_t k = t->left < n ? (size_t)t->left : n;
            if (t->type == 'L' || t->type == 'x' || t->type == 'g' || t->type == 'K') buf_add(&t->meta, p, k);
            else if (t->out) {
                if (fwrite(p, 1, k, t->out) != k) return seterr("cannot write in the sandbox: %s", strerror(errno));
                sha256_add(&t->sha, p, k);
            }
            t->left -= k; p += k; n -= k;
            if (!t->left) {
                if (t->type == 'L') {
                    size_t m = t->meta.n < sizeof t->longname - 1 ? t->meta.n : sizeof t->longname - 1;
                    memcpy(t->longname, t->meta.p, m);
                    t->longname[m] = 0;
                } else if (t->type == 'x') {
                    pax_path(&t->meta, t->paxpath, sizeof t->paxpath);
                } else if (t->out) {
                    char hex[65];
                    if (fclose(t->out)) { t->out = NULL; return seterr("cannot write in the sandbox: %s", strerror(errno)); }
                    t->out = NULL;
                    sha256_hex_done(&t->sha, hex);
                    fent_add(t->files, t->path, t->size, hex, 'f');
                }
                t->phase = 2;
            }
        } else {
            size_t k = t->pad < n ? (size_t)t->pad : n;
            t->pad -= k; p += k; n -= k;
            if (!t->pad) t->phase = 0;
        }
        if (t->phase == 2 && !t->pad) t->phase = 0;
    }
    return 0;
}

/* Unpacks a tar (or .tar.gz) into root. */
static int tar_restore(const char *backup, const char *root, fentries *files, int *was_gz) {
    tarstate *t = (tarstate *)xmalloc(sizeof *t);
    int rc;
    memset(t, 0, sizeof *t);
    t->root = root;
    t->name = base_name(backup);
    t->files = files;
    rc = stream_file(backup, tar_cb, t, was_gz);
    if (t->out) fclose(t->out);
    if (rc == 0 && !t->ended && t->zeros == 0) {
        if (t->phase == 1) rc = seterr("%s: the archive ends inside %s (truncated)", t->name, t->path);
        else if (t->headers == 0) rc = seterr("%s: not a tar archive, or empty", t->name);
        else rc = seterr("%s: the archive has no end blocks (truncated)", t->name);
    } else if (rc == 0 && !t->ended && (t->phase != 0 || t->hn)) {
        rc = seterr("%s: the archive ends inside a block (truncated)", t->name);
    }
    buf_free(&t->meta);
    free(t);
    return rc;
}

/* Copies a folder into root, hashing every file; symbolic links are recorded, not followed. */
static int dir_restore(const char *src, const char *root, const char *rel, fentries *files) {
    char from[AR_PATH], to[AR_PATH];
    DIR *d;
    struct dirent *e;
    snprintf(from, sizeof from, "%s%s%s", src, *rel ? "/" : "", rel);
    snprintf(to, sizeof to, "%s%s%s", root, *rel ? "/" : "", rel);
    if (mkdirs(to, 0755)) return -1;
    if (!(d = opendir(from))) return seterr("cannot read the folder %s: %s", from, strerror(errno));
    while ((e = readdir(d)) != NULL) {
        char sub[AR_PATH], f[AR_PATH], t[AR_PATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name) >= (int)sizeof sub
            || snprintf(f, sizeof f, "%s/%s", src, sub) >= (int)sizeof f
            || snprintf(t, sizeof t, "%s/%s", root, sub) >= (int)sizeof t) {
            closedir(d);
            return seterr("path too long: %s", sub);
        }
        if (lstat(f, &st)) { closedir(d); return seterr("cannot read %s: %s", f, strerror(errno)); }
        if (S_ISDIR(st.st_mode)) {
            fent_add(files, sub, 0, "", 'd');
            if (dir_restore(src, root, sub, files)) { closedir(d); return -1; }
        } else if (S_ISLNK(st.st_mode)) {
            char target[AR_PATH], hex[65];
            ssize_t k = readlink(f, target, sizeof target - 1);
            if (k < 0) { closedir(d); return seterr("cannot read the link %s: %s", f, strerror(errno)); }
            target[k] = 0;
            sha256_hex(target, (size_t)k, hex);
            fent_add(files, sub, 0, hex, 's');
        } else if (S_ISREG(st.st_mode)) {
            FILE *in = fopen(f, "rb"), *out;
            uint8_t tmp[65536];
            size_t k;
            int64_t n = 0;
            sha256 s;
            char hex[65];
            if (!in) { closedir(d); return seterr("cannot read %s: %s", f, strerror(errno)); }
            if (!(out = fopen(t, "wb"))) { fclose(in); closedir(d); return seterr("cannot write %s: %s", t, strerror(errno)); }
            sha256_init(&s);
            while ((k = fread(tmp, 1, sizeof tmp, in)) > 0) {
                sha256_add(&s, tmp, k);
                n += (int64_t)k;
                if (fwrite(tmp, 1, k, out) != k) { fclose(in); fclose(out); closedir(d); return seterr("cannot write %s: %s", t, strerror(errno)); }
            }
            if (ferror(in)) { fclose(in); fclose(out); closedir(d); return seterr("cannot read %s: %s", f, strerror(errno)); }
            fclose(in);
            if (fclose(out)) { closedir(d); return seterr("cannot write %s: %s", t, strerror(errno)); }
            sha256_hex_done(&s, hex);
            fent_add(files, sub, n, hex, 'f');
        }
    }
    closedir(d);
    return 0;
}
