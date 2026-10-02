/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql DB: unit tests and model tests.
 *   test_db            the full run
 *   test_db quick      fewer seeds
 * Model tests: random runs of put, get, delete, scan, commit, rollback and
 * reopen against a plain in-memory model, compared after every step. Seeded,
 * so any failure replays. */
#include "dbtest.h"
#include <sys/wait.h>

/* ---- basics ---- */
static int t_basic(void) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(1 << 20);
    uint32_t a, b2;
    char buf[64];
    size_t vn;
    int rc;
    ram_new(&rf, 1 << 24, 1 << 24);
    rc = db_open_ram(&db, &rf, mem, 1 << 20, 0);
    CHECK(rc == ALTSQL_OK, "open: %d %s", rc, altsql_db_errmsg(db));
    CHECK(altsql_db_bucket(db, "a", 0, &a) == ALTSQL_NOTFOUND, "bucket should not exist");
    CHECK(altsql_db_bucket(db, "a", 1, &a) == ALTSQL_OK && a == 64, "create bucket a: %u", a);
    CHECK(altsql_db_bucket(db, "b", 1, &b2) == ALTSQL_OK && b2 == 65, "create bucket b");
    CHECK(altsql_db_put(db, a, "k1", 2, "hello", 5) == ALTSQL_OK, "put");
    CHECK(altsql_db_get(db, a, "k1", 2, buf, sizeof buf, &vn) == ALTSQL_OK && vn == 5 && !memcmp(buf, "hello", 5), "get");
    CHECK(altsql_db_get(db, b2, "k1", 2, buf, sizeof buf, &vn) == ALTSQL_NOTFOUND, "buckets are separate");
    CHECK(altsql_db_get(db, a, "k1", 2, buf, 3, &vn) == ALTSQL_DB_SHORT && vn == 5, "short buffer reports the length");
    CHECK(altsql_db_get(db, a, "k1", 2, NULL, 0, &vn) == ALTSQL_DB_SHORT && vn == 5, "length only");
    CHECK(altsql_db_get(db, 1, "k1", 2, buf, sizeof buf, &vn) == ALTSQL_MISUSE, "the catalog is not a bucket");
    CHECK(altsql_db_get(db, 99, "k1", 2, buf, sizeof buf, &vn) == ALTSQL_MISUSE, "unknown bucket");
    CHECK(altsql_db_del(db, a, "k1", 2) == ALTSQL_OK, "del");
    CHECK(altsql_db_del(db, a, "k1", 2) == ALTSQL_NOTFOUND, "del again");
    CHECK(altsql_db_get(db, a, "k1", 2, buf, sizeof buf, &vn) == ALTSQL_NOTFOUND, "gone");
    /* keys at the limit */
    {
        uint8_t big[2000];
        memset(big, 'x', sizeof big);
        CHECK(altsql_db_put(db, a, big, db->maxkey - 1, "v", 1) == ALTSQL_OK, "longest key");
        CHECK(altsql_db_put(db, a, big, db->maxkey, "v", 1) == ALTSQL_TOOBIG, "key too long");
    }
    /* a read transaction cannot write; a failed write leaves it usable */
    CHECK(altsql_db_begin(db, 0) == ALTSQL_OK, "begin read");
    CHECK(altsql_db_put(db, a, "x", 1, "y", 1) == ALTSQL_MISUSE, "write in a read transaction");
    CHECK(altsql_db_commit(db) == ALTSQL_OK, "end read");
    CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
    CHECK(altsql_db_begin(db, 1) == ALTSQL_MISUSE, "nested begin");
    CHECK(altsql_db_put(db, a, "t1", 2, "one", 3) == ALTSQL_OK, "put in txn");
    CHECK(altsql_db_rollback(db) == ALTSQL_OK, "rollback");
    CHECK(altsql_db_get(db, a, "t1", 2, buf, sizeof buf, &vn) == ALTSQL_NOTFOUND, "rolled back");
    /* reopen: buckets and data survive */
    CHECK(altsql_db_put(db, a, "p", 1, "persist", 7) == ALTSQL_OK, "put");
    altsql_db_close(db);
    rc = db_open_ram(&db, &rf, mem, 1 << 20, 0);
    CHECK(rc == ALTSQL_OK, "reopen: %d", rc);
    CHECK(altsql_db_bucket(db, "a", 0, &a) == ALTSQL_OK && a == 64, "bucket after reopen");
    CHECK(altsql_db_get(db, a, "p", 1, buf, sizeof buf, &vn) == ALTSQL_OK && vn == 7, "data after reopen");
    CHECK(check_slots(db, 1) == 0, "checks");
    CHECK(pins_clear(db), "pins");
    altsql_db_close(db);
    ram_free(&rf);
    free(mem);
    return 0;
}

