/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql Restore: a scripted fuzz pass over the three built-in readers (SQLite file walker,
 * tar reader, gzip inflater). Build it with AddressSanitizer and UBSan (make fuzz does) so
 * any out-of-bounds access, overflow or undefined behaviour stops the run with a report.
 *
 *   fuzz_readers SEEDDIR ITERATIONS [RANDOMSEED]
 *
 * SEEDDIR holds the starting files (tools/fuzz_seeds.py makes them): *.sqlite, *.tar and
 * *.gz. Each iteration takes one seed and damages it: flipped bits, bytes set to edge values
 * (0x00, 0x7F, 0x80, 0xFF), 16- and 32-bit fields set to edge values, a cut, a block copied
 * over another, bytes inserted. Then the matching reader runs on it: sqlite_verify(),
 * tar_restore() into a scratch folder, or stream_file(). A reader may accept or refuse the
 * file; it must never crash. The PRNG is seeded, so a run can be repeated exactly. */
#define main altsql_restore_main
#include "../altsql_restore.c"
#undef main

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {                    /* xorshift64* */
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static size_t rndn(size_t n) { return n ? (size_t)rnd() % n : 0; }

static void mutate(buf *b) {
    int k, rounds = 1 + (int)rndn(4);
    static const uint8_t edge[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE };
    static const uint32_t edge32[] = { 0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFu, 0x10000u, 512, 65536 };
    for (k = 0; k < rounds && b->n; k++) {
        size_t at = rndn(b->n);
        switch (rndn(7)) {
        case 0: b->p[at] ^= (uint8_t)(1u << rndn(8)); break;
        case 1: b->p[at] = edge[rndn(sizeof edge)]; break;
        case 2: if (at + 2 <= b->n) { uint32_t v = edge32[rndn(9)]; b->p[at] = (uint8_t)(v >> 8); b->p[at + 1] = (uint8_t)v; } break;
        case 3: if (at + 4 <= b->n) { uint32_t v = edge32[rndn(9)]; b->p[at] = (uint8_t)(v >> 24); b->p[at + 1] = (uint8_t)(v >> 16); b->p[at + 2] = (uint8_t)(v >> 8); b->p[at + 3] = (uint8_t)v; } break;
        case 4: b->n = at; break;                                       /* a cut */
        case 5: {                                                       /* a block copied over another */
            size_t from = rndn(b->n), len = 1 + rndn(1024);
            if (from + len > b->n) len = b->n - from;
            if (at + len > b->n) len = b->n - at;
            memmove(b->p + at, b->p + from, len);
            break;
        }
        default: {                                                      /* bytes inserted */
            size_t len = 1 + rndn(64), i;
            buf_need(b, len);
            memmove(b->p + at + len, b->p + at, b->n - at);
            for (i = 0; i < len; i++) b->p[at + i] = (uint8_t)rnd();
            b->n += len;
        }
        }
    }
}

typedef struct seed { char path[AR_PATH]; int kind; uint8_t *d; size_t n; } seed;   /* kind: 1 sqlite, 2 tar, 3 gz */

static int sink_cb(void *ctx, const uint8_t *p, size_t n) { (void)ctx; (void)p; (void)n; return 0; }

int main(int argc, char **argv) {
    seed seeds[64];
    int ns = 0, i;
    long iters, it, accepted[4] = { 0 }, refused[4] = { 0 }, tried[4] = { 0 };
    DIR *dir;
    struct dirent *e;
    char tmpf[AR_PATH], tmpd[AR_PATH];
    if (argc < 3) { fprintf(stderr, "usage: fuzz_readers SEEDDIR ITERATIONS [RANDOMSEED]\n"); return 2; }
    iters = atol(argv[2]);
    if (argc > 3) g_rng ^= (uint64_t)strtoull(argv[3], NULL, 10) * 0x100000001B3ull;
    crc_init();
    if (!(dir = opendir(argv[1]))) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    while ((e = readdir(dir)) != NULL && ns < 64) {
        size_t l = strlen(e->d_name);
        int kind = l > 7 && !strcmp(e->d_name + l - 7, ".sqlite") ? 1 : l > 4 && !strcmp(e->d_name + l - 4, ".tar") ? 2
                 : l > 3 && !strcmp(e->d_name + l - 3, ".gz") ? 3 : 0;
        if (!kind) continue;
        snprintf(seeds[ns].path, sizeof seeds[ns].path, "%s/%s", argv[1], e->d_name);
        seeds[ns].kind = kind;
        if (!(seeds[ns].d = (uint8_t *)read_all(seeds[ns].path, &seeds[ns].n))) continue;
        ns++;
    }
    closedir(dir);
    if (!ns) { fprintf(stderr, "no seeds in %s\n", argv[1]); return 2; }
    snprintf(tmpf, sizeof tmpf, "%s/fuzz-%d.bin", "build", (int)getpid());
    snprintf(tmpd, sizeof tmpd, "%s/fuzz-%d.d", "build", (int)getpid());
    for (it = 0; it < iters; it++) {
        seed *s = &seeds[rndn((size_t)ns)];
        buf b = { 0 };
        int rc;
        buf_add(&b, s->d, s->n);
        mutate(&b);
        if (write_file(tmpf, b.p, b.n)) { fprintf(stderr, "%s\n", g_err); return 2; }
        g_err[0] = 0;
        if (s->kind == 1) {
            items out = { 0 };
            strs notes = { 0 };
            rc = sqlite_verify(tmpf, &out, &notes);
            items_free(&out);
            strs_free(&notes);
        } else if (s->kind == 2) {
            fentries files = { 0 };
            items out = { 0 };
            int gz;
            mkdirs(tmpd, 0755);
            rc = tar_restore(tmpf, tmpd, &files, &gz);
            if (!rc) fent_items(&files, &out);
            fent_free(&files);
            items_free(&out);
            rmtree(tmpd);
        } else {
            int gz;
            rc = stream_file(tmpf, sink_cb, NULL, &gz);
        }
        tried[s->kind]++;
        if (rc) refused[s->kind]++; else accepted[s->kind]++;
        buf_free(&b);
    }
    unlink(tmpf);
    printf("seeds: %d files from %s\n", ns, argv[1]);
    for (i = 0; i < ns; i++) printf("  %s (%lu bytes)\n", base_name(seeds[i].path), (unsigned long)seeds[i].n);
    printf("damaged SQLite files: %ld (refused %ld, accepted %ld)\n", tried[1], refused[1], accepted[1]);
    printf("damaged tar archives: %ld (refused %ld, accepted %ld)\n", tried[2], refused[2], accepted[2]);
    printf("damaged gzip files:   %ld (refused %ld, accepted %ld)\n", tried[3], refused[3], accepted[3]);
    printf("no crash, no sanitizer report in %ld runs\n", iters);
    for (i = 0; i < ns; i++) free(seeds[i].d);
    return 0;
}
