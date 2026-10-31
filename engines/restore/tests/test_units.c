/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql Restore: unit tests of the parts that need no outside software.
 *   SHA-256 and HMAC-SHA256 against published test vectors (FIPS 180-4 examples, RFC 4231)
 *   CRC-32 of "123456789"
 *   the inflater on stored, fixed-Huffman, dynamic-Huffman and two-member gzip files
 *   every cut-short gzip file fails (all lengths of the small files, a sample of the large)
 *   flipped bits in a gzip file fail, or change nothing (a header byte no one reads)
 *   unsafe archive paths are refused
 *   pax records that run past the header's data are not read past it (review, 0.5.1)
 *   psql's own commands in a plain dump are found, and the copy for psql is right (review, 0.5.1)
 * Run from engines/restore (make test does). */
#define main altsql_restore_main
#include "../altsql_restore.c"
#undef main

static int g_fails = 0, g_checks = 0;
#define T(cond, ...) do { g_checks++; if (!(cond)) { g_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct sink { sha256 s; uint64_t n; } sink;
static int sink_cb(void *ctx, const uint8_t *p, size_t n) { sink *k = (sink *)ctx; sha256_add(&k->s, p, n); k->n += n; return 0; }

/* Unpacks path; 0 with the output's SHA-256 in hex, or -1. */
static int gunzip_hash(const char *path, char hex[65], uint64_t *n) {
    sink k;
    int gz, rc;
    sha256_init(&k.s);
    k.n = 0;
    g_err[0] = 0;
    rc = stream_file(path, sink_cb, &k, &gz);
    sha256_hex_done(&k.s, hex);
    if (n) *n = k.n;
    return rc;
}

static uint8_t *load(const char *path, size_t *n) { return (uint8_t *)read_all(path, n); }

/* Every step-th prefix must fail to unpack, except a cut exactly at the end of a gzip
 * member (boundary), which is a whole gzip file by itself: no reader of gzip can tell. The
 * formats inside catch that one (tar end blocks, SQLite's page count, a dump's last line). */
static int cuts_fail(const uint8_t *d, size_t n, size_t step, size_t boundary, const char *tmp, long *tried) {
    size_t len;
    int bad = 0;
    for (len = 0; len < n; len += step) {
        char hex[65];
        if (write_file(tmp, d, len)) return -1;
        (*tried)++;
        if (len >= 2 && gunzip_hash(tmp, hex, NULL) == 0) {
            if (len == boundary) { printf("  a cut at the end of the first of two gzip members (%lu bytes) is a whole gzip file, as expected\n", (unsigned long)len); continue; }
            bad++;
            printf("  a cut at %lu bytes of %lu unpacked without error\n", (unsigned long)len, (unsigned long)n);
        }
    }
    return bad;
}

