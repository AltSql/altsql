/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * AltSql implementation
 * ====================================================================== */
#include <string.h>
#include <stdarg.h>
#if ALTSQL_ENABLE_SQL || ALTSQL_ENABLE_TEXT
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#endif

/* Helpers that some build configurations do not use. */
#if defined(__GNUC__) || defined(__clang__)
#define AS_FN static __attribute__((unused))
#else
#define AS_FN static
#endif

/* ---- On-flash format ----------------------------------------------------
 * Sector header, 32 bytes:
 *   0 magic "ASQL"  4 version  5 write_align  8 seq  12 sector_size
 *   16 sector_count  20 gc_of: seq of the sector being reclaimed when this
 *   one was opened for its copies, else 0  24 zero  28 crc32 of bytes 0..27
 * Record, 12-byte header then payload, padded to write_align:
 *   0 marker 0xA5  1 type  2 payload length (LE16)  4 seq (LE32)
 *   8 crc32 of bytes 1..7 and the payload
 * Payloads:
 *   PUT  klen(1) key value      DEL  klen(1) key
 *   ROW  series id (LE16) then the columns, fixed width little-endian,
 *        text as len(1) + bytes
 * Series schemas are PUT records under the reserved key 0x01 'S' name,
 * value = series id (LE16) + schema text.                               */
#define AS_MAGIC    0x4C515341u
#define AS_VERSION  1u
#define AS_SH       32u
#define AS_RH       12u
#define AS_MARK     0xA5u
#define AS_RESERVE  2u          /* sectors kept free for reclaiming space */
#define AS_TOMB     1u          /* key index: deleted, record reclaimed    */
#define AS_MAXCOLS  16
#define AS_NAMELEN  24          /* series name including the NUL           */
#define AS_MAXKEY   200u
#define AS_MAXSCHEMA 255u

enum { AS_R_PUT = 1, AS_R_DEL = 2, AS_R_ROW = 3 };
enum { AS_T_TIME = 1, AS_T_INT, AS_T_LONG, AS_T_FLOAT, AS_T_REAL, AS_T_TEXT };

#define AS_TIME_NONE ((int64_t)(-9223372036854775807LL - 1))

typedef struct as_sector { uint32_t seq, first_rseq; int64_t max_time; } as_sector;
typedef struct as_slot { uint32_t hash, addr; } as_slot;
typedef struct as_series {
    char     name[AS_NAMELEN];
    uint16_t id;
    uint8_t  ncols, used;
    uint8_t  types[AS_MAXCOLS];
} as_series;

struct altsql {
    altsql_flash fl;
    uint32_t  ss, sc, align, cap;
    as_sector *sec;
    uint32_t  head, tail, used, head_off, next_seq, next_rseq;
    as_slot  *kv;
    uint32_t  kv_slots, kv_used;
    uint8_t   kv_complete, replica, broken, full;   /* full: last reclaim found nothing to free */
    as_series *series;
    uint16_t  max_series, pad2_;
    uint8_t  *rbuf;
    uint32_t  rbuf_size;
    uint8_t  *work;            /* the rest of the memory block: SQL and export */
    size_t    work_size, mem_used;
    uint32_t  gc_runs, rows_dropped;
    uint32_t  synced;          /* newest seq the gateway has confirmed (sync)  */
    uint32_t  gc_of;           /* while reclaiming: seq of the sector reclaimed */
    char      err[96];
};

typedef struct as_rec { uint8_t type; uint32_t seq, addr, len, sector; const uint8_t *p; } as_rec;
typedef struct as_iter { uint32_t k, off, only; } as_iter;   /* only: stop after this run position */

/* ---- Small helpers ------------------------------------------------------ */
AS_FN void as_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
AS_FN void as_put32(uint8_t *p, uint32_t v) { as_put16(p, v & 0xFFFFu); as_put16(p + 2, v >> 16); }
AS_FN void as_put64(uint8_t *p, uint64_t v) { as_put32(p, (uint32_t)v); as_put32(p + 4, (uint32_t)(v >> 32)); }
AS_FN uint32_t as_get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
AS_FN uint32_t as_get32(const uint8_t *p) { return as_get16(p) | (as_get16(p + 2) << 16); }
AS_FN uint64_t as_get64(const uint8_t *p) { return (uint64_t)as_get32(p) | ((uint64_t)as_get32(p + 4) << 32); }