/* ---- the model test ---- */
typedef struct mt {
    altsql_db *db;
    ramfile rf;
    void *mem;
    size_t msize;
    uint32_t ps, nb, b[3];
    model c, w;             /* committed and working state */
    int intx;
    uint32_t s;
    unsigned long ops, commits, rollbacks, reopens, scans, bigs;
} mt;

static uint32_t rkey(mt *t, uint8_t *k) {
    uint32_t r = xs(&t->s) % 100, n, i;
    if (r < 60) {                                   /* "k" + number: shared prefixes */
        n = (uint32_t)sprintf((char *)k, "k%u", xs(&t->s) % 600);
    } else if (r < 85) {                            /* time-ordered, binary, big-endian */
        uint32_t v = xs(&t->s) % 2000;
        k[0] = 0; k[1] = 0; k[2] = (uint8_t)(v >> 8); k[3] = (uint8_t)v; n = 4;
    } else {                                        /* random bytes, any length to 40 */
        n = xs(&t->s) % 41;
        for (i = 0; i < n; i++) k[i] = (uint8_t)(xs(&t->s) % 4);
    }
    return n;
}
static uint32_t rlen(mt *t) {
    uint32_t r = xs(&t->s) % 100;
    if (r < 70) return xs(&t->s) % 40;
    if (r < 90) return xs(&t->s) % (t->ps / 3);
    t->bigs++;
    if (r < 98) return t->ps / 4 + xs(&t->s) % (3 * t->ps);
    return xs(&t->s) % 70000;
}

static int mt_reopen(mt *t) {
    int rc;
    altsql_db_close(t->db);
    rc = db_open_ram(&t->db, &t->rf, t->mem, t->msize, t->ps);
    CHECK(rc == ALTSQL_OK, "reopen: %d %s", rc, altsql_db_errmsg(t->db));
    t->reopens++;
    return 0;
}

static int mt_full(mt *t) {
    uint32_t i;
    model *m = t->intx ? &t->w : &t->c;
    for (i = 0; i < t->nb; i++) if (verify_bucket(t->db, m, t->b[i])) return 1;
    if (!t->intx && check_slots(t->db, 1)) return 1;
    return 0;
}

