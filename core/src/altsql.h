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

#define ALTSQL_VERSION "0.1.0-alpha"

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
