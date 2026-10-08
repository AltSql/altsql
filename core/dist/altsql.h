/*
 * AltSql: the tiny database that runs from sensor to gateway.
 *
 * Key-value and time-series on the microcontroller, SQL on the gateway,
 * one storage format on both. Records sync from device to gateway byte
 * for byte, with no translation step in between.
 *
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 *
 * ---------------------------------------------------------------------
 * Single-file library. In exactly one C file of your project:
 *
 *     #define ALTSQL_IMPLEMENTATION
 *     #include "altsql.h"
 *
 * Every other file just includes "altsql.h".
 *
 * Build switches (define before including; 1 = on, 0 = off):
 *
 *     ALTSQL_ENABLE_TS    time-series layer            default 1
 *     ALTSQL_ENABLE_SQL   SQL layer (needs TS)         default 1
 *     ALTSQL_ENABLE_SYNC  device-to-gateway sync       default 1
 *     ALTSQL_ENABLE_TEXT  text export and import       default 1
 *     ALTSQL_PORT_FILE    flash emulated in a file (POSIX, for gateways and tools)
 *     ALTSQL_PORT_RAM     flash emulated in RAM, with power-cut simulation (tests)
 *
 * A sensor build typically sets ALTSQL_ENABLE_SQL 0 and ALTSQL_ENABLE_TEXT 0.
 * Code that is switched off is not compiled at all.
 *
 * Memory: AltSql never calls malloc. You hand it one block of memory at
 * open time and it carves everything it needs out of that block.
 * ---------------------------------------------------------------------
 */
#ifndef ALTSQL_H
#define ALTSQL_H

#include <stddef.h>
#include <stdint.h>

#ifndef ALTSQL_ENABLE_TS
#define ALTSQL_ENABLE_TS 1
#endif
#ifndef ALTSQL_ENABLE_SQL
#define ALTSQL_ENABLE_SQL 1
#endif
#ifndef ALTSQL_ENABLE_SYNC
#define ALTSQL_ENABLE_SYNC 1
#endif
#ifndef ALTSQL_ENABLE_TEXT
#define ALTSQL_ENABLE_TEXT 1
#endif
#if ALTSQL_ENABLE_SQL && !ALTSQL_ENABLE_TS
#error "ALTSQL_ENABLE_SQL needs ALTSQL_ENABLE_TS"
#endif

#define ALTSQL_VERSION "0.4.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes. Zero and positive values are not errors. */
enum {
    ALTSQL_OK       = 0,
    ALTSQL_NOTFOUND = 1,   /* key, series or database not found            */
    ALTSQL_DONE     = 2,   /* sync: caught up, nothing more to read         */
    ALTSQL_ERROR    = -1,  /* generic failure; see altsql_errmsg()          */
    ALTSQL_IOERR    = -2,  /* the flash driver reported an error            */
    ALTSQL_CORRUPT  = -3,  /* damaged data that could not be recovered      */
    ALTSQL_FULL     = -4,  /* storage full of live key-value data           */
    ALTSQL_NOMEM    = -5,  /* working memory exhausted                      */
    ALTSQL_TOOBIG   = -6,  /* record larger than the record buffer          */
    ALTSQL_MISUSE   = -7,  /* bad arguments or a call in the wrong state    */
    ALTSQL_SYNTAX   = -8,  /* SQL or schema syntax error                    */
    ALTSQL_SCHEMA   = -9,  /* unknown table or column, or type mismatch     */
    ALTSQL_EXISTS   = -10  /* series already exists                         */
};

/* ---- Flash driver ----------------------------------------------------
 * The engine sees storage as sector_count sectors of sector_size bytes.
 * write() may only clear bits (NOR flash rules); erase() sets a whole
 * sector to 0xFF. Return 0 on success, anything else on failure.
 * map() is optional: return a pointer to len readable bytes at addr when
 * the flash is memory-mapped (fast path), or leave it NULL.            */
typedef struct altsql_flash {
    void    *ctx;
    uint32_t sector_size;   /* power of two, 256 bytes or more        */
    uint32_t sector_count;  /* 4 or more                              */
    uint32_t write_align;   /* 1, 2, 4, 8 or 16 bytes                 */
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t sector);
    const uint8_t *(*map)(void *ctx, uint32_t addr, uint32_t len);
} altsql_flash;

typedef struct altsql_config {
    void    *mem;            /* working memory, owned by the caller          */
    size_t   mem_size;
    uint32_t kv_slots;       /* key index size; 0 = 64                        */
    uint16_t max_series;     /* 0 = 8                                         */
    uint16_t max_record;     /* largest record in bytes; 0 = 256              */
    uint8_t  create;         /* format the flash if it holds no database      */
    uint8_t  replica;        /* gateway copy: accepts sync, refuses local writes */
} altsql_config;

typedef struct altsql altsql;

/* Values as seen by SQL and the typed append call. */
enum { ALTSQL_NULL = 0, ALTSQL_INTEGER = 1, ALTSQL_REAL = 2, ALTSQL_TEXT = 3 };
typedef struct altsql_value {
    int type;
    int len;                  /* text length in bytes                         */
    union { int64_t i; double r; const char *s; } u;
} altsql_value;

/* ---- Open, format, info ----------------------------------------------
 * altsql_open finds the database on the flash (formatting it first when
 * cfg->create is set and there is none). It never formats flash that holds
 * a database of another geometry. On failure *db may still be set, so that
 * altsql_errmsg(*db) can say why; the handle cannot be used otherwise. */
int         altsql_format(const altsql_flash *flash);
int         altsql_open(altsql **db, const altsql_flash *flash, const altsql_config *cfg);
void        altsql_close(altsql *db);
const char *altsql_errmsg(const altsql *db);

typedef struct altsql_info {
    uint32_t sector_size, sectors, used_sectors, free_sectors;
    uint32_t last_seq;        /* sequence number of the newest record         */
    uint32_t kv_keys;         /* keys in the index                            */
    uint32_t gc_runs;         /* sectors reclaimed since open                 */
    uint32_t rows_dropped;    /* time-series rows rolled over since open      */
    uint32_t mem_used;        /* bytes of the working memory in use           */
} altsql_info;
int altsql_info_get(altsql *db, altsql_info *out);

/* ---- Key-value --------------------------------------------------------
 * Keys are text of 1 to 200 bytes. Values are any bytes. */
int altsql_put(altsql *db, const char *key, const void *val, size_t len);
int altsql_get(altsql *db, const char *key, void *buf, size_t cap, size_t *len);
int altsql_del(altsql *db, const char *key);
typedef int (*altsql_kv_cb)(void *ctx, const char *key, size_t klen, const void *val, size_t vlen);
int altsql_kv_each(altsql *db, altsql_kv_cb cb, void *ctx);

#if ALTSQL_ENABLE_TS
/* ---- Time-series ------------------------------------------------------
 * Schema: comma-separated "name:type" pairs, first column of type time.
 *   time  int64 (any unit you like, e.g. seconds or milliseconds)
 *   int   int32        long  int64
 *   float float32      real  float64
 *   text  up to 255 bytes
 * A float column reads back as the shortest decimal that gives the same
 * 32-bit value: store 21.53 and you read 21.53. 
 * Example: "time:time,machine:int,temp:float"
 * When storage fills, the oldest rows are overwritten (rollover). */
int altsql_ts_create(altsql *db, const char *series, const char *schema);

/* Variadic append, values in schema order:
 *   time, long -> int64_t    int -> int    float, real -> double    text -> const char * */
int altsql_append(altsql *db, const char *series, ...);
int altsql_ts_append(altsql *db, const char *series, const altsql_value *vals, int n);

typedef int (*altsql_row_cb)(void *ctx, int ncol, const altsql_value *vals, const char *const *names);
/* Rows with from <= time <= to, oldest first. names is NULL here. */
int altsql_ts_scan(altsql *db, const char *series, int64_t from, int64_t to,
                   altsql_row_cb cb, void *ctx);

typedef struct altsql_stats { uint32_t count; double min, max, sum, avg; int64_t first, last; } altsql_stats;
/* Count, min, max, sum and average of one numeric column over rows with time >= since.
 * This is what on-device rules use ("above 60 for 10 seconds"). */
int altsql_ts_window(altsql *db, const char *series, const char *column, int64_t since, altsql_stats *out);

typedef int (*altsql_series_cb)(void *ctx, const char *name, const char *schema);
int altsql_series_each(altsql *db, altsql_series_cb cb, void *ctx);
#endif

#if ALTSQL_ENABLE_SQL
/* ---- SQL ----------------------------------------------------------------
 * CREATE TABLE, INSERT and SELECT with WHERE, GROUP BY, HAVING, ORDER BY,
 * LIMIT/OFFSET, COUNT/SUM/AVG/MIN/MAX and ABS/ROUND/LENGTH/LOWER/UPPER.
 * Each time-series is a table; the key-value store is the table "kv".
 * Statements may be separated by ';'. cb may be NULL. */
int altsql_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx);
#endif

#if ALTSQL_ENABLE_SYNC
/* ---- Sync ---------------------------------------------------------------
 * Device side: copy records newer than after_seq into buf, in the same
 * byte format they are stored in. filter (optional) returns non-zero to
 * send a record; kind is ALTSQL_REC_KV, ALTSQL_REC_ROW or
 * ALTSQL_REC_SCHEMA (a series definition; series_id is its id). A gateway
 * needs the definitions of the series it receives, so filters normally
 * pass them; a link too small for them can leave them out if the gateway
 * was given them at setup. Without a filter everything is sent. Returns ALTSQL_DONE when caught up,
 * ALTSQL_OK when more records are waiting. Once the gateway has the batch,
 * resume from *last_seq. Pass the gateway's confirmed position as
 * after_seq: deletes it has not confirmed are kept until it has.
 * Known limit: the device keeps one confirmed position, the highest any
 * gateway has reached. With several gateways, one that lags behind can
 * miss a delete once the sector holding it is reclaimed.
 * Gateway side: a database opened with cfg.replica = 1 applies the bytes.
 * Records it already has are skipped, so re-sending is always safe. Give
 * the replica at least the device's max_record. One device per replica. */
enum { ALTSQL_REC_KV = 1, ALTSQL_REC_ROW = 2, ALTSQL_REC_SCHEMA = 3 };
typedef int (*altsql_sync_filter)(void *ctx, int kind, int series_id);
int altsql_sync_read(altsql *db, uint32_t after_seq, void *buf, size_t cap, size_t *len,
                     uint32_t *last_seq, altsql_sync_filter filter, void *fctx);
int altsql_sync_apply(altsql *db, const void *buf, size_t len, uint32_t *last_seq);
int altsql_series_id(altsql *db, const char *series);
#endif

#if ALTSQL_ENABLE_TEXT
/* ---- Readable text form ---------------------------------------------------
 * One record per line, keys sorted, rows oldest first. Import reverses it. */
typedef int (*altsql_write_cb)(void *ctx, const char *data, size_t len);
int altsql_export(altsql *db, altsql_write_cb out, void *ctx);
int altsql_import(altsql *db, const char *text, size_t len);
#endif

#ifdef ALTSQL_PORT_FILE
/* Flash emulated in a file, memory-mapped. sector_size 0 detects the
 * geometry of an existing file. Returns 0 on success. */
int  altsql_file_flash_open(altsql_flash *f, const char *path, uint32_t sector_size,
                            uint32_t sector_count, uint32_t write_align, int create);
void altsql_file_flash_close(altsql_flash *f);
#endif

#ifdef ALTSQL_PORT_RAM
/* Flash emulated in RAM. Set budget to the number of bytes the "chip"
 * may still program before power is cut (-1 = never). At the cut, the
 * byte being written is partly programmed and an erase in progress is
 * left half done, like real flash. */
typedef struct altsql_ram_flash {
    uint8_t *mem;
    uint32_t size, sector_size;
    int64_t  budget;          /* bytes left before the power cut; -1 = never */
    uint32_t rng;             /* seed for how the interrupted byte or erase ends up */
    int      dead;            /* 0 powered, 1 cut while writing, 2 cut while erasing */
    uint64_t bytes_written, erases;
} altsql_ram_flash;
void altsql_ram_flash_init(altsql_flash *f, altsql_ram_flash *r, uint8_t *mem,
                           uint32_t sector_size, uint32_t sector_count, uint32_t write_align);
#endif

#ifdef __cplusplus
}
#endif
#endif /* ALTSQL_H */


/* ======================================================================
 * Implementation. Compiled only where ALTSQL_IMPLEMENTATION is defined.
 * ====================================================================== */
#if defined(ALTSQL_IMPLEMENTATION) && !defined(ALTSQL_IMPL_DONE)
#define ALTSQL_IMPL_DONE

/* ---- 00_prelude.c ------------------------------------------------ */
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

/* ---- 10_storage.c ------------------------------------------------ */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Storage base: a log of records spread over flash sectors.
 *
 * The log is a circular run of sectors, oldest (tail) to newest (head).
 * Records are only ever appended at the head. When the head fills, the
 * next free sector becomes the head. When free sectors run low, the
 * tail sector is reclaimed: live key-value records are copied to the
 * head, time-series rows are dropped (rollover), and the sector is
 * retired, to be erased when it is next used. Every sector takes its
 * turn, so wear spreads evenly.
 *
 * Power-cut safety: a record only counts once its checksum is complete.
 * A write torn by a power cut leaves damaged bytes that the scanner steps
 * over, and new records go after them. Nothing older is ever rewritten.
 * ====================================================================== */

static int as_note(altsql *db, const as_rec *r);                 /* key-value layer */
static int as_rec_is_live(altsql *db, as_rec *r, int *live);
static void as_kv_repoint(altsql *db, const as_rec *r, uint32_t new_addr);

/* ---- Flash access ---------------------------------------------------- */
static const uint8_t *as_view(altsql *db, uint32_t addr, uint32_t len, uint8_t *dst) {
    if (db->fl.map) return db->fl.map(db->fl.ctx, addr, len);
    return db->fl.read(db->fl.ctx, addr, dst, len) == 0 ? dst : NULL;
}

static int as_write(altsql *db, uint32_t addr, const void *buf, uint32_t len) {
    if (db->fl.write(db->fl.ctx, addr, buf, len) != 0) {
        db->broken = 1;
        return as_err(db, ALTSQL_IOERR, "flash write failed");
    }
    return ALTSQL_OK;
}

static int as_erase(altsql *db, uint32_t s) {
    if (db->fl.erase(db->fl.ctx, s) != 0) {
        db->broken = 1;
        return as_err(db, ALTSQL_IOERR, "flash erase failed");
    }
    return ALTSQL_OK;
}

static int as_flash_ok(const altsql_flash *f) {
    uint32_t a;
    if (!f || !f->read || !f->write || !f->erase) return 0;
    if (f->sector_size < 256u || (f->sector_size & (f->sector_size - 1u))) return 0;
    if (f->sector_count < AS_RESERVE + 2u) return 0;
    if ((uint64_t)f->sector_size * f->sector_count > 0xFFFFFFFFull) return 0;
    a = f->write_align;
    return a == 1 || a == 2 || a == 4 || a == 8 || a == 16;
}

/* Entry check for public calls. */
static int as_ready(altsql *db) {
    if (!db) return ALTSQL_MISUSE;
    if (db->broken) return as_err(db, ALTSQL_MISUSE, "database is closed or must be reopened after an error");
    return ALTSQL_OK;
}

/* ---- Sector headers ---------------------------------------------------- */
static void as_hdr_make(uint8_t *h, uint32_t seq, uint32_t ss, uint32_t sc, uint32_t align, uint32_t gc_of) {
    memset(h, 0, AS_SH);
    as_put32(h, AS_MAGIC);
    h[4] = AS_VERSION;
    h[5] = (uint8_t)align;
    as_put32(h + 8, seq);
    as_put32(h + 12, ss);
    as_put32(h + 16, sc);
    as_put32(h + 20, gc_of);
    as_put32(h + 28, as_crc32(0, h, 28));
}

static int as_hdr_valid(const uint8_t *h) {
    return as_get32(h) == AS_MAGIC && h[4] == AS_VERSION && as_get32(h + 28) == as_crc32(0, h, 28);
}

static int as_hdr_check(const uint8_t *h, uint32_t ss, uint32_t sc, uint32_t *seq) {
    if (!as_hdr_valid(h)) return 0;
    if (as_get32(h + 12) != ss || as_get32(h + 16) != sc) return 0;
    *seq = as_get32(h + 8);
    return *seq != 0;
}

/* ---- The run of sectors ------------------------------------------------ */
static uint32_t as_run_sector(const altsql *db, uint32_t k) { return (db->tail + k) % db->sc; }
static int as_in_run(const altsql *db, uint32_t s) { return ((s + db->sc - db->tail) % db->sc) < db->used; }

/* 1 if [addr, addr+len) is all 0xFF, 0 if not, <0 on read failure. */
static int as_all_ff(altsql *db, uint32_t addr, uint32_t len) {
    while (len) {
        uint32_t n = len < db->rbuf_size ? len : db->rbuf_size, i;
        const uint8_t *p = as_view(db, addr, n, db->rbuf);
        if (!p) return -1;
        for (i = 0; i < n; i++) if (p[i] != 0xFF) return 0;
        addr += n;
        len -= n;
    }
    return 1;
}

/* ---- Reading records ----------------------------------------------------- */
#define AS_ALL 0xFFFFFFFFu

static void as_iter_start(as_iter *it, uint32_t k, uint32_t only) {
    it->k = k;
    it->off = AS_SH;
    it->only = only;
}

/* Next good record in log order: 1 = record in *r, 0 = end, <0 = error.
 * r->p stays valid until the next read from flash. When the flash is not
 * memory-mapped, the record is read into db->rbuf (header, then payload). */