static int mt_run(uint32_t seed, uint32_t ps, size_t msize, uint32_t steps) {
    mt t;
    uint32_t step, i;
    int rc;
    memset(&t, 0, sizeof t);
    t.s = seed * 7919u + 13;
    t.ps = ps;
    t.msize = msize;
    t.mem = malloc(msize);
    ram_new(&t.rf, 1u << 26, 1u << 26);
    rc = db_open_ram(&t.db, &t.rf, t.mem, msize, ps);
    CHECK(rc == ALTSQL_OK, "seed %u: open %d %s", seed, rc, altsql_db_errmsg(t.db));
    t.nb = 3;
    for (i = 0; i < t.nb; i++) {
        char name[16];
        sprintf(name, "b%u", i);
        CHECK(altsql_db_bucket(t.db, name, 1, &t.b[i]) == ALTSQL_OK, "bucket");
    }
    for (step = 0; step < steps; step++) {
        uint32_t r = xs(&t.s) % 1000, bi = xs(&t.s) % t.nb, b = t.b[bi], kn, vn, vs;
        uint8_t k[64];
        model *m = t.intx ? &t.w : &t.c;
        int f;
        t.ops++;
        kn = rkey(&t, k);
        if (r < 420) {                                     /* put */
            vn = rlen(&t);
            vs = xs(&t.s);
            fill(g_vbuf, vs, vn);
            rc = altsql_db_put(t.db, b, k, kn, g_vbuf, vn);
            CHECK(rc == ALTSQL_OK, "seed %u step %u: put %d %s", seed, step, rc, altsql_db_errmsg(t.db));
            mput(m, b, k, kn, vs, vn);
        } else if (r < 580) {                              /* delete */
            int had = mdel(m, b, k, kn);
            rc = altsql_db_del(t.db, b, k, kn);
            CHECK(rc == (had ? ALTSQL_OK : ALTSQL_NOTFOUND), "seed %u step %u: del %d (model %d) %s", seed, step, rc, had, altsql_db_errmsg(t.db));
        } else if (r < 760) {                              /* get */
            size_t got;
            uint32_t ix = mfind(m, b, k, kn, &f);
            if (!f && m->n && xs(&t.s) % 2) {              /* mostly look up keys that exist */
                ix = xs(&t.s) % m->n;
                b = m->e[ix].b; kn = m->e[ix].kn; memcpy(k, m->e[ix].k, kn); f = 1;
            }
            rc = altsql_db_get(t.db, b, k, kn, g_vbuf, sizeof g_vbuf, &got);
            if (!f) CHECK(rc == ALTSQL_NOTFOUND, "seed %u step %u: get of a missing key: %d", seed, step, rc);
            else {
                CHECK(rc == ALTSQL_OK && got == m->e[ix].vn, "seed %u step %u: get %d len %zu want %u (%s)", seed, step, rc, got, m->e[ix].vn, altsql_db_errmsg(t.db));
                fill(g_vexp, m->e[ix].vs, m->e[ix].vn);
                CHECK(!memcmp(g_vbuf, g_vexp, got), "seed %u step %u: value differs", seed, step);
            }
        } else if (r < 800) {                              /* begin */
            if (!t.intx) {
                CHECK(altsql_db_begin(t.db, 1) == ALTSQL_OK, "begin");
                mcopy(&t.w, &t.c);
                t.intx = 1;
            }
        } else if (r < 830) {                              /* commit */
            if (t.intx) {
                rc = altsql_db_commit(t.db);
                CHECK(rc == ALTSQL_OK, "seed %u step %u: commit %d %s", seed, step, rc, altsql_db_errmsg(t.db));
                mcopy(&t.c, &t.w);
                t.intx = 0;
                t.commits++;
            }
        } else if (r < 845) {                              /* rollback */
            if (t.intx) {
                CHECK(altsql_db_rollback(t.db) == ALTSQL_OK, "rollback");
                t.intx = 0;
                t.rollbacks++;
            }
        } else if (r < 860) {                              /* reopen */
            if (!t.intx && mt_reopen(&t)) return 1;
        } else if (r < 960) {                              /* a short scan from a random key */
            altsql_db_cursor c;
            uint32_t ix = mfind(m, b, k, kn, &f), n = xs(&t.s) % 30;
            int dir = xs(&t.s) % 2 ? 1 : -1;
            size_t ckn = 0;
            const void *ck = 0;
            t.scans++;
            rc = altsql_db_seek(&c, t.db, b, k, kn);
            if (ix >= m->n || m->e[ix].b != b) { CHECK(rc == ALTSQL_NOTFOUND, "seed %u step %u: seek past the end: %d", seed, step, rc); continue; }
            CHECK(rc == ALTSQL_OK, "seed %u step %u: seek %d", seed, step, rc);
            while (n--) {
                altsql_db_key(&c, &ck, &ckn);
                CHECK(ckn == m->e[ix].kn && !memcmp(ck, m->e[ix].k, ckn), "seed %u step %u: scan key differs", seed, step);
                if (xs(&t.s) % 8 == 0) {                   /* a write while the cursor is open */
                    uint8_t k2[64];
                    uint32_t kn2 = rkey(&t, k2), vs2 = xs(&t.s), vn2 = xs(&t.s) % 30;
                    fill(g_vbuf, vs2, vn2);
                    if (xs(&t.s) % 2) {
                        CHECK(altsql_db_put(t.db, b, k2, kn2, g_vbuf, vn2) == ALTSQL_OK, "put during scan");
                        mput(m, b, k2, kn2, vs2, vn2);
                    } else {
                        int had = mdel(m, b, k2, kn2);
                        CHECK(altsql_db_del(t.db, b, k2, kn2) == (had ? ALTSQL_OK : ALTSQL_NOTFOUND), "del during scan");
                    }
                    /* the current entry may be gone: the model position is the first key >= it */
                    ix = mfind(m, b, (const uint8_t *)ck, (uint32_t)ckn, &f);
                    if (!f) {                              /* deleted: next lands on the next key, prev on the one before */
                        if (dir > 0) { ix--; }
                    }
                }
                rc = dir > 0 ? altsql_db_next(&c) : altsql_db_prev(&c);
                if (dir > 0) ix++; else ix--;
                if (ix >= m->n || m->e[ix].b != b || ix == (uint32_t)-1) {
                    CHECK(rc == ALTSQL_NOTFOUND, "seed %u step %u: scan should end: %d", seed, step, rc);
                    break;
                }
                CHECK(rc == ALTSQL_OK, "seed %u step %u: scan step %d (%s)", seed, step, rc, altsql_db_errmsg(t.db));
            }
        } else {                                           /* full check */
            if (mt_full(&t)) { printf("  seed %u step %u\n", seed, step); return 1; }
        }
        CHECK(pins_clear(t.db), "seed %u step %u: a page stayed pinned", seed, step);
    }
    if (t.intx) { CHECK(altsql_db_commit(t.db) == ALTSQL_OK, "final commit"); mcopy(&t.c, &t.w); t.intx = 0; }
    if (mt_full(&t)) return 1;
    if (mt_reopen(&t) || mt_full(&t)) return 1;
    altsql_db_close(t.db);
    ram_free(&t.rf);
    free(t.mem);
    mfree(&t.c);
    mfree(&t.w);
    return 0;
}

