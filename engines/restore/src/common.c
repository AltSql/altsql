/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql Restore: shared helpers. Errors, growable buffers, item lists, paths,
 * recursive removal, number formatting. Part of altsql_restore.c (one program). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AR_PATH 4096

/* An error message for the run: the first one set wins, later ones are dropped. */
static char g_err[2048];
static int seterr(const char *fmt, ...) {
    va_list ap;
    if (g_err[0]) return -1;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "altsql-restore: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "altsql-restore: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- growable byte buffer ---------------------------------------------------------- */
typedef struct buf { uint8_t *p; size_t n, cap; } buf;
static void buf_need(buf *b, size_t more) {
    if (b->n + more <= b->cap) return;
    while (b->cap < b->n + more) b->cap = b->cap ? b->cap * 2 : 4096;
    b->p = (uint8_t *)xrealloc(b->p, b->cap);
}
static void buf_add(buf *b, const void *d, size_t n) { if (!n) return; buf_need(b, n); memcpy(b->p + b->n, d, n); b->n += n; }
static void buf_printf(buf *b, const char *fmt, ...) {
    va_list ap;
    int k;
    char tmp[1024];
    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) { buf_add(b, tmp, (size_t)k); return; }
    buf_need(b, (size_t)k + 1);
    va_start(ap, fmt);
    vsnprintf((char *)b->p + b->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}
static const char *buf_str(buf *b) { buf_need(b, 1); b->p[b->n] = 0; return (const char *)b->p; }
static void buf_free(buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---- what a restore found: one item per table (or per top-level folder) ------------ */
typedef struct item {
    char    *name;
    int64_t  rows;        /* rows of a table; files of an archive folder; bytes for "(total bytes)" */
    char     sum[65];     /* checksum, hex                                                         */
} item;
typedef struct items { item *v; int n, cap; } items;
static item *items_add(items *L, const char *name, int64_t rows, const char *sum) {
    item *it;
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 16; L->v = (item *)xrealloc(L->v, (size_t)L->cap * sizeof *L->v); }
    it = &L->v[L->n++];
    it->name = xstrdup(name);
    if (strlen(it->name) > 250) it->name[250] = 0;   /* the history keeps names of up to 255 bytes */
    it->rows = rows;
    snprintf(it->sum, sizeof it->sum, "%s", sum ? sum : "");
    return it;
}
static int item_cmp(const void *a, const void *b) { return strcmp(((const item *)a)->name, ((const item *)b)->name); }
static void items_sort(items *L) { if (L->n > 1) qsort(L->v, (size_t)L->n, sizeof *L->v, item_cmp); }
static item *items_find(const items *L, const char *name) {
    int i;
    for (i = 0; i < L->n; i++) if (!strcmp(L->v[i].name, name)) return &L->v[i];
    return NULL;
}
static void items_free(items *L) {
    int i;
    for (i = 0; i < L->n; i++) free(L->v[i].name);
    free(L->v);
    L->v = NULL; L->n = L->cap = 0;
}

/* ---- string list --------------------------------------------------------------------- */
typedef struct strs { char **v; int n, cap; } strs;
static void strs_add(strs *L, const char *s) {
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 8; L->v = (char **)xrealloc(L->v, (size_t)L->cap * sizeof *L->v); }
    L->v[L->n++] = xstrdup(s);
}
static void strs_addf(strs *L, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    strs_add(L, tmp);
}
static int strs_has(const strs *L, const char *s) {
    int i;
    for (i = 0; i < L->n; i++) if (!strcmp(L->v[i], s)) return 1;
    return 0;
}
static void strs_free(strs *L) {
    int i;
    for (i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
    L->v = NULL; L->n = L->cap = 0;
}

/* ---- numbers and time ------------------------------------------------------------------- */
/* 1234567 -> "1,234,567" in one of four rotating static buffers. */
static const char *commas(int64_t v) {
    static char ring[4][32];
    static int k;
    char tmp[32], *o = ring[k++ & 3];
    int n, i, j = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)u);
    if (neg) o[j++] = '-';
    for (i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) o[j++] = ',';
        o[j++] = tmp[i];
    }
    o[j] = 0;
    return o;
}
static double now_mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void sleep_ms(long ms) {
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

/* ---- files and paths ------------------------------------------------------------------- */
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_exec(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode) && !access(p, X_OK); }

static int mkdirs(const char *path, mode_t mode) {
    char tmp[AR_PATH];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return seterr("path too long: %s", path);
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, mode) && errno != EEXIST) return seterr("cannot make %s: %s", tmp, strerror(errno));
            tmp[i] = c;
        }
    }
    return 0;
}

/* rm -rf, without following symbolic links. Returns 0 when the path is gone. */
static int rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        int bad = 0;
        if (!d) {
            chmod(path, 0700);                     /* a folder unpacked without read permission */
            d = opendir(path);
        }
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char sub[AR_PATH];
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub) { bad = 1; continue; }
                if (rmtree(sub)) bad = 1;
            }
            closedir(d);
        }
        if (rmdir(path)) {
            chmod(path, 0700);
            if (rmdir(path)) return -1;
        }
        return bad;
    }
    return unlink(path) && errno != ENOENT ? -1 : 0;
}

static int write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return seterr("cannot write %s: %s", path, strerror(errno));
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); return seterr("cannot write %s: %s", path, strerror(errno)); }
    if (fclose(f)) return seterr("cannot write %s: %s", path, strerror(errno));
    return 0;
}

/* The whole file, NUL-terminated. NULL if it cannot be read. */
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    buf b = { 0 };
    uint8_t tmp[65536];
    size_t k;
    if (!f) return NULL;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&b, tmp, k);
    if (ferror(f)) { fclose(f); buf_free(&b); return NULL; }
    fclose(f);
    if (len) *len = b.n;
    buf_str(&b);
    return (char *)b.p;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}