int main(void) {
    char hex[65];
    uint64_t n;
    const char *data_sha = "d3a79dd519ac41263da23596a666172d7e330ee0b962f3222ad8af7a7709e3b0";   /* the vec_* text */
    const char *tmp = "build/unit_tmp.gz";
    crc_init();
    printf("AltSql Restore %s: unit tests\n", ALTSQL_RESTORE_VERSION);

    /* SHA-256 */
    sha256_hex("abc", 3, hex);
    T(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) = %s", hex);
    sha256_hex("", 0, hex);
    T(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('') = %s", hex);
    sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    T(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448 bits) = %s", hex);
    {
        sha256 s;
        int i;
        sha256_init(&s);
        for (i = 0; i < 1000000; i++) sha256_add(&s, "a", 1);
        sha256_hex_done(&s, hex);
        T(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(a x 1e6) = %s", hex);
    }
    hmac_sha256_hex((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, hex);
    T(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac (RFC 4231 case 2) = %s", hex);
    {
        uint8_t key[131];
        memset(key, 0xaa, sizeof key);
        hmac_sha256_hex(key, sizeof key, "Test Using Larger Than Block-Size Key - Hash Key First", 54, hex);
        T(!strcmp(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac (RFC 4231 case 6) = %s", hex);
    }
    T(crc32_add(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32(123456789)");
    printf("  SHA-256, HMAC-SHA256 and CRC-32 test vectors: done\n");

    /* the inflater */
    T(gunzip_hash("tests/fixtures/vec_stored.gz", hex, &n) == 0 && !strcmp(hex, data_sha) && n == 1960, "stored blocks: %s %s", g_err, hex);
    T(gunzip_hash("tests/fixtures/vec_fixed.gz", hex, &n) == 0 && !strcmp(hex, data_sha), "fixed Huffman: %s %s", g_err, hex);
    T(gunzip_hash("tests/fixtures/vec_multi.gz", hex, &n) == 0 && !strcmp(hex, data_sha), "two members: %s %s", g_err, hex);
    T(gunzip_hash("tests/fixtures/shop_pg.sql.gz", hex, &n) == 0 && n == 247455 &&
      !strcmp(hex, "1fd10f817570a58bf223c6135b2a950f81f7114bdda9a9bc7c28a617c4f793bf"), "dynamic Huffman (gzip -9 of a pg_dump): %s %s", g_err, hex);
    printf("  inflate: stored, fixed and dynamic Huffman blocks, two members: done\n");

    {
        const char *files[] = { "tests/fixtures/vec_stored.gz", "tests/fixtures/vec_fixed.gz", "tests/fixtures/vec_multi.gz", "tests/fixtures/shop_pg.sql.gz" };
        long tried = 0;
        int i, bad = 0;
        for (i = 0; i < 4; i++) {
            size_t len;
            uint8_t *d = load(files[i], &len);
            T(d != NULL, "read %s", files[i]);
            if (!d) continue;
            bad += cuts_fail(d, len, len > 10000 ? 97 : 1, i == 2 ? 74 : 0, tmp, &tried);
            if (len > 10000) {                     /* and the last 64 lengths, one by one */
                size_t k;
                for (k = len - 64; k < len; k++) {
                    char h2[65];
                    write_file(tmp, d, k);
                    tried++;
                    if (gunzip_hash(tmp, h2, NULL) == 0) { bad++; printf("  a cut at %lu bytes unpacked without error\n", (unsigned long)k); }
                }
            }
            free(d);
        }
        T(bad == 0, "%d cut-short gzip files unpacked without error", bad);
        printf("  cut-short gzip files: %ld lengths tried, %d unpacked without an error\n", tried, bad);
    }
    {
        size_t len, i;
        uint8_t *d = load("tests/fixtures/shop_pg.sql.gz", &len);
        uint32_t seed = 12345;
        int caught = 0, harmless = 0, missed = 0;
        for (i = 0; d && i < 2000; i++) {
            size_t pos;
            int bit;
            char h2[65];
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            pos = seed % len;
            bit = (int)((seed >> 20) & 7);
            d[pos] ^= (uint8_t)(1 << bit);
            write_file(tmp, d, len);
            if (gunzip_hash(tmp, h2, NULL) != 0) caught++;
            else if (!strcmp(h2, "1fd10f817570a58bf223c6135b2a950f81f7114bdda9a9bc7c28a617c4f793bf")) harmless++;
            else { missed++; printf("  a flip at byte %lu bit %d gave other data without an error\n", (unsigned long)pos, bit); }
            d[pos] ^= (uint8_t)(1 << bit);
        }
        free(d);
        T(missed == 0, "%d flipped bits gave other data without an error", missed);
        printf("  flipped bits in a gzip file: 2000 tried, %d failed with an error, %d changed nothing (header bytes), %d gave other data silently\n", caught, harmless, missed);
    }
    unlink(tmp);

    /* archive paths */
    {
        char out[256];
        g_err[0] = 0; T(safe_path("../etc/passwd", out, sizeof out) != 0, "../ refused");
        g_err[0] = 0; T(safe_path("a/../../b", out, sizeof out) != 0, "a/../../b refused");
        g_err[0] = 0; T(safe_path("/etc/passwd", out, sizeof out) != 0, "absolute refused");
        g_err[0] = 0; T(safe_path("./a//b/./c", out, sizeof out) == 0 && !strcmp(out, "a/b/c"), "./a//b/./c -> %s", out);
        g_err[0] = 0; T(safe_path("a..b/c", out, sizeof out) == 0 && !strcmp(out, "a..b/c"), "a..b/c kept");
        printf("  archive paths: absolute and \"..\" refused: done\n");
    }

    /* pax records: a record whose length digits leave it shorter than its own header must not
     * be read past the data (a 5-byte over-read under ASan before 0.5.1, when the data filled
     * its buffer exactly: 4096 bytes) */
    {
        buf m = { 0 };
        char out[256] = "", rec[4092];
        memset(rec, 'a', sizeof rec);
        memcpy(rec, "4091 c=", 7);
        rec[4090] = '\n';
        buf_add(&m, rec, 4091);
        buf_add(&m, "0001 ", 5);
        T(m.n == m.cap, "the test needs the data to fill its buffer (%lu of %lu)", (unsigned long)m.n, (unsigned long)m.cap);
        pax_path(&m, out, sizeof out);
        T(out[0] == 0, "no path from a broken record");
        m.n = 0;
        buf_add(&m, "18 path=a/b/c.txt\n", 18);
        out[0] = 0;
        pax_path(&m, out, sizeof out);
        T(!strcmp(out, "a/b/c.txt"), "a good pax path is read: %s", out);
        m.n = 0;
        buf_add(&m, "99999999999999999999 path=x\n", 28);
        out[0] = 0;
        pax_path(&m, out, sizeof out);
        T(out[0] == 0, "a huge record length is refused");
        buf_free(&m);
        printf("  pax records: read only inside the header's data: done\n");
    }

    /* psql's own commands in a plain dump (run on this machine, not in the sandbox, before 0.5.1) */
    {
        static const char *const bad[] = {
            "SELECT 1;\n\\! id\n",
            "SELECT 1; \\! id\n",
            "CREATE TABLE t (a int);\n\\o /tmp/x\n",
            "\\copy t to '/tmp/x'\n",
            "\\i /etc/passwd\n",
            "\\connect \"host=127.0.0.1 dbname=x\"\n",
            "\\connect db otheruser\n",
            "\\connect postgresql://h/db\n",
            "\\restrict k \\\\ \\! id\n",
            "COPY t (a) FROM stdin;\n1\n\\.\n\\! id\n",
            "SELECT $q$ x $q$; \\! id\n",
            "SELECT 'it''s'; \\! id\n",
            "SELECT E'a\\'b'; \\! id\n",
            NULL };
        static const char *const good[] = {
            "SET standard_conforming_strings = on;\nCREATE FUNCTION f() RETURNS text LANGUAGE sql AS $_$ SELECT '\\s\\w' $_$;\n",
            "INSERT INTO t VALUES ('C:\\new\\! x'), (E'it\\'s \\\\! x'), ($$ \\! $$), ($a$ $$ \\o $a$);\n",
            "/* \\! /* nested */ \\o */ SELECT 1; -- \\! comment\n",
            "COPY public.t (a, b) FROM stdin;\n\\N\t\\\\! x\n\\o\tdata\n\\.\nSELECT 1;\n",
            "SET standard_conforming_strings = off;\nINSERT INTO t VALUES ('a\\' \\! b');\n",
            "\\connect shop\n\\connect -reuse-previous=on \"dbname='it''s'\"\n",
            NULL };
        const char *in = "build/unit_dump.sql", *outp = "build/unit_dump.psql";
        int i, nb = 0, ng = 0;
        for (i = 0; bad[i]; i++) {
            write_file(in, bad[i], strlen(bad[i]));
            g_err[0] = 0;
            if (pg_dump_commands(in, NULL, NULL) == 0) printf("  not refused: %s", bad[i]); else nb++;
        }
        T(nb == i, "%d of %d dumps with psql commands refused", nb, i);
        for (i = 0; good[i]; i++) {
            write_file(in, good[i], strlen(good[i]));
            g_err[0] = 0;
            if (pg_dump_commands(in, NULL, NULL) != 0) printf("  refused: %s  (%s)\n", good[i], g_err); else ng++;
        }
        T(ng == i, "%d of %d dumps without psql commands accepted", ng, i);
        {   /* the copy for psql: the dump's own \restrict lines out, ours in, \connect between */
            const char *d = "\\restrict abc\nSELECT 1;\n\\unrestrict abc\n\\connect shop\n\\restrict abc\nSELECT 'x';\n\\unrestrict abc\n";
            const char *want = "\\restrict KEY\nSELECT 1;\n\\unrestrict KEY\n\\connect shop\n\\restrict KEY\nSELECT 'x';\n";
            char *got;
            write_file(in, d, strlen(d));
            g_err[0] = 0;
            T(pg_dump_commands(in, outp, "KEY") == 0, "the copy is written: %s", g_err);
            got = read_all(outp, NULL);
            T(got && !strcmp(got, want), "the copy for psql is right:\n%s", got ? got : "(none)");
            free(got);
            g_err[0] = 0;
            T(pg_dump_commands(in, outp, NULL) == 0, "the copy without a key is written");
            got = read_all(outp, NULL);
            T(got && !strcmp(got, "SELECT 1;\n\\connect shop\nSELECT 'x';\n"), "without \\restrict, the dump's own lines are left out:\n%s", got ? got : "(none)");
            free(got);
        }
        T(psql_restricts("psql (PostgreSQL) 16.15 (Ubuntu 16.15-0ubuntu0.24.04.1)"), "16.15 has \\restrict");
        T(psql_restricts("psql (PostgreSQL) 18.0"), "18.0 has \\restrict");
        T(psql_restricts("psql (PostgreSQL) 13.22"), "13.22 has \\restrict");
        T(!psql_restricts("psql (PostgreSQL) 16.9"), "16.9 has not");
        T(!psql_restricts("psql (PostgreSQL) 17.5"), "17.5 has not");
        T(!psql_restricts("psql (PostgreSQL) 12.22"), "12 has not");
        T(!psql_restricts(""), "unknown: not assumed");
        unlink(in);
        unlink(outp);
        printf("  psql commands in plain dumps: found and refused, the copy for psql right: done\n");
    }
    printf("%d checks, %d failed\n%s\n", g_checks, g_fails, g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
