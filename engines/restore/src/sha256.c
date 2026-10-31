/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), written for AltSql Restore. */

typedef struct sha256 { uint32_t h[8]; uint64_t len; uint8_t blk[64]; uint32_t n; } sha256;

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = SHA_ROR(w[i - 15], 7) ^ SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA_ROR(w[i - 2], 17) ^ SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (SHA_ROR(e, 6) ^ SHA_ROR(e, 11) ^ SHA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        t2 = (SHA_ROR(a, 2) ^ SHA_ROR(a, 13) ^ SHA_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

static void sha256_add(sha256 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    if (s->n) {
        while (n && s->n < 64) { s->blk[s->n++] = *p++; n--; }
        if (s->n < 64) return;
        sha256_block(s, s->blk);
        s->n = 0;
    }
    while (n >= 64) { sha256_block(s, p); p += 64; n -= 64; }
    while (n--) s->blk[s->n++] = *p++;
}

static void sha256_done(sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    int i;
    sha256_add(s, &pad, 1);
    while (s->n != 56) sha256_add(s, &z, 1);
    for (i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_add(s, l, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void hex_of(const uint8_t *d, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * n] = 0;
}

static void sha256_hex_done(sha256 *s, char out[65]) {
    uint8_t d[32];
    sha256_done(s, d);
    hex_of(d, 32, out);
}

static void sha256_hex(const void *data, size_t n, char out[65]) {
    sha256 s;
    sha256_init(&s);
    sha256_add(&s, data, n);
    sha256_hex_done(&s, out);
}

static void hmac_sha256_hex(const uint8_t *key, size_t kn, const void *msg, size_t mn, char out[65]) {
    uint8_t k[64], ipad[64], opad[64], inner[32], d[32];
    sha256 s;
    int i;
    memset(k, 0, sizeof k);
    if (kn > 64) { sha256_init(&s); sha256_add(&s, key, kn); sha256_done(&s, k); }
    else if (kn) memcpy(k, key, kn);
    for (i = 0; i < 64; i++) { ipad[i] = (uint8_t)(k[i] ^ 0x36); opad[i] = (uint8_t)(k[i] ^ 0x5c); }
    sha256_init(&s); sha256_add(&s, ipad, 64); sha256_add(&s, msg, mn); sha256_done(&s, inner);
    sha256_init(&s); sha256_add(&s, opad, 64); sha256_add(&s, inner, 32); sha256_done(&s, d);
    hex_of(d, 32, out);
}

/* The SHA-256 of a file's bytes as they are on disk. */
static int sha256_file(const char *path, char out[65], uint64_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t tmp[65536];
    size_t k;
    sha256 s;
    if (!f) return seterr("cannot read %s: %s", path, strerror(errno));
    sha256_init(&s);
    *size = 0;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) { sha256_add(&s, tmp, k); *size += k; }
    if (ferror(f)) { fclose(f); return seterr("cannot read %s: %s", path, strerror(errno)); }
    fclose(f);
    sha256_hex_done(&s, out);
    return 0;
}