static int as_next(altsql *db, as_iter *it, as_rec *r) {
    while (it->k < db->used && (it->only == AS_ALL || it->k <= it->only)) {
        uint32_t s = as_run_sector(db, it->k), base = s * db->ss;
        uint32_t end = (s == db->head) ? db->head_off : db->ss;
        while (it->off + AS_RH <= end) {
            uint32_t a = base + it->off, len;
            const uint8_t *h = as_view(db, a, AS_RH, db->rbuf), *p;
            if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
            if (h[0] != AS_MARK) {
                if (h[0] == 0xFF) {
                    int ff = as_all_ff(db, a, end - it->off);
                    if (ff < 0) return as_err(db, ALTSQL_IOERR, "flash read failed");
                    if (ff) break;                  /* nothing more in this sector */
                }
                it->off += db->align;               /* step over damaged bytes */
                continue;
            }
            len = as_get16(h + 2);
            if (it->off + AS_RH + len > end || AS_RH + len > db->rbuf_size) {
                it->off += db->align;
                continue;
            }
            if (db->fl.map) p = db->fl.map(db->fl.ctx, a, AS_RH + len);
            else p = db->fl.read(db->fl.ctx, a + AS_RH, db->rbuf + AS_RH, len) == 0 ? db->rbuf : NULL;
            if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
            if (as_crc32(as_crc32(0, p + 1, 7), p + AS_RH, len) != as_get32(p + 8)) {
                it->off += db->align;
                continue;
            }
            r->type = p[1];
            r->len = len;
            r->seq = as_get32(p + 4);
            r->addr = a;
            r->sector = s;
            r->p = p + AS_RH;
            it->off += as_alignup(db, AS_RH + len);
            return 1;
        }
        it->k++;
        it->off = AS_SH;
    }
    return 0;
}

/* ---- Writing records ------------------------------------------------------ */
static int as_activate(altsql *db) {
    uint32_t s = (db->head + 1) % db->sc;
    uint8_t h[AS_SH];
    int rc;
    if (db->used >= db->sc) return as_err(db, ALTSQL_FULL, "no free sector");
    rc = as_erase(db, s);
    if (rc) return rc;
    as_hdr_make(h, db->next_seq, db->ss, db->sc, db->align, db->gc_of);
    rc = as_write(db, s * db->ss, h, AS_SH);
    if (rc) return rc;
    db->sec[s].seq = db->next_seq++;
    db->sec[s].first_rseq = 0;
    db->sec[s].max_time = AS_TIME_NONE;
    db->head = s;
    db->head_off = AS_SH;
    db->used++;
    return ALTSQL_OK;
}

static int as_gc(altsql *db);

/* How a write may find room (as_reserve). */
enum {
    AS_RES_NOGC = 0,   /* reclaiming itself: any free sector, never reclaim */
    AS_RES_GC   = 1,   /* normal write: keep AS_RESERVE sectors free, reclaim */
    AS_RES_DEL  = 2    /* delete: as normal, then one reserve sector if full */
};

/* Makes room at the head for a record with a payload of len bytes.
 * Call before filling db->rbuf: reclaiming space reads through rbuf.
 * When a full turn of reclaiming frees nothing, the database remembers
 * it is full and fails fast (no pointless erasing) until a key-value
 * write succeeds. A delete may still use one reserve sector, so space
 * can always be freed. */
static int as_reserve(altsql *db, uint32_t len, int mode) {
    uint32_t total, guard = 0;
    int rc;
    if (db->broken) return as_err(db, ALTSQL_IOERR, "reopen the database after an I/O error");
    if (len > 0xFFFFu) return as_err(db, ALTSQL_TOOBIG, "record too large");
    total = as_alignup(db, AS_RH + len);
    if (total > db->rbuf_size || total > db->cap) return as_err(db, ALTSQL_TOOBIG, "record too large");
    for (;;) {
        uint32_t freec = db->sc - db->used;
        if (db->head_off + total <= db->ss) return ALTSQL_OK;
        if (freec > AS_RESERVE || (mode == AS_RES_NOGC && freec > 0)) return as_activate(db);
        if (mode == AS_RES_NOGC) return as_err(db, ALTSQL_FULL, "no free sector");
        rc = (db->full || ++guard > db->sc) ? ALTSQL_FULL : as_gc(db);
        if (rc == ALTSQL_FULL) {
            db->full = 1;
            if (mode == AS_RES_DEL && db->sc - db->used > 1) return as_activate(db);
            return as_err(db, ALTSQL_FULL, "storage is full of live key-value data");
        }
        if (rc) return rc;
    }
}

/* Writes the record whose payload is already in db->rbuf + AS_RH. */
static int as_commit(altsql *db, uint8_t type, uint32_t seq, uint32_t len, uint32_t *out_addr) {
    uint8_t *h = db->rbuf;
    uint32_t total = as_alignup(db, AS_RH + len), addr;
    int rc;
    h[0] = AS_MARK;
    h[1] = type;
    as_put16(h + 2, len);
    as_put32(h + 4, seq);
    as_put32(h + 8, as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, len));
    memset(h + AS_RH + len, 0, total - AS_RH - len);
    addr = db->head * db->ss + db->head_off;
    rc = as_write(db, addr, h, total);
    if (rc) return rc;
    if (!db->sec[db->head].first_rseq) db->sec[db->head].first_rseq = seq;
    db->head_off += total;
    if (seq >= db->next_rseq) db->next_rseq = seq + 1;
    if (out_addr) *out_addr = addr;
    return ALTSQL_OK;
}

/* Record as it sits in rbuf right after as_commit, for as_note(). */
static void as_rec_fresh(altsql *db, as_rec *r, uint8_t type, uint32_t seq, uint32_t len, uint32_t addr) {
    r->type = type;
    r->seq = seq;
    r->len = len;
    r->addr = addr;
    r->sector = addr / db->ss;
    r->p = db->rbuf + AS_RH;
}

/* ---- Reclaiming the oldest sector ------------------------------------------ */

/* A delete the gateway has not confirmed yet is carried forward, so a
 * gateway that was offline for a while still learns about it. */
static int as_keep_del(const altsql *db, const as_rec *r) {
#if ALTSQL_ENABLE_SYNC
    return !db->replica && r->seq > db->synced;
#else
    (void)db;
    (void)r;
    return 0;
#endif
}

static int as_gc(altsql *db) {
    as_iter it;
    as_rec r;
    uint32_t t = db->tail, addr;
    uint8_t z[16];
    int rc, live;
    if (db->used < 2) return as_err(db, ALTSQL_FULL, "storage full");
    as_iter_start(&it, 0, 0);
    while ((rc = as_next(db, &it, &r)) == 1) {
        if (r.type == AS_R_ROW) { db->rows_dropped++; continue; }
        if (r.type == AS_R_DEL && !as_keep_del(db, &r)) { as_kv_repoint(db, &r, AS_TOMB); continue; }
        if (r.type != AS_R_PUT && r.type != AS_R_DEL) continue;
        rc = as_rec_is_live(db, &r, &live);
        if (rc) return rc;
        if (!live) continue;
        db->gc_of = db->sec[t].seq;        /* a sector opened now is marked as ours */
        rc = as_reserve(db, r.len, AS_RES_NOGC);
        db->gc_of = 0;
        if (rc) return rc;
        if (r.p != db->rbuf + AS_RH) memmove(db->rbuf + AS_RH, r.p, r.len);
        rc = as_commit(db, r.type, db->replica ? r.seq : db->next_rseq, r.len, &addr);
        if (rc) return rc;
        as_kv_repoint(db, &r, addr);
    }
    if (rc < 0) return rc;
    /* Retire the sector by clearing its magic number. It is erased only
     * when it next becomes the head, so an erase cut short by a power loss
     * can never leave a half-erased sector inside the log. */
    memset(z, 0, sizeof z);
    rc = as_write(db, t * db->ss, z, db->align > 4u ? db->align : 4u);
    if (rc) return rc;
    db->sec[t].seq = 0;
    db->sec[t].first_rseq = 0;
    db->sec[t].max_time = AS_TIME_NONE;
    db->tail = (t + 1) % db->sc;
    db->used--;
    db->gc_runs++;
    return ALTSQL_OK;
}

/* ---- Mounting ------------------------------------------------------------------ */
static int as_mount(altsql *db) {
    uint32_t i, best = 0, hs = 0, valid_end = AS_SH, last = 0, off, align = 0, gc_of = 0;
    int foreign = 0;
    as_iter it;
    as_rec r;
    int rc;
    for (i = 0; i < db->sc; i++) {
        uint32_t seq = 0;
        const uint8_t *h = as_view(db, i * db->ss, AS_SH, db->rbuf);
        if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
        db->sec[i].seq = as_hdr_check(h, db->ss, db->sc, &seq) ? seq : 0;
        db->sec[i].first_rseq = 0;
        db->sec[i].max_time = AS_TIME_NONE;
        if (db->sec[i].seq > best) { best = db->sec[i].seq; hs = i; align = h[5]; gc_of = as_get32(h + 20); }
        else if (!db->sec[i].seq && as_hdr_valid(h))
            foreign = 1;               /* a valid header, but for another geometry */
    }
    if (!best && foreign)
        return as_err(db, ALTSQL_MISUSE, "flash holds a database with another sector size or count");
    if (!best) return as_err(db, ALTSQL_NOTFOUND, "no database on this flash");
    if (align != db->align) return as_err(db, ALTSQL_MISUSE, "flash was formatted with another write_align");

    /* The log is the run of sectors ending at the head whose sequence
     * numbers count down by one. Anything else is free space. */
    db->head = hs;
    db->used = 1;
    while (db->used < db->sc && db->sec[(hs + db->sc - db->used) % db->sc].seq == best - db->used)
        db->used++;
    db->tail = (hs + db->sc + 1 - db->used) % db->sc;
    /* A reclaim cut short after it opened a new sector: the tail it was
     * copying is still here, and the new sector holds only copies of it.
     * Drop that sector; the reclaim simply runs again. Without this, each
     * such power cut would cost a spare sector for good. */
    db->next_seq = best + 1;
    if (gc_of && db->used > 1 && gc_of == best - db->used + 1) {
        db->used--;
        db->head = (hs + db->sc - 1) % db->sc;
        db->next_seq = best;          /* its number is used again when that sector is */
    }
    for (i = 0; i < db->sc; i++) if (!as_in_run(db, i)) db->sec[i].seq = 0;
    db->next_rseq = 1;

    db->head_off = db->ss;                    /* scan the whole head sector */
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        if (r.sector == db->head) valid_end = it.off;
        rc = as_note(db, &r);
        if (rc) return rc;
    }
    if (rc < 0) return rc;

    /* Resume after the last byte ever programmed in the head sector, so a
     * torn write is never written over. */
    off = db->ss;
    while (off > AS_SH && !last) {
        uint32_t n = off - AS_SH < db->rbuf_size ? off - AS_SH : db->rbuf_size, j;
        const uint8_t *p = as_view(db, db->head * db->ss + off - n, n, db->rbuf);
        if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
        for (j = n; j > 0; j--) if (p[j - 1] != 0xFF) { last = off - n + j; break; }
        off -= n;
    }
    if (last) last = as_alignup(db, last);
    db->head_off = valid_end > last ? valid_end : last;
    if (db->head_off < AS_SH) db->head_off = AS_SH;
    if (db->head_off > db->ss) db->head_off = db->ss;
    return ALTSQL_OK;
}

/* ---- Public: format, open, close, info ------------------------------------------ */
int altsql_format(const altsql_flash *f) {
    uint8_t h[AS_SH];
    uint32_t i;
    if (!as_flash_ok(f)) return ALTSQL_MISUSE;
    for (i = 0; i < f->sector_count; i++) if (f->erase(f->ctx, i) != 0) return ALTSQL_IOERR;
    as_hdr_make(h, 1, f->sector_size, f->sector_count, f->write_align, 0);
    return f->write(f->ctx, 0, h, AS_SH) != 0 ? ALTSQL_IOERR : ALTSQL_OK;
}

static void *as_take(uint8_t *base, size_t size, size_t *off, size_t n) {
    void *p;
    size_t o = (*off + 7u) & ~(size_t)7u;
    if (o > size || n > size - o) return NULL;
    p = base + o;
    *off = o + n;
    return p;
}

int altsql_open(altsql **out, const altsql_flash *f, const altsql_config *cfg) {
    uint8_t *m;
    size_t off = 0, o;
    altsql *db;
    uint32_t rec;
    int rc;
    if (!out) return ALTSQL_MISUSE;
    *out = NULL;
    if (!cfg || !cfg->mem || !as_flash_ok(f)) return ALTSQL_MISUSE;
    m = (uint8_t *)cfg->mem;
    db = (altsql *)as_take(m, cfg->mem_size, &off, sizeof *db);
    if (!db) return ALTSQL_NOMEM;
    memset(db, 0, sizeof *db);
    db->fl = *f;
    db->ss = f->sector_size;
    db->sc = f->sector_count;
    db->align = f->write_align;
    db->cap = db->ss - AS_SH;
    db->replica = cfg->replica ? 1 : 0;
    db->kv_complete = 1;
    db->kv_slots = cfg->kv_slots ? cfg->kv_slots : 64;
    db->max_series = cfg->max_series ? cfg->max_series : 8;
    rec = cfg->max_record ? cfg->max_record : 256;
    if (rec < 64) rec = 64;
    rec = as_alignup(db, rec);
    if (rec > db->cap) rec = db->cap & ~(db->align - 1);
    if (rec > 0xFFFFu + AS_RH) rec = 0xFFFFu + AS_RH;
    db->rbuf_size = rec;
    db->sec = (as_sector *)as_take(m, cfg->mem_size, &off, sizeof(as_sector) * db->sc);
    db->kv = (as_slot *)as_take(m, cfg->mem_size, &off, sizeof(as_slot) * db->kv_slots);
    db->series = (as_series *)as_take(m, cfg->mem_size, &off, sizeof(as_series) * db->max_series);
    db->rbuf = (uint8_t *)as_take(m, cfg->mem_size, &off, db->rbuf_size);
    if (!db->sec || !db->kv || !db->series || !db->rbuf) return ALTSQL_NOMEM;
    memset(db->kv, 0, sizeof(as_slot) * db->kv_slots);
    memset(db->series, 0, sizeof(as_series) * db->max_series);
    db->mem_used = off;
    o = (off + 7u) & ~(size_t)7u;
    if (o < cfg->mem_size) {
        db->work = m + o;
        db->work_size = cfg->mem_size - o;
    }
    rc = as_mount(db);
    if (rc == ALTSQL_NOTFOUND && cfg->create) {
        rc = altsql_format(f);
        if (rc == ALTSQL_OK) {
            memset(db->kv, 0, sizeof(as_slot) * db->kv_slots);
            memset(db->series, 0, sizeof(as_series) * db->max_series);
            db->kv_used = 0;
            db->kv_complete = 1;
            rc = as_mount(db);
        } else {
            as_err(db, rc, "formatting the flash failed");
        }
    }
    *out = db;                 /* also on failure, so altsql_errmsg() can explain */
    if (rc) db->broken = 1;
    return rc;
}

void altsql_close(altsql *db) {
    if (db) db->broken = 1;
}

const char *altsql_errmsg(const altsql *db) {
    return db ? db->err : "no database";
}

int altsql_info_get(altsql *db, altsql_info *o) {
    if (!db || !o) return ALTSQL_MISUSE;
    memset(o, 0, sizeof *o);
    o->sector_size = db->ss;
    o->sectors = db->sc;
    o->used_sectors = db->used;
    o->free_sectors = db->sc - db->used;
    o->last_seq = db->next_rseq - 1;
    o->kv_keys = db->kv_used;
    o->gc_runs = db->gc_runs;
    o->rows_dropped = db->rows_dropped;
    o->mem_used = (uint32_t)db->mem_used;
    return ALTSQL_OK;
}

/* ---- 20_kv.c ----------------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Key-value layer
 *
 * A small hash index in RAM maps each key to its newest record (PUT or
 * DEL). It is rebuilt by scanning the log at open. If more keys arrive
 * than the index has room for, lookups fall back to scanning the log.
 * ====================================================================== */

#if ALTSQL_ENABLE_TS
static void as_series_note(altsql *db, const uint8_t *name, uint32_t nlen, const uint8_t *val, uint32_t vlen);
#endif

/* Compares the key of the record at addr with key. Uses a stack buffer,
 * never db->rbuf, so a record the caller holds in rbuf stays intact. */
static int as_key_at(altsql *db, uint32_t addr, const uint8_t *key, uint32_t klen, uint8_t *type, int *match) {
    uint8_t b[AS_RH + 1 + AS_MAXKEY + 2];
    const uint8_t *p;
    *match = 0;
    *type = 0;
    if (klen > AS_MAXKEY) return ALTSQL_OK;
    p = as_view(db, addr, AS_RH + 1, b);
    if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
    *type = p[1];
    if (p[AS_RH] != klen) return ALTSQL_OK;
    p = as_view(db, addr, AS_RH + 1 + klen, b);
    if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
    *match = memcmp(p + AS_RH + 1, key, klen) == 0;
    return ALTSQL_OK;
}

/* 1 = found (*slot), 0 = not found (*slot = where to insert, or AS_ALL
 * when the index is full), <0 = error. A tombstone (AS_TOMB) stands for a
 * key whose delete record was reclaimed: it keeps probe chains intact
 * and matches no key. */