/* ---- time-ordered appends fill their pages ---- */
static int t_append(void) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(1 << 22);
    uint32_t b, i;
    altsql_db_info info;
    uint8_t k[8], v[20];
    ram_new(&rf, 1 << 26, 1 << 26);
    CHECK(db_open_ram(&db, &rf, mem, 1 << 22, 4096) == ALTSQL_OK, "open");
    CHECK(altsql_db_bucket(db, "ts", 1, &b) == ALTSQL_OK, "bucket");
    CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
    for (i = 0; i < 100000; i++) {
        k[0] = (uint8_t)(i >> 24); k[1] = (uint8_t)(i >> 16); k[2] = (uint8_t)(i >> 8); k[3] = (uint8_t)i;
        memset(v, (int)i, sizeof v);
        CHECK(altsql_db_put(db, b, k, 4, v, sizeof v) == ALTSQL_OK, "append %u", i);
    }
    CHECK(altsql_db_commit(db) == ALTSQL_OK, "commit");
    altsql_db_info_get(db, &info);
    /* 100,000 cells of 27 bytes + 2 for the offset = 2.9 MB of cells: about 710 full 4 KB pages */
    printf("  append: 100,000 keys in order -> %u pages, depth %u\n", info.pages, info.depth);
    CHECK(info.pages < 760, "appends in order should fill their pages: %u pages", info.pages);
    CHECK(check_slots(db, 1) == 0, "checks");
    altsql_db_close(db);
    ram_free(&rf);
    free(mem);
    return 0;
}