/* CRC-32 (IEEE, reflected), four bits at a time: small table, decent speed. */
static uint32_t as_crc32(uint32_t crc, const void *data, size_t n) {
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}

static uint32_t as_hash(const uint8_t *k, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *k++; h *= 16777619u; }
    return h;
}

static int as_err(altsql *db, int rc, const char *msg) {
    if (db) {
        size_t n = strlen(msg);
        if (n >= sizeof db->err) n = sizeof db->err - 1;
        memcpy(db->err, msg, n);
        db->err[n] = 0;
    }
    return rc;
}

/* Appends a short detail such as a column name to the current message. */
AS_FN int as_err2(altsql *db, int rc, const char *msg, const char *detail, size_t dn) {
    size_t n;
    as_err(db, rc, msg);
    n = strlen(db->err);
    if (dn > sizeof db->err - 1 - n) dn = sizeof db->err - 1 - n;
    memcpy(db->err + n, detail, dn);
    db->err[n + dn] = 0;
    return rc;
}

static uint32_t as_alignup(const altsql *db, uint32_t n) { return (n + db->align - 1) & ~(db->align - 1); }

AS_FN int as_is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
AS_FN int as_is_ident_char(int c) { return as_is_ident_start(c) || (c >= '0' && c <= '9'); }
AS_FN int as_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
AS_FN int as_ieq(const char *a, size_t an, const char *b, size_t bn) {
    size_t i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++) if (as_lower((unsigned char)a[i]) != as_lower((unsigned char)b[i])) return 0;
    return 1;
}

#if ALTSQL_ENABLE_SQL || ALTSQL_ENABLE_TEXT
/* A real number as short text that reads back to the same value.
 *   mode 'd'  exact for a 64-bit double (%.15g, or %.17g when needed)
 *   mode 'f'  exact for a 32-bit float: 21.53 rather than 21.530000686645508
 *   mode 'v'  for viewing: 'f' when the value is exactly a float, else 'd'
 * Whole numbers keep a ".0" so they still read as real. */
AS_FN size_t as_fmt_real(double r, char *buf, size_t cap, int mode) {
    int p, n = 0;
    if (!(r == r) || r - r != 0.0) {                 /* NaN or infinite */
        n = snprintf(buf, cap, "%g", r);
        return n < 0 ? 0 : (size_t)n;
    }
    if (mode == 'v') mode = (r >= -3.4e38 && r <= 3.4e38 && (double)(float)r == r) ? 'f' : 'd';
    if (mode == 'f' && r >= -3.4e38 && r <= 3.4e38) {
        float f = (float)r;
        for (p = 6; p <= 9; p++) {
            n = snprintf(buf, cap, "%.*g", p, r);
            if ((float)strtod(buf, NULL) == f) break;
        }
    } else {
        n = snprintf(buf, cap, "%.15g", r);
        if (strtod(buf, NULL) != r) n = snprintf(buf, cap, "%.17g", r);
    }
    if (n < 0) return 0;
    if (!strpbrk(buf, ".eEn") && (size_t)n + 2 < cap) { buf[n++] = '.'; buf[n++] = '0'; buf[n] = 0; }
    return (size_t)n;
}

/* A number as text for viewing; other values give an empty string. */
AS_FN size_t as_fmt_value(const altsql_value *v, char *buf, size_t cap) {
    int n;
    if (v->type == ALTSQL_REAL) return as_fmt_real(v->u.r, buf, cap, 'v');
    if (v->type != ALTSQL_INTEGER) { if (cap) buf[0] = 0; return 0; }
    n = snprintf(buf, cap, "%lld", (long long)v->u.i);
    return n < 0 ? 0 : (size_t)n;
}
#endif