static int as_kv_lookup(altsql *db, const uint8_t *key, uint32_t klen, uint32_t h, uint32_t *slot) {
    uint32_t i = h % db->kv_slots, n, tomb = AS_ALL;
    for (n = 0; n < db->kv_slots; n++, i = (i + 1) % db->kv_slots) {
        as_slot *s = &db->kv[i];
        if (s->addr == 0) { *slot = tomb != AS_ALL ? tomb : i; return 0; }
        if (s->addr == AS_TOMB) { if (tomb == AS_ALL) tomb = i; continue; }
        if (s->hash == h) {
            uint8_t type;
            int match, rc = as_key_at(db, s->addr, key, klen, &type, &match);
            if (rc) return rc;
            if (match) { *slot = i; return 1; }
        }
    }
    *slot = tomb;
    return 0;
}

static int as_kv_index(altsql *db, const uint8_t *key, uint32_t klen, uint32_t addr) {
    uint32_t h = as_hash(key, klen), slot;
    int rc = as_kv_lookup(db, key, klen, h, &slot);
    if (rc < 0) return rc;
    if (rc == 1) { db->kv[slot].addr = addr; return ALTSQL_OK; }
    if (slot != AS_ALL && db->kv[slot].addr == AS_TOMB) {     /* reuse a tombstone */
        db->kv[slot].hash = h;
        db->kv[slot].addr = addr;
        return ALTSQL_OK;
    }
    if (slot == AS_ALL || (db->kv_used + 1u) * 4u > db->kv_slots * 3u) {
        db->kv_complete = 0;              /* index full: fall back to scans */
        return ALTSQL_OK;
    }
    db->kv[slot].hash = h;
    db->kv[slot].addr = addr;
    db->kv_used++;
    return ALTSQL_OK;
}

/* Index slot currently pointing at addr, or AS_ALL. No flash reads. */
static uint32_t as_slot_of(const altsql *db, uint32_t h, uint32_t addr) {
    uint32_t i = h % db->kv_slots, n;
    for (n = 0; n < db->kv_slots; n++, i = (i + 1) % db->kv_slots) {
        if (db->kv[i].addr == 0) break;
        if (db->kv[i].hash == h && db->kv[i].addr == addr) return i;
    }
    return AS_ALL;
}

/* The record at r->addr moved to new_addr (or was reclaimed: AS_TOMB). */
static void as_kv_repoint(altsql *db, const as_rec *r, uint32_t new_addr) {
    uint32_t klen, s;
    if (r->len < 1) return;
    klen = r->p[0];
    if (1u + klen > r->len) return;
    s = as_slot_of(db, as_hash(r->p + 1, klen), r->addr);
    if (s != AS_ALL) db->kv[s].addr = new_addr;
}

/* Newest record for key: *addr = 0 if none; *type = AS_R_PUT or AS_R_DEL. */
static int as_kv_latest(altsql *db, const uint8_t *key, uint32_t klen, uint32_t *addr, uint8_t *type) {
    uint32_t slot;
    int rc;
    *addr = 0;
    *type = 0;
    rc = as_kv_lookup(db, key, klen, as_hash(key, klen), &slot);
    if (rc < 0) return rc;
    if (rc == 1) {
        int match;
        uint32_t a = db->kv[slot].addr;
        rc = as_key_at(db, a, key, klen, type, &match);
        if (rc) return rc;
        *addr = a;
        return ALTSQL_OK;
    }
    if (db->kv_complete) return ALTSQL_OK;
    {   /* the index overflowed at some point: scan the whole log */
        as_iter it;
        as_rec r;
        uint8_t k[AS_MAXKEY + 2];
        memcpy(k, key, klen);
        as_iter_start(&it, 0, AS_ALL);
        while ((rc = as_next(db, &it, &r)) == 1) {
            if ((r.type == AS_R_PUT || r.type == AS_R_DEL) && r.len >= 1u + klen &&
                r.p[0] == klen && memcmp(r.p + 1, k, klen) == 0) {
                *addr = r.addr;
                *type = r.type;
            }
        }
        return rc < 0 ? rc : ALTSQL_OK;
    }
}

/* Is this PUT or DEL the newest record of its key? May re-read *r into rbuf. */
static int as_rec_is_live(altsql *db, as_rec *r, int *live) {
    uint32_t klen, h, addr;
    uint8_t k[AS_MAXKEY + 2], type;
    int rc;
    *live = 0;
    if (r->len < 1) return ALTSQL_OK;
    klen = r->p[0];
    if (klen == 0 || klen > AS_MAXKEY || 1u + klen > r->len) return ALTSQL_OK;
    h = as_hash(r->p + 1, klen);
    if (as_slot_of(db, h, r->addr) != AS_ALL) { *live = 1; return ALTSQL_OK; }
    if (db->kv_complete) return ALTSQL_OK;
    memcpy(k, r->p + 1, klen);
    rc = as_kv_latest(db, k, klen, &addr, &type);
    if (rc) return rc;
    *live = addr == r->addr;
    if (!db->fl.map) {       /* the scan reused rbuf: read this record back */
        if (db->fl.read(db->fl.ctx, r->addr, db->rbuf, AS_RH + r->len) != 0)
            return as_err(db, ALTSQL_IOERR, "flash read failed");
        r->p = db->rbuf + AS_RH;
    }
    return ALTSQL_OK;
}

/* Bookkeeping for every record that enters the log (open, write, sync). */
static int as_note(altsql *db, const as_rec *r) {
    as_sector *s = &db->sec[r->sector];
    if (r->seq >= db->next_rseq) db->next_rseq = r->seq + 1;
    if (!s->first_rseq) s->first_rseq = r->seq;
    if (r->type == AS_R_ROW) {
        if (r->len >= 10) {
            int64_t t = (int64_t)as_get64(r->p + 2);
            if (t > s->max_time) s->max_time = t;
        }
        return ALTSQL_OK;
    }
    if ((r->type == AS_R_PUT || r->type == AS_R_DEL) && r->len >= 1) {
        uint32_t klen = r->p[0];
        int rc;
        if (klen == 0 || klen > AS_MAXKEY || 1u + klen > r->len) return ALTSQL_OK;
        rc = as_kv_index(db, r->p + 1, klen, r->addr);
        if (rc) return rc;
#if ALTSQL_ENABLE_TS
        if (r->type == AS_R_PUT && klen >= 3 && r->p[1] == 0x01 && r->p[2] == 'S')
            as_series_note(db, r->p + 3, klen - 2, r->p + 1 + klen, r->len - 1 - klen);
#endif
    }
    return ALTSQL_OK;
}

/* PUT or DEL. key and val must not point into db->rbuf. */
static int as_kv_write(altsql *db, uint8_t type, const uint8_t *key, uint32_t klen, const void *val, size_t vlen) {
    uint32_t len, addr, seq;
    as_rec r;
    int rc;
    if (vlen > 0xFFFFu) return as_err(db, ALTSQL_TOOBIG, "value too large");
    len = 1u + klen + (uint32_t)vlen;
    rc = as_reserve(db, len, type == AS_R_DEL ? AS_RES_DEL : AS_RES_GC);
    if (rc) return rc;
    db->rbuf[AS_RH] = (uint8_t)klen;
    memcpy(db->rbuf + AS_RH + 1, key, klen);
    if (vlen) memcpy(db->rbuf + AS_RH + 1 + klen, val, vlen);
    seq = db->next_rseq;
    rc = as_commit(db, type, seq, len, &addr);
    if (rc) return rc;
    db->full = 0;                  /* this may have made old records reclaimable */
    as_rec_fresh(db, &r, type, seq, len, addr);
    return as_note(db, &r);
}

static int as_user_key(altsql *db, const char *key, size_t *klen) {
    int rc = as_ready(db);
    if (rc) return rc;
    *klen = key ? strlen(key) : 0;
    if (*klen == 0 || *klen > AS_MAXKEY) return as_err(db, ALTSQL_MISUSE, "key must be 1 to 200 bytes");
    if ((unsigned char)key[0] == 0x01) return as_err(db, ALTSQL_MISUSE, "keys starting with byte 0x01 are reserved");
    return ALTSQL_OK;
}

int altsql_put(altsql *db, const char *key, const void *val, size_t len) {
    size_t klen;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    if (len && !val) return as_err(db, ALTSQL_MISUSE, "value is NULL");
    return as_kv_write(db, AS_R_PUT, (const uint8_t *)key, (uint32_t)klen, val, len);
}

int altsql_get(altsql *db, const char *key, void *buf, size_t cap, size_t *outlen) {
    size_t klen;
    uint32_t addr, len, vlen, n;
    uint8_t type, hb[AS_RH];
    const uint8_t *h;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    rc = as_kv_latest(db, (const uint8_t *)key, (uint32_t)klen, &addr, &type);
    if (rc) return rc;
    if (!addr || type != AS_R_PUT) return ALTSQL_NOTFOUND;
    h = as_view(db, addr, AS_RH, hb);
    if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
    len = as_get16(h + 2);
    if (len < 1u + (uint32_t)klen) return as_err(db, ALTSQL_CORRUPT, "damaged record");
    vlen = len - 1u - (uint32_t)klen;
    if (outlen) *outlen = vlen;
    n = vlen < cap ? vlen : (uint32_t)cap;
    if (n && buf) {
        if (db->fl.map) {
            const uint8_t *p = db->fl.map(db->fl.ctx, addr + AS_RH + 1 + (uint32_t)klen, n);
            if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
            memcpy(buf, p, n);
        } else if (db->fl.read(db->fl.ctx, addr + AS_RH + 1 + (uint32_t)klen, buf, n) != 0) {
            return as_err(db, ALTSQL_IOERR, "flash read failed");
        }
    }
    return ALTSQL_OK;
}

int altsql_del(altsql *db, const char *key) {
    size_t klen;
    uint32_t addr;
    uint8_t type;
    int rc = as_user_key(db, key, &klen);
    if (rc) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    rc = as_kv_latest(db, (const uint8_t *)key, (uint32_t)klen, &addr, &type);
    if (rc) return rc;
    if (!addr || type != AS_R_PUT) return ALTSQL_NOTFOUND;
    return as_kv_write(db, AS_R_DEL, (const uint8_t *)key, (uint32_t)klen, NULL, 0);
}

/* Live user keys in log order. Needs a complete index. */
int altsql_kv_each(altsql *db, altsql_kv_cb cb, void *ctx) {
    as_iter it;
    as_rec r;
    int rc;
    if (!db || !cb) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        uint32_t klen;
        if (r.type != AS_R_PUT || r.len < 1) continue;
        klen = r.p[0];
        if (!klen || 1u + klen > r.len || r.p[1] == 0x01) continue;
        if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
        if (cb(ctx, (const char *)r.p + 1, klen, r.p + 1 + klen, r.len - 1 - klen)) break;
    }
    return rc < 0 ? rc : ALTSQL_OK;
}

/* ---- 30_ts.c ----------------------------------------------------- */
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

/* ---- 40_sql.c ---------------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * SQL layer (gateway builds)
 *
 * A small recursive-descent parser builds an expression tree in the
 * working memory; the executor streams rows from the log through WHERE,
 * then grouping, HAVING, ordering and LIMIT. Each time-series is a table
 * whose columns come from its schema; "kv" is the key-value store.
 * ====================================================================== */
#if ALTSQL_ENABLE_SQL

#define AS_TMP_SIZE   4096u      /* per-row scratch for LOWER/UPPER */
#define AS_MAXITEMS   64
#define AS_MAXGROUP   16
#define AS_MAXDEPTH   200        /* deepest expression: parsing and evaluating
                                    recurse, so this bounds the stack they use */
#define AS_MAXNODES   10000      /* largest expression once aliases are expanded:
                                    bounds the work done for each row */

typedef struct as_arena { uint8_t *base; size_t cap, used; } as_arena;

static void *as_alloc(as_arena *a, size_t n) {
    void *p;
    n = (n + 7u) & ~(size_t)7u;
    if (n > a->cap - a->used) return NULL;
    p = a->base + a->used;
    a->used += n;
    return p;
}

/* ---- Values ----------------------------------------------------------------- */
static int as_isnum(const altsql_value *v) { return v->type == ALTSQL_INTEGER || v->type == ALTSQL_REAL; }
static double as_num(const altsql_value *v) {
    return v->type == ALTSQL_INTEGER ? (double)v->u.i : v->type == ALTSQL_REAL ? v->u.r : 0.0;
}

/* Order: NULL < numbers < text. Integers and reals compare by value. */
static int as_cmp(const altsql_value *a, const altsql_value *b) {
    if (a->type == ALTSQL_NULL || b->type == ALTSQL_NULL)
        return (a->type != ALTSQL_NULL) - (b->type != ALTSQL_NULL);
    if (as_isnum(a) && as_isnum(b)) {
        if (a->type == ALTSQL_INTEGER && b->type == ALTSQL_INTEGER)
            return (a->u.i > b->u.i) - (a->u.i < b->u.i);
        { double x = as_num(a), y = as_num(b); return (x > y) - (x < y); }
    }
    if (as_isnum(a)) return -1;
    if (as_isnum(b)) return 1;
    {
        int n = a->len < b->len ? a->len : b->len, c = memcmp(a->u.s, b->u.s, (size_t)n);
        return c ? (c > 0) - (c < 0) : (a->len > b->len) - (a->len < b->len);
    }
}

static int as_truth(const altsql_value *v) {
    return (v->type == ALTSQL_INTEGER && v->u.i != 0) || (v->type == ALTSQL_REAL && v->u.r != 0.0);
}

static uint32_t as_vhash(const altsql_value *v) {
    uint8_t b[8];
    if (v->type == ALTSQL_TEXT) return as_hash((const uint8_t *)v->u.s, (uint32_t)v->len) ^ 0x5bd1e995u;
    if (v->type == ALTSQL_NULL) return 0x9e3779b9u;
    if (v->type == ALTSQL_REAL && v->u.r >= -9.2e18 && v->u.r <= 9.2e18 && v->u.r == (double)(int64_t)v->u.r) {
        as_put64(b, (uint64_t)(int64_t)v->u.r);          /* 2.0 groups with 2 */
    } else if (v->type == ALTSQL_REAL) {
        uint64_t x;
        memcpy(&x, &v->u.r, 8);
        as_put64(b, x);
    } else {
        as_put64(b, (uint64_t)v->u.i);
    }
    return as_hash(b, 8);
}

static int as_vcopy(as_arena *A, altsql_value *dst, const altsql_value *src) {
    *dst = *src;
    if (src->type == ALTSQL_TEXT && src->len > 0) {
        char *p = (char *)as_alloc(A, (size_t)src->len);
        if (!p) return ALTSQL_NOMEM;
        memcpy(p, src->u.s, (size_t)src->len);
        dst->u.s = p;
    }
    return ALTSQL_OK;
}

static void as_setint(altsql_value *v, int64_t i) { v->type = ALTSQL_INTEGER; v->len = 0; v->u.i = i; }
static void as_setreal(altsql_value *v, double r) { v->type = ALTSQL_REAL; v->len = 0; v->u.r = r; }
static void as_setnull(altsql_value *v) { v->type = ALTSQL_NULL; v->len = 0; v->u.i = 0; }

/* ---- Lexer --------------------------------------------------------------------- */
enum { K_END, K_ID, K_INT, K_REAL, K_STR, K_OP };
enum { OP_LE = 'l', OP_GE = 'g', OP_NE = 'n' };

typedef struct as_tok { int k, op; const char *s; size_t n; int64_t i; double r; } as_tok;

typedef struct as_parser {
    altsql *db;
    const char *p, *pend;          /* pend: end of the token consumed last */
    as_tok t;
    as_arena *A;
    int rc;
    int nest;                      /* recursion depth of the expression parser */
} as_parser;

static int as_perr(as_parser *P, const char *msg) {
    if (!P->rc) {
        size_t n = P->t.k == K_END ? 3 : (P->t.n > 24 ? 24 : P->t.n);
        as_err2(P->db, ALTSQL_SYNTAX, msg, P->t.k == K_END ? "end" : P->t.s, n);
        P->rc = ALTSQL_SYNTAX;
    }
    return P->rc;
}

static void as_lex(as_parser *P) {
    const char *s = P->p;
    as_tok *t = &P->t;
    P->pend = s;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (s[0] == '-' && s[1] == '-') { while (*s && *s != '\n') s++; continue; }
        break;
    }
    t->s = s;
    t->op = 0;
    if (!*s) { t->k = K_END; t->n = 0; P->p = s; return; }
    if (as_is_ident_start((unsigned char)*s)) {
        while (as_is_ident_char((unsigned char)*s)) s++;
        t->k = K_ID;
    } else if ((*s >= '0' && *s <= '9') || (*s == '.' && s[1] >= '0' && s[1] <= '9')) {
        char *e;
        int real = 0;
        const char *q = s;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.' || *q == 'e' || *q == 'E') real = 1;
        if (!real) {
            errno = 0;
            t->i = strtoll(s, &e, 10);
            if (e == q && errno != ERANGE) { t->k = K_INT; s = q; }
            else real = 1;
        }
        if (real) { t->r = strtod(s, &e); t->k = K_REAL; s = e; }
    } else if (*s == '\'') {
        s++;
        while (*s && !(*s == '\'' && s[1] != '\'')) s += (*s == '\'') ? 2 : 1;
        if (!*s) { t->k = K_END; t->n = 0; P->p = s; as_perr(P, "unterminated string near "); return; }
        s++;
        t->k = K_STR;
    } else {
        t->k = K_OP;
        t->op = *s++;
        if (t->op == '<' && *s == '=') { t->op = OP_LE; s++; }
        else if (t->op == '>' && *s == '=') { t->op = OP_GE; s++; }
        else if (t->op == '<' && *s == '>') { t->op = OP_NE; s++; }
        else if (t->op == '!' && *s == '=') { t->op = OP_NE; s++; }
        else if (t->op == '=' && *s == '=') { s++; }
        else if (!strchr("(),*+-/%=<>;", t->op)) { t->n = 1; P->p = s; as_perr(P, "unexpected character "); return; }
    }
    t->n = (size_t)(s - t->s);
    P->p = s;
}