/* ---- freed pages come back: transactions that free far more pages than the
 * free-list window holds, and a file that stops growing ---- */
static int t_reuse(void) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(48 * 1024);
    uint32_t b, i, round, pages[10];
    uint8_t k[8];
    altsql_db_info info;
    model m;
    memset(&m, 0, sizeof m);
    ram_new(&rf, 1 << 26, 1 << 26);
    CHECK(db_open_ram(&db, &rf, mem, 48 * 1024, 512) == ALTSQL_OK, "open");
    CHECK(db->flw < 400, "the window should be small here (%u)", db->flw);
    CHECK(altsql_db_bucket(db, "r", 1, &b) == ALTSQL_OK, "bucket");
    for (round = 0; round < 10; round++) {
        CHECK(altsql_db_begin(db, 1) == ALTSQL_OK, "begin");
        for (i = 0; i < 4000; i++) {
            uint32_t vs = round * 100000 + i, vn = 60 + (i % 40);
            k[0] = (uint8_t)(i >> 8); k[1] = (uint8_t)i;
            fill(g_vbuf, vs, vn);
            CHECK(altsql_db_put(db, b, k, 2, g_vbuf, vn) == ALTSQL_OK, "round %u put %u: %s", round, i, altsql_db_errmsg(db));
            mput(&m, b, k, 2, vs, vn);
        }
        CHECK(altsql_db_commit(db) == ALTSQL_OK, "commit: %s", altsql_db_errmsg(db));
        altsql_db_info_get(db, &info);
        pages[round] = info.pages;
        if (check_slots(db, 1)) return 1;
    }
    printf("  reuse: ten rewrites of 4,000 entries, %u-entry free-list window: %u pages after round 3, %u after round 6, %u after round 10\n",
           db->flw, pages[2], pages[5], pages[9]);
    /* Each rewrite frees about as many pages as it writes. They wait two commits, so the file settles at
     * about three versions of the data plus the free list, and then stays there: no creep. */
    CHECK(pages[9] <= pages[5] + pages[5] / 100, "the file keeps growing: %u pages after round 6, %u after round 10", pages[5], pages[9]);
    if (verify_bucket(db, &m, b)) return 1;
    altsql_db_close(db);
    ram_free(&rf);
    mfree(&m);
    free(mem);
    return 0;
}

/* ---- a damaged commit header: its checksum gives it away, the other header is used ---- */
static int t_header_damage(void) {
    ramfile rf;
    altsql_db *db;
    void *mem = malloc(256 * 1024);
    uint32_t b, i, slot;
    uint64_t last;
    altsql_db_info info;
    uint8_t k[4];
    size_t vn;
    ram_new(&rf, 1 << 22, 1 << 22);
    CHECK(db_open_ram(&db, &rf, mem, 256 * 1024, 1024) == ALTSQL_OK, "open");
    CHECK(altsql_db_bucket(db, "h", 1, &b) == ALTSQL_OK, "bucket");
    for (i = 0; i < 3; i++) {                               /* three commits, each changing key 0 */
        k[0] = 0; k[1] = 0; k[2] = 0; k[3] = (uint8_t)i;
        CHECK(altsql_db_put(db, b, "key", 3, k, 4) == ALTSQL_OK, "put");
    }
    altsql_db_info_get(db, &info);
    last = info.txn;
    altsql_db_close(db);
    slot = (uint32_t)(last & 1);
    for (i = 16; i < 60; i++) {                             /* any byte of the newest header but the magic and version */
        uint8_t keep = rf.mem[slot * 1024 + i];
        rf.mem[slot * 1024 + i] ^= 0x10;
        CHECK(db_open_ram(&db, &rf, mem, 256 * 1024, 1024) == ALTSQL_OK, "open with byte %u of the header damaged: %s", i, altsql_db_errmsg(db));
        altsql_db_info_get(db, &info);
        CHECK(info.txn == last - 1, "byte %u damaged: opened at transaction %llu, want %llu", i, (unsigned long long)info.txn, (unsigned long long)(last - 1));
        CHECK(altsql_db_get(db, b, "key", 3, k, sizeof k, &vn) == ALTSQL_OK && vn == 4 && k[3] == 1, "byte %u damaged: the commit before", i);
        if (check_slots(db, 0)) return 1;
        altsql_db_close(db);
        rf.mem[slot * 1024 + i] = keep;
    }
    printf("  damaged header: each of 44 bytes changed in turn, the file opens at the commit before: ok\n");
    ram_free(&rf);
    free(mem);
    return 0;
}