static int as_kw(const as_parser *P, const char *kw) {
    return P->t.k == K_ID && as_ieq(P->t.s, P->t.n, kw, strlen(kw));
}
static int as_isop(const as_parser *P, int op) { return P->t.k == K_OP && P->t.op == op; }
static int as_accept_kw(as_parser *P, const char *kw) { if (as_kw(P, kw)) { as_lex(P); return 1; } return 0; }
static int as_accept_op(as_parser *P, int op) { if (as_isop(P, op)) { as_lex(P); return 1; } return 0; }
static int as_expect_kw(as_parser *P, const char *kw) { if (as_accept_kw(P, kw)) return 1; as_perr(P, "syntax error near "); return 0; }
static int as_expect_op(as_parser *P, int op) { if (as_accept_op(P, op)) return 1; as_perr(P, "syntax error near "); return 0; }

static int as_reserved(const as_parser *P) {
    static const char *const kws[] = { "SELECT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER",
        "LIMIT", "OFFSET", "ASC", "DESC", "AND", "OR", "NOT", "LIKE", "IS", "NULL", "BETWEEN",
        "AS", "VALUES", "INTO", 0 };
    int i;
    for (i = 0; kws[i]; i++) if (as_kw(P, kws[i])) return 1;
    return 0;
}

/* ---- Expressions ---------------------------------------------------------------- */
enum { E_LIT = 1, E_COL, E_NAME, E_NEG, E_NOT, E_BIN, E_AND, E_OR, E_ISNULL, E_BETWEEN, E_LIKE, E_FN, E_AGG, E_STAR };
enum { F_COUNT = 1, F_SUM, F_AVG, F_MIN, F_MAX, F_ABS, F_ROUND, F_LENGTH, F_LOWER, F_UPPER };

typedef struct as_expr {
    uint8_t k, op, notf, fn;
    int16_t idx;
    struct as_expr *a, *b, *c;
    altsql_value v;
    const char *s0;
    size_t sn;
    uint16_t depth;                /* measured once names are resolved; 0 = not yet */
    uint32_t size;                 /* nodes, counting shared alias trees each time */
} as_expr;

static as_expr *as_node(as_parser *P, int k, const char *s0) {
    as_expr *e = (as_expr *)as_alloc(P->A, sizeof *e);
    if (!e) { if (!P->rc) P->rc = as_err(P->db, ALTSQL_NOMEM, "out of working memory"); return NULL; }
    memset(e, 0, sizeof *e);
    e->k = (uint8_t)k;
    e->s0 = s0;
    return e;
}
static void as_span(as_parser *P, as_expr *e) { if (e) e->sn = (size_t)(P->pend - e->s0); }

static as_expr *as_parse_expr(as_parser *P);

static int as_fn_id(const char *s, size_t n) {
    static const char *const f[] = { "", "COUNT", "SUM", "AVG", "MIN", "MAX", "ABS", "ROUND", "LENGTH", "LOWER", "UPPER" };
    int i;
    for (i = 1; i <= F_UPPER; i++) if (as_ieq(s, n, f[i], strlen(f[i]))) return i;
    return 0;
}

static as_expr *as_parse_primary(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e;
    if (P->rc) return NULL;
    if (P->t.k == K_INT || P->t.k == K_REAL) {
        e = as_node(P, E_LIT, s0);
        if (!e) return NULL;
        if (P->t.k == K_INT) as_setint(&e->v, P->t.i); else as_setreal(&e->v, P->t.r);
        as_lex(P);
        as_span(P, e);
        return e;
    }
    if (P->t.k == K_STR) {
        size_t i, n = 0;
        char *d;
        e = as_node(P, E_LIT, s0);
        d = (char *)as_alloc(P->A, P->t.n);
        if (!e || !d) { if (!P->rc) P->rc = as_err(P->db, ALTSQL_NOMEM, "out of working memory"); return NULL; }
        for (i = 1; i + 1 < P->t.n; i++) {
            d[n++] = P->t.s[i];
            if (P->t.s[i] == '\'') i++;
        }
        e->v.type = ALTSQL_TEXT;
        e->v.u.s = d;
        e->v.len = (int)n;
        as_lex(P);
        as_span(P, e);
        return e;
    }
    if (as_accept_kw(P, "NULL")) {
        e = as_node(P, E_LIT, s0);
        if (e) { as_setnull(&e->v); as_span(P, e); }
        return e;
    }
    if (as_accept_op(P, '(')) {
        e = as_parse_expr(P);
        if (!as_expect_op(P, ')')) return NULL;
        return e;
    }
    if (P->t.k == K_ID && !as_reserved(P)) {
        const char *name = P->t.s;
        size_t n = P->t.n;
        as_lex(P);
        if (as_accept_op(P, '(')) {
            int fn = as_fn_id(name, n);
            if (!fn) { P->t.s = name; P->t.n = n; as_perr(P, "unknown function "); return NULL; }
            e = as_node(P, E_FN, s0);
            if (!e) return NULL;
            e->fn = (uint8_t)fn;
            if (fn == F_COUNT && as_isop(P, '*')) {
                e->a = as_node(P, E_STAR, P->t.s);
                as_lex(P);
            } else {
                e->a = as_parse_expr(P);
                if (as_accept_op(P, ',')) e->b = as_parse_expr(P);
            }
            if (!as_expect_op(P, ')')) return NULL;
            if (!e->a || (fn != F_ROUND && e->b)) { as_perr(P, "wrong number of arguments near "); return NULL; }
            as_span(P, e);
            return e;
        }
        e = as_node(P, E_NAME, s0);
        if (e) e->sn = n;
        return e;
    }
    as_perr(P, "syntax error near ");
    return NULL;
}

/* Every recursive step of the parser goes through here, so hostile input
 * such as 100,000 nested brackets gives an error instead of a stack overflow. */
static int as_nest_in(as_parser *P) {
    if (++P->nest <= AS_MAXDEPTH) return 1;
    if (!P->rc) P->rc = as_err(P->db, ALTSQL_SYNTAX, "expression nested too deeply");
    return 0;
}

static as_expr *as_parse_unary(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = NULL;
    if (as_accept_op(P, '-')) {
        e = as_node(P, E_NEG, s0);
        if (as_nest_in(P) && e) { e->a = as_parse_unary(P); as_span(P, e); }
        P->nest--;
        return P->rc ? NULL : e;
    }
    if (as_accept_op(P, '+')) {
        if (as_nest_in(P)) e = as_parse_unary(P);
        P->nest--;
        return P->rc ? NULL : e;
    }
    return as_parse_primary(P);
}

static as_expr *as_bin(as_parser *P, int k, int op, as_expr *a, as_expr *b, const char *s0) {
    as_expr *e = as_node(P, k, s0);
    if (!e) return NULL;
    e->op = (uint8_t)op;
    e->a = a;
    e->b = b;
    as_span(P, e);
    return e;
}

static as_expr *as_parse_mul(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_unary(P);
    while (!P->rc && P->t.k == K_OP && (P->t.op == '*' || P->t.op == '/' || P->t.op == '%')) {
        int op = P->t.op;
        as_lex(P);
        e = as_bin(P, E_BIN, op, e, as_parse_unary(P), s0);
    }
    return e;
}

static as_expr *as_parse_add(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_mul(P);
    while (!P->rc && P->t.k == K_OP && (P->t.op == '+' || P->t.op == '-')) {
        int op = P->t.op;
        as_lex(P);
        e = as_bin(P, E_BIN, op, e, as_parse_mul(P), s0);
    }
    return e;
}

static as_expr *as_parse_cmp(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_add(P);
    int neg;
    if (P->rc) return NULL;
    if (P->t.k == K_OP && strchr("=<>", P->t.op ? P->t.op : 1)) {
        int op = P->t.op;
        as_lex(P);
        return as_bin(P, E_BIN, op, e, as_parse_add(P), s0);
    }
    if (P->t.k == K_OP && (P->t.op == OP_LE || P->t.op == OP_GE || P->t.op == OP_NE)) {
        int op = P->t.op;
        as_lex(P);
        return as_bin(P, E_BIN, op, e, as_parse_add(P), s0);
    }
    if (as_accept_kw(P, "IS")) {
        as_expr *r = as_node(P, E_ISNULL, s0);
        if (!r) return NULL;
        r->notf = (uint8_t)as_accept_kw(P, "NOT");
        if (!as_expect_kw(P, "NULL")) return NULL;
        r->a = e;
        as_span(P, r);
        return r;
    }
    neg = as_accept_kw(P, "NOT");
    if (as_accept_kw(P, "LIKE")) {
        as_expr *r = as_bin(P, E_LIKE, 0, e, as_parse_add(P), s0);
        if (r) r->notf = (uint8_t)neg;
        return r;
    }
    if (as_accept_kw(P, "BETWEEN")) {
        as_expr *r = as_node(P, E_BETWEEN, s0);
        if (!r) return NULL;
        r->notf = (uint8_t)neg;
        r->a = e;
        r->b = as_parse_add(P);
        if (!as_expect_kw(P, "AND")) return NULL;
        r->c = as_parse_add(P);
        as_span(P, r);
        return r;
    }
    if (neg) { as_perr(P, "expected LIKE or BETWEEN near "); return NULL; }
    return e;
}

static as_expr *as_parse_not(as_parser *P) {
    const char *s0 = P->t.s;
    if (as_accept_kw(P, "NOT")) {
        as_expr *e = as_node(P, E_NOT, s0);
        if (as_nest_in(P) && e) { e->a = as_parse_not(P); as_span(P, e); }
        P->nest--;
        return P->rc ? NULL : e;
    }
    return as_parse_cmp(P);
}

static as_expr *as_parse_and(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_not(P);
    while (!P->rc && as_accept_kw(P, "AND")) e = as_bin(P, E_AND, 0, e, as_parse_not(P), s0);
    return e;
}

/* 1 if the tree is deeper than AS_MAXDEPTH. Stops descending at the limit,
 * so it never recurses deeper than that itself. A long chain such as
 * 1+1+1+... is built by a loop, not by recursion, so it is checked here. */
static int as_too_deep(const as_expr *e, int d) {
    if (!e) return 0;
    if (d > AS_MAXDEPTH) return 1;
    return as_too_deep(e->a, d + 1) || as_too_deep(e->b, d + 1) || as_too_deep(e->c, d + 1);
}

static as_expr *as_parse_expr(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = NULL;
    if (as_nest_in(P)) {
        e = as_parse_and(P);
        while (!P->rc && as_accept_kw(P, "OR")) e = as_bin(P, E_OR, 0, e, as_parse_and(P), s0);
    }
    P->nest--;
    if (!P->rc && P->nest == 0 && as_too_deep(e, 1))
        P->rc = as_err(P->db, ALTSQL_SYNTAX, "expression nested too deeply");
    return P->rc ? NULL : e;
}

/* ---- Evaluation ------------------------------------------------------------------ */
typedef struct as_acc {
    int64_t n, isum;
    double rsum;
    int real, has, cap[2];
    altsql_value mn, mx;
    char *buf[2];                 /* text buffers for MIN and MAX, reused */
} as_acc;
typedef struct as_ctx { const altsql_value *row; as_acc *acc; as_arena *tmp; as_arena *perm; } as_ctx;

static int as_like(const char *s, size_t sn, const char *p, size_t pn) {
    size_t si = 0, pi = 0, sp = (size_t)-1, ss = 0;
    while (si < sn) {
        if (pi < pn && p[pi] == '%') { sp = pi++; ss = si; }
        else if (pi < pn && (p[pi] == '_' || as_lower((unsigned char)p[pi]) == as_lower((unsigned char)s[si]))) { si++; pi++; }
        else if (sp != (size_t)-1) { pi = sp + 1; si = ++ss; }
        else return 0;
    }
    while (pi < pn && p[pi] == '%') pi++;
    return pi == pn;
}

static double as_round(double x, int64_t n) {
    double m = 1.0, y;
    int64_t i;
    if (n < 0) n = 0;
    if (n > 15) n = 15;
    for (i = 0; i < n; i++) m *= 10.0;
    y = x * m;
    if (!(y > -9.0e18 && y < 9.0e18)) return x;         /* too big to round, or not a number */
    y = (double)(int64_t)(y >= 0 ? y + 0.5 : y - 0.5);
    return y / m;
}

static int as_eval(as_ctx *c, const as_expr *e, altsql_value *o);

static int as_mul_overflows(int64_t a, int64_t b) {
    if (a > 0) return b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    return b > 0 ? a < INT64_MIN / b : (a != 0 && b < INT64_MAX / a);
}

/* Integer arithmetic stays integer; on overflow the result becomes real. */
static int as_arith(int op, const altsql_value *x, const altsql_value *y, altsql_value *o) {
    double a, b;
    if (!as_isnum(x) || !as_isnum(y)) { as_setnull(o); return 0; }
    if (x->type == ALTSQL_INTEGER && y->type == ALTSQL_INTEGER) {
        int64_t i = x->u.i, j = y->u.i;
        switch (op) {
        case '+':
            if ((j > 0 && i > INT64_MAX - j) || (j < 0 && i < INT64_MIN - j)) break;
            as_setint(o, i + j); return 0;
        case '-':
            if ((j < 0 && i > INT64_MAX + j) || (j > 0 && i < INT64_MIN + j)) break;
            as_setint(o, i - j); return 0;
        case '*':
            if (as_mul_overflows(i, j)) break;
            as_setint(o, i * j); return 0;
        case '/':
            if (j == 0) as_setnull(o);
            else if (j == -1 && i == INT64_MIN) break;
            else as_setint(o, i / j);
            return 0;
        default:
            if (j == 0) as_setnull(o);
            else as_setint(o, j == -1 ? 0 : i % j);
            return 0;
        }
    }
    a = as_num(x);
    b = as_num(y);
    switch (op) {
    case '+': as_setreal(o, a + b); return 0;
    case '-': as_setreal(o, a - b); return 0;
    case '*': as_setreal(o, a * b); return 0;
    case '/': if (b == 0.0) as_setnull(o); else as_setreal(o, a / b); return 0;
    default:
        if (b == 0.0) as_setnull(o);
        else {
            double q = a / b;
            if (!(q > -9.0e18 && q < 9.0e18)) as_setnull(o);
            else as_setreal(o, a - (double)(int64_t)q * b);
        }
        return 0;
    }
}

static int as_eval(as_ctx *c, const as_expr *e, altsql_value *o) {
    altsql_value x, y, z;
    int rc;
    switch (e->k) {
    case E_LIT: *o = e->v; return 0;
    case E_COL:
        if (c->row) *o = c->row[e->idx]; else as_setnull(o);
        return 0;
    case E_AGG: {
        const as_acc *a = &c->acc[e->idx];
        switch (e->fn) {
        case F_COUNT: as_setint(o, a->n); return 0;
        case F_SUM:
            if (!a->has) as_setnull(o);
            else if (a->real) as_setreal(o, a->rsum);
            else as_setint(o, a->isum);
            return 0;
        case F_AVG:
            if (!a->has || !a->n) as_setnull(o);
            else as_setreal(o, (a->real ? a->rsum : (double)a->isum) / (double)a->n);
            return 0;
        case F_MIN: if (a->has) *o = a->mn; else as_setnull(o); return 0;
        default:    if (a->has) *o = a->mx; else as_setnull(o); return 0;
        }
    }
    case E_NEG:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if (x.type == ALTSQL_INTEGER && x.u.i != INT64_MIN) as_setint(o, -x.u.i);
        else if (x.type == ALTSQL_INTEGER) as_setreal(o, -(double)x.u.i);
        else if (x.type == ALTSQL_REAL) as_setreal(o, -x.u.r);
        else as_setnull(o);
        return 0;
    case E_NOT:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if (x.type == ALTSQL_NULL) as_setnull(o); else as_setint(o, !as_truth(&x));
        return 0;
    case E_AND: case E_OR: {
        int isand = e->k == E_AND, xn, yn, xt, yt;
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        xn = x.type == ALTSQL_NULL;
        xt = as_truth(&x);
        if (!xn && xt != isand) { as_setint(o, xt); return 0; }     /* short circuit */
        if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
        yn = y.type == ALTSQL_NULL;
        yt = as_truth(&y);
        if (!yn && yt != isand) { as_setint(o, yt); return 0; }
        if (xn || yn) as_setnull(o); else as_setint(o, isand);
        return 0;
    }
    case E_BIN:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
        if (strchr("+-*/%", e->op)) return as_arith(e->op, &x, &y, o);
        if (x.type == ALTSQL_NULL || y.type == ALTSQL_NULL) { as_setnull(o); return 0; }
        rc = as_cmp(&x, &y);
        switch (e->op) {
        case '=':   as_setint(o, rc == 0); break;
        case OP_NE: as_setint(o, rc != 0); break;
        case '<':   as_setint(o, rc < 0); break;
        case OP_LE: as_setint(o, rc <= 0); break;
        case '>':   as_setint(o, rc > 0); break;
        default:    as_setint(o, rc >= 0); break;
        }
        return 0;
    case E_ISNULL:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        as_setint(o, (x.type == ALTSQL_NULL) != e->notf);
        return 0;
    case E_BETWEEN:
        if ((rc = as_eval(c, e->a, &x)) != 0 || (rc = as_eval(c, e->b, &y)) != 0 || (rc = as_eval(c, e->c, &z)) != 0) return rc;
        if (x.type == ALTSQL_NULL || y.type == ALTSQL_NULL || z.type == ALTSQL_NULL) { as_setnull(o); return 0; }
        as_setint(o, (as_cmp(&x, &y) >= 0 && as_cmp(&x, &z) <= 0) != e->notf);
        return 0;
    case E_LIKE:
        if ((rc = as_eval(c, e->a, &x)) != 0 || (rc = as_eval(c, e->b, &y)) != 0) return rc;
        if (x.type != ALTSQL_TEXT || y.type != ALTSQL_TEXT) { as_setnull(o); return 0; }
        as_setint(o, as_like(x.u.s, (size_t)x.len, y.u.s, (size_t)y.len) != e->notf);
        return 0;
    case E_FN:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        switch (e->fn) {
        case F_ABS:
            if (x.type == ALTSQL_INTEGER && x.u.i == INT64_MIN) as_setreal(o, -(double)x.u.i);
            else if (x.type == ALTSQL_INTEGER) as_setint(o, x.u.i < 0 ? -x.u.i : x.u.i);
            else if (x.type == ALTSQL_REAL) as_setreal(o, x.u.r < 0 ? -x.u.r : x.u.r);
            else as_setnull(o);
            return 0;
        case F_ROUND: {
            int64_t n = 0;
            if (e->b) {
                if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
                if (y.type == ALTSQL_INTEGER) n = y.u.i;
                else {                              /* as_round keeps 0 to 15 digits */
                    double d = as_num(&y);
                    n = d >= 15.0 ? 15 : d > 0.0 ? (int64_t)d : 0;
                }
            }
            if (!as_isnum(&x)) as_setnull(o); else as_setreal(o, as_round(as_num(&x), n));
            return 0;
        }
        case F_LENGTH:
            if (x.type == ALTSQL_TEXT) as_setint(o, x.len);
            else if (x.type == ALTSQL_NULL) as_setnull(o);
            else {
                char nb[40];
                as_setint(o, (int64_t)as_fmt_value(&x, nb, sizeof nb));
            }
            return 0;
        default: {        /* LOWER, UPPER */
            char *d;
            int i;
            if (x.type != ALTSQL_TEXT) { *o = x; return 0; }
            d = (char *)as_alloc(c->tmp, (size_t)x.len + 1);
            if (!d) return ALTSQL_NOMEM;
            for (i = 0; i < x.len; i++) {
                int ch = (unsigned char)x.u.s[i];
                d[i] = (char)(e->fn == F_LOWER ? as_lower(ch) : (ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
            }
            *o = x;
            o->u.s = d;
            return 0;
        }
        }
    default:
        as_setnull(o);
        return 0;
    }
}

/* ---- Queries ------------------------------------------------------------------------ */
typedef struct as_group {
    struct as_group *next, *order;
    uint32_t hash;
    int hasrep;
    altsql_value *keys, *rep;
    as_acc *acc;
} as_group;

typedef struct as_q {
    altsql *db;
    as_arena *A, tmp;
    int kind;                               /* 0 no table, 1 series, 2 kv */
    as_series *S;
    int ncols;
    const char *cname[AS_MAXCOLS];
    size_t cnlen[AS_MAXCOLS];
    as_expr *items[AS_MAXITEMS];
    const char *names[AS_MAXITEMS];
    const char *alias[AS_MAXITEMS];         /* "AS name" of each item, or NULL */
    size_t alen[AS_MAXITEMS];
    int nitems;
    as_expr *where, *having;
    as_expr *group[AS_MAXGROUP];
    int ngroup;
    as_expr *order[AS_MAXGROUP];
    int odesc[AS_MAXGROUP], norder;
    as_expr *aggs[AS_MAXITEMS];
    int naggs, grouped;
    int64_t limit, offset, emitted, skipped, tmin;
    int stop;
    altsql_row_cb cb;
    void *cbctx;
    as_group **tab, *first, *last;
    uint32_t nbuckets;
    altsql_value **rows;
    size_t nrows, caprows;
    altsql_value *outv;
} as_q;

static void as_colname_store(void *ctx, int idx, const char *name, size_t n) {
    as_q *q = (as_q *)ctx;
    q->cname[idx] = name;
    q->cnlen[idx] = n;
}

static int as_has_agg(const as_expr *e) {
    if (!e) return 0;
    if (e->k == E_AGG) return 1;
    return as_has_agg(e->a) || as_has_agg(e->b) || as_has_agg(e->c);
}

/* Depth and size of a resolved expression. An alias is resolved by sharing
 * the item's tree, so a chain of aliases (b = a + a, c = b + b, ...) can
 * stand for a huge expression; each node is measured once, so this stays
 * quick, and the caller refuses expressions that are too deep or too big. */
static void as_measure(as_expr *e) {
    as_expr *kid[3];
    uint32_t size = 1, d = 0;
    int i;
    if (e->depth) return;
    kid[0] = e->a; kid[1] = e->b; kid[2] = e->c;
    for (i = 0; i < 3; i++) {
        if (!kid[i]) continue;
        as_measure(kid[i]);
        if (kid[i]->depth > d) d = kid[i]->depth;
        size += kid[i]->size;
        if (size > AS_MAXNODES) size = AS_MAXNODES + 1;
    }
    e->depth = (uint16_t)(d < AS_MAXDEPTH ? d + 1 : AS_MAXDEPTH + 1);
    e->size = size;
}

static int as_checked(as_parser *P, as_q *q, as_expr *e) {
    if (!e) return 0;
    as_measure(e);
    if (e->depth > AS_MAXDEPTH || e->size > AS_MAXNODES)
        return P->rc = as_err(q->db, ALTSQL_SYNTAX, "expression too large once aliases are expanded");
    return 0;
}

/* Resolves names to columns (or to select items by alias) and collects
 * aggregates. in_agg: inside an aggregate call. */
static int as_resolve(as_parser *P, as_q *q, as_expr *e, int allow_agg, int in_agg) {
    int i, rc;
    if (!e) return 0;
    if (e->k == E_NAME) {
        for (i = 0; i < q->ncols; i++) {
            if (as_ieq(e->s0, e->sn, q->cname[i], q->cnlen[i])) {
                e->k = E_COL;
                e->idx = (int16_t)i;
                return 0;
            }
        }
        for (i = 0; i < q->nitems && !in_agg; i++) {
            if (q->alias[i] && as_ieq(e->s0, e->sn, q->alias[i], q->alen[i]) && q->items[i]->k != E_NAME) {
                if (!allow_agg && as_has_agg(q->items[i]))
                    return P->rc = as_err(q->db, ALTSQL_SYNTAX, "aggregate functions are not allowed here");
                *e = *q->items[i];            /* the item is already resolved */
                return 0;
            }
        }
        return P->rc = as_err2(q->db, ALTSQL_SCHEMA, "no such column: ", e->s0, e->sn);
    }
    if (e->k == E_FN && e->fn <= F_MAX) {
        if (!allow_agg || in_agg) return P->rc = as_err(q->db, ALTSQL_SYNTAX, "aggregate functions are not allowed here");
        if (q->naggs >= AS_MAXITEMS) return P->rc = as_err(q->db, ALTSQL_NOMEM, "too many aggregates");
        e->k = E_AGG;
        e->idx = (int16_t)q->naggs;
        q->aggs[q->naggs++] = e;
        if (e->a && e->a->k != E_STAR) return as_resolve(P, q, e->a, allow_agg, 1);
        return 0;
    }
    if ((rc = as_resolve(P, q, e->a, allow_agg, in_agg)) != 0) return rc;
    if ((rc = as_resolve(P, q, e->b, allow_agg, in_agg)) != 0) return rc;
    return as_resolve(P, q, e->c, allow_agg, in_agg);
}

/* Name of a select item: alias, column name, or the expression text. */
static const char *as_strdup_n(as_arena *A, const char *s, size_t n) {
    char *d = (char *)as_alloc(A, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* An integer literal or an alias in GROUP BY / ORDER BY refers to a select item. */
static int as_item_ref(as_q *q, const as_expr *e) {
    int i;
    if (e->k == E_LIT && e->v.type == ALTSQL_INTEGER && e->v.u.i >= 1 && e->v.u.i <= q->nitems)
        return (int)e->v.u.i - 1;
    if (e->k == E_NAME)
        for (i = 0; i < q->nitems; i++)
            if (q->alias[i] && as_ieq(e->s0, e->sn, q->alias[i], q->alen[i])) return i;
    return -1;
}

/* Lower bound on the time column implied by WHERE, used to skip sectors. */
static void as_q_bounds(as_q *q, const as_expr *e) {
    const as_expr *lit = NULL;
    int op, flip = 0;
    int64_t L;
    if (!e || q->kind != 1) return;
    if (e->k == E_AND) { as_q_bounds(q, e->a); as_q_bounds(q, e->b); return; }
    if (e->k == E_BETWEEN && !e->notf && e->a->k == E_COL && e->a->idx == 0 && e->b->k == E_LIT) {
        lit = e->b;
        op = OP_GE;
    } else if (e->k == E_BIN) {
        if (e->a->k == E_COL && e->a->idx == 0 && e->b->k == E_LIT) lit = e->b;
        else if (e->b->k == E_COL && e->b->idx == 0 && e->a->k == E_LIT) { lit = e->a; flip = 1; }
        op = e->op;
        if (flip) op = op == '<' ? '>' : op == '>' ? '<' : op == OP_LE ? OP_GE : op == OP_GE ? OP_LE : op;
    } else {
        return;
    }
    if (!lit || !as_isnum(&lit->v) || !(op == '>' || op == OP_GE || op == '=')) return;
    if (lit->v.type == ALTSQL_REAL) {
        if (!(lit->v.u.r > -9.0e18 && lit->v.u.r < 9.0e18)) return;
        L = (int64_t)lit->v.u.r - 1;
    } else {
        L = lit->v.u.i;
    }
    if (L > q->tmin) q->tmin = L;
}

/* Keeps a copy of v as the running MIN (which 0) or MAX (which 1). */
static int as_acc_keep(as_arena *A, as_acc *a, int which, const altsql_value *v) {
    altsql_value *dst = which ? &a->mx : &a->mn;
    *dst = *v;
    if (v->type == ALTSQL_TEXT && v->len > 0) {
        if (a->cap[which] < v->len) {
            int c = v->len < 32 ? 32 : v->len;
            a->buf[which] = (char *)as_alloc(A, (size_t)c);
            if (!a->buf[which]) return ALTSQL_NOMEM;
            a->cap[which] = c;
        }
        memcpy(a->buf[which], v->u.s, (size_t)v->len);
        dst->u.s = a->buf[which];
    }
    return ALTSQL_OK;
}

static int as_acc_add(as_arena *A, as_acc *a, int fn, const altsql_value *v, int star) {
    if (fn == F_COUNT) { if (star || v->type != ALTSQL_NULL) a->n++; return 0; }
    if (v->type == ALTSQL_NULL) return 0;
    if (fn == F_SUM || fn == F_AVG) {
        if (!as_isnum(v)) return 0;
        a->n++;
        if (v->type == ALTSQL_INTEGER && !a->real) {
            int64_t x = v->u.i;
            if ((x > 0 && a->isum > INT64_MAX - x) || (x < 0 && a->isum < INT64_MIN - x)) {
                a->rsum = (double)a->isum + (double)x;      /* would overflow: go real */
                a->real = 1;
            } else {
                a->isum += x;
            }
        } else {
            if (!a->real) { a->rsum = (double)a->isum; a->real = 1; }
            a->rsum += as_num(v);
        }
        a->has = 1;
        return 0;
    }
    if (fn == F_MIN && a->has && as_cmp(v, &a->mn) >= 0) return 0;
    if (fn == F_MAX && a->has && as_cmp(v, &a->mx) <= 0) return 0;
    a->has = 1;
    return as_acc_keep(A, a, fn == F_MAX, v);
}

static int as_q_emit(as_q *q, const altsql_value *v) {
    if (q->skipped < q->offset) { q->skipped++; return 0; }
    if (q->limit >= 0 && q->emitted >= q->limit) { q->stop = 1; return 0; }
    q->emitted++;
    if (q->cb && q->cb(q->cbctx, q->nitems, v, q->names)) q->stop = 1;
    if (q->limit >= 0 && q->emitted >= q->limit) q->stop = 1;
    return 0;
}

static int as_rowcmp(const as_q *q, const altsql_value *a, const altsql_value *b) {
    int i, c;
    for (i = 0; i < q->norder; i++) {
        c = as_cmp(&a[q->nitems + i], &b[q->nitems + i]);
        if (c) return q->odesc[i] ? -c : c;
    }
    return 0;
}

/* Copies the current output into row, reusing its text space when it fits. */
static int as_q_fill(as_q *q, altsql_value *row, int reuse) {
    int i, n = q->nitems + q->norder;
    for (i = 0; i < n; i++) {
        const altsql_value *v = &q->outv[i];
        if (reuse && v->type == ALTSQL_TEXT && row[i].type == ALTSQL_TEXT && v->len <= row[i].len) {
            char *d = (char *)(size_t)row[i].u.s;       /* our own copy, made earlier */
            memcpy(d, v->u.s, (size_t)v->len);
            row[i].len = v->len;
            continue;
        }
        if (as_vcopy(q->A, &row[i], v)) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
    }
    return 0;
}

/* Max-heap on the ORDER BY keys: the root is the worst row kept. */
static void as_heap_down(as_q *q, size_t i) {
    for (;;) {
        size_t c = 2 * i + 1;
        altsql_value *t;
        if (c >= q->nrows) return;
        if (c + 1 < q->nrows && as_rowcmp(q, q->rows[c + 1], q->rows[c]) > 0) c++;
        if (as_rowcmp(q, q->rows[c], q->rows[i]) <= 0) return;
        t = q->rows[i]; q->rows[i] = q->rows[c]; q->rows[c] = t;
        i = c;
    }
}

static void as_heap_up(as_q *q, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        altsql_value *t;
        if (as_rowcmp(q, q->rows[i], q->rows[p]) <= 0) return;
        t = q->rows[i]; q->rows[i] = q->rows[p]; q->rows[p] = t;
        i = p;
    }
}

/* Evaluates the select items (and ORDER BY keys) for one row or group.
 * With ORDER BY, rows are kept for sorting; with ORDER BY and LIMIT only
 * the best OFFSET + LIMIT rows are kept, so memory stays small. */
static int as_q_out(as_q *q, as_ctx *c) {
    int i, rc, n = q->nitems + q->norder;
    int64_t keep = -1;
    for (i = 0; i < q->nitems; i++) if ((rc = as_eval(c, q->items[i], &q->outv[i])) != 0) return rc;
    if (!q->norder) return as_q_emit(q, q->outv);
    for (i = 0; i < q->norder; i++) if ((rc = as_eval(c, q->order[i], &q->outv[q->nitems + i])) != 0) return rc;
    if (q->limit >= 0 && q->offset <= INT64_MAX - q->limit) keep = q->limit + q->offset;
    if (keep == 0) return 0;
    if (keep > 0 && (int64_t)q->nrows == keep) {                /* top-N: replace the worst */
        if (as_rowcmp(q, q->outv, q->rows[0]) >= 0) return 0;
        if ((rc = as_q_fill(q, q->rows[0], 1)) != 0) return rc;
        as_heap_down(q, 0);
        return 0;
    }
    if (q->nrows == q->caprows) {
        size_t nc = q->caprows ? q->caprows * 2 : 256;
        altsql_value **nr = (altsql_value **)as_alloc(q->A, nc * sizeof *nr);
        if (!nr) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
        if (q->nrows) memcpy(nr, q->rows, q->nrows * sizeof *nr);
        q->rows = nr;
        q->caprows = nc;
    }
    {
        altsql_value *row = (altsql_value *)as_alloc(q->A, (size_t)n * sizeof *row);
        if (!row) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
        if ((rc = as_q_fill(q, row, 0)) != 0) return rc;
        q->rows[q->nrows++] = row;
        if (keep > 0) as_heap_up(q, q->nrows - 1);
    }
    return 0;
}

static as_group *as_q_newgroup(as_q *q, uint32_t h, const altsql_value *keys, const altsql_value *row) {
    as_group *g = (as_group *)as_alloc(q->A, sizeof *g);
    int i;
    if (!g) return NULL;
    memset(g, 0, sizeof *g);
    g->hash = h;
    g->keys = (altsql_value *)as_alloc(q->A, sizeof(altsql_value) * (size_t)(q->ngroup + 1));
    g->rep = (altsql_value *)as_alloc(q->A, sizeof(altsql_value) * (size_t)(q->ncols + 1));
    g->acc = (as_acc *)as_alloc(q->A, sizeof(as_acc) * (size_t)(q->naggs + 1));
    if (!g->keys || !g->rep || !g->acc) return NULL;
    memset(g->acc, 0, sizeof(as_acc) * (size_t)(q->naggs + 1));
    for (i = 0; i < q->ngroup; i++) if (as_vcopy(q->A, &g->keys[i], &keys[i])) return NULL;
    for (i = 0; i < q->ncols; i++) {
        if (row) { if (as_vcopy(q->A, &g->rep[i], &row[i])) return NULL; }
        else as_setnull(&g->rep[i]);
    }
    g->hasrep = row != NULL;
    if (q->last) q->last->order = g; else q->first = g;
    q->last = g;
    return g;
}

static int as_q_group(as_q *q, const altsql_value *row) {
    altsql_value keys[AS_MAXGROUP];
    as_ctx c;
    as_group *g;
    uint32_t h = 0;
    int i, rc;
    c.row = row; c.acc = NULL; c.tmp = &q->tmp; c.perm = q->A;
    for (i = 0; i < q->ngroup; i++) {
        if ((rc = as_eval(&c, q->group[i], &keys[i])) != 0) return rc;
        h = h * 31u + as_vhash(&keys[i]);
    }
    if (q->ngroup == 0) g = q->first;
    else {
        for (g = q->tab[h & (q->nbuckets - 1)]; g; g = g->next) {
            if (g->hash != h) continue;
            for (i = 0; i < q->ngroup; i++) {
                if (as_cmp(&g->keys[i], &keys[i]) != 0 || g->keys[i].type != keys[i].type) {
                    if (!(as_isnum(&g->keys[i]) && as_isnum(&keys[i]) && as_cmp(&g->keys[i], &keys[i]) == 0)) break;
                }
            }
            if (i == q->ngroup) break;
        }
        if (!g) {
            g = as_q_newgroup(q, h, keys, row);
            if (!g) return as_err(q->db, ALTSQL_NOMEM, "too many groups for working memory");
            g->next = q->tab[h & (q->nbuckets - 1)];
            q->tab[h & (q->nbuckets - 1)] = g;
        }
    }
    if (!g->hasrep && row) {         /* bare columns next to aggregates: first row */
        for (i = 0; i < q->ncols; i++)
            if (as_vcopy(q->A, &g->rep[i], &row[i])) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
        g->hasrep = 1;
    }
    for (i = 0; i < q->naggs; i++) {
        const as_expr *ae = q->aggs[i];
        altsql_value v;
        if (ae->a && ae->a->k == E_STAR) { as_acc_add(q->A, &g->acc[i], ae->fn, NULL, 1); continue; }
        if ((rc = as_eval(&c, ae->a, &v)) != 0) return rc;
        if (as_acc_add(q->A, &g->acc[i], ae->fn, &v, 0)) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
    }
    return 0;
}

static int as_q_row(as_q *q, const altsql_value *row) {
    as_ctx c;
    altsql_value w;
    int rc;
    q->tmp.used = 0;
    c.row = row; c.acc = NULL; c.tmp = &q->tmp; c.perm = q->A;
    if (q->where) {
        if ((rc = as_eval(&c, q->where, &w)) != 0) return rc;
        if (!as_truth(&w)) return 0;
    }
    if (q->grouped) return as_q_group(q, row);
    return as_q_out(q, &c);
}

static int as_q_scan(as_q *q) {
    altsql *db = q->db;
    altsql_value row[AS_MAXCOLS];
    as_iter it;
    as_rec r;
    uint32_t k;
    int rc = 0;
    for (k = 0; k < db->used && !q->stop; k++) {
        if (q->kind == 1 && q->tmin != AS_TIME_NONE && db->sec[as_run_sector(db, k)].max_time < q->tmin) continue;
        as_iter_start(&it, k, k);
        while (!q->stop && (rc = as_next(db, &it, &r)) == 1) {
            if (q->kind == 1) {
                if (r.type != AS_R_ROW || r.len < 2 || as_get16(r.p) != q->S->id) continue;
                if (as_row_decode(q->S, r.p, r.len, row)) continue;
                if (row[0].u.i < q->tmin) continue;
            } else {
                uint32_t klen;
                if (r.type != AS_R_PUT || r.len < 1) continue;
                klen = r.p[0];
                if (!klen || 1u + klen > r.len || r.p[1] == 0x01) continue;
                if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
                row[0].type = ALTSQL_TEXT; row[0].u.s = (const char *)r.p + 1; row[0].len = (int)klen;
                row[1].type = ALTSQL_TEXT; row[1].u.s = (const char *)r.p + 1 + klen; row[1].len = (int)(r.len - 1 - klen);
            }
            if ((rc = as_q_row(q, row)) != 0) return rc;
        }
        if (rc < 0) return rc;
    }
    return 0;
}

/* Bottom-up merge sort of collected rows by the ORDER BY keys. */

static int as_q_sort(as_q *q) {
    size_t n = q->nrows, w, i;
    altsql_value **src = q->rows, **dst = (altsql_value **)as_alloc(q->A, (n + 1) * sizeof *dst), **t;
    if (!dst) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            size_t a = i, am = i + w < n ? i + w : n, b = am, bm = i + 2 * w < n ? i + 2 * w : n, o = i;
            while (a < am && b < bm) dst[o++] = as_rowcmp(q, src[b], src[a]) < 0 ? src[b++] : src[a++];
            while (a < am) dst[o++] = src[a++];
            while (b < bm) dst[o++] = src[b++];
        }
        t = src; src = dst; dst = t;
    }
    q->rows = src;
    return 0;
}

static int as_is_const(const as_expr *e) {
    if (!e) return 1;
    if (e->k == E_NAME || e->k == E_STAR || (e->k == E_FN && e->fn <= F_MAX)) return 0;
    return as_is_const(e->a) && as_is_const(e->b) && as_is_const(e->c);
}

static int as_select(as_parser *P, altsql_row_cb cb, void *cbctx) {
    altsql *db = P->db;
    as_q *q = (as_q *)as_alloc(P->A, sizeof *q);
    const char *tname = NULL;
    size_t tlen = 0;
    char *schema = NULL;
    int i, star = 0, rc;
    if (!q) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    memset(q, 0, sizeof *q);
    q->db = db;
    q->A = P->A;
    q->limit = -1;
    q->tmin = AS_TIME_NONE;
    q->cb = cb;
    q->cbctx = cbctx;
    q->tmp.base = db->work;             /* per-row scratch: start of working memory */
    q->tmp.cap = AS_TMP_SIZE;
    q->tmp.used = 0;

    /* select list */
    if (as_accept_op(P, '*')) star = 1;
    else {
        do {
            as_expr *e;
            if (q->nitems >= AS_MAXITEMS) return as_err(db, ALTSQL_NOMEM, "too many result columns");
            e = as_parse_expr(P);
            if (!e) return P->rc;
            if (as_accept_kw(P, "AS") || (P->t.k == K_ID && !as_reserved(P))) {
                if (P->t.k != K_ID) return as_perr(P, "expected a name after AS near ");
                q->alias[q->nitems] = P->t.s;
                q->alen[q->nitems] = P->t.n;
                as_lex(P);
            }
            q->items[q->nitems++] = e;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "FROM")) {
        if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
        tname = P->t.s;
        tlen = P->t.n;
        as_lex(P);
    }
    if (as_accept_kw(P, "WHERE")) { q->where = as_parse_expr(P); if (P->rc) return P->rc; }
    if (as_accept_kw(P, "GROUP")) {
        if (!as_expect_kw(P, "BY")) return P->rc;
        do {
            if (q->ngroup >= AS_MAXGROUP) return as_err(db, ALTSQL_NOMEM, "too many GROUP BY terms");
            q->group[q->ngroup++] = as_parse_expr(P);
            if (P->rc) return P->rc;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "HAVING")) { q->having = as_parse_expr(P); if (P->rc) return P->rc; }
    if (as_accept_kw(P, "ORDER")) {
        if (!as_expect_kw(P, "BY")) return P->rc;
        do {
            if (q->norder >= AS_MAXGROUP) return as_err(db, ALTSQL_NOMEM, "too many ORDER BY terms");
            q->order[q->norder] = as_parse_expr(P);
            if (P->rc) return P->rc;
            q->odesc[q->norder] = as_accept_kw(P, "DESC");
            if (!q->odesc[q->norder]) as_accept_kw(P, "ASC");
            q->norder++;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "LIMIT")) {
        if (P->t.k != K_INT) return as_perr(P, "LIMIT needs a whole number near ");
        q->limit = P->t.i;
        as_lex(P);
        if (as_accept_kw(P, "OFFSET")) {
            if (P->t.k != K_INT) return as_perr(P, "OFFSET needs a whole number near ");
            q->offset = P->t.i;
            as_lex(P);
        }
    }
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");

    /* table and its columns */
    if (tname) {
        if (as_ieq(tname, tlen, "kv", 2)) {
            q->kind = 2;
            q->ncols = 2;
            q->cname[0] = "key"; q->cnlen[0] = 3;
            q->cname[1] = "value"; q->cnlen[1] = 5;
            if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
        } else {
            uint8_t types[AS_MAXCOLS];
            q->kind = 1;
            q->S = as_series_find(db, tname, tlen);
            if (!q->S) return as_err2(db, ALTSQL_SCHEMA, "no such table: ", tname, tlen);
            schema = (char *)as_alloc(P->A, AS_MAXSCHEMA + 1);
            if (!schema) return as_err(db, ALTSQL_NOMEM, "out of working memory");
            if ((rc = as_series_schema(db, q->S, schema, AS_MAXSCHEMA + 1)) != 0) return rc;
            q->ncols = as_schema_parse(schema, strlen(schema), types, as_colname_store, q);
            if (q->ncols < 1) return as_err(db, ALTSQL_CORRUPT, "series schema damaged");
        }
    }
    if (star) {
        if (!q->kind) return as_err(db, ALTSQL_SYNTAX, "SELECT * needs a FROM clause");
        for (i = 0; i < q->ncols; i++) {
            as_expr *e = as_node(P, E_COL, q->cname[i]);
            if (!e) return P->rc;
            e->idx = (int16_t)i;
            e->sn = q->cnlen[i];
            q->items[i] = e;
        }
        q->nitems = q->ncols;
    }

    /* resolve names; GROUP BY and ORDER BY may name a select item */
    {   /* an item may use the aliases of the items before it, not after */
        const char *saved[AS_MAXITEMS];
        for (i = 0; i < q->nitems; i++) { saved[i] = q->alias[i]; q->alias[i] = NULL; }
        for (i = 0; i < q->nitems; i++) {
            if (as_resolve(P, q, q->items[i], 1, 0) || as_checked(P, q, q->items[i])) return P->rc;
            q->alias[i] = saved[i];
        }
    }
    for (i = 0; i < q->ngroup; i++) {
        int ref = as_item_ref(q, q->group[i]);
        if (ref >= 0) {
            if (as_has_agg(q->items[ref])) return as_err(db, ALTSQL_SYNTAX, "GROUP BY cannot use an aggregate");
            q->group[i] = q->items[ref];
        } else if (as_resolve(P, q, q->group[i], 0, 0) || as_checked(P, q, q->group[i])) {
            return P->rc;
        }
    }
    for (i = 0; i < q->norder; i++) {
        int ref = as_item_ref(q, q->order[i]);
        if (ref >= 0) q->order[i] = q->items[ref];
        else if (as_resolve(P, q, q->order[i], 1, 0) || as_checked(P, q, q->order[i])) return P->rc;
    }
    if (q->where && (as_resolve(P, q, q->where, 0, 0) || as_checked(P, q, q->where))) return P->rc;
    if (q->having && (as_resolve(P, q, q->having, 1, 0) || as_checked(P, q, q->having))) return P->rc;
    q->grouped = q->ngroup > 0 || q->naggs > 0;
    if (q->having && !q->grouped) return as_err(db, ALTSQL_SYNTAX, "HAVING needs GROUP BY or an aggregate");

    /* result column names */
    for (i = 0; i < q->nitems; i++) {
        const as_expr *e = q->items[i];
        q->names[i] = q->alias[i] ? as_strdup_n(P->A, q->alias[i], q->alen[i])
                                  : as_strdup_n(P->A, e->s0, e->sn);
        if (!q->names[i]) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    }
    q->outv = (altsql_value *)as_alloc(P->A, sizeof(altsql_value) * (size_t)(q->nitems + q->norder + 1));
    if (!q->outv) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    if (q->grouped) {
        q->nbuckets = 4096;
        q->tab = (as_group **)as_alloc(P->A, sizeof(as_group *) * q->nbuckets);
        if (!q->tab) return as_err(db, ALTSQL_NOMEM, "out of working memory");
        memset(q->tab, 0, sizeof(as_group *) * q->nbuckets);
        if (q->ngroup == 0 && !as_q_newgroup(q, 0, NULL, NULL)) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    }
    as_q_bounds(q, q->where);

    /* run */
    if (q->kind) rc = as_q_scan(q);
    else rc = as_q_row(q, NULL);
    if (rc) return rc < 0 ? rc : as_err(db, rc, "query failed");
    if (q->grouped) {
        as_group *g;
        for (g = q->first; g && !q->stop; g = g->order) {
            as_ctx c;
            altsql_value w;
            q->tmp.used = 0;
            c.row = g->rep; c.acc = g->acc; c.tmp = &q->tmp; c.perm = q->A;
            if (q->having) {
                if ((rc = as_eval(&c, q->having, &w)) != 0) return rc;
                if (!as_truth(&w)) continue;
            }
            if ((rc = as_q_out(q, &c)) != 0) return rc;
        }
    }
    if (q->norder) {
        size_t r;
        if ((rc = as_q_sort(q)) != 0) return rc;
        for (r = 0; r < q->nrows && !q->stop; r++) as_q_emit(q, q->rows[r]);
    }
    return ALTSQL_OK;
}

static int as_type_from_sql(const char *s, size_t n) {
    if (as_ieq(s, n, "TIME", 4) || as_ieq(s, n, "TIMESTAMP", 9)) return AS_T_TIME;
    if (as_ieq(s, n, "INT", 3) || as_ieq(s, n, "SMALLINT", 8) || as_ieq(s, n, "TINYINT", 7)) return AS_T_INT;
    if (as_ieq(s, n, "INTEGER", 7) || as_ieq(s, n, "BIGINT", 6) || as_ieq(s, n, "LONG", 4)) return AS_T_LONG;
    if (as_ieq(s, n, "FLOAT", 5)) return AS_T_FLOAT;
    if (as_ieq(s, n, "REAL", 4) || as_ieq(s, n, "DOUBLE", 6)) return AS_T_REAL;
    if (as_ieq(s, n, "TEXT", 4) || as_ieq(s, n, "VARCHAR", 7) || as_ieq(s, n, "CHAR", 4)) return AS_T_TEXT;
    return 0;
}

static int as_create(as_parser *P) {
    char name[AS_NAMELEN], schema[AS_MAXSCHEMA + 1];
    size_t sl = 0;
    int ifne = 0, col = 0, rc;
    if (!as_expect_kw(P, "TABLE")) return P->rc;
    if (as_accept_kw(P, "IF")) {
        if (!as_expect_kw(P, "NOT") || !as_expect_kw(P, "EXISTS")) return P->rc;
        ifne = 1;
    }
    if (P->t.k != K_ID || P->t.n >= AS_NAMELEN) return as_perr(P, "expected a table name of up to 23 characters near ");
    memcpy(name, P->t.s, P->t.n);
    name[P->t.n] = 0;
    as_lex(P);
    if (!as_expect_op(P, '(')) return P->rc;
    do {
        const char *cn, *tn;
        size_t cl;
        int t;
        if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
        cn = P->t.s; cl = P->t.n;
        as_lex(P);
        if (P->t.k != K_ID || !(t = as_type_from_sql(P->t.s, P->t.n)))
            return as_perr(P, "expected a type (TIME, INT, INTEGER, FLOAT, REAL, TEXT) near ");
        as_lex(P);
        if (as_accept_op(P, '(')) {                       /* VARCHAR(n): size ignored */
            while (P->t.k != K_END && !as_isop(P, ')')) as_lex(P);
            if (!as_expect_op(P, ')')) return P->rc;
        }
        if (col == 0 && t != AS_T_TIME)
            return as_err(P->db, ALTSQL_SCHEMA, "the first column must be TIME: it holds each row's timestamp");
        if (col > 0 && t == AS_T_TIME) t = AS_T_LONG;    /* further timestamps are plain integers */
        col++;
        tn = as_type_name(t);
        if (sl + cl + strlen(tn) + 3 > AS_MAXSCHEMA) return as_err(P->db, ALTSQL_TOOBIG, "table definition too long");
        if (sl) schema[sl++] = ',';
        memcpy(schema + sl, cn, cl); sl += cl;
        schema[sl++] = ':';
        memcpy(schema + sl, tn, strlen(tn)); sl += strlen(tn);
    } while (as_accept_op(P, ','));
    if (!as_expect_op(P, ')')) return P->rc;
    /* the whole statement is read before anything is written */
    if (P->rc) return P->rc;
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    schema[sl] = 0;
    rc = altsql_ts_create(P->db, name, schema);
    if (rc == ALTSQL_EXISTS && ifne) { P->db->err[0] = 0; return ALTSQL_OK; }
    return rc;
}

static int as_insert(as_parser *P) {
    altsql *db = P->db;
    const char *tn;
    size_t tl;
    as_series *S = NULL;
    int map[AS_MAXCOLS], nlist = 0, ncols, i, rc, iskv;
    char schema[AS_MAXSCHEMA + 1];
    const char *cname[AS_MAXCOLS];
    size_t cnlen[AS_MAXCOLS];
    if (!as_expect_kw(P, "INTO")) return P->rc;
    if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
    tn = P->t.s; tl = P->t.n;
    as_lex(P);
    iskv = as_ieq(tn, tl, "kv", 2);
    if (iskv) ncols = 2;
    else {
        uint8_t types[AS_MAXCOLS];
        as_q tmp;
        S = as_series_find(db, tn, tl);
        if (!S) return as_err2(db, ALTSQL_SCHEMA, "no such table: ", tn, tl);
        if ((rc = as_series_schema(db, S, schema, sizeof schema)) != 0) return rc;
        memset(&tmp, 0, sizeof tmp);
        ncols = as_schema_parse(schema, strlen(schema), types, as_colname_store, &tmp);
        for (i = 0; i < ncols; i++) { cname[i] = tmp.cname[i]; cnlen[i] = tmp.cnlen[i]; }
    }
    for (i = 0; i < ncols; i++) map[i] = i;
    if (as_accept_op(P, '(')) {
        do {
            int j, found = -1;
            if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
            for (j = 0; j < ncols; j++) {
                const char *nm = iskv ? (j ? "value" : "key") : cname[j];
                size_t nl = iskv ? (j ? 5 : 3) : cnlen[j];
                if (as_ieq(P->t.s, P->t.n, nm, nl)) found = j;
            }
            if (found < 0) return as_err2(db, ALTSQL_SCHEMA, "no such column: ", P->t.s, P->t.n);
            if (nlist >= ncols) return as_err(db, ALTSQL_SCHEMA, "too many columns");
            map[nlist++] = found;
            as_lex(P);
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        if (nlist != ncols) return as_err(db, ALTSQL_SCHEMA, "INSERT must give every column");
    }
    if (!as_expect_kw(P, "VALUES")) return P->rc;
    do {
        altsql_value in[AS_MAXCOLS], v[AS_MAXCOLS];
        as_ctx c;
        int n = 0;
        size_t mark = P->A->used;
        c.row = NULL; c.acc = NULL; c.tmp = P->A; c.perm = P->A;
        if (!as_expect_op(P, '(')) return P->rc;
        do {
            as_expr *e;
            if (n >= ncols) return as_err(db, ALTSQL_SCHEMA, "too many values");
            e = as_parse_expr(P);
            if (!e) return P->rc;
            if (!as_is_const(e)) return as_err2(db, ALTSQL_SCHEMA, "values must be constants: ", e->s0, e->sn);
            if ((rc = as_eval(&c, e, &in[n])) != 0) return rc;
            n++;
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        /* rows are written one by one as they are read: a row is written
         * only once what follows it is known to be valid */
        if (P->rc) return P->rc;
        if (P->t.k != K_END && !as_isop(P, ';') && !as_isop(P, ',')) return as_perr(P, "syntax error near ");
        if (n != ncols) return as_err(db, ALTSQL_SCHEMA, "wrong number of values");
        for (i = 0; i < ncols; i++) v[map[i]] = in[i];
        if (iskv) {
            char kb[AS_MAXKEY + 1], nb[40];
            const void *val;
            size_t vl;
            if (v[0].type != ALTSQL_TEXT || v[0].len < 1 || v[0].len > (int)AS_MAXKEY) return as_err(db, ALTSQL_SCHEMA, "kv key must be text of 1 to 200 bytes");
            memcpy(kb, v[0].u.s, (size_t)v[0].len);
            kb[v[0].len] = 0;
            if (v[1].type == ALTSQL_TEXT) { val = v[1].u.s; vl = (size_t)v[1].len; }
            else if (v[1].type != ALTSQL_NULL) { vl = as_fmt_value(&v[1], nb, sizeof nb); val = nb; }
            else { val = ""; vl = 0; }
            rc = altsql_put(db, kb, val, vl);
        } else {
            char nm[AS_NAMELEN];
            memcpy(nm, S->name, sizeof nm);
            rc = altsql_ts_append(db, nm, v, ncols);
        }
        P->A->used = mark;
        if (rc) return rc;
    } while (as_accept_op(P, ','));
    return ALTSQL_OK;
}

static int as_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx);

/* Every error comes with a message, whatever path produced it. */
int altsql_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx) {
    int rc;
    if (!db || !sql) return ALTSQL_MISUSE;
    rc = as_exec(db, sql, cb, ctx);
    if (rc < 0 && !db->err[0]) as_err(db, rc, rc == ALTSQL_SYNTAX ? "syntax error" : "statement failed");
    return rc;
}

static int as_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx) {
    as_parser P;
    as_arena A;
    if (as_ready(db)) return ALTSQL_MISUSE;
    if (db->work_size < AS_TMP_SIZE * 2) return as_err(db, ALTSQL_NOMEM, "SQL needs more working memory (mem_size)");
    memset(&P, 0, sizeof P);
    P.db = db;
    P.p = sql;
    P.A = &A;
    db->err[0] = 0;
    as_lex(&P);
    while (!P.rc) {
        int rc;
        while (as_accept_op(&P, ';')) {}
        if (P.rc || P.t.k == K_END) return P.rc;
        A.base = db->work + AS_TMP_SIZE;
        A.cap = db->work_size - AS_TMP_SIZE;
        A.used = 0;
        if (as_accept_kw(&P, "SELECT")) {
            rc = as_select(&P, cb, ctx);
        } else if (as_accept_kw(&P, "CREATE")) {
            rc = as_create(&P);
        } else if (as_accept_kw(&P, "INSERT")) {
            rc = as_insert(&P);
        } else {
            return as_perr(&P, "expected SELECT, CREATE or INSERT near ");
        }
        if (rc) {
            if (rc < 0 && !db->err[0]) as_err(db, rc, rc == ALTSQL_NOMEM ? "out of working memory" : "statement failed");
            return rc;
        }
        if (P.rc) return P.rc;
        if (P.t.k != K_END && !as_isop(&P, ';')) return as_perr(&P, "syntax error near ");
    }
    return P.rc;
}