/* ---- the POSIX file and its lock ---- */
static int t_posix(void) {
    altsql_db_file f;
    altsql_db_posix p;
    altsql_db *db;
    altsql_db_config cfg;
    void *mem = malloc(1 << 20);
    const char *path = "build/test_posix.adb";
    uint32_t b;
    char buf[16];
    size_t vn;
    pid_t pid;
    int st;
    remove(path);
    CHECK(altsql_db_posix_open(&f, &p, path, 1) == ALTSQL_OK, "posix open");
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = mem; cfg.mem_size = 1 << 20; cfg.create = 1;
    CHECK(altsql_db_open(&db, &f, &cfg) == ALTSQL_OK, "open: %s", altsql_db_errmsg(db));
    CHECK(altsql_db_bucket(db, "a", 1, &b) == ALTSQL_OK, "bucket");
    CHECK(altsql_db_put(db, b, "key", 3, "value", 5) == ALTSQL_OK, "put");
    pid = fork();
    if (pid == 0) {                                  /* another process finds the file locked */
        altsql_db_file f2;
        altsql_db_posix p2;
        _exit(altsql_db_posix_open(&f2, &p2, path, 0) == ALTSQL_DB_BUSY ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a second process should find the file busy");
    altsql_db_close(db);
    altsql_db_posix_close(&p);
    CHECK(altsql_db_posix_open(&f, &p, path, 0) == ALTSQL_OK, "posix reopen");
    cfg.create = 0;
    CHECK(altsql_db_open(&db, &f, &cfg) == ALTSQL_OK, "reopen");
    CHECK(altsql_db_bucket(db, "a", 0, &b) == ALTSQL_OK, "bucket");
    CHECK(altsql_db_get(db, b, "key", 3, buf, sizeof buf, &vn) == ALTSQL_OK && vn == 5, "get after reopen");
    altsql_db_close(db);
    altsql_db_posix_close(&p);
    remove(path);
    free(mem);
    return 0;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    uint32_t seed, nseeds = quick ? 12 : 60;
    unsigned long runs = 0;
    printf("AltSql DB %s: unit and model tests\n", ALTSQL_DB_VERSION);
    if (t_basic()) return 1;
    printf("  basics: ok\n");
    if (t_posix()) return 1;
    printf("  POSIX file and lock: ok\n");
    if (t_append()) return 1;
    if (t_reuse() || t_header_damage()) return 1;
    for (seed = 1; seed <= nseeds; seed++) {
        /* small pages with a small cache: deep trees, early writes; 4 KB pages: big values */
        if (mt_run(seed, 512, 48 * 1024, 3000)) return 1;
        if (mt_run(seed, 4096, 192 * 1024, 2000)) return 1;
        if (mt_run(seed, 1024, 1 << 20, 2000)) return 1;
        runs += 3;
    }
    printf("  model tests: %lu runs, %u seeds, page sizes 512, 1024 and 4096: ok\n", runs, nseeds);
    printf("all passed\n");
    return g_fail ? 1 : 0;
}