#endif /* ALTSQL_ENABLE_SQL */

/* ---- 50_sync.c --------------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Sync: device to gateway, record for record.
 *
 * The device hands out its records exactly as they sit in flash. The
 * gateway replica checks each one (marker, length, checksum, layout) and
 * appends it to its own log unchanged. Both ends hold the same bytes, so
 * there is no translation step in between.
 *
 * Sequence numbers make it safe to resend: the device resumes after the
 * newest record the gateway confirmed, and the replica skips anything it
 * already has.
 * ====================================================================== */
#if ALTSQL_ENABLE_SYNC

/* Checks the layout of a payload that arrives from outside. */
static int as_payload_ok(uint8_t type, const uint8_t *p, uint32_t len) {
    if (type == AS_R_PUT || type == AS_R_DEL) {
        uint32_t klen;
        if (len < 1) return 0;
        klen = p[0];
        if (!(klen >= 1 && klen <= AS_MAXKEY && 1u + klen <= len)) return 0;
#if ALTSQL_ENABLE_TS
        if (klen >= 2 && p[1] == 0x01 && p[2] == 'S') {         /* a series definition */
            uint8_t types[AS_MAXCOLS];
            if (type == AS_R_DEL) return 0;                     /* series are never deleted */
            if (!as_series_name_ok(p + 3, klen - 2) || len < 1u + klen + 2u) return 0;
            if (as_get16(p + 1 + klen) == 0) return 0;
            return as_schema_parse((const char *)p + 1 + klen + 2, len - 1 - klen - 2, types, NULL, NULL) >= 1;
        }
#endif
        return 1;
    }
    return type == AS_R_ROW && len >= 10;           /* series id + time */
}

/* What a record is, for the sync filter; *sid is the series id, or 0. */
static int as_rec_kind(const as_rec *r, int *sid) {
    uint32_t klen;
    *sid = 0;
    if (r->type == AS_R_ROW) {
        if (r->len >= 2) *sid = (int)as_get16(r->p);
        return ALTSQL_REC_ROW;
    }
    klen = r->len >= 1 ? r->p[0] : 0;
    if (r->type == AS_R_PUT && klen >= 3 && r->p[1] == 0x01 && r->p[2] == 'S') {   /* series definition */
        if (r->len >= 1u + klen + 2u) *sid = (int)as_get16(r->p + 1 + klen);
        return ALTSQL_REC_SCHEMA;
    }
    return ALTSQL_REC_KV;
}

int altsql_sync_read(altsql *db, uint32_t after_seq, void *buf, size_t cap, size_t *len,
                     uint32_t *last_seq, altsql_sync_filter filter, void *fctx) {
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;
    uint32_t k, last = after_seq;
    as_iter it;
    as_rec r;
    int rc = 0;
    if (!db || !buf || !len || !last_seq) return ALTSQL_MISUSE;
    *len = 0;
    *last_seq = after_seq;
    if ((rc = as_ready(db)) != 0) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "sync_read runs on the device, not on a replica");
    /* The gateway has everything up to after_seq: deletes older than that
     * no longer need to be carried forward. */
    if (after_seq < db->next_rseq && after_seq > db->synced) db->synced = after_seq;

    for (k = 0; k < db->used; k++) {
        /* Sequence numbers grow along the log, so a whole sector can be
         * skipped when the next one starts at or before after_seq + 1. */
        if (k + 1 < db->used) {
            uint32_t nf = db->sec[as_run_sector(db, k + 1)].first_rseq;
            if (nf && nf <= after_seq + 1u) continue;
        }
        as_iter_start(&it, k, k);
        while ((rc = as_next(db, &it, &r)) == 1) {
            uint32_t need;
            if (r.seq <= after_seq) continue;
            if (filter) {
                int sid, kind = as_rec_kind(&r, &sid);
                if (!filter(fctx, kind, sid)) { last = r.seq; continue; }
            }
            need = AS_RH + r.len;
            if (n + need > cap) {
                if (n == 0 && last == after_seq)
                    return as_err(db, ALTSQL_TOOBIG, "sync buffer is smaller than one record");
                *len = n;
                *last_seq = last;
                return ALTSQL_OK;                   /* more records are waiting */
            }
            memcpy(out + n, r.p - AS_RH, need);     /* header and payload, as stored */
            n += need;
            last = r.seq;
        }
        if (rc < 0) return rc;
    }
    *len = n;
    *last_seq = last;
    return ALTSQL_DONE;
}

int altsql_sync_apply(altsql *db, const void *buf, size_t len, uint32_t *last_seq) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int rc;
    if (!db || (!buf && len)) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->replica) return as_err(db, ALTSQL_MISUSE, "sync_apply needs a database opened with replica = 1");
    while (off < len) {
        const uint8_t *h = p + off;
        uint32_t plen, seq, addr;
        uint8_t type;
        as_rec r;
        if (len - off < AS_RH || h[0] != AS_MARK) return as_err(db, ALTSQL_CORRUPT, "sync data is damaged");
        type = h[1];
        plen = as_get16(h + 2);
        seq = as_get32(h + 4);
        if (len - off - AS_RH < plen) return as_err(db, ALTSQL_CORRUPT, "sync data is cut short");
        if (as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, plen) != as_get32(h + 8))
            return as_err(db, ALTSQL_CORRUPT, "sync data failed its checksum");
        if (!as_payload_ok(type, h + AS_RH, plen)) return as_err(db, ALTSQL_CORRUPT, "sync data holds an unknown record");
        if (seq >= db->next_rseq) {                 /* new to this replica */
            rc = as_reserve(db, plen, AS_RES_GC);
            if (rc) return rc;
            memcpy(db->rbuf + AS_RH, h + AS_RH, plen);
            rc = as_commit(db, type, seq, plen, &addr);
            if (rc) return rc;
            as_rec_fresh(db, &r, type, seq, plen, addr);
            if (type != AS_R_ROW) db->full = 0;
            rc = as_note(db, &r);
            if (rc) return rc;
        }
        off += AS_RH + plen;
    }
    if (last_seq) *last_seq = db->next_rseq - 1;
    return ALTSQL_OK;
}

int altsql_series_id(altsql *db, const char *series) {
#if ALTSQL_ENABLE_TS
    as_series *S;
    if (!db || !series) return 0;
    S = as_series_find(db, series, strlen(series));
    return S ? (int)S->id : 0;
#else
    (void)db;
    (void)series;
    return 0;
#endif
}

#endif /* ALTSQL_ENABLE_SYNC */

/* ---- 60_text.c --------------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Readable text form
 *
 *   # altsql text export v1
 *   series readings "time:time,machine:int,temp:float"
 *   kv "cfg.interval" "60"
 *   row readings 1700000000 3 21.53
 *
 * Series first (by id), then keys in byte order, then rows oldest first.
 * Two databases with the same content export to the same text, whatever
 * their flash history, so exports can be compared and diffed. Strings are
 * in double quotes with \" \\ \n \r \t and \xHH escapes.
 * ====================================================================== */
#if ALTSQL_ENABLE_TEXT

typedef struct as_wr {
    altsql_write_cb out;
    void *ctx;
    int stopped;
    size_t n;
    char buf[256];
} as_wr;

static void as_wr_flush(as_wr *w) {
    if (w->n && !w->stopped && w->out(w->ctx, w->buf, w->n)) w->stopped = 1;
    w->n = 0;
}

static void as_wr_put(as_wr *w, const char *s, size_t n) {
    while (n) {
        size_t k = sizeof w->buf - w->n;
        if (k > n) k = n;
        memcpy(w->buf + w->n, s, k);
        w->n += k;
        s += k;
        n -= k;
        if (w->n == sizeof w->buf) as_wr_flush(w);
    }
}

static void as_wr_str(as_wr *w, const char *s) { as_wr_put(w, s, strlen(s)); }

static void as_wr_quoted(as_wr *w, const uint8_t *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    as_wr_put(w, "\"", 1);
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        char e[4];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; as_wr_put(w, e, 2); }
        else if (c == '\n') as_wr_put(w, "\\n", 2);
        else if (c == '\r') as_wr_put(w, "\\r", 2);
        else if (c == '\t') as_wr_put(w, "\\t", 2);
        else if (c < 0x20 || c == 0x7F) {
            e[0] = '\\'; e[1] = 'x'; e[2] = hex[c >> 4]; e[3] = hex[c & 15];
            as_wr_put(w, e, 4);
        } else {
            as_wr_put(w, (const char *)&c, 1);
        }
    }
    as_wr_put(w, "\"", 1);
}

/* Key of the record at addr, via a caller buffer of AS_RH + 1 + AS_MAXKEY bytes. */
static const uint8_t *as_key_view(altsql *db, uint32_t addr, uint8_t *b, uint32_t *klen) {
    const uint8_t *p = as_view(db, addr, AS_RH + 1, b);
    if (!p) return NULL;
    *klen = p[AS_RH];
    if (*klen > AS_MAXKEY) *klen = AS_MAXKEY;
    p = as_view(db, addr, AS_RH + 1 + *klen, b);
    return p ? p + AS_RH + 1 : NULL;
}

/* Byte order of the keys of two records; *err is set on a read failure. */
static int as_key_cmp(altsql *db, uint32_t a, uint32_t b, int *err) {
    uint8_t ba[AS_RH + 1 + AS_MAXKEY], bb[AS_RH + 1 + AS_MAXKEY];
    uint32_t la, lb;
    const uint8_t *ka = as_key_view(db, a, ba, &la), *kb;
    int c;
    if (!ka) { *err = 1; return 0; }
    kb = as_key_view(db, b, bb, &lb);      /* separate buffer: ka stays valid */
    if (!kb) { *err = 1; return 0; }
    c = memcmp(ka, kb, la < lb ? la : lb);
    return c ? c : (la > lb) - (la < lb);
}

/* In-place heapsort of record addresses by key: no extra memory. */
static void as_sift(altsql *db, uint32_t *a, size_t i, size_t n, int *err) {
    for (;;) {
        size_t c = 2 * i + 1;
        uint32_t t;
        if (c >= n) return;
        if (c + 1 < n && as_key_cmp(db, a[c + 1], a[c], err) > 0) c++;
        if (as_key_cmp(db, a[c], a[i], err) <= 0) return;
        t = a[i]; a[i] = a[c]; a[c] = t;
        i = c;
    }
}

static void as_sort_keys(altsql *db, uint32_t *a, size_t n, int *err) {
    size_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) as_sift(db, a, i, n, err);
    for (i = n - 1; i > 0; i--) {
        uint32_t t = a[0]; a[0] = a[i]; a[i] = t;
        as_sift(db, a, 0, i, err);
    }
}

int altsql_export(altsql *db, altsql_write_cb out, void *ctx) {
    as_wr w;
    as_iter it;
    as_rec r;
    uint32_t *keys = NULL;
    size_t nkeys = 0, maxkeys, i;
    int rc = 0, err = 0;
    if (!db || !out) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
    w.out = out;
    w.ctx = ctx;
    w.stopped = 0;
    w.n = 0;
    as_wr_str(&w, "# altsql text export v1\n");

#if ALTSQL_ENABLE_TS
    {   /* series, by id */
        uint32_t last = 0;
        for (;;) {
            as_series *S = NULL;
            char schema[AS_MAXSCHEMA + 1];
            for (i = 0; i < db->max_series; i++) {
                as_series *e = &db->series[i];
                if (e->used && e->id > last && (!S || e->id < S->id)) S = e;
            }
            if (!S) break;
            last = S->id;
            rc = as_series_schema(db, S, schema, sizeof schema);
            if (rc) return rc;
            as_wr_str(&w, "series ");
            as_wr_str(&w, S->name);
            as_wr_str(&w, " ");
            as_wr_quoted(&w, (const uint8_t *)schema, strlen(schema));
            as_wr_str(&w, "\n");
        }
    }
#endif

    /* keys, in byte order: the addresses are gathered in working memory */
    maxkeys = db->work ? db->work_size / sizeof(uint32_t) : 0;
    keys = (uint32_t *)db->work;
    as_iter_start(&it, 0, AS_ALL);
    while ((rc = as_next(db, &it, &r)) == 1) {
        uint32_t klen;
        if (r.type != AS_R_PUT || r.len < 1) continue;
        klen = r.p[0];
        if (!klen || klen > AS_MAXKEY || 1u + klen > r.len || r.p[1] == 0x01) continue;
        if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
        if (nkeys == maxkeys) return as_err(db, ALTSQL_NOMEM, "export needs 4 bytes of working memory per key");
        keys[nkeys++] = r.addr;
    }
    if (rc < 0) return rc;
    as_sort_keys(db, keys, nkeys, &err);
    if (err) return as_err(db, ALTSQL_IOERR, "flash read failed");
    for (i = 0; i < nkeys && !w.stopped; i++) {
        uint8_t hb[AS_RH];
        const uint8_t *h = as_view(db, keys[i], AS_RH, hb), *p;
        uint32_t len, klen;
        if (!h) return as_err(db, ALTSQL_IOERR, "flash read failed");
        len = as_get16(h + 2);
        p = as_view(db, keys[i], AS_RH + len, db->rbuf);
        if (!p) return as_err(db, ALTSQL_IOERR, "flash read failed");
        p += AS_RH;
        klen = p[0];
        as_wr_str(&w, "kv ");
        as_wr_quoted(&w, p + 1, klen);
        as_wr_str(&w, " ");
        as_wr_quoted(&w, p + 1 + klen, len - 1 - klen);
        as_wr_str(&w, "\n");
    }

#if ALTSQL_ENABLE_TS
    /* rows, oldest first */
    as_iter_start(&it, 0, AS_ALL);
    while (!w.stopped && (rc = as_next(db, &it, &r)) == 1) {
        altsql_value v[AS_MAXCOLS];
        as_series *S;
        uint32_t c;
        if (r.type != AS_R_ROW || r.len < 2) continue;
        S = as_series_by_id(db, as_get16(r.p));
        if (!S || as_row_decode(S, r.p, r.len, v)) continue;
        as_wr_str(&w, "row ");
        as_wr_str(&w, S->name);
        for (c = 0; c < S->ncols; c++) {
            char nb[40];
            as_wr_put(&w, " ", 1);
            if (v[c].type == ALTSQL_TEXT) {
                as_wr_quoted(&w, (const uint8_t *)v[c].u.s, (size_t)v[c].len);
            } else if (v[c].type == ALTSQL_REAL) {
                as_wr_put(&w, nb, as_fmt_real(v[c].u.r, nb, sizeof nb, S->types[c] == AS_T_FLOAT ? 'f' : 'd'));
            } else {
                as_wr_put(&w, nb, as_fmt_value(&v[c], nb, sizeof nb));
            }
        }
        as_wr_str(&w, "\n");
    }
    if (rc < 0) return rc;
#endif

    as_wr_flush(&w);
    return w.stopped ? as_err(db, ALTSQL_ERROR, "export stopped by the writer") : ALTSQL_OK;
}

/* ---- Import ------------------------------------------------------------------ */
typedef struct as_rd {
    const char *p, *end;
    char *buf;                 /* unescaped strings go here, reset per line */
    size_t cap, used;
} as_rd;

static void as_rd_ws(as_rd *r) { while (r->p < r->end && (*r->p == ' ' || *r->p == '\t')) r->p++; }

/* A bare word (name or number) up to the next space. */
static int as_rd_word(as_rd *r, const char **s, size_t *n) {
    as_rd_ws(r);
    *s = r->p;
    while (r->p < r->end && *r->p != ' ' && *r->p != '\t') r->p++;
    *n = (size_t)(r->p - *s);
    return *n > 0;
}

static int as_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A quoted string, unescaped into the line buffer. 0 = ok, 1 = bad syntax, 2 = no room. */
static int as_rd_quoted(as_rd *r, const char **s, size_t *n) {
    char *d = r->buf + r->used;
    size_t k = 0, room = r->cap - r->used;
    as_rd_ws(r);
    if (r->p >= r->end || *r->p != '"') return 1;
    r->p++;
    for (;;) {
        int c;
        if (r->p >= r->end) return 1;
        c = (unsigned char)*r->p++;
        if (c == '"') break;
        if (c == '\\') {
            if (r->p >= r->end) return 1;
            c = (unsigned char)*r->p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'x') {
                int hi, lo;
                if (r->end - r->p < 2) return 1;
                hi = as_hexval((unsigned char)r->p[0]);
                lo = as_hexval((unsigned char)r->p[1]);
                if (hi < 0 || lo < 0) return 1;
                c = hi * 16 + lo;
                r->p += 2;
            } else if (c != '"' && c != '\\') {
                return 1;
            }
        }
        if (k == room) return 2;
        d[k++] = (char)c;
    }
    *s = d;
    *n = k;
    r->used += k;
    return 0;
}

static int as_imp_err(altsql *db, int rc, unsigned long line, const char *msg) {
    char m[96];
    snprintf(m, sizeof m, "import line %lu: %s", line, msg);
    return as_err(db, rc, m);
}

int altsql_import(altsql *db, const char *text, size_t len) {
    const char *p = text, *end = text + len;
    unsigned long line = 0;
    int rc;
    if (!db || (!text && len)) return ALTSQL_MISUSE;
    if ((rc = as_ready(db)) != 0) return rc;
    if (db->replica) return as_err(db, ALTSQL_MISUSE, "a replica only changes through sync");
    if (!db->work || db->work_size < 512) return as_err(db, ALTSQL_NOMEM, "import needs working memory (mem_size)");
    while (p < end) {
        const char *eol = p, *w;
        size_t wn;
        as_rd r;
        int q;
        while (eol < end && *eol != '\n') eol++;
        line++;
        r.p = p;
        r.end = (eol > p && eol[-1] == '\r') ? eol - 1 : eol;
        r.buf = (char *)db->work;
        r.cap = db->work_size;
        r.used = 0;
        p = eol < end ? eol + 1 : end;
        as_rd_ws(&r);
        if (r.p >= r.end || *r.p == '#') continue;          /* blank line or comment */
        if (!as_rd_word(&r, &w, &wn)) continue;

        if (as_ieq(w, wn, "kv", 2)) {
            const char *k, *v;
            size_t kn, vn;
            char key[AS_MAXKEY + 1];
            if ((q = as_rd_quoted(&r, &k, &kn)) != 0 || (q = as_rd_quoted(&r, &v, &vn)) != 0)
                return as_imp_err(db, q == 2 ? ALTSQL_NOMEM : ALTSQL_SYNTAX, line, q == 2 ? "line too long for working memory" : "expected kv \"key\" \"value\"");
            if (kn == 0 || kn > AS_MAXKEY || memchr(k, 0, kn))
                return as_imp_err(db, ALTSQL_SYNTAX, line, "key must be 1 to 200 bytes, without zero bytes");
            memcpy(key, k, kn);
            key[kn] = 0;
            rc = altsql_put(db, key, v, vn);
            if (rc) return rc;
#if ALTSQL_ENABLE_TS
        } else if (as_ieq(w, wn, "series", 6)) {
            const char *nm, *sc;
            size_t nn, sn;
            char name[AS_NAMELEN], schema[AS_MAXSCHEMA + 1];
            if (!as_rd_word(&r, &nm, &nn) || nn >= AS_NAMELEN || (q = as_rd_quoted(&r, &sc, &sn)) != 0 || sn > AS_MAXSCHEMA)
                return as_imp_err(db, ALTSQL_SYNTAX, line, "expected series name \"schema\"");
            if (!as_series_name_ok((const uint8_t *)nm, (uint32_t)nn) || memchr(sc, 0, sn))
                return as_imp_err(db, ALTSQL_SYNTAX, line, "bad series name or schema");
            memcpy(name, nm, nn);
            name[nn] = 0;
            memcpy(schema, sc, sn);
            schema[sn] = 0;
            rc = altsql_ts_create(db, name, schema);
            if (rc == ALTSQL_EXISTS) {                         /* fine if it is the same */
                char have[AS_MAXSCHEMA + 1];
                const as_series *S = as_series_find(db, name, nn);
                if (!S) return as_imp_err(db, ALTSQL_SCHEMA, line, "series name not found");
                rc = as_series_schema(db, S, have, sizeof have);
                if (rc) return rc;
                if (strcmp(have, schema) != 0)
                    return as_imp_err(db, ALTSQL_SCHEMA, line, "series exists with a different schema");
            } else if (rc) {
                return rc;
            }
        } else if (as_ieq(w, wn, "row", 3)) {
            altsql_value v[AS_MAXCOLS];
            const char *nm;
            size_t nn;
            as_series *S;
            uint32_t c;
            if (!as_rd_word(&r, &nm, &nn) || !(S = as_series_find(db, nm, nn)))
                return as_imp_err(db, ALTSQL_SCHEMA, line, "row for an unknown series");
            for (c = 0; c < S->ncols; c++) {
                v[c].len = 0;
                if (S->types[c] == AS_T_TEXT) {
                    const char *s;
                    size_t sn;
                    if ((q = as_rd_quoted(&r, &s, &sn)) != 0)
                        return as_imp_err(db, q == 2 ? ALTSQL_NOMEM : ALTSQL_SYNTAX, line, q == 2 ? "line too long for working memory" : "expected a quoted text value");
                    v[c].type = ALTSQL_TEXT;
                    v[c].u.s = s;
                    v[c].len = sn > 255 ? 256 : (int)sn;
                } else {
                    char nb[64], *e;
                    const char *s;
                    size_t sn;
                    if (!as_rd_word(&r, &s, &sn) || sn >= sizeof nb)
                        return as_imp_err(db, ALTSQL_SYNTAX, line, "expected a number");
                    memcpy(nb, s, sn);
                    nb[sn] = 0;
                    errno = 0;
                    if (S->types[c] == AS_T_FLOAT || S->types[c] == AS_T_REAL) {
                        v[c].type = ALTSQL_REAL;
                        v[c].u.r = strtod(nb, &e);
                    } else {
                        v[c].type = ALTSQL_INTEGER;
                        v[c].u.i = strtoll(nb, &e, 10);
                        if (errno == ERANGE) e = nb;
                    }
                    if (e != nb + sn) return as_imp_err(db, ALTSQL_SYNTAX, line, "bad number");
                }
            }
            as_rd_ws(&r);
            if (r.p != r.end) return as_imp_err(db, ALTSQL_SCHEMA, line, "too many values for this series");
            rc = altsql_ts_append(db, S->name, v, (int)S->ncols);
            if (rc) return rc;
#endif
        } else {
            return as_imp_err(db, ALTSQL_SYNTAX, line, "expected series, kv or row");
        }
    }
    return ALTSQL_OK;
}

#endif /* ALTSQL_ENABLE_TEXT */

/* ---- 70_port_ram.c ----------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Port: flash emulated in RAM, with power-cut simulation (tests)
 *
 * Behaves like NOR flash: a write can only clear bits, an erase sets a
 * whole sector to 0xFF. budget counts the bytes the "chip" may still
 * program; an erase costs AS_RAM_ERASE_COST. When the budget runs out the
 * power is cut: the byte being written is left partly programmed (some of
 * its bits cleared, some not), or the sector being erased is left half
 * erased, and every later call fails until the test "reboots" by calling
 * altsql_ram_flash_init() again over the same memory. dead tells how the
 * power was lost: 1 while writing, 2 while erasing.
 * ====================================================================== */
#ifdef ALTSQL_PORT_RAM

#define AS_RAM_ERASE_COST 64

static uint32_t as_ram_rand(altsql_ram_flash *r) {
    uint32_t x = r->rng ? r->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->rng = x;
    return x;
}

static int as_ram_bad(const altsql_ram_flash *r, uint32_t addr, uint32_t len) {
    return r->dead || addr > r->size || len > r->size - addr;
}

static int as_ram_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    if (as_ram_bad(r, addr, len)) return -1;
    memcpy(buf, r->mem + addr, len);
    return 0;
}

static const uint8_t *as_ram_map(void *ctx, uint32_t addr, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    return as_ram_bad(r, addr, len) ? NULL : r->mem + addr;
}

static int as_ram_write(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;
    if (as_ram_bad(r, addr, len)) return -1;
    for (i = 0; i < len; i++) {
        if (r->budget == 0) {
            r->mem[addr + i] &= (uint8_t)(s[i] | as_ram_rand(r));   /* partly programmed */
            r->dead = 1;
            return -1;
        }
        if (r->budget > 0) r->budget--;
        r->mem[addr + i] &= s[i];
        r->bytes_written++;
    }
    return 0;
}

static int as_ram_erase(void *ctx, uint32_t sector) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    uint8_t *p;
    uint32_t i;
    if (r->dead || sector >= r->size / r->sector_size) return -1;
    p = r->mem + (size_t)sector * r->sector_size;
    if (r->budget >= 0 && r->budget < AS_RAM_ERASE_COST) {
        for (i = 0; i < r->sector_size; i++) {             /* half erased */
            uint32_t x = as_ram_rand(r);
            if ((x & 3u) == 0) p[i] = 0xFF;
            else if ((x & 3u) == 1) p[i] |= (uint8_t)(x >> 8);
        }
        r->budget = 0;
        r->dead = 2;                                        /* 2: cut while erasing */
        return -1;
    }
    if (r->budget > 0) r->budget -= AS_RAM_ERASE_COST;
    memset(p, 0xFF, r->sector_size);
    r->erases++;
    return 0;
}

/* Does not touch mem, so calling it again "reboots" over the same flash.
 * Resets budget (-1), the counters and the random seed (set rng after). */
void altsql_ram_flash_init(altsql_flash *f, altsql_ram_flash *r, uint8_t *mem,
                           uint32_t sector_size, uint32_t sector_count, uint32_t write_align) {
    memset(r, 0, sizeof *r);
    r->mem = mem;
    r->size = sector_size * sector_count;
    r->sector_size = sector_size;
    r->budget = -1;
    r->rng = 0x2545F491u;
    memset(f, 0, sizeof *f);
    f->ctx = r;
    f->sector_size = sector_size;
    f->sector_count = sector_count;
    f->write_align = write_align;
    f->read = as_ram_read;
    f->write = as_ram_write;
    f->erase = as_ram_erase;
    f->map = as_ram_map;
}

#endif /* ALTSQL_PORT_RAM */

/* ---- 71_port_file.c ---------------------------------------------- */
/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Port: flash emulated in a file (POSIX), for gateways, tools and tests
 *
 * The file is memory-mapped and follows NOR rules like real flash: a
 * write can only clear bits, an erase sets a sector to 0xFF. With
 * sector_size 0, the geometry of an existing file is read from it.
 * Needs POSIX: build with -D_POSIX_C_SOURCE=200809L (or gnu99).
 * ====================================================================== */
#ifdef ALTSQL_PORT_FILE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct as_filefl {
    int      fd;
    uint8_t *mem;
    size_t   size;
    uint32_t ss;
} as_filefl;

static int as_file_bad(const as_filefl *c, uint32_t addr, uint32_t len) {
    return addr > c->size || len > c->size - addr;
}

static int as_file_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    if (as_file_bad(c, addr, len)) return -1;
    memcpy(buf, c->mem + addr, len);
    return 0;
}

static const uint8_t *as_file_map(void *ctx, uint32_t addr, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    return as_file_bad(c, addr, len) ? NULL : c->mem + addr;
}

static int as_file_write(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;
    if (as_file_bad(c, addr, len)) return -1;
    for (i = 0; i < len; i++) c->mem[addr + i] &= s[i];
    return 0;
}

static int as_file_erase(void *ctx, uint32_t sector) {
    as_filefl *c = (as_filefl *)ctx;
    if ((size_t)sector * c->ss >= c->size) return -1;
    memset(c->mem + (size_t)sector * c->ss, 0xFF, c->ss);
    return 0;
}

/* Finds sector size, count and write alignment from any valid sector header. */
static int as_file_detect(const uint8_t *m, size_t size, uint32_t *ss, uint32_t *sc, uint32_t *align) {
    uint32_t cand;
    for (cand = 256; cand && cand <= (1u << 24); cand <<= 1) {
        size_t k;
        if (size % cand) continue;
        for (k = 0; k < size / cand; k++) {
            const uint8_t *h = m + k * cand;
            if (as_hdr_valid(h) &&
                as_get32(h + 12) == cand && (size_t)as_get32(h + 16) * cand == size) {
                *ss = cand;
                *sc = as_get32(h + 16);
                *align = h[5];
                return 0;
            }
        }
    }
    return -1;
}

int altsql_file_flash_open(altsql_flash *f, const char *path, uint32_t sector_size,
                           uint32_t sector_count, uint32_t write_align, int create) {
    struct stat st;
    as_filefl *c;
    size_t size;
    uint8_t *m;
    int fd, fresh = 0;
    if (!f || !path) return -1;
    memset(f, 0, sizeof *f);
    fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0644);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (st.st_size == 0) {
        if (!create || !sector_size || !sector_count) { close(fd); return -1; }
        size = (size_t)sector_size * sector_count;
        if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
        fresh = 1;
    } else {
        size = (size_t)st.st_size;
    }
    m = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    if (fresh) {
        memset(m, 0xFF, size);                        /* a new chip is erased */
    } else {
        uint32_t ss = 0, sc = 0, al = 0;
        if (as_file_detect(m, size, &ss, &sc, &al) == 0) {
            if ((sector_size && sector_size != ss) || (sector_count && sector_count != sc)) {
                munmap(m, size);
                close(fd);
                return -1;                            /* asked for another geometry */
            }
            sector_size = ss;
            sector_count = sc;
            if (!write_align) write_align = al;
        } else {
            if (!sector_size) sector_size = 4096;
            if (!sector_count) sector_count = (uint32_t)(size / sector_size);
            if ((size_t)sector_size * sector_count != size) { munmap(m, size); close(fd); return -1; }
        }
    }
    c = (as_filefl *)malloc(sizeof *c);
    if (!c) { munmap(m, size); close(fd); return -1; }
    c->fd = fd;
    c->mem = m;
    c->size = size;
    c->ss = sector_size;
    f->ctx = c;
    f->sector_size = sector_size;
    f->sector_count = sector_count;
    f->write_align = write_align ? write_align : 4;
    f->read = as_file_read;
    f->write = as_file_write;
    f->erase = as_file_erase;
    f->map = as_file_map;
    return 0;
}

void altsql_file_flash_close(altsql_flash *f) {
    as_filefl *c;
    if (!f || !f->ctx) return;
    c = (as_filefl *)f->ctx;
    msync(c->mem, c->size, MS_SYNC);
    munmap(c->mem, c->size);
    close(c->fd);
    free(c);
    f->ctx = NULL;
}

#endif /* ALTSQL_PORT_FILE */

#endif /* ALTSQL_IMPLEMENTATION */
