/*
 * AltSql DB: the gateway database for a whole fleet.
 *
 * Devices keep running AltSql Core. A gateway that serves many of them
 * keeps their records in AltSql DB: one file for every device, in a
 * copy-on-write B-tree with two commit headers. Reads and writes by key
 * take the direct path, with no SQL step at all.
 *
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 *
 * ---------------------------------------------------------------------
 * Built on AltSql Core. In exactly one C file, after Core's implementation:
 *
 *     #define ALTSQL_IMPLEMENTATION
 *     #include "altsql.h"
 *     #define ALTSQL_DB_IMPLEMENTATION
 *     #include "altsql_db.h"
 *
 * Every other file just includes "altsql_db.h".
 *
 * Ports (define before including):
 *     ALTSQL_DB_PORT_FILE   a POSIX file that takes an exclusive lock
 *     ALTSQL_DB_PORT_RAM    a file in RAM that can lose power at any write
 *                           and fail any call, for tests and demos
 *
 * Memory: AltSql DB never calls malloc. The caller hands it one block at
 * open, and the page cache and everything else is carved out of it.
 *
 * The file: pages of one size (512 bytes to 64 KB, 4 KB by default).
 * Pages 0 and 1 hold two 64-byte commit headers; the header of transaction
 * N goes to page N mod 2. A commit writes every changed page to a new
 * place, syncs, writes its header, and syncs again. A page freed by
 * transaction N is reused no earlier than N+2, so both headers on disk
 * always describe complete trees.
 * ---------------------------------------------------------------------
 */
#ifndef ALTSQL_DB_H
#define ALTSQL_DB_H

#include "altsql.h"

#define ALTSQL_DB_VERSION "0.2.0-alpha"

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes of AltSql DB's own, beside Core's. */
enum {
    ALTSQL_DB_BUSY   = -21,   /* the file is locked by another handle                */
    ALTSQL_DB_FAILED = -22,   /* the transaction failed: it can only be rolled back   */
    ALTSQL_DB_SHORT  = -23,   /* buffer too small: the length needed is in *vn       */
    ALTSQL_DB_GAP    = -24    /* sync: the batch starts past the gateway's position;
                                 nothing applied, send again from *last_seq          */
};

#define ALTSQL_DB_MAXKEY   1024   /* bytes in a key, its bucket number included */
#define ALTSQL_DB_MAXDEPTH 20     /* levels of the tree                         */

/* ---- File port -------------------------------------------------------------
 * Five calls; each returns 0 on success. read fails rather than return fewer
 * bytes. write may extend the file. sync returns once everything written so
 * far would survive a power cut. */
typedef struct altsql_db_file {
    void *ctx;
    int (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    int (*write)(void *ctx, uint64_t off, const void *buf, size_t n);
    int (*sync)(void *ctx);
    int (*size)(void *ctx, uint64_t *size);
    int (*truncate)(void *ctx, uint64_t size);
} altsql_db_file;

typedef struct altsql_db_config {
    void    *mem;          /* working memory, owned by the caller                  */
    size_t   mem_size;     /* the page cache takes what the rest leaves; 1 MB or more suits a gateway */
    uint32_t page_size;    /* new files: a power of two, 512 to 65536; 0 = 4096    */
    uint8_t  create;       /* create the database when the file is empty          */
    size_t   sql_mem;      /* memory for SQL statements; 0 = an eighth of mem_size, 16 KB to 8 MB */
} altsql_db_config;

typedef struct altsql_db altsql_db;

/* A cursor lives on the caller's stack. It stays valid across writes in
 * the same handle: after a write it finds its place again by key. */
typedef struct altsql_db_cursor {
    altsql_db *db;
    uint64_t   gen;
    uint32_t   space;
    int        state;
    uint16_t   depth, klen, plen, pad_;
    uint32_t   fi;
    uint8_t    pre[64];
    uint32_t   pg[ALTSQL_DB_MAXDEPTH];
    uint16_t   ix[ALTSQL_DB_MAXDEPTH];
    uint8_t    key[ALTSQL_DB_MAXKEY];
} altsql_db_cursor;

/* ---- Open and close ----------------------------------------------------------
 * On failure *db may still be set, so that altsql_db_errmsg(*db) can say why;
 * the handle cannot be used otherwise. */
int         altsql_db_open(altsql_db **db, const altsql_db_file *file, const altsql_db_config *cfg);
void        altsql_db_close(altsql_db *db);
const char *altsql_db_errmsg(const altsql_db *db);

/* ---- Transactions: one at a time per handle ------------------------------------
 * A call made outside a transaction commits by itself. */
int altsql_db_begin(altsql_db *db, int writable);
int altsql_db_commit(altsql_db *db);
int altsql_db_rollback(altsql_db *db);

/* ---- Buckets: named key spaces of any bytes ------------------------------------
 * Names have 1 to 63 bytes. *b is the bucket's number for the calls below. */
int altsql_db_bucket(altsql_db *db, const char *name, int create, uint32_t *b);

/* ---- The direct path: no parsing, no planning ------------------------------------
 * get: ALTSQL_NOTFOUND when the key is absent; ALTSQL_DB_SHORT when cap is
 * smaller than the value, with the length needed in *vn. */
int altsql_db_get(altsql_db *db, uint32_t b, const void *k, size_t kn,
                  void *buf, size_t cap, size_t *vn);
int altsql_db_put(altsql_db *db, uint32_t b, const void *k, size_t kn,
                  const void *v, size_t vn);
int altsql_db_del(altsql_db *db, uint32_t b, const void *k, size_t kn);

/* ---- Ordered scans ---------------------------------------------------------------
 * seek: the first key >= k (k NULL and kn 0: the first key of the bucket).
 * last: the bucket's last key. next and prev return ALTSQL_NOTFOUND at the
 * ends. key points into the cursor and stays valid until it moves. */
int altsql_db_seek(altsql_db_cursor *c, altsql_db *db, uint32_t b, const void *k, size_t kn);
int altsql_db_last(altsql_db_cursor *c, altsql_db *db, uint32_t b);
int altsql_db_next(altsql_db_cursor *c);
int altsql_db_prev(altsql_db_cursor *c);
int altsql_db_key(altsql_db_cursor *c, const void **k, size_t *kn);
int altsql_db_value(altsql_db_cursor *c, void *buf, size_t cap, size_t *vn);

/* ---- Tables: rows by primary key, no SQL --------------------------------------------
 * columns: "name:type,..." with Core's types (time, int, long, float, real, and
 * text up to 255 bytes), up to 24 columns, in any order. key: the names of the
 * primary key's columns, comma-separated. Rows hold no NULLs. row_put inserts
 * or replaces; row_insert inserts and refuses a key that is already there
 * (ALTSQL_EXISTS), as SQL's INSERT does. Every row write, from these calls, from
 * SQL and from sync, goes through one internal function, so both interfaces
 * keep the same rules. table_drop takes a table and its rows away, as DROP
 * TABLE does; not for synced tables. Text values that row_get and row_read
 * return point into the engine's memory and stay valid until the next call on
 * the handle.
 * Synced tables are filled by altsql_db_sync_apply: columns device and seq,
 * then the series' own; their key is (device, time, seq); row_put refuses
 * them, row_del takes rows away (retention). The synced table kv holds every
 * device's key-value pairs: columns device, key, value; key (device, key). */
#define ALTSQL_DB_MAXCOLS 24
enum { ALTSQL_DB_TABLE = 1, ALTSQL_DB_SYNCED = 2 };
typedef struct altsql_db_tableinfo {
    int kind, ncols, nkey;
    int key[ALTSQL_DB_MAXCOLS];             /* column index of each key column           */
    int types[ALTSQL_DB_MAXCOLS];           /* 1 time 2 int 3 long 4 float 5 real 6 text */
    const char *names[ALTSQL_DB_MAXCOLS];   /* valid until the next call on the handle   */
} altsql_db_tableinfo;
int altsql_db_table_create(altsql_db *db, const char *name, const char *columns, const char *key);
int altsql_db_table_info(altsql_db *db, const char *table, altsql_db_tableinfo *out);
int altsql_db_table_drop(altsql_db *db, const char *table);
/* Calls cb for each table, with ALTSQL_DB_TABLE or ALTSQL_DB_SYNCED; a cb that returns
 * non-zero stops the walk and its value comes back. */
int altsql_db_tables(altsql_db *db, int (*cb)(void *ctx, const char *name, int kind), void *ctx);
int altsql_db_row_put(altsql_db *db, const char *table, const altsql_value *cols, int ncols);
int altsql_db_row_insert(altsql_db *db, const char *table, const altsql_value *cols, int ncols);
int altsql_db_row_get(altsql_db *db, const char *table, const altsql_value *key, int nkey,
                      altsql_value *cols, int ncols);
int altsql_db_row_del(altsql_db *db, const char *table, const altsql_value *key, int nkey);
/* The rows whose first key columns equal prefix (nprefix 0: every row), in key
 * order; move with altsql_db_next and altsql_db_prev. */
int altsql_db_row_seek(altsql_db_cursor *c, altsql_db *db, const char *table,
                       const altsql_value *prefix, int nprefix);
/* The last of those rows: a device's newest reading, say. */
int altsql_db_row_last(altsql_db_cursor *c, altsql_db *db, const char *table,
                       const altsql_value *prefix, int nprefix);
int altsql_db_row_read(altsql_db_cursor *c, altsql_value *cols, int ncols);

/* ---- Sync from AltSql Core devices ------------------------------------------------------
 * batch: the bytes altsql_sync_read gave the device, unchanged. device: the
 * number the application already knows the device by. after_seq: the position
 * the device read the batch from (the after_seq it gave altsql_sync_read).
 * When after_seq is past the gateway's position for the device, records in
 * between are missing: nothing is applied, ALTSQL_DB_GAP comes back with the
 * gateway's position in *last_seq, and the device sends again from there. So a
 * device may send several batches ahead over a link that loses, repeats or
 * reorders them, and every record is still applied once, in order. Records the
 * gateway already has are skipped; the rest go in one transaction with the device's
 * new position, which *last_seq reports once it is committed: send it back to
 * the device as its confirmed position. A damaged record ends the batch: what
 * came before it is kept and ALTSQL_CORRUPT returned. A series whose layout
 * differs from the table of that name refuses the whole batch (ALTSQL_SCHEMA,
 * the message names the series). Inside an open transaction the batch joins it,
 * and the position counts once the caller commits. */
int altsql_db_sync_apply(altsql_db *db, int64_t device, uint32_t after_seq,
                         const void *batch, size_t len, uint32_t *last_seq);
/* The last sequence number applied for a device (0 for a new one). */
int altsql_db_sync_state(altsql_db *db, int64_t device, uint32_t *last_seq);

/* ---- SQL ----------------------------------------------------------------------------------------
 * Core's gateway SQL over the tree: CREATE TABLE [IF NOT EXISTS] with an optional PRIMARY KEY,
 * DROP TABLE [IF EXISTS], INSERT [OR REPLACE] (all or nothing), UPDATE ... SET ... [WHERE],
 * DELETE FROM ... [WHERE], SELECT with WHERE, GROUP BY, HAVING, ORDER BY, LIMIT and OFFSET, and
 * EXPLAIN SELECT, which names the plan. column [NOT] IN (value, ...) takes literals and
 * parameters. A table without a primary key keeps its rows in the order of its first column, then
 * of arrival (Core reads a series in arrival order, so rows that arrive out of time order come
 * back in another order without ORDER BY). A synced table takes SELECT and DELETE (retention),
 * not INSERT, UPDATE or DROP. UPDATE and DELETE are all or nothing: an error partway rolls back
 * the transaction the statement opened, or fails the caller's. Statements may be separated by
 * ';'. cb may be NULL; it must not call back into the handle. */
int altsql_db_exec(altsql_db *db, const char *sql, altsql_row_cb cb, void *ctx);

/* Prepared statements, up to four at a time, in the handle's SQL memory. Parameters are written
 * ? and counted from 1; a bound text value holds up to 255 bytes and is copied. Columns are
 * counted from 0. step gives ALTSQL_OK with a row and ALTSQL_DONE when there are no more; a text
 * value from altsql_db_column stays valid until the next step. Rows come in batches of up to
 * 8 KB: a SELECT without GROUP BY or ORDER BY goes on after the last row's key, others run
 * again and skip the rows already given, so read long sorted results with altsql_db_exec. The
 * statement is parsed again at each run; prepare checks it once with 0 for each parameter. */
typedef struct altsql_db_stmt altsql_db_stmt;
int  altsql_db_prepare(altsql_db *db, const char *sql, altsql_db_stmt **st);
int  altsql_db_bind(altsql_db_stmt *st, int i, const altsql_value *v);   /* v NULL: NULL */
int  altsql_db_step(altsql_db_stmt *st);
int  altsql_db_column_count(altsql_db_stmt *st);
int  altsql_db_column(altsql_db_stmt *st, int i, altsql_value *out);
const char *altsql_db_column_name(altsql_db_stmt *st, int i);
int  altsql_db_reset(altsql_db_stmt *st);
void altsql_db_finalize(altsql_db_stmt *st);

/* ---- Info and checks -------------------------------------------------------------- */
typedef struct altsql_db_info {
    uint32_t page_size, pages, free_pages, depth, cache_pages;
    uint64_t txn;                 /* number of the last commit                  */
    size_t   mem_used;            /* bytes of the working memory in use         */
    uint64_t reads, writes, syncs, cache_hits, cache_misses;
    uint64_t sql_rows;            /* rows the last SELECT read from its table   */
    uint64_t sql_changed;         /* rows the last INSERT, UPDATE or DELETE changed */
} altsql_db_info;
int altsql_db_info_get(altsql_db *db, altsql_db_info *out);

/* Walks the whole tree and the free list under one commit header and checks
 * every page: order of keys, depth, overflow chains, and that each page is
 * used once, by the tree or the free list. slot: 0 or 1 for that header, -1
 * for the current one. mem: one bit per page of the file. ALTSQL_NOTFOUND
 * when the slot holds no valid header; ALTSQL_CORRUPT with a message when a
 * check fails. Not inside a write transaction. */
typedef struct altsql_db_check_report {
    uint64_t txn, entries;
    uint32_t depth, tree_pages, overflow_pages, freelist_pages, free_entries;
} altsql_db_check_report;
int altsql_db_check(altsql_db *db, int slot, void *mem, size_t mem_size, altsql_db_check_report *rep);

#ifdef ALTSQL_DB_PORT_FILE
/* A POSIX file with an exclusive lock (ALTSQL_DB_BUSY when another process
 * holds it). nosync = 1 skips syncing, for tests only. */
typedef struct altsql_db_posix { int fd; int nosync; } altsql_db_posix;
int  altsql_db_posix_open(altsql_db_file *f, altsql_db_posix *p, const char *path, int create);
void altsql_db_posix_close(altsql_db_posix *p);
#endif

#ifdef ALTSQL_DB_PORT_RAM
/* A file in RAM. mem holds what reads see; disk what a power cut keeps,
 * both cap bytes. Every write since the last sync is recorded in log.
 * cut > 0: the power goes during that write from now (counting 1, 2, ...);
 * every call after it fails until altsql_db_ram_powercut, which builds the
 * file that survives: the synced state, any mix of the writes since, and
 * any mix of the 512-byte sectors of the write that was cut.
 * fail > 0: that call from now fails, once, without effect. */
typedef struct altsql_db_ram {
    uint8_t *mem, *disk, *log;
    uint64_t cap, size, dsize, logcap, logused;
    int64_t  cut, fail;
    int      dead, overflow, fail_kind;      /* fail_kind: 1 read 2 write 3 sync 4 size 5 truncate */
    uint64_t calls, writes, syncs, fails;
} altsql_db_ram;
void altsql_db_ram_init(altsql_db_file *f, altsql_db_ram *r, uint8_t *mem, uint8_t *disk,
                        uint64_t cap, uint8_t *log, uint64_t logcap);
void altsql_db_ram_powercut(altsql_db_ram *r, uint32_t seed);
#endif

#ifdef __cplusplus
}
#endif
#endif /* ALTSQL_DB_H */

/* ======================================================================
 * Implementation. Compiled only where ALTSQL_DB_IMPLEMENTATION is defined.
 * ====================================================================== */
#if defined(ALTSQL_DB_IMPLEMENTATION) && !defined(ALTSQL_DB_IMPL_DONE)
#define ALTSQL_DB_IMPL_DONE
#ifndef ALTSQL_IMPL_DONE
#error "AltSql DB needs AltSql Core's implementation first: define ALTSQL_IMPLEMENTATION and include altsql.h before it"
#endif

#include <stdio.h>
#ifdef ALTSQL_DB_PORT_FILE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

/* ---- Format ---------------------------------------------------------------
 * Page header, 24 bytes:
 *   0 type  1 flags (0)  2 entries (LE16)  4 start of free space  6 end of
 *   free space (0 means 65536)  8 transaction that wrote the page (LE64)
 *   16 checksum (reserved, 0)  20 leaf: 0; branch: rightmost child;
 *   overflow and free-list pages: next page in the chain
 * Leaf cell:     key length (varint), value length (varint), key, then the
 *                value, or the first overflow page (LE32) when the cell
 *                would be larger than maxcell
 * Branch cell:   child (LE32), key length (varint), key. Child i holds the
 *                keys below key i; the rightmost child holds the rest.
 * Free-list page entries, 12 bytes each: page (LE32), transaction that
 * freed it (LE64); 0 for a page freed by the transaction that wrote it.
 * Commit header, 64 bytes: "ASQLTREE", version 2, page size, transaction,
 * root, page count, first page and entries of the ready list, next key
 * space, first page and entries of the newest list, 8 reserved bytes,
 * CRC-32 of bytes 0 to 59.
 * Two free lists, each a chain of free-list pages. Commit T's ready list
 * holds pages freed before T: the next transaction may use all of it. Its
 * newest list holds the pages T freed, which wait one more commit (a page
 * freed by N is reused no earlier than N+2). The commit after copies the
 * newest list into its ready list, so no transaction walks past entries it
 * cannot use.                                                              */
#define ASD_PH      24u
#define ASD_HS      64u
#define ASD_LEAF    1
#define ASD_BRANCH  2
#define ASD_OVFL    3
#define ASD_FREE    4
#define ASD_NONE    0xFFFFFFFFu
#define ASD_MINFR   24u
#define ASD_FLE     12u
#define ASD_KS_CAT  1u
#define ASD_KS_USER 64u
#define ASD_K_BUCKET 3

typedef struct asd_frame { uint32_t pg, next; uint16_t pin; uint8_t dirty, ref; } asd_frame;
#define ASD_NTAB    8
#define ASD_ROWBUF  8192
#define ASD_KS_SYNC 2u
#define ASD_K_TABLE  1
#define ASD_K_SYNCED 2
#define ASD_F_KV     1
#define ASD_F_ROWID  2
struct asd_table {
    uint32_t ks;
    uint8_t  kind, ncols, nkey, flags;
    uint8_t  types[ALTSQL_DB_MAXCOLS], key[ALTSQL_DB_MAXCOLS];
    char     name[32];
    char     cols[ALTSQL_DB_MAXCOLS][32];
};
#define ASD_NSTMT     4
#define ASD_MAXPARAM  16
#define ASD_STMTTXT   4096
#define ASD_STMTROWS  8192
#define ASD_STMTNAMES 1024
#if ALTSQL_ENABLE_SQL
struct altsql_db_stmt {
    uint64_t  rows64[ASD_STMTROWS / 8];   /* a batch of rows: size, values, text bytes    */
    altsql_db *db;
    int       used, np, ncols, state, more, stream, err;   /* state: 0 to run, 1 rows, 2 done */
    uint32_t  nrows, pos, off, rused, rkn;
    uint64_t  delivered, skip;
    altsql_value par[ASD_MAXPARAM];
    char      ptxt[ASD_MAXPARAM][256];
    const char *nm[AS_MAXITEMS];
    char      names[ASD_STMTNAMES];
    char      text[ASD_STMTTXT];          /* IN lists written out, ? kept                 */
    uint8_t   rk[ALTSQL_DB_MAXKEY + 8];   /* the key of the last row given                */
};
#endif
typedef struct asd_hdr { uint64_t txn; uint32_t root, npages, flhead, flcount, nextks, nxhead, nxcount; } asd_hdr;

struct altsql_db {
    altsql_db_file f;
    uint32_t  ps, usable, maxcell, maxkey, epp, ovd;
    asd_hdr   com;                 /* the last commit                              */
    int       tx;                  /* 0 none, 1 read, 2 write                      */
    int       failed, reload;
    uint64_t  cur;                 /* number of the next commit                    */
    uint64_t  mods, gen, fsize;
    uint32_t  root, npages, nextks;
    /* free list: entries held in memory, the rest in chains of pages */
    uint32_t *avpg, *hopg;         /* reusable now / not before two commits pass   */
    uint64_t *avtag, *hotag;
    uint32_t  nav, nho, flw, flnext, fltail;   /* the ready list still to load      */
    uint32_t  dhead, dtail, dcount;  /* reusable entries spilled in this transaction: the front of the next ready list */
    uint32_t  hhead, hcount;         /* this transaction's frees spilled: the next newest list */
    uint32_t  nxnext, nxleft;        /* the last commit's newest list, copied at commit */
    /* page cache */
    asd_frame *fr;
    uint8_t  *pages;
    uint32_t  nfr, hmask, hand, *hash, *sortb;
    uint8_t  *sa, *sb, *sc, *cell, *bcell, *kb[2];
    uint32_t  known[16], nknown, kpos;
    struct asd_table *tabs;        /* tables lately used                           */
    uint32_t  ntabs, tpos;
    uint8_t  *rowbuf;              /* a row being packed, or read for the caller   */
    struct { int64_t dev; uint32_t sid, ks; } maps[8];   /* (device, series) to table */
    uint32_t  nmaps, mpos;
    uint8_t  *sqlmem;              /* SQL statements' working memory               */
    size_t    sqlsize, sqlexec;    /* all of it; the part statements run in        */
    uint64_t  sqlrows;             /* rows the last SELECT read                    */
    uint64_t  sqlchanged;          /* rows the last INSERT, UPDATE or DELETE changed */
    struct altsql_db_stmt *stmts, *curstmt;   /* prepared statements; the one running */
    void     *cursq;               /* the SELECT running for it                    */
    uint32_t  nstmt, sqlnst;       /* statement slots; statements in the last text */
    int       sqldry;              /* checking a text: parse, write nothing        */
    uint32_t  rowctr;              /* hidden keys given in this transaction        */
#if ALTSQL_ENABLE_SQL
    struct altsql core;            /* Core's parser reports its errors here        */
#endif
    uint32_t  seqpg, seqix;        /* the last leaf insert, for splits that suit time order */
    size_t    mem_used;
    uint64_t  nread, nwrite, nsync, hits, misses;
    char      err[96];
};

typedef struct asd_path { int depth, found; uint32_t fi[ALTSQL_DB_MAXDEPTH], ix[ALTSQL_DB_MAXDEPTH]; } asd_path;
typedef struct asd_cell { const uint8_t *k, *v; uint32_t kn, vn, ov, size; } asd_cell;

#define ASD_PAGE(db, fi) ((db)->pages + (size_t)(fi) * (db)->ps)
#if defined(__GNUC__) || defined(__clang__)
#define ASD_HOT static inline __attribute__((always_inline))
#else
#define ASD_HOT static
#endif

static int asd_err(altsql_db *db, int rc, const char *msg) {
    if (db) {
        size_t n = strlen(msg);
        if (n >= sizeof db->err) n = sizeof db->err - 1;
        memcpy(db->err, msg, n);
        db->err[n] = 0;
    }
    return rc;
}
#define ASD_CORRUPT(db, m) asd_err((db), ALTSQL_CORRUPT, (m))

/* ---- Page fields -------------------------------------------------------- */
static uint32_t asd_n(const uint8_t *p)  { return as_get16(p + 2); }
static uint32_t asd_lo(const uint8_t *p) { return as_get16(p + 4); }
static uint32_t asd_hi(const uint8_t *p) { uint32_t v = as_get16(p + 6); return v ? v : 65536u; }
static uint64_t asd_txn(const uint8_t *p) { return as_get64(p + 8); }
static uint32_t asd_x(const uint8_t *p)  { return as_get32(p + 20); }
static uint32_t asd_off(const uint8_t *p, uint32_t i) { return as_get16(p + ASD_PH + 2 * i); }
static void asd_set_n(uint8_t *p, uint32_t v)  { as_put16(p + 2, v); }
static void asd_set_lo(uint8_t *p, uint32_t v) { as_put16(p + 4, v); }
static void asd_set_hi(uint8_t *p, uint32_t v) { as_put16(p + 6, v & 0xFFFFu); }
static void asd_set_x(uint8_t *p, uint32_t v)  { as_put32(p + 20, v); }

static void asd_pinit(const altsql_db *db, uint8_t *p, int type) {
    memset(p, 0, db->ps);
    p[0] = (uint8_t)type;
    asd_set_lo(p, ASD_PH);
    asd_set_hi(p, db->ps);
    as_put64(p + 8, db->cur);
}

/* ---- Varints ------------------------------------------------------------- */
static uint32_t asd_vlen(uint32_t v) { uint32_t n = 1; while (v >= 0x80) { v >>= 7; n++; } return n; }
static uint32_t asd_vput(uint8_t *p, uint32_t v) {
    uint32_t n = 0;
    while (v >= 0x80) { p[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}
/* Checked: 0 when the varint runs past end or is longer than 5 bytes. */
static uint32_t asd_vget(const uint8_t *p, const uint8_t *end, uint32_t *v) {
    uint32_t r = 0, n = 0, s = 0;
    while (p + n < end && n < 5) {
        uint8_t b = p[n++];
        r |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) { *v = r; return n; }
        s += 7;
    }
    return 0;
}
/* Unchecked, for pages already validated. */
ASD_HOT uint32_t asd_vgetu(const uint8_t *p, uint32_t *v) {
    uint32_t r = 0, n = 0, s = 0;
    if (p[0] < 0x80) { *v = p[0]; return 1; }
    for (;;) {
        uint8_t b = p[n++];
        r |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80) || n == 5) { *v = r; return n; }
        s += 7;
    }
}

/* Key space numbers, as SQLite4 writes varints: memcmp order is numeric order. */
static uint32_t asd_ksput(uint8_t *p, uint32_t v) {
    if (v <= 240) { p[0] = (uint8_t)v; return 1; }
    if (v <= 2287) { v -= 240; p[0] = (uint8_t)(v / 256 + 241); p[1] = (uint8_t)(v % 256); return 2; }
    if (v <= 67823) { v -= 2288; p[0] = 249; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; return 3; }
    if (v <= 0xFFFFFFu) { p[0] = 250; p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 4; }
    p[0] = 251; p[1] = (uint8_t)(v >> 24); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 8); p[4] = (uint8_t)v;
    return 5;
}

/* ---- Cells --------------------------------------------------------------- */
static int asd_inline(const altsql_db *db, uint32_t kn, uint32_t vn) {
    return (uint64_t)asd_vlen(kn) + asd_vlen(vn) + kn + vn <= db->maxcell;
}

static void asd_leaf_cell(const altsql_db *db, const uint8_t *p, uint32_t i, asd_cell *c) {
    const uint8_t *q = p + asd_off(p, i);
    uint32_t a = asd_vgetu(q, &c->kn);
    a += asd_vgetu(q + a, &c->vn);
    c->k = q + a;
    if (asd_inline(db, c->kn, c->vn)) { c->v = c->k + c->kn; c->ov = 0; c->size = a + c->kn + c->vn; }
    else { c->v = 0; c->ov = as_get32(c->k + c->kn); c->size = a + c->kn + 4; }
}

ASD_HOT const uint8_t *asd_lkey(const uint8_t *p, uint32_t i, uint32_t *kn) {
    const uint8_t *q = p + asd_off(p, i);
    uint32_t vn, a;
    if (q[0] < 0x80 && q[1] < 0x80) { *kn = q[0]; return q + 2; }
    a = asd_vgetu(q, kn);
    a += asd_vgetu(q + a, &vn);
    return q + a;
}

ASD_HOT const uint8_t *asd_bkey(const uint8_t *p, uint32_t i, uint32_t *kn) {
    const uint8_t *q = p + asd_off(p, i) + 4;
    return q + asd_vgetu(q, kn);
}

static uint32_t asd_csize(const altsql_db *db, const uint8_t *p, uint32_t i) {
    if (p[0] == ASD_LEAF) { asd_cell c; asd_leaf_cell(db, p, i, &c); return c.size; }
    else { uint32_t kn, a = asd_vgetu(p + asd_off(p, i) + 4, &kn); return 4 + a + kn; }
}

static uint32_t asd_child(const uint8_t *p, uint32_t i) {
    return i < asd_n(p) ? as_get32(p + asd_off(p, i)) : asd_x(p);
}
static void asd_set_child(uint8_t *p, uint32_t i, uint32_t pg) {
    if (i < asd_n(p)) as_put32(p + asd_off(p, i), pg); else asd_set_x(p, pg);
}

/* Keys compare as bytes. Eight at a time where the compiler can swap bytes. */
ASD_HOT int asd_cmp(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn) {
    uint32_t n = an < bn ? an : bn, i = 0;
#if (defined(__GNUC__) || defined(__clang__)) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y) { x = __builtin_bswap64(x); y = __builtin_bswap64(y); return x < y ? -1 : 1; }
    }
#endif
    for (; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* Leaf: index of the first key >= k. */
static uint32_t asd_lfind(const uint8_t *p, const uint8_t *k, uint32_t kn, int *found) {
    uint32_t lo = 0, hi = asd_n(p);
    *found = 0;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, cn;
        const uint8_t *ck = asd_lkey(p, mid, &cn);
        int r = asd_cmp(ck, cn, k, kn);
        if (r < 0) lo = mid + 1;
        else { hi = mid; if (r == 0) *found = 1; }
    }
    return lo;
}

/* Branch: the child for k: the first i with k < key i, else n. */
static uint32_t asd_bfind(const uint8_t *p, const uint8_t *k, uint32_t kn) {
    uint32_t lo = 0, hi = asd_n(p);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, cn;
        const uint8_t *ck = asd_bkey(p, mid, &cn);
        if (asd_cmp(k, kn, ck, cn) < 0) hi = mid; else lo = mid + 1;
    }
    return lo;
}

/* A page read from the file is checked before any cell is trusted. */
static int asd_validate(altsql_db *db, const uint8_t *p) {
    uint32_t t = p[0], n = asd_n(p), lo = asd_lo(p), hi = asd_hi(p), i, total = 0;
    const uint8_t *end = p + db->ps;
    if (asd_txn(p) > db->com.txn + 1) return ASD_CORRUPT(db, "page from a later transaction than the header");
    if (t == ASD_LEAF || t == ASD_BRANCH) {
        if (lo != ASD_PH + 2 * n || hi < lo || hi > db->ps) return ASD_CORRUPT(db, "damaged page header");
        for (i = 0; i < n; i++) {
            uint32_t off = asd_off(p, i), a, b = 0, kn, vn = 0, sz;
            if (off < hi || off >= db->ps) return ASD_CORRUPT(db, "damaged cell offset");
            if (t == ASD_LEAF) {
                a = asd_vget(p + off, end, &kn);
                b = a ? asd_vget(p + off + a, end, &vn) : 0;
                if (!b) return ASD_CORRUPT(db, "damaged cell");
                sz = a + b + kn + (asd_inline(db, kn, vn) ? vn : 4u);
            } else {
                if (off + 4 > db->ps || !(a = asd_vget(p + off + 4, end, &kn))) return ASD_CORRUPT(db, "damaged cell");
                sz = 4 + a + kn;
            }
            if (kn > db->maxkey || (uint64_t)off + sz > db->ps) return ASD_CORRUPT(db, "damaged cell");
            if (t == ASD_LEAF && !asd_inline(db, kn, vn)) {      /* a large value: its first page must be a page */
                uint32_t ov = as_get32(p + off + a + b + kn);
                if (ov < 2 || ov >= db->npages) return ASD_CORRUPT(db, "damaged cell");
            }
            total += sz;
        }
        if (total > db->ps - lo) return ASD_CORRUPT(db, "damaged page: its cells overlap");   /* else compacting it overflows */
        return ALTSQL_OK;
    }
    if (t == ASD_FREE) return n <= db->epp ? ALTSQL_OK : ASD_CORRUPT(db, "damaged free-list page");
    if (t == ASD_OVFL) return ALTSQL_OK;
    return ASD_CORRUPT(db, "unknown page type");
}

/* ---- Page cache ---------------------------------------------------------- */
static uint32_t asd_hpos(const altsql_db *db, uint32_t pg) { return (pg * 2654435761u) & db->hmask; }

static uint32_t asd_lookup(const altsql_db *db, uint32_t pg) {
    uint32_t fi = db->hash[asd_hpos(db, pg)];
    while (fi != ASD_NONE) {
        if (db->fr[fi].pg == pg) return fi;
        fi = db->fr[fi].next;
    }
    return ASD_NONE;
}
static void asd_hadd(altsql_db *db, uint32_t fi) {
    uint32_t h = asd_hpos(db, db->fr[fi].pg);
    db->fr[fi].next = db->hash[h];
    db->hash[h] = fi;
}
static void asd_hdel(altsql_db *db, uint32_t fi) {
    uint32_t *pp = &db->hash[asd_hpos(db, db->fr[fi].pg)];
    while (*pp != ASD_NONE) {
        if (*pp == fi) { *pp = db->fr[fi].next; break; }
        pp = &db->fr[*pp].next;
    }
    db->fr[fi].pg = ASD_NONE;
    db->fr[fi].dirty = 0;
}

static int asd_wpage(altsql_db *db, uint32_t pg, const uint8_t *buf) {
    uint64_t off = (uint64_t)pg * db->ps;
    db->nwrite++;
    if (db->f.write(db->f.ctx, off, buf, db->ps)) return asd_err(db, ALTSQL_IOERR, "write failed");
    if (off + db->ps > db->fsize) db->fsize = off + db->ps;
    return ALTSQL_OK;
}

/* A frame to reuse, by the clock. A changed page is written early to its new
 * place: safe, because no commit header points at it yet. */
static int asd_victim(altsql_db *db, uint32_t *out) {
    uint32_t tries = 0, lim = 2 * db->nfr + 1;
    while (tries++ < lim) {
        uint32_t fi = db->hand;
        asd_frame *f = &db->fr[fi];
        db->hand = fi + 1 == db->nfr ? 0 : fi + 1;
        if (f->pin) continue;
        if (f->pg == ASD_NONE) { *out = fi; return ALTSQL_OK; }
        if (f->ref) { f->ref = 0; continue; }
        if (f->dirty) {
            int rc = asd_wpage(db, f->pg, ASD_PAGE(db, fi));
            if (rc) return rc;
            f->dirty = 0;
        }
        asd_hdel(db, fi);
        *out = fi;
        return ALTSQL_OK;
    }
    return asd_err(db, ALTSQL_NOMEM, "page cache too small: every page is in use");
}

/* The page, pinned: the caller unpins it. */
static int asd_get(altsql_db *db, uint32_t pg, uint32_t *out) {
    uint32_t fi;
    int rc;
    if (pg < 2 || pg >= db->npages) return ASD_CORRUPT(db, "page number out of range");
    fi = asd_lookup(db, pg);
    if (fi != ASD_NONE) {
        db->hits++;
        db->fr[fi].ref = 1;
        db->fr[fi].pin++;
        *out = fi;
        return ALTSQL_OK;
    }
    db->misses++;
    if ((rc = asd_victim(db, &fi)) != 0) return rc;
    db->nread++;
    if (db->f.read(db->f.ctx, (uint64_t)pg * db->ps, ASD_PAGE(db, fi), db->ps))
        return asd_err(db, ALTSQL_IOERR, "read failed");
    if ((rc = asd_validate(db, ASD_PAGE(db, fi))) != 0) return rc;
    db->fr[fi].pg = pg;
    db->fr[fi].dirty = 0;
    db->fr[fi].ref = 1;
    db->fr[fi].pin = 1;
    asd_hadd(db, fi);
    *out = fi;
    return ALTSQL_OK;
}
#define asd_unpin(db, fi) ((db)->fr[fi].pin--)

/* A frame for a page this transaction writes from scratch. */
static int asd_frame_for(altsql_db *db, uint32_t pg, int type, uint32_t *out) {
    uint32_t fi = asd_lookup(db, pg);
    if (fi == ASD_NONE) {
        int rc = asd_victim(db, &fi);
        if (rc) return rc;
        db->fr[fi].pg = pg;
        db->fr[fi].pin = 0;
        asd_hadd(db, fi);
    }
    asd_pinit(db, ASD_PAGE(db, fi), type);
    db->fr[fi].dirty = 1;
    db->fr[fi].ref = 1;
    db->fr[fi].pin++;
    db->mods++;
    *out = fi;
    return ALTSQL_OK;
}

static void asd_forget(altsql_db *db, uint32_t pg) {
    uint32_t fi = asd_lookup(db, pg);
    if (fi == ASD_NONE) return;
    if (db->fr[fi].pin) db->fr[fi].dirty = 0;
    else asd_hdel(db, fi);
}

/* Drops the pages of the transaction under way. */
static void asd_drop_owned(altsql_db *db) {
    uint32_t i;
    for (i = 0; i < db->nfr; i++)
        if (db->fr[i].pg != ASD_NONE && asd_txn(ASD_PAGE(db, i)) == db->cur) {
            db->fr[i].pin = 0;
            asd_hdel(db, i);
        }
}

/* ---- Free list ------------------------------------------------------------- */
static int asd_alloc(altsql_db *db, uint32_t *pg, int noload);
static int asd_free(altsql_db *db, uint32_t pg, uint64_t txn);

/* Loads the next page of the ready list: its entries are reusable now. Its
 * own page is free from then on; it comes back in *lp, *lt for the caller
 * to free. 1 when there is no room. */
static int asd_flload(altsql_db *db, uint32_t *lp, uint64_t *lt) {
    uint32_t fi, n, i, pg = db->flnext;
    uint8_t *p;
    int rc;
    if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
    p = ASD_PAGE(db, fi);
    n = asd_n(p);
    if (p[0] != ASD_FREE || n > db->fltail || n > db->epp) { asd_unpin(db, fi); return ASD_CORRUPT(db, "damaged free list"); }
    if (db->nav + n > db->flw) { asd_unpin(db, fi); return 1; }
    for (i = 0; i < n; i++) {
        uint32_t e = as_get32(p + ASD_PH + i * ASD_FLE);
        uint64_t et = as_get64(p + ASD_PH + i * ASD_FLE + 4);
        if (e < 2 || e >= db->npages || (et && et + 2 > db->cur)) { asd_unpin(db, fi); return ASD_CORRUPT(db, "damaged free list"); }
        db->avpg[db->nav + i] = e;
        db->avtag[db->nav + i] = et;
    }
    db->nav += n;
    db->flnext = asd_x(p);
    db->fltail -= n;
    *lp = pg;
    *lt = asd_txn(p);
    asd_unpin(db, fi);
    db->mods++;
    return ALTSQL_OK;
}

/* Moves up to one page of entries from memory to a new free-list page.
 * Reusable entries spilled in a transaction form a chain that goes in front
 * of the ready list at commit; its own frees form the next newest list.
 * Loading never meets either chain again in this transaction. */
static int asd_spill(altsql_db *db, int hold) {
    uint32_t *pgs = hold ? db->hopg : db->avpg, *n = hold ? &db->nho : &db->nav, pg, fi, k, i, lp = 0;
    uint64_t *tags = hold ? db->hotag : db->avtag, lt = 0;
    uint8_t *p;
    int rc;
    if (!db->nav && db->flnext) {          /* the new page from the ready list, not the end of the file */
        rc = asd_flload(db, &lp, &lt);
        if (rc < 0) return rc;
        if (rc) lp = 0;
    }
    if ((rc = asd_alloc(db, &pg, 1)) != 0) return rc;
    if ((rc = asd_frame_for(db, pg, ASD_FREE, &fi)) != 0) return rc;
    p = ASD_PAGE(db, fi);
    k = *n < db->epp ? *n : db->epp;
    for (i = 0; i < k; i++) {
        uint32_t j = *n - k + i;
        as_put32(p + ASD_PH + i * ASD_FLE, pgs[j]);
        as_put64(p + ASD_PH + i * ASD_FLE + 4, tags[j]);
    }
    *n -= k;
    asd_set_n(p, k);
    if (hold) {
        asd_set_x(p, db->hhead);
        db->hhead = pg;
        db->hcount += k;
    } else {
        asd_set_x(p, db->dhead);
        db->dhead = pg;
        if (!db->dtail) db->dtail = pg;
        db->dcount += k;
    }
    asd_unpin(db, fi);
    return lp ? asd_free(db, lp, lt) : ALTSQL_OK;   /* the loaded list page: there is room now */
}

/* A page this transaction no longer uses. Written in this transaction: no
 * commit header knows it, so it can be reused at once. Otherwise the last
 * commit may still need it: reused no earlier than two commits on. */
static int asd_free(altsql_db *db, uint32_t pg, uint64_t txn) {
    int rc;
    db->mods++;
    if (txn == db->cur) {
        uint32_t fi = asd_lookup(db, pg);
        if (fi != ASD_NONE) db->fr[fi].dirty = 0;
        if (db->nav == db->flw && (rc = asd_spill(db, 0)) != 0) return rc;
        db->avpg[db->nav] = pg;
        db->avtag[db->nav++] = 0;
    } else {
        if (db->nho == db->flw && (rc = asd_spill(db, 1)) != 0) return rc;
        db->hopg[db->nho] = pg;
        db->hotag[db->nho++] = db->cur;
    }
    return ALTSQL_OK;
}

static int asd_alloc(altsql_db *db, uint32_t *pg, int noload) {
    for (;;) {
        uint32_t lp;
        uint64_t lt;
        int rc;
        if (db->nav) { *pg = db->avpg[--db->nav]; db->mods++; return ALTSQL_OK; }
        if (noload || !db->flnext) break;
        rc = asd_flload(db, &lp, &lt);
        if (rc < 0) return rc;
        if (rc == 1) break;
        if ((rc = asd_free(db, lp, lt)) != 0) return rc;
    }
    if (db->npages >= 0x7FFFFFF0u) return asd_err(db, ALTSQL_FULL, "the file has reached its largest size");
    *pg = db->npages++;
    db->mods++;
    return ALTSQL_OK;
}

/* At commit. The last commit's newest list is reusable from the next commit
 * on: its entries are copied to pages of this transaction, in front of the
 * ready list, and its own pages are freed. Then the entries held in memory
 * go to new pages: reusable ones in front of the ready list, this
 * transaction's frees into the next newest list. */
static int asd_flflush(altsql_db *db, uint32_t *flhead, uint32_t *nxhead) {
    uint32_t fi, ofi, pg, n, i, old;
    uint64_t t;
    uint8_t *p, *o;
    int rc;
    while ((old = db->nxnext) != 0) {
        if ((rc = asd_get(db, old, &ofi)) != 0) return rc;
        o = ASD_PAGE(db, ofi);
        n = asd_n(o);
        rc = o[0] != ASD_FREE || n > db->nxleft || n > db->epp;
        for (i = 0; i < n && !rc; i++) {
            uint32_t e = as_get32(o + ASD_PH + i * ASD_FLE);
            rc = e < 2 || e >= db->npages;
        }
        if (rc) { asd_unpin(db, ofi); return ASD_CORRUPT(db, "damaged free list"); }
        if (n) {
            if ((rc = asd_alloc(db, &pg, 0)) != 0 || (rc = asd_frame_for(db, pg, ASD_FREE, &fi)) != 0) { asd_unpin(db, ofi); return rc; }
            p = ASD_PAGE(db, fi);
            o = ASD_PAGE(db, ofi);
            memcpy(p + ASD_PH, o + ASD_PH, (size_t)n * ASD_FLE);
            asd_set_n(p, n);
            asd_set_x(p, db->dhead);
            db->dhead = pg;
            if (!db->dtail) db->dtail = pg;
            db->dcount += n;
            asd_unpin(db, fi);
        }
        o = ASD_PAGE(db, ofi);
        db->nxnext = asd_x(o);
        db->nxleft -= n;
        t = asd_txn(o);
        asd_unpin(db, ofi);
        if ((rc = asd_free(db, old, t)) != 0) return rc;
    }
    if (db->nxleft) return ASD_CORRUPT(db, "damaged free list");
    while (db->nho) if ((rc = asd_spill(db, 1)) != 0) return rc;
    while (db->nav) if ((rc = asd_spill(db, 0)) != 0) return rc;
    if (db->dtail) {                       /* the reusable chain goes in front of the ready list */
        if ((rc = asd_get(db, db->dtail, &fi)) != 0) return rc;
        p = ASD_PAGE(db, fi);
        if (p[0] != ASD_FREE || asd_txn(p) != db->cur) { asd_unpin(db, fi); return ASD_CORRUPT(db, "damaged free list"); }
        asd_set_x(p, db->flnext);
        db->fr[fi].dirty = 1;
        asd_unpin(db, fi);
        db->flnext = db->dhead;
        db->fltail += db->dcount;
        db->dhead = db->dtail = db->dcount = 0;
    }
    *flhead = db->flnext;
    *nxhead = db->hhead;
    return ALTSQL_OK;
}

/* ---- Commit headers -------------------------------------------------------- */
static void asd_hdr_put(const altsql_db *db, uint8_t *h, const asd_hdr *x) {
    memset(h, 0, ASD_HS);
    memcpy(h, "ASQLTREE", 8);
    as_put32(h + 8, 2);
    as_put32(h + 12, db->ps);
    as_put64(h + 16, x->txn);
    as_put32(h + 24, x->root);
    as_put32(h + 28, x->npages);
    as_put32(h + 32, x->flhead);
    as_put32(h + 36, x->flcount);
    as_put32(h + 40, x->nextks);
    as_put32(h + 44, x->nxhead);
    as_put32(h + 48, x->nxcount);
    as_put32(h + 60, as_crc32(0, h, 60));
}

static int asd_hdr_get(const uint8_t *h, uint32_t *ps, asd_hdr *x) {
    uint32_t p = as_get32(h + 12);
    if (memcmp(h, "ASQLTREE", 8) || as_get32(h + 8) != 2 || as_get32(h + 60) != as_crc32(0, h, 60)) return 0;
    if (p < 512 || p > 65536 || (p & (p - 1))) return 0;
    x->txn = as_get64(h + 16);
    x->root = as_get32(h + 24);
    x->npages = as_get32(h + 28);
    x->flhead = as_get32(h + 32);
    x->flcount = as_get32(h + 36);
    x->nextks = as_get32(h + 40);
    x->nxhead = as_get32(h + 44);
    x->nxcount = as_get32(h + 48);
    if (x->npages < 2 || x->root == 1 || (x->root && x->root >= x->npages) || x->flhead == 1 ||
        (x->flhead && x->flhead >= x->npages) || x->nxhead == 1 || (x->nxhead && x->nxhead >= x->npages) ||
        x->nextks < ASD_KS_USER) return 0;
    *ps = p;
    return 1;
}

/* Reads both headers and picks the newest one whose pages are all there. */
static int asd_pick(altsql_db *db, uint64_t fsize, asd_hdr *best, uint32_t *bps) {
    uint8_t h[ASD_HS];
    asd_hdr x[2];
    uint32_t ps[2] = { 0, 0 }, cand;
    int ok[2] = { 0, 0 }, i;
    if (fsize >= ASD_HS) {
        db->nread++;
        if (db->f.read(db->f.ctx, 0, h, ASD_HS)) return asd_err(db, ALTSQL_IOERR, "read failed");
        ok[0] = asd_hdr_get(h, &ps[0], &x[0]);
    }
    for (cand = 512; cand <= 65536; cand *= 2) {
        if (ok[0] && cand != ps[0]) continue;
        if (cand + ASD_HS > fsize) break;
        db->nread++;
        if (db->f.read(db->f.ctx, cand, h, ASD_HS)) return asd_err(db, ALTSQL_IOERR, "read failed");
        if (asd_hdr_get(h, &ps[1], &x[1]) && ps[1] == cand) { ok[1] = 1; break; }
    }
    for (i = 0; i < 2; i++)
        if (ok[i] && ((uint64_t)x[i].npages * ps[i] > fsize || (x[i].txn & 1) != (uint64_t)i)) ok[i] = 0;
    if (ok[0] && ok[1] && ps[0] != ps[1]) ok[x[0].txn > x[1].txn ? 1 : 0] = 0;
    if (!ok[0] && !ok[1]) return ASD_CORRUPT(db, "no valid commit header: not an AltSql DB file, or damaged");
    i = ok[0] && (!ok[1] || x[0].txn > x[1].txn) ? 0 : 1;
    *best = x[i];
    *bps = ps[i];
    return ALTSQL_OK;
}

/* The view of the last commit. */
static void asd_view(altsql_db *db) {
    db->root = db->com.root;
    db->npages = db->com.npages;
    db->nextks = db->com.nextks;
    db->cur = db->com.txn + 1;
}

static int asd_reload(altsql_db *db) {
    uint64_t sz;
    asd_hdr best;
    uint32_t ps;
    int rc;
    if (db->f.size(db->f.ctx, &sz)) return asd_err(db, ALTSQL_IOERR, "could not read the file size");
    if ((rc = asd_pick(db, sz, &best, &ps)) != 0) return rc;
    if (ps != db->ps) return ASD_CORRUPT(db, "page size changed");
    db->com = best;
    db->fsize = sz;
    db->reload = 0;
    db->gen++;
    db->nknown = 0;
    db->ntabs = 0;
    db->nmaps = 0;
    asd_view(db);
    return ALTSQL_OK;
}

/* ---- Open and close ---------------------------------------------------------- */
static uint8_t *asd_carve(uint8_t **m, size_t *left, size_t n) {
    uint8_t *p = *m;
    n = (n + 15) & ~(size_t)15;
    if (n > *left) return 0;
    *m += n;
    *left -= n;
    return p;
}

static int asd_setup(altsql_db *db, uint32_t ps, uint8_t *m, size_t left, size_t sqlreq) {
    size_t start = left, per;
    uint32_t i, hs;
    db->ps = ps;
    db->usable = ps - ASD_PH;
    db->maxcell = (db->usable - 8) / 4;
    db->maxkey = db->maxcell - 11 < ALTSQL_DB_MAXKEY ? db->maxcell - 11 : ALTSQL_DB_MAXKEY;
    db->epp = (ps - ASD_PH) / ASD_FLE;
    db->ovd = ps - ASD_PH;
    db->flw = 4 * db->epp < 64 ? 64 : 4 * db->epp;
    if (left / 64 / (2 * 12) > db->flw) db->flw = (uint32_t)(left / 64 / (2 * 12) > 0x3FFFFFF ? 0x3FFFFFF : left / 64 / (2 * 12));
    if (!(db->sa = asd_carve(&m, &left, ps)) || !(db->sb = asd_carve(&m, &left, ps)) ||
        !(db->sc = asd_carve(&m, &left, ps)) || !(db->cell = asd_carve(&m, &left, db->maxcell + 16)) ||
        !(db->bcell = asd_carve(&m, &left, ALTSQL_DB_MAXKEY + 16)) ||
        !(db->kb[0] = asd_carve(&m, &left, ALTSQL_DB_MAXKEY + 16)) ||
        !(db->kb[1] = asd_carve(&m, &left, ALTSQL_DB_MAXKEY + 16)) ||
        !(db->avpg = (uint32_t *)(void *)asd_carve(&m, &left, db->flw * 4u)) ||
        !(db->hopg = (uint32_t *)(void *)asd_carve(&m, &left, db->flw * 4u)) ||
        !(db->avtag = (uint64_t *)(void *)asd_carve(&m, &left, db->flw * 8u)) ||
        !(db->hotag = (uint64_t *)(void *)asd_carve(&m, &left, db->flw * 8u)) ||
        !(db->tabs = (struct asd_table *)(void *)asd_carve(&m, &left, ASD_NTAB * sizeof(struct asd_table))) ||
        !(db->rowbuf = asd_carve(&m, &left, ASD_ROWBUF)))
        return asd_err(db, ALTSQL_NOMEM, "working memory too small");
    per = (size_t)ps + sizeof(asd_frame) + 4 + 16;   /* a cache frame, its hash share included */
    db->sqlsize = sqlreq;
    if (!sqlreq) {                                    /* an eighth, leaving the cache its minimum */
        size_t cache_min = ASD_MINFR * per + 512, want = left / 8 < ((size_t)8 << 20) ? left / 8 : ((size_t)8 << 20);
        if (left < cache_min) want = 0;
        else if (want > left - cache_min) want = left - cache_min;
        db->sqlsize = want < 8192 ? 0 : want;
    }
    db->sqlsize &= ~(size_t)15;
    if (db->sqlsize && !(db->sqlmem = asd_carve(&m, &left, db->sqlsize)))
        return asd_err(db, ALTSQL_NOMEM, "working memory too small for sql_mem");
    db->sqlexec = db->sqlsize;
#if ALTSQL_ENABLE_SQL
    {                                                /* prepared statements: four, one or none */
        size_t one = (sizeof(struct altsql_db_stmt) + 15) & ~(size_t)15;
        db->nstmt = db->sqlsize >= 4 * one + (256u << 10) ? ASD_NSTMT : db->sqlsize >= one + (64u << 10) ? 1 : 0;
        db->sqlexec = db->sqlsize - db->nstmt * one;
        db->stmts = (struct altsql_db_stmt *)(void *)(db->sqlmem + db->sqlexec);
        for (i = 0; i < db->nstmt; i++) db->stmts[i].used = 0;
    }
#endif
    db->nfr = (uint32_t)((left > 256 ? left - 256 : 0) / per);
    if (db->nfr < ASD_MINFR) return asd_err(db, ALTSQL_NOMEM, "working memory too small for the page cache");
    for (hs = 1; hs < 2 * db->nfr; hs *= 2) {}
    db->hmask = hs - 1;
    db->hash = (uint32_t *)(void *)asd_carve(&m, &left, (size_t)hs * 4);
    db->fr = (asd_frame *)(void *)asd_carve(&m, &left, (size_t)db->nfr * sizeof(asd_frame));
    db->sortb = (uint32_t *)(void *)asd_carve(&m, &left, (size_t)db->nfr * 4);
    db->pages = asd_carve(&m, &left, (size_t)db->nfr * ps);
    if (!db->hash || !db->fr || !db->sortb || !db->pages)
        return asd_err(db, ALTSQL_NOMEM, "working memory too small for the page cache");
    for (i = 0; i < hs; i++) db->hash[i] = ASD_NONE;
    for (i = 0; i < db->nfr; i++) { db->fr[i].pg = ASD_NONE; db->fr[i].pin = 0; db->fr[i].dirty = 0; db->fr[i].ref = 0; }
    db->mem_used += start - left;
    return ALTSQL_OK;
}

static int asd_create(altsql_db *db) {
    asd_hdr x;
    uint32_t slot;
    memset(&x, 0, sizeof x);
    x.npages = 2;
    x.nextks = ASD_KS_USER;
    for (slot = 0; slot < 2; slot++) {
        x.txn = slot;
        memset(db->sa, 0, db->ps);
        asd_hdr_put(db, db->sa, &x);
        if (asd_wpage(db, slot, db->sa)) return asd_err(db, ALTSQL_IOERR, "could not write the new file");
    }
    db->nsync++;
    if (db->f.sync(db->f.ctx)) return asd_err(db, ALTSQL_IOERR, "could not sync the new file");
    db->com = x;
    return ALTSQL_OK;
}

int altsql_db_open(altsql_db **out, const altsql_db_file *file, const altsql_db_config *cfg) {
    uint8_t *m;
    size_t left, skip;
    altsql_db *db;
    uint64_t sz;
    uint32_t ps;
    asd_hdr best;
    int rc;
    if (!out) return ALTSQL_MISUSE;
    *out = 0;
    if (!file || !cfg || !cfg->mem || !file->read || !file->write || !file->sync || !file->size || !file->truncate)
        return ALTSQL_MISUSE;
    m = (uint8_t *)cfg->mem;
    skip = (size_t)((16 - ((uintptr_t)m & 15)) & 15);
    if (cfg->mem_size < skip + sizeof(altsql_db) + 64) return ALTSQL_NOMEM;
    m += skip;
    left = cfg->mem_size - skip;
    db = (altsql_db *)(void *)m;
    memset(db, 0, sizeof *db);
    m += (sizeof(altsql_db) + 15) & ~(size_t)15;
    left -= (sizeof(altsql_db) + 15) & ~(size_t)15;
    db->mem_used = cfg->mem_size - left;
    db->f = *file;
    *out = db;
    if (file->size(file->ctx, &sz)) return asd_err(db, ALTSQL_IOERR, "could not read the file size");
    if (sz == 0) {
        if (!cfg->create) return asd_err(db, ALTSQL_NOTFOUND, "the file holds no database");
        ps = cfg->page_size ? cfg->page_size : 4096;
        if (ps < 512 || ps > 65536 || (ps & (ps - 1))) return asd_err(db, ALTSQL_MISUSE, "page size must be a power of two from 512 to 65536");
        if ((rc = asd_setup(db, ps, m, left, cfg->sql_mem)) != 0) return rc;
        if ((rc = asd_create(db)) != 0) return rc;
        db->fsize = 2u * (uint64_t)ps;
    } else {
        if ((rc = asd_pick(db, sz, &best, &ps)) != 0) return rc;
        if ((rc = asd_setup(db, ps, m, left, cfg->sql_mem)) != 0) return rc;
        db->com = best;
        db->fsize = sz;
    }
    asd_view(db);
    return ALTSQL_OK;
}

void altsql_db_close(altsql_db *db) {
    if (db && db->tx) altsql_db_rollback(db);
}

const char *altsql_db_errmsg(const altsql_db *db) { return db ? db->err : "no handle"; }

/* ---- Transactions -------------------------------------------------------------- */
int altsql_db_begin(altsql_db *db, int writable) {
    int rc;
    if (!db) return ALTSQL_MISUSE;
    if (db->tx) return asd_err(db, ALTSQL_MISUSE, "a transaction is already open");
    if (db->reload && (rc = asd_reload(db)) != 0) return rc;
    asd_view(db);
    db->tx = writable ? 2 : 1;
    db->failed = 0;
    if (writable) {
        db->nav = db->nho = 0;
        db->dhead = db->dtail = db->dcount = 0;
        db->hhead = db->hcount = 0;
        db->rowctr = 0;
        db->flnext = db->com.flhead;
        db->fltail = db->com.flcount;
        db->nxnext = db->com.nxhead;
        db->nxleft = db->com.nxcount;
        db->seqpg = ASD_NONE;
        db->mods = 0;
    }
    return ALTSQL_OK;
}

static void asd_end(altsql_db *db) {
    db->tx = 0;
    db->failed = 0;
    asd_view(db);
}

int altsql_db_rollback(altsql_db *db) {
    if (!db) return ALTSQL_MISUSE;
    if (!db->tx) return asd_err(db, ALTSQL_MISUSE, "no transaction to roll back");
    if (db->tx == 2 && db->mods) {
        asd_drop_owned(db);
        db->gen++;
        db->nknown = 0;
        db->ntabs = 0;
        db->nmaps = 0;
    }
    asd_end(db);
    return ALTSQL_OK;
}

static void asd_sort(altsql_db *db, uint32_t *a, uint32_t n) {   /* by page number */
    uint32_t gap, i, j;
    for (gap = n / 2; gap; gap /= 2)
        for (i = gap; i < n; i++) {
            uint32_t t = a[i];
            for (j = i; j >= gap && db->fr[a[j - gap]].pg > db->fr[t].pg; j -= gap) a[j] = a[j - gap];
            a[j] = t;
        }
}

int altsql_db_commit(altsql_db *db) {
    uint8_t h[ASD_HS];
    asd_hdr nh;
    uint32_t head = 0, nxh = 0, i, n = 0;
    int rc;
    if (!db) return ALTSQL_MISUSE;
    if (!db->tx) return asd_err(db, ALTSQL_MISUSE, "no transaction to commit");
    if (db->tx == 1 || !db->mods) { asd_end(db); return ALTSQL_OK; }
    if (db->failed) return asd_err(db, ALTSQL_DB_FAILED, "the transaction failed: roll it back");
    if ((rc = asd_flflush(db, &head, &nxh)) != 0) goto fail;
    for (i = 0; i < db->nfr; i++)
        if (db->fr[i].dirty && db->fr[i].pg != ASD_NONE) db->sortb[n++] = i;
    asd_sort(db, db->sortb, n);
    for (i = 0; i < n; i++) {
        if ((rc = asd_wpage(db, db->fr[db->sortb[i]].pg, ASD_PAGE(db, db->sortb[i]))) != 0) goto fail;
        db->fr[db->sortb[i]].dirty = 0;
    }
    db->nsync++;
    if (db->f.sync(db->f.ctx)) { rc = ALTSQL_IOERR; goto fail; }
    nh.txn = db->cur;
    nh.root = db->root;
    nh.npages = db->npages;
    nh.flhead = head;
    nh.flcount = db->fltail;
    nh.nextks = db->nextks;
    nh.nxhead = nxh;
    nh.nxcount = db->hcount;
    asd_hdr_put(db, h, &nh);
    db->nwrite++;
    if (db->f.write(db->f.ctx, (uint64_t)(db->cur & 1) * db->ps, h, ASD_HS)) { rc = ALTSQL_IOERR; goto fail; }
    db->nsync++;
    if (db->f.sync(db->f.ctx)) { rc = ALTSQL_IOERR; goto fail; }
    db->com = nh;
    asd_end(db);
    /* pages left past the end by an interrupted commit are cut off */
    if (db->fsize > (uint64_t)db->npages * db->ps && !db->f.truncate(db->f.ctx, (uint64_t)db->npages * db->ps))
        db->fsize = (uint64_t)db->npages * db->ps;
    return ALTSQL_OK;
fail:
    /* The file holds this commit or the one before; the next call reads which. */
    asd_drop_owned(db);
    db->tx = 0;
    db->failed = 0;
    db->reload = 1;
    db->gen++;
    db->nknown = 0;
    db->ntabs = 0;
    db->nmaps = 0;
    return asd_err(db, rc == ALTSQL_NOMEM || rc == ALTSQL_CORRUPT ? rc : ALTSQL_IOERR,
                   "commit failed: the file holds the last commit or this one");
}

/* ---- The tree ---------------------------------------------------------------------- */
static void asd_release(altsql_db *db, asd_path *p) {
    int i;
    for (i = 0; i < p->depth; i++) asd_unpin(db, p->fi[i]);
    p->depth = 0;
}

/* Root to leaf, every page pinned. */
static int asd_descend(altsql_db *db, const uint8_t *k, uint32_t kn, asd_path *p) {
    uint32_t pg = db->root, fi;
    int rc;
    p->depth = 0;
    p->found = 0;
    while (pg) {
        uint8_t *pp;
        if (p->depth == ALTSQL_DB_MAXDEPTH) return ASD_CORRUPT(db, "tree too deep");
        if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
        pp = ASD_PAGE(db, fi);
        p->fi[p->depth] = fi;
        if (pp[0] == ASD_LEAF) { p->ix[p->depth++] = asd_lfind(pp, k, kn, &p->found); return ALTSQL_OK; }
        p->depth++;
        if (pp[0] != ASD_BRANCH) return ASD_CORRUPT(db, "unexpected page in the tree");
        p->ix[p->depth - 1] = asd_bfind(pp, k, kn);
        pg = asd_child(pp, p->ix[p->depth - 1]);
    }
    return ALTSQL_OK;
}

/* For reading: the leaf stays pinned when the key is there. */
static int asd_find(altsql_db *db, const uint8_t *k, uint32_t kn, uint32_t *ofi, uint32_t *oix) {
    uint32_t pg = db->root, fi, d = 0;
    int rc, found;
    while (pg) {
        uint8_t *pp;
        if (++d > ALTSQL_DB_MAXDEPTH) return ASD_CORRUPT(db, "tree too deep");
        fi = pg < db->npages ? asd_lookup(db, pg) : ASD_NONE;
        if (fi != ASD_NONE) { db->hits++; db->fr[fi].ref = 1; db->fr[fi].pin++; }
        else if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
        pp = ASD_PAGE(db, fi);
        if (pp[0] == ASD_LEAF) {
            uint32_t ix = asd_lfind(pp, k, kn, &found);
            if (!found) { asd_unpin(db, fi); return ALTSQL_NOTFOUND; }
            *ofi = fi;
            *oix = ix;
            return ALTSQL_OK;
        }
        if (pp[0] != ASD_BRANCH) { asd_unpin(db, fi); return ASD_CORRUPT(db, "unexpected page in the tree"); }
        pg = asd_child(pp, asd_bfind(pp, k, kn));
        asd_unpin(db, fi);
    }
    return ALTSQL_NOTFOUND;
}

/* Copy on write: every page on the path that an earlier commit wrote gets a
 * new place, and its parent points there. */
static int asd_touch(altsql_db *db, asd_path *p) {
    int l, rc;
    for (l = 0; l < p->depth; l++) {
        uint32_t fi = p->fi[l];
        uint8_t *src = ASD_PAGE(db, fi);
        uint64_t t = asd_txn(src);
        if (t != db->cur) {
            uint32_t oldpg = db->fr[fi].pg, newpg, nfi;
            if ((rc = asd_alloc(db, &newpg, 0)) != 0) return rc;
            if ((rc = asd_frame_for(db, newpg, src[0], &nfi)) != 0) return rc;
            memcpy(ASD_PAGE(db, nfi), src, db->ps);
            as_put64(ASD_PAGE(db, nfi) + 8, db->cur);
            asd_unpin(db, fi);
            p->fi[l] = nfi;
            if ((rc = asd_free(db, oldpg, t)) != 0) return rc;
            if (l == 0) db->root = newpg;
            else asd_set_child(ASD_PAGE(db, p->fi[l - 1]), p->ix[l - 1], newpg);
        }
        db->fr[p->fi[l]].dirty = 1;
    }
    db->mods++;
    return ALTSQL_OK;
}

static void asd_compact(altsql_db *db, uint8_t *p) {
    uint8_t *t = db->sc;
    uint32_t n = asd_n(p), i, hi = db->ps;
    memcpy(t, p, db->ps);
    for (i = 0; i < n; i++) {
        uint32_t sz = asd_csize(db, t, i);
        hi -= sz;
        memcpy(p + hi, t + asd_off(t, i), sz);
        as_put16(p + ASD_PH + 2 * i, hi);
    }
    asd_set_hi(p, hi);
    memset(p + asd_lo(p), 0, hi - asd_lo(p));
}

/* 0 when the cell went in, 1 when the page has no room for it. */
static int asd_pinsert(altsql_db *db, uint8_t *p, uint32_t ix, const uint8_t *cell, uint32_t len) {
    uint32_t n = asd_n(p), lo = asd_lo(p), hi = asd_hi(p);
    if (hi - lo < len + 2) {
        uint32_t used = 0, i;
        for (i = 0; i < n; i++) used += asd_csize(db, p, i);
        if (db->ps - ASD_PH - 2 * n - used < len + 2) return 1;
        asd_compact(db, p);
        hi = asd_hi(p);
    }
    hi -= len;
    memcpy(p + hi, cell, len);
    memmove(p + ASD_PH + 2 * (ix + 1), p + ASD_PH + 2 * ix, 2 * (n - ix));
    as_put16(p + ASD_PH + 2 * ix, hi);
    asd_set_n(p, n + 1);
    asd_set_lo(p, lo + 2);
    asd_set_hi(p, hi);
    return 0;
}

static void asd_premove(altsql_db *db, uint8_t *p, uint32_t ix) {
    uint32_t n = asd_n(p), off = asd_off(p, ix), sz = asd_csize(db, p, ix);
    memmove(p + ASD_PH + 2 * ix, p + ASD_PH + 2 * (ix + 1), 2 * (n - ix - 1));
    asd_set_n(p, n - 1);
    asd_set_lo(p, asd_lo(p) - 2);
    memset(p + off, 0, sz);
    if (off == asd_hi(p)) asd_set_hi(p, off + sz);
}

static void asd_append(uint8_t *p, const uint8_t *cell, uint32_t len) {
    uint32_t n = asd_n(p), hi = asd_hi(p) - len;
    memcpy(p + hi, cell, len);
    as_put16(p + ASD_PH + 2 * n, hi);
    asd_set_n(p, n + 1);
    asd_set_lo(p, ASD_PH + 2 * (n + 1));
    asd_set_hi(p, hi);
}

/* The cells of a page with one more cell put in at ix, as one list. */
typedef struct asd_vl { const uint8_t *p, *cell; uint32_t ix, clen; } asd_vl;
static const uint8_t *asd_vcell(const altsql_db *db, const asd_vl *v, uint32_t j, uint32_t *len) {
    if (j == v->ix) { *len = v->clen; return v->cell; }
    if (j > v->ix) j--;
    *len = asd_csize(db, v->p, j);
    return v->p + asd_off(v->p, j);
}

/* Index of the first cell of the right half: about half the bytes each side. */
static uint32_t asd_mid(const altsql_db *db, const asd_vl *v, uint32_t total) {
    uint32_t j, len, tot = 0, acc = 0;
    for (j = 0; j < total; j++) { asd_vcell(db, v, j, &len); tot += len + 2; }
    for (j = 0; j < total; j++) {
        asd_vcell(db, v, j, &len);
        if (j > 0 && acc + len + 2 > tot / 2) break;
        acc += len + 2;
    }
    if (j >= total) j = total - 1;
    if (j < 1) j = 1;
    return j;
}

static int asd_split_leaf(altsql_db *db, asd_path *p, uint32_t ix, const uint8_t *cell, uint32_t clen,
                          uint32_t *rpg, uint8_t *sep, uint32_t *seplen) {
    uint32_t lfi = p->fi[p->depth - 1], rfi, n, s, j, len, an, bn, i = 0;
    uint8_t *L = ASD_PAGE(db, lfi), *R;
    const uint8_t *a, *b;
    asd_vl v;
    int rc;
    n = asd_n(L);
    v.p = L; v.ix = ix; v.cell = cell; v.clen = clen;
    if (ix == n) s = n;                                     /* added at the end: the old page stays full */
    else {
        int at = db->seqpg == db->fr[lfi].pg && ix == db->seqix + 1 && ix > 0;   /* in order after the last insert */
        if (!at && ix > 0) {
            /* The new key ends a run its left neighbour belongs to (a device's readings, say) and
             * the right neighbour starts another: splitting there keeps the run's page full. */
            uint32_t kn, vn0, ln, rn, cl = 0, cr = 0, h = asd_vgetu(cell, &kn);
            const uint8_t *k, *lk, *rk;
            h += asd_vgetu(cell + h, &vn0);
            k = cell + h;                                   /* the new cell's key */
            lk = asd_lkey(L, ix - 1, &ln);
            rk = asd_lkey(L, ix, &rn);
            while (cl < kn && cl < ln && k[cl] == lk[cl]) cl++;
            while (cr < kn && cr < rn && k[cr] == rk[cr]) cr++;
            at = cl >= cr + 4;
        }
        if (at) {                                           /* the new cell ends the left page; its run goes on there */
            uint32_t left = clen + 2, right = clen + 2;
            for (j = 0; j < ix; j++) left += asd_csize(db, L, j) + 2;
            for (j = ix; j < n; j++) right += asd_csize(db, L, j) + 2;
            s = left <= db->usable ? ix + 1 : right <= db->usable ? ix : asd_mid(db, &v, n + 1);
        } else s = asd_mid(db, &v, n + 1);
    }
    if ((rc = asd_alloc(db, &j, 0)) != 0) return rc;
    if ((rc = asd_frame_for(db, j, ASD_LEAF, &rfi)) != 0) return rc;
    R = ASD_PAGE(db, rfi);
    if (s == n && ix == n) asd_append(R, cell, clen);
    else {
        asd_pinit(db, db->sa, ASD_LEAF);
        for (j = 0; j < s; j++) { const uint8_t *c = asd_vcell(db, &v, j, &len); asd_append(db->sa, c, len); }
        for (j = s; j <= n; j++) { const uint8_t *c = asd_vcell(db, &v, j, &len); asd_append(R, c, len); }
        memcpy(L, db->sa, db->ps);
    }
    /* the separator: the shortest prefix of the right page's first key above the left page's last */
    a = asd_lkey(L, asd_n(L) - 1, &an);
    b = asd_lkey(R, 0, &bn);
    while (i < an && i < bn && a[i] == b[i]) i++;
    *seplen = i < bn ? i + 1 : bn;
    memcpy(sep, b, *seplen);
    if (ix < s) { db->seqpg = db->fr[lfi].pg; db->seqix = ix; }
    else { db->seqpg = db->fr[rfi].pg; db->seqix = ix - s; }
    *rpg = db->fr[rfi].pg;
    asd_unpin(db, rfi);
    return ALTSQL_OK;
}

static int asd_split_branch(altsql_db *db, asd_path *p, int l, uint32_t ix, const uint8_t *cell, uint32_t clen,
                            uint32_t *rpg, uint8_t *up, uint32_t *uplen) {
    uint32_t bfi = p->fi[l], rfi, n, s, j, len, upchild, rightmost, pg;
    uint8_t *B = ASD_PAGE(db, bfi), *R;
    const uint8_t *c;
    asd_vl v;
    int rc;
    n = asd_n(B);
    rightmost = asd_x(B);
    v.p = B; v.ix = ix; v.cell = cell; v.clen = clen;
    s = ix == n ? n : asd_mid(db, &v, n + 1);               /* cell s moves up */
    if ((rc = asd_alloc(db, &pg, 0)) != 0) return rc;
    if ((rc = asd_frame_for(db, pg, ASD_BRANCH, &rfi)) != 0) return rc;
    R = ASD_PAGE(db, rfi);
    c = asd_vcell(db, &v, s, &len);
    upchild = as_get32(c);
    j = asd_vgetu(c + 4, uplen);
    memcpy(up, c + 4 + j, *uplen);
    asd_pinit(db, db->sa, ASD_BRANCH);
    for (j = 0; j < s; j++) { c = asd_vcell(db, &v, j, &len); asd_append(db->sa, c, len); }
    asd_set_x(db->sa, upchild);
    for (j = s + 1; j <= n; j++) { c = asd_vcell(db, &v, j, &len); asd_append(R, c, len); }
    asd_set_x(R, rightmost);
    memcpy(B, db->sa, db->ps);
    *rpg = pg;
    asd_unpin(db, rfi);
    return ALTSQL_OK;
}

static uint32_t asd_bcell(uint8_t *out, uint32_t child, const uint8_t *k, uint32_t kn) {
    uint32_t a;
    as_put32(out, child);
    a = 4 + asd_vput(out + 4, kn);
    memcpy(out + a, k, kn);
    return a + kn;
}

/* Puts a cell into the leaf at the end of the path, splitting upward as needed. */
static int asd_insert(altsql_db *db, asd_path *p, const uint8_t *cell, uint32_t clen) {
    int l = p->depth - 1, rc, w = 0;
    uint32_t ix = p->ix[l], rpg, seplen, bl;
    uint8_t *sep = db->kb[0];
    if (!asd_pinsert(db, ASD_PAGE(db, p->fi[l]), ix, cell, clen)) {
        db->seqpg = db->fr[p->fi[l]].pg;
        db->seqix = ix;
        return ALTSQL_OK;
    }
    if ((rc = asd_split_leaf(db, p, ix, cell, clen, &rpg, sep, &seplen)) != 0) return rc;
    while (l > 0) {
        uint32_t lpg = db->fr[p->fi[l]].pg;
        uint8_t *B;
        l--;
        B = ASD_PAGE(db, p->fi[l]);
        ix = p->ix[l];
        asd_set_child(B, ix, rpg);
        bl = asd_bcell(db->bcell, lpg, sep, seplen);
        if (!asd_pinsert(db, B, ix, db->bcell, bl)) return ALTSQL_OK;
        w ^= 1;
        if ((rc = asd_split_branch(db, p, l, ix, db->bcell, bl, &rpg, db->kb[w], &seplen)) != 0) return rc;
        sep = db->kb[w];
    }
    {   /* the root split: a new root over the two halves */
        uint32_t fi, pg;
        if ((rc = asd_alloc(db, &pg, 0)) != 0) return rc;
        if ((rc = asd_frame_for(db, pg, ASD_BRANCH, &fi)) != 0) return rc;
        bl = asd_bcell(db->bcell, db->fr[p->fi[0]].pg, sep, seplen);
        asd_append(ASD_PAGE(db, fi), db->bcell, bl);
        asd_set_x(ASD_PAGE(db, fi), rpg);
        db->root = pg;
        asd_unpin(db, fi);
    }
    return ALTSQL_OK;
}

/* ---- Large values: chains of overflow pages, written and read past the cache ---- */
static int asd_ovwrite(altsql_db *db, const uint8_t *v, uint32_t vn, uint32_t *first) {
    uint32_t pg, next = 0, done = 0, n;
    uint8_t *s = db->sc;
    int rc;
    if ((rc = asd_alloc(db, &pg, 0)) != 0) return rc;
    *first = pg;
    while (done < vn) {
        n = vn - done < db->ovd ? vn - done : db->ovd;
        next = 0;
        if (done + n < vn && (rc = asd_alloc(db, &next, 0)) != 0) return rc;
        memset(s, 0, ASD_PH);
        s[0] = ASD_OVFL;
        as_put64(s + 8, db->cur);
        asd_set_x(s, next);
        memcpy(s + ASD_PH, v + done, n);
        if (n < db->ovd) memset(s + ASD_PH + n, 0, db->ovd - n);
        asd_forget(db, pg);
        if ((rc = asd_wpage(db, pg, s)) != 0) return rc;
        done += n;
        pg = next;
    }
    db->mods++;
    return ALTSQL_OK;
}

static int asd_ovread(altsql_db *db, uint32_t pg, uint32_t vn, uint8_t *out) {
    uint32_t done = 0, n;
    while (done < vn) {
        if (pg < 2 || pg >= db->npages) return ASD_CORRUPT(db, "damaged overflow chain");
        db->nread++;
        if (db->f.read(db->f.ctx, (uint64_t)pg * db->ps, db->sc, db->ps)) return asd_err(db, ALTSQL_IOERR, "read failed");
        if (db->sc[0] != ASD_OVFL || asd_txn(db->sc) > db->cur) return ASD_CORRUPT(db, "damaged overflow chain");
        n = vn - done < db->ovd ? vn - done : db->ovd;
        memcpy(out + done, db->sc + ASD_PH, n);
        done += n;
        pg = asd_x(db->sc);
    }
    return ALTSQL_OK;
}

static int asd_ovfree(altsql_db *db, uint32_t pg, uint32_t vn) {
    uint32_t cnt = (vn + db->ovd - 1) / db->ovd, i;
    uint8_t h[ASD_PH];
    int rc;
    for (i = 0; i < cnt; i++) {
        uint32_t next;
        if (pg < 2 || pg >= db->npages) return ASD_CORRUPT(db, "damaged overflow chain");
        db->nread++;
        if (db->f.read(db->f.ctx, (uint64_t)pg * db->ps, h, ASD_PH)) return asd_err(db, ALTSQL_IOERR, "read failed");
        if (h[0] != ASD_OVFL) return ASD_CORRUPT(db, "damaged overflow chain");
        next = asd_x(h);
        if ((rc = asd_free(db, pg, asd_txn(h))) != 0) return rc;
        pg = next;
    }
    return ALTSQL_OK;
}

static int asd_value(altsql_db *db, const uint8_t *page, uint32_t ix, void *buf, size_t cap, size_t *vn) {
    asd_cell c;
    asd_leaf_cell(db, page, ix, &c);
    if (vn) *vn = c.vn;
    if (c.vn > cap) return asd_err(db, ALTSQL_DB_SHORT, "buffer too small for the value");
    if (c.v) { if (c.vn) memcpy(buf, c.v, c.vn); return ALTSQL_OK; }
    return asd_ovread(db, c.ov, c.vn, (uint8_t *)buf);
}

/* ---- Put and delete on the tree ---------------------------------------------------- */
static int asd_put(altsql_db *db, const uint8_t *k, uint32_t kn, const uint8_t *v, uint32_t vn) {
    asd_path p;
    asd_cell old;
    uint32_t ov = 0, a;
    int rc, had = 0;
    if ((rc = asd_descend(db, k, kn, &p)) != 0) goto out;
    if (p.depth && p.found) {
        asd_leaf_cell(db, ASD_PAGE(db, p.fi[p.depth - 1]), p.ix[p.depth - 1], &old);
        if (!old.ov && old.vn == vn && (!vn || !memcmp(old.v, v, vn))) goto out;   /* the same value */
        had = 1;
    }
    if (!asd_inline(db, kn, vn) && (rc = asd_ovwrite(db, v, vn, &ov)) != 0) goto out;
    a = asd_vput(db->cell, kn);
    a += asd_vput(db->cell + a, vn);
    memcpy(db->cell + a, k, kn);
    a += kn;
    if (ov) { as_put32(db->cell + a, ov); a += 4; }
    else { if (vn) memcpy(db->cell + a, v, vn); a += vn; }
    if (!p.depth) {                                          /* the first leaf */
        uint32_t fi, pg;
        if ((rc = asd_alloc(db, &pg, 0)) != 0) goto out;
        if ((rc = asd_frame_for(db, pg, ASD_LEAF, &fi)) != 0) goto out;
        asd_append(ASD_PAGE(db, fi), db->cell, a);
        db->root = pg;
        db->seqpg = pg;
        db->seqix = 0;
        asd_unpin(db, fi);
        goto out;
    }
    if ((rc = asd_touch(db, &p)) != 0) goto out;
    if (had) {
        uint8_t *L = ASD_PAGE(db, p.fi[p.depth - 1]);
        asd_leaf_cell(db, L, p.ix[p.depth - 1], &old);
        ov = old.ov;
        vn = old.vn;
        asd_premove(db, L, p.ix[p.depth - 1]);
        if (ov && (rc = asd_ovfree(db, ov, vn)) != 0) goto out;
    }
    rc = asd_insert(db, &p, db->cell, a);
out:
    asd_release(db, &p);
    return rc;
}

/* A root with a single child gives way to it. */
static int asd_collapse(altsql_db *db) {
    while (db->root) {
        uint32_t fi, child, pg = db->root;
        uint64_t t;
        uint8_t *P;
        int rc = asd_get(db, pg, &fi);
        if (rc) return rc;
        P = ASD_PAGE(db, fi);
        if (P[0] != ASD_BRANCH || asd_n(P) > 0) { asd_unpin(db, fi); return ALTSQL_OK; }
        child = asd_x(P);
        t = asd_txn(P);
        asd_unpin(db, fi);
        if ((rc = asd_free(db, pg, t)) != 0) return rc;
        db->root = child;
    }
    return ALTSQL_OK;
}

static int asd_delete(altsql_db *db, const uint8_t *k, uint32_t kn) {
    asd_path p;
    asd_cell c;
    int rc, l;
    uint8_t *L;
    if ((rc = asd_descend(db, k, kn, &p)) != 0) goto out;
    if (!p.depth || !p.found) { rc = ALTSQL_NOTFOUND; goto out; }
    if ((rc = asd_touch(db, &p)) != 0) goto out;
    l = p.depth - 1;
    L = ASD_PAGE(db, p.fi[l]);
    asd_leaf_cell(db, L, p.ix[l], &c);
    {
        uint32_t ov = c.ov, vn = c.vn;
        asd_premove(db, L, p.ix[l]);
        if (ov && (rc = asd_ovfree(db, ov, vn)) != 0) goto out;
    }
    if (asd_n(L) > 0) goto out;
    /* empty pages leave the tree, bottom up */
    for (;;) {
        uint8_t *B;
        uint32_t ci, nb;
        if ((rc = asd_free(db, db->fr[p.fi[l]].pg, db->cur)) != 0) goto out;
        if (l == 0) { db->root = 0; goto out; }
        l--;
        B = ASD_PAGE(db, p.fi[l]);
        ci = p.ix[l];
        nb = asd_n(B);
        if (nb == 0) continue;                               /* its only child went: it goes too */
        if (ci < nb) asd_premove(db, B, ci);
        else { asd_set_x(B, as_get32(B + asd_off(B, nb - 1))); asd_premove(db, B, nb - 1); }
        break;
    }
    rc = asd_collapse(db);
out:
    asd_release(db, &p);
    return rc;
}

/* ---- Buckets and the catalog ---------------------------------------------------------
 * Key space 1: 'n' + name -> kind, key space (LE32); 'k' + key space -> kind, name. */
static void asd_known(altsql_db *db, uint32_t b) {
    uint32_t i;
    for (i = 0; i < db->nknown; i++) if (db->known[i] == b) return;
    if (db->nknown < 16) db->known[db->nknown++] = b;
    else { db->known[db->kpos] = b; db->kpos = (db->kpos + 1) & 15; }
}

static int asd_ready(altsql_db *db) {
    if (db->tx && db->failed) return asd_err(db, ALTSQL_DB_FAILED, "the transaction failed: roll it back");
    if (!db->tx && db->reload) return asd_reload(db);
    return ALTSQL_OK;
}

static int asd_bucket_ok(altsql_db *db, uint32_t b) {
    uint8_t k[16];
    uint32_t i, kn, fi, ix;
    int rc, kind;
    for (i = 0; i < db->nknown; i++) if (db->known[i] == b) return ALTSQL_OK;
    if (b < ASD_KS_USER) return asd_err(db, ALTSQL_MISUSE, "not a bucket");
    kn = asd_ksput(k, ASD_KS_CAT);
    k[kn++] = 'k';
    kn += asd_ksput(k + kn, b);
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_NOTFOUND) return asd_err(db, ALTSQL_MISUSE, "not a bucket");
    if (rc) return rc;
    {
        asd_cell c;
        asd_leaf_cell(db, ASD_PAGE(db, fi), ix, &c);
        kind = !c.ov && c.vn ? c.v[0] : 0;
        asd_unpin(db, fi);
    }
    if (kind != ASD_K_BUCKET) return asd_err(db, ALTSQL_MISUSE, "a table's key space cannot be used as a bucket");
    asd_known(db, b);
    return ALTSQL_OK;
}

static int asd_tkey(altsql_db *db, uint32_t b, const void *k, size_t kn, uint8_t *out, uint32_t *on) {
    uint32_t p = asd_ksput(out, b);
    if (kn + p > db->maxkey) return asd_err(db, ALTSQL_TOOBIG, "key too long");
    if (kn) memcpy(out + p, k, kn);
    *on = p + (uint32_t)kn;
    return ALTSQL_OK;
}

/* A write inside the open transaction, or in one of its own. */
static int asd_write(altsql_db *db, int put, const uint8_t *tk, uint32_t tkn, const void *v, uint32_t vn) {
    int rc, own = 0;
    uint64_t before;
    if (!db->tx) {
        if ((rc = altsql_db_begin(db, 1)) != 0) return rc;
        own = 1;
    }
    before = db->mods;
    rc = put ? asd_put(db, tk, tkn, (const uint8_t *)v, vn) : asd_delete(db, tk, tkn);
    if (rc < 0) {
        if (own) altsql_db_rollback(db);
        else if (db->mods != before) db->failed = 1;
        return rc;
    }
    if (db->mods != before) db->gen++;
    if (own) {
        int rc2 = altsql_db_commit(db);
        if (rc2) return rc2;
    }
    return rc;
}

int altsql_db_bucket(altsql_db *db, const char *name, int create, uint32_t *b) {
    uint8_t k[80], v[80];
    uint32_t kn, fi, ix, sp = 0;
    size_t n;
    int rc, own = 0;
    if (!db || !name || !b) return ALTSQL_MISUSE;
    n = strlen(name);
    if (n == 0 || n > 63) return asd_err(db, ALTSQL_MISUSE, "a bucket name has 1 to 63 bytes");
    if ((rc = asd_ready(db)) != 0) return rc;
    kn = asd_ksput(k, ASD_KS_CAT);
    k[kn++] = 'n';
    memcpy(k + kn, name, n);
    kn += (uint32_t)n;
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_OK) {
        asd_cell c;
        int kind;
        asd_leaf_cell(db, ASD_PAGE(db, fi), ix, &c);
        kind = !c.ov && c.vn >= 5 ? c.v[0] : -1;
        sp = kind >= 0 ? as_get32(c.v + 1) : 0;
        asd_unpin(db, fi);
        if (kind < 0) return ASD_CORRUPT(db, "damaged catalog");
        if (kind != ASD_K_BUCKET) return asd_err(db, ALTSQL_MISUSE, "that name belongs to a table, not a bucket");
        asd_known(db, sp);
        *b = sp;
        return ALTSQL_OK;
    }
    if (rc != ALTSQL_NOTFOUND) return rc;
    if (!create) return asd_err(db, ALTSQL_NOTFOUND, "no such bucket");
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot create a bucket");
    if (!db->tx) {
        if ((rc = altsql_db_begin(db, 1)) != 0) return rc;
        own = 1;
    }
    if (db->nextks == ASD_NONE) { rc = asd_err(db, ALTSQL_FULL, "no key space numbers left"); goto done; }
    sp = db->nextks++;
    db->mods++;
    v[0] = ASD_K_BUCKET;
    as_put32(v + 1, sp);
    if ((rc = asd_write(db, 1, k, kn, v, 5)) != 0) goto done;
    kn = asd_ksput(k, ASD_KS_CAT);
    k[kn++] = 'k';
    kn += asd_ksput(k + kn, sp);
    memcpy(v + 1, name, n);
    rc = asd_write(db, 1, k, kn, v, 1 + (uint32_t)n);
done:
    if (own) {
        if (rc) altsql_db_rollback(db);
        else rc = altsql_db_commit(db);
    } else if (rc) db->failed = 1;
    if (!rc) { asd_known(db, sp); *b = sp; }
    return rc;
}

/* ---- The direct path ------------------------------------------------------------------ */
int altsql_db_get(altsql_db *db, uint32_t b, const void *k, size_t kn, void *buf, size_t cap, size_t *vn) {
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn, fi, ix;
    int rc;
    if (!db || (!k && kn) || (!buf && cap)) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_bucket_ok(db, b)) != 0 ||
        (rc = asd_tkey(db, b, k, kn, tk, &tkn)) != 0) return rc;
    if ((rc = asd_find(db, tk, tkn, &fi, &ix)) != 0) return rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, buf, cap, vn);
    asd_unpin(db, fi);
    return rc;
}

int altsql_db_put(altsql_db *db, uint32_t b, const void *k, size_t kn, const void *v, size_t vn) {
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn;
    int rc;
    if (!db || (!k && kn) || (!v && vn)) return ALTSQL_MISUSE;
    if (vn > 0x7FFFFFFFu) return asd_err(db, ALTSQL_TOOBIG, "value too large");
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if ((rc = asd_ready(db)) != 0 || (rc = asd_bucket_ok(db, b)) != 0 ||
        (rc = asd_tkey(db, b, k, kn, tk, &tkn)) != 0) return rc;
    return asd_write(db, 1, tk, tkn, v, (uint32_t)vn);
}

int altsql_db_del(altsql_db *db, uint32_t b, const void *k, size_t kn) {
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn;
    int rc;
    if (!db || (!k && kn)) return ALTSQL_MISUSE;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if ((rc = asd_ready(db)) != 0 || (rc = asd_bucket_ok(db, b)) != 0 ||
        (rc = asd_tkey(db, b, k, kn, tk, &tkn)) != 0) return rc;
    return asd_write(db, 0, tk, tkn, 0, 0);
}

/* ---- Cursors ------------------------------------------------------------------------------
 * state: 0 unused, 1 on an entry, 2 past the bucket's end, 3 before its start. */
static int asd_cleaf_first(altsql_db_cursor *c, int l, uint32_t pg, int last) {
    altsql_db *db = c->db;
    for (;; l++) {
        uint32_t fi, n;
        uint8_t *P;
        int rc;
        if (l >= ALTSQL_DB_MAXDEPTH) return ASD_CORRUPT(db, "tree too deep");
        if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
        P = ASD_PAGE(db, fi);
        n = asd_n(P);
        c->pg[l] = pg;
        if (P[0] == ASD_LEAF) {
            asd_unpin(db, fi);
            if (!n) return ASD_CORRUPT(db, "empty leaf inside the tree");
            c->ix[l] = (uint16_t)(last ? n - 1 : 0);
            c->depth = (uint16_t)(l + 1);
            return ALTSQL_OK;
        }
        if (P[0] != ASD_BRANCH) { asd_unpin(db, fi); return ASD_CORRUPT(db, "unexpected page in the tree"); }
        c->ix[l] = (uint16_t)(last ? n : 0);
        pg = asd_child(P, c->ix[l]);
        asd_unpin(db, fi);
    }
}

/* Moves to the next leaf (dir 1) or the previous one (dir -1). 1 at the end of the tree. */
static int asd_cleaf_step(altsql_db_cursor *c, int dir) {
    altsql_db *db = c->db;
    int l = c->depth - 2;
    while (l >= 0) {
        uint32_t fi, n, pg;
        int rc = asd_get(db, c->pg[l], &fi);
        if (rc) return rc;
        n = asd_n(ASD_PAGE(db, fi));
        if (dir > 0 ? c->ix[l] < n : c->ix[l] > 0) {
            c->ix[l] = (uint16_t)(c->ix[l] + dir);
            pg = asd_child(ASD_PAGE(db, fi), c->ix[l]);
            asd_unpin(db, fi);
            return asd_cleaf_first(c, l + 1, pg, dir < 0);
        }
        asd_unpin(db, fi);
        l--;
    }
    return 1;
}

/* The cursor's leaf, straight from the cache when its frame still holds it.
 * Not pinned: used before the next call into the cache. */
static int asd_cleafpage(altsql_db_cursor *c, uint8_t **P) {
    altsql_db *db = c->db;
    uint32_t pg = c->pg[c->depth - 1], fi = c->fi;
    if (fi >= db->nfr || db->fr[fi].pg != pg) {
        int rc = asd_get(db, pg, &fi);
        if (rc) return rc;
        asd_unpin(db, fi);
        c->fi = fi;
    }
    *P = ASD_PAGE(db, fi);
    return ALTSQL_OK;
}

/* Saves the key of the entry under the cursor; 1 when it lies outside the bucket. */
static int asd_cload(altsql_db_cursor *c) {
    altsql_db *db = c->db;
    uint32_t kn;
    const uint8_t *k;
    uint8_t *P;
    int rc = asd_cleafpage(c, &P);
    if (rc) return rc;
    k = asd_lkey(P, c->ix[c->depth - 1], &kn);
    if (kn > sizeof c->key) return ASD_CORRUPT(db, "key too long");
    memcpy(c->key, k, kn);
    c->klen = (uint16_t)kn;
    c->gen = db->gen;
    if (kn < c->plen || memcmp(c->key, c->pre, c->plen)) return 1;
    return ALTSQL_OK;
}

/* Positions at the first entry >= k: 0 on an entry, 1 at the end of the tree. */
static int asd_cseek(altsql_db_cursor *c, const uint8_t *k, uint32_t kn, int *found) {
    altsql_db *db = c->db;
    uint32_t pg = db->root;
    int l = 0;
    *found = 0;
    c->depth = 0;
    if (!pg) return 1;
    for (;; l++) {
        uint32_t fi, n;
        uint8_t *P;
        int rc;
        if (l >= ALTSQL_DB_MAXDEPTH) return ASD_CORRUPT(db, "tree too deep");
        if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
        P = ASD_PAGE(db, fi);
        n = asd_n(P);
        c->pg[l] = pg;
        c->depth = (uint16_t)(l + 1);
        if (P[0] == ASD_LEAF) {
            c->ix[l] = (uint16_t)asd_lfind(P, k, kn, found);
            asd_unpin(db, fi);
            if (c->ix[l] < n) return ALTSQL_OK;
            return asd_cleaf_step(c, 1);
        }
        if (P[0] != ASD_BRANCH) { asd_unpin(db, fi); return ASD_CORRUPT(db, "unexpected page in the tree"); }
        c->ix[l] = (uint16_t)asd_bfind(P, k, kn);
        pg = asd_child(P, c->ix[l]);
        asd_unpin(db, fi);
    }
}

/* One entry back; 1 before the first entry of the tree. */
static int asd_cback(altsql_db_cursor *c) {
    if (c->depth && c->ix[c->depth - 1] > 0) { c->ix[c->depth - 1]--; return ALTSQL_OK; }
    return c->depth ? asd_cleaf_step(c, -1) : 1;
}

/* One entry on; 1 past the last entry of the tree. */
static int asd_cfwd(altsql_db_cursor *c) {
    uint8_t *P;
    int rc = asd_cleafpage(c, &P);
    if (rc) return rc;
    if ((uint32_t)c->ix[c->depth - 1] + 1 < asd_n(P)) { c->ix[c->depth - 1]++; return ALTSQL_OK; }
    return asd_cleaf_step(c, 1);
}

/* After a move: load the entry, or mark the end. */
static int asd_cland(altsql_db_cursor *c, int rc, int endstate) {
    if (rc < 0) { c->state = 0; return rc; }
    if (rc == 0 && (rc = asd_cload(c)) < 0) { c->state = 0; return rc; }
    if (rc) { c->state = endstate; return ALTSQL_NOTFOUND; }
    c->state = 1;
    return ALTSQL_OK;
}

static int asd_cstart(altsql_db_cursor *c, altsql_db *db, uint32_t b) {
    int rc;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_bucket_ok(db, b)) != 0) return rc;
    c->db = db;
    c->space = b;
    c->state = 0;
    c->fi = ASD_NONE;
    c->plen = (uint16_t)asd_ksput(c->pre, b);
    return ALTSQL_OK;
}

int altsql_db_seek(altsql_db_cursor *c, altsql_db *db, uint32_t b, const void *k, size_t kn) {
    uint32_t tkn;
    int rc, found;
    if (!c || !db || (!k && kn)) return ALTSQL_MISUSE;
    if ((rc = asd_cstart(c, db, b)) != 0) return rc;
    if ((rc = asd_tkey(db, b, k, kn, c->key, &tkn)) != 0) return rc;
    return asd_cland(c, asd_cseek(c, c->key, tkn, &found), 2);
}

/* The last entry under the cursor's prefix: seek just past the prefix, step back. */
static int asd_clast(altsql_db_cursor *c) {
    uint8_t k[72];
    uint32_t n = c->plen;
    int rc, found;
    memcpy(k, c->pre, n);
    while (n > 0 && k[n - 1] == 0xFF) n--;
    if (n == 0) k[n++] = 0xFF;              /* no key starts with 0xFF: the end of the tree */
    else k[n - 1]++;
    rc = asd_cseek(c, k, n, &found);
    if (rc < 0) return rc;
    if (rc == 1) {                          /* at the end of the tree: from the last entry */
        uint32_t fi;
        if (!c->depth) return 1;
        if ((rc = asd_get(c->db, c->pg[c->depth - 1], &fi)) != 0) return rc;
        c->ix[c->depth - 1] = (uint16_t)asd_n(ASD_PAGE(c->db, fi));
        asd_unpin(c->db, fi);
    }
    return asd_cback(c);
}

int altsql_db_last(altsql_db_cursor *c, altsql_db *db, uint32_t b) {
    int rc;
    if (!c || !db) return ALTSQL_MISUSE;
    if ((rc = asd_cstart(c, db, b)) != 0) return rc;
    return asd_cland(c, asd_clast(c), 3);
}

int altsql_db_next(altsql_db_cursor *c) {
    altsql_db *db;
    int rc, found;
    if (!c || !c->db || !c->state) return ALTSQL_MISUSE;
    db = c->db;
    if ((rc = asd_ready(db)) != 0) return rc;
    if (c->state == 2) return ALTSQL_NOTFOUND;
    if (c->state == 3) {
        rc = asd_cseek(c, c->pre, c->plen, &found);
        return asd_cland(c, rc, 2);
    }
    if (c->gen != db->gen) {                /* the tree changed: find the place again */
        rc = asd_cseek(c, c->key, c->klen, &found);
        if (rc || !found) return asd_cland(c, rc, 2);
    } else {                                /* the common case: the next entry on the same leaf */
        uint8_t *P;
        uint32_t ix = (uint32_t)c->ix[c->depth - 1] + 1, kn;
        if ((rc = asd_cleafpage(c, &P)) != 0) { c->state = 0; return rc; }
        if (ix < asd_n(P)) {
            const uint8_t *k = asd_lkey(P, ix, &kn);
            c->ix[c->depth - 1] = (uint16_t)ix;
            if (kn > sizeof c->key) { c->state = 0; return ASD_CORRUPT(db, "key too long"); }
            memcpy(c->key, k, kn);
            c->klen = (uint16_t)kn;
            if (kn < c->plen || memcmp(c->key, c->pre, c->plen)) { c->state = 2; return ALTSQL_NOTFOUND; }
            return ALTSQL_OK;
        }
    }
    return asd_cland(c, asd_cfwd(c), 2);
}

int altsql_db_prev(altsql_db_cursor *c) {
    altsql_db *db;
    int rc, found;
    if (!c || !c->db || !c->state) return ALTSQL_MISUSE;
    db = c->db;
    if ((rc = asd_ready(db)) != 0) return rc;
    if (c->state == 3) return ALTSQL_NOTFOUND;
    if (c->state == 2) return asd_cland(c, asd_clast(c), 3);
    if (c->gen != db->gen) {
        rc = asd_cseek(c, c->key, c->klen, &found);
        if (rc < 0) return asd_cland(c, rc, 3);
        if (rc == 1) {                       /* nothing at or after the key: start from the very end */
            uint32_t fi, n;
            if (!c->depth) return asd_cland(c, 1, 3);
            if ((rc = asd_get(db, c->pg[c->depth - 1], &fi)) != 0) return asd_cland(c, rc, 3);
            n = asd_n(ASD_PAGE(db, fi));
            asd_unpin(db, fi);
            c->ix[c->depth - 1] = (uint16_t)n;
        }
    }
    return asd_cland(c, asd_cback(c), 3);
}

int altsql_db_key(altsql_db_cursor *c, const void **k, size_t *kn) {
    if (!c || !k || !kn) return ALTSQL_MISUSE;
    if (c->state != 1) return ALTSQL_NOTFOUND;
    *k = c->key + c->plen;
    *kn = (size_t)c->klen - c->plen;
    return ALTSQL_OK;
}

int altsql_db_value(altsql_db_cursor *c, void *buf, size_t cap, size_t *vn) {
    altsql_db *db;
    uint32_t fi;
    int rc;
    if (!c || !c->db || (!buf && cap)) return ALTSQL_MISUSE;
    if (c->state != 1) return ALTSQL_NOTFOUND;
    db = c->db;
    if ((rc = asd_ready(db)) != 0) return rc;
    if (c->gen != db->gen) {
        uint32_t ix;
        rc = asd_find(db, c->key, c->klen, &fi, &ix);
        if (rc) return rc;
        rc = asd_value(db, ASD_PAGE(db, fi), ix, buf, cap, vn);
        asd_unpin(db, fi);
        return rc;
    }
    {
        uint8_t *P;
        if ((rc = asd_cleafpage(c, &P)) != 0) return rc;
        return asd_value(db, P, c->ix[c->depth - 1], buf, cap, vn);
    }
}

/* ---- Tables ------------------------------------------------------------------------------
 * Catalog entry 'n' + name: kind (1), key space (LE32), columns (1), key columns (1), flags
 * (1), the key's column indexes, then the columns as "name:type,...".
 * A table's row: key = key space, then the key columns, each written so that keys compare as
 * bytes; value = every column packed as Core packs a series row, without the series number.
 * A synced table's row: key = (device, time, seq); value = the device's payload, unchanged.
 * kv: key = (device, key); value = the device's put payload (key length, key, value). */

/* Whole numbers: a length byte, then only the bytes the number needs, big-endian.
 * 0x88 + n for zero and up, 0x87 - n below zero, so the order is the numbers' order. */
static uint32_t asd_enc_int(uint8_t *p, int64_t v) {
    uint64_t u = v < 0 ? ~(uint64_t)v : (uint64_t)v;
    uint32_t n = 0, i;
    while (n < 8 && (u >> (8 * n)) != 0) n++;
    p[0] = (uint8_t)(v < 0 ? 0x87 - n : 0x88 + n);
    for (i = 0; i < n; i++) p[1 + i] = (uint8_t)((uint64_t)v >> (8 * (n - 1 - i)));
    return 1 + n;
}
static uint32_t asd_dec_int(const uint8_t *p, uint32_t avail, int64_t *v) {
    uint32_t n, i;
    uint64_t u;
    if (!avail) return 0;
    if (p[0] >= 0x88 && p[0] <= 0x90) { n = p[0] - 0x88u; u = 0; }
    else if (p[0] >= 0x7F && p[0] <= 0x87) { n = 0x87u - p[0]; u = ~(uint64_t)0; }
    else return 0;
    if (1 + n > avail) return 0;
    for (i = 0; i < n; i++) u = (u << 8) | p[1 + i];
    *v = (int64_t)u;
    return 1 + n;
}
/* Reals: the sign bit flipped for a positive number, every bit for a negative one. */
static uint32_t asd_enc_real(uint8_t *p, double d, int is_float) {
    int i;
    if (d == 0) d = 0;                                   /* -0 is stored as 0 */
    if (is_float) {
        float f = (float)d;
        uint32_t b;
        memcpy(&b, &f, 4);
        b = (b & 0x80000000u) ? ~b : (b | 0x80000000u);
        for (i = 0; i < 4; i++) p[i] = (uint8_t)(b >> (24 - 8 * i));
        return 4;
    } else {
        uint64_t b;
        memcpy(&b, &d, 8);
        b = (b >> 63) ? ~b : (b | ((uint64_t)1 << 63));
        for (i = 0; i < 8; i++) p[i] = (uint8_t)(b >> (56 - 8 * i));
        return 8;
    }
}
/* Text: 0x00 written as 0x00 0xFF, then 0x00 0x00 to end the column. */
static uint32_t asd_enc_text(uint8_t *p, const char *s, uint32_t n) {
    uint32_t i, o = 0;
    for (i = 0; i < n; i++) {
        p[o++] = (uint8_t)s[i];
        if (!s[i]) p[o++] = 0xFF;
    }
    p[o++] = 0;
    p[o++] = 0;
    return o;
}

/* A value converted to a column's type, as Core converts: whole numbers widen to reals. */
static int asd_coerce(altsql_db *db, int t, const altsql_value *in, altsql_value *out) {
    *out = *in;
    if (in->type == ALTSQL_NULL) return asd_err(db, ALTSQL_SCHEMA, "rows hold no NULL values");
    if (t == AS_T_TEXT) {
        if (in->type != ALTSQL_TEXT) return asd_err(db, ALTSQL_SCHEMA, "text column needs a text value");
        if (in->len < 0 || in->len > 255 || (in->len && !in->u.s)) return asd_err(db, ALTSQL_TOOBIG, "text value longer than 255 bytes");
        return ALTSQL_OK;
    }
    if (in->type == ALTSQL_TEXT) return asd_err(db, ALTSQL_SCHEMA, "numeric column needs a number");
    if (t == AS_T_FLOAT || t == AS_T_REAL) {
        out->type = ALTSQL_REAL;
        out->u.r = in->type == ALTSQL_INTEGER ? (double)in->u.i : in->u.r;
        if (out->u.r != out->u.r) return asd_err(db, ALTSQL_SCHEMA, "NaN is not stored");
        return ALTSQL_OK;
    }
    if (in->type == ALTSQL_REAL) {
        if (!(in->u.r > -9.2e18 && in->u.r < 9.2e18)) return asd_err(db, ALTSQL_SCHEMA, "value out of range for an integer column");
        out->u.i = (int64_t)in->u.r;
    }
    out->type = ALTSQL_INTEGER;
    if (t == AS_T_INT && (out->u.i < INT32_MIN || out->u.i > INT32_MAX))
        return asd_err(db, ALTSQL_SCHEMA, "value out of range for an int column (32-bit)");
    return ALTSQL_OK;
}

/* A coerced value written into a key; 0 when it does not fit in room. */
static uint32_t asd_kenc(int t, const altsql_value *v, uint8_t *p, uint32_t room) {
    if (t == AS_T_TEXT) return room >= 2u * (uint32_t)v->len + 2 ? asd_enc_text(p, v->u.s, (uint32_t)v->len) : 0;
    if (t == AS_T_FLOAT) return room >= 4 ? asd_enc_real(p, v->u.r, 1) : 0;
    if (t == AS_T_REAL) return room >= 8 ? asd_enc_real(p, v->u.r, 0) : 0;
    return room >= 9 ? asd_enc_int(p, v->u.i) : 0;
}

static uint32_t asd_rpack(const uint8_t *types, uint32_t n, const altsql_value *v, uint8_t *dst) {
    uint32_t i, off = 0;
    for (i = 0; i < n; i++) {
        switch (types[i]) {
        case AS_T_TIME: case AS_T_LONG: as_put64(dst + off, (uint64_t)v[i].u.i); off += 8; break;
        case AS_T_INT: as_put32(dst + off, (uint32_t)(int32_t)v[i].u.i); off += 4; break;
        case AS_T_FLOAT: { float f = (float)v[i].u.r; uint32_t b; memcpy(&b, &f, 4); as_put32(dst + off, b); off += 4; break; }
        case AS_T_REAL: { uint64_t b; memcpy(&b, &v[i].u.r, 8); as_put64(dst + off, b); off += 8; break; }
        default:
            dst[off++] = (uint8_t)v[i].len;
            if (v[i].len) memcpy(dst + off, v[i].u.s, (size_t)v[i].len);
            off += (uint32_t)v[i].len;
        }
    }
    return off;
}

/* Unpacks columns; text values point into p. -1 when the bytes run short. */
static int asd_runpack(const uint8_t *types, uint32_t n, const uint8_t *p, uint32_t len, altsql_value *v) {
    uint32_t off = 0, i;
    for (i = 0; i < n; i++) {
        uint8_t t = types[i];
        uint32_t w = (t == AS_T_INT || t == AS_T_FLOAT) ? 4u : (t == AS_T_TEXT ? 1u : 8u);
        if (off + w > len) return -1;
        v[i].len = 0;
        switch (t) {
        case AS_T_TIME: case AS_T_LONG: v[i].type = ALTSQL_INTEGER; v[i].u.i = (int64_t)as_get64(p + off); break;
        case AS_T_INT: v[i].type = ALTSQL_INTEGER; v[i].u.i = (int32_t)as_get32(p + off); break;
        case AS_T_FLOAT: { uint32_t b = as_get32(p + off); float f; memcpy(&f, &b, 4); v[i].type = ALTSQL_REAL; v[i].u.r = as_nice_float(f); break; }
        case AS_T_REAL: { uint64_t b = as_get64(p + off); v[i].type = ALTSQL_REAL; memcpy(&v[i].u.r, &b, 8); break; }
        default:
            w = 1u + p[off];
            if (off + w > len) return -1;
            v[i].type = ALTSQL_TEXT; v[i].u.s = (const char *)p + off + 1; v[i].len = p[off];
        }
        off += w;
    }
    return 0;
}

/* "name:type,..." into a table's columns. -1 on a syntax error or a repeated name. */
static int asd_cols_parse(const char *s, size_t n, struct asd_table *T, uint32_t first) {
    size_t i = 0;
    uint32_t col = first, j;
    while (i < n) {
        size_t a, an, b;
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
        t = as_type_of(s + b, i - b);
        if (!t || an > 31 || col >= ALTSQL_DB_MAXCOLS) return -1;
        for (j = 0; j < col; j++) if (as_ieq(T->cols[j], strlen(T->cols[j]), s + a, an)) return -1;
        memcpy(T->cols[col], s + a, an);
        T->cols[col][an] = 0;
        T->types[col] = (uint8_t)t;
        col++;
        while (i < n && s[i] == ' ') i++;
        if (i < n) { if (s[i] != ',') return -1; i++; }
    }
    if (col == first) return -1;
    T->ncols = (uint8_t)col;
    return 0;
}

static int asd_col_index(const struct asd_table *T, const char *name, size_t n) {
    int i;
    for (i = 0; i < T->ncols; i++) if (as_ieq(T->cols[i], strlen(T->cols[i]), name, n)) return i;
    return -1;
}

static uint32_t asd_catkey(uint8_t *k, int byname, const char *name, size_t n, uint32_t ks) {
    uint32_t kn = asd_ksput(k, ASD_KS_CAT);
    if (byname) { k[kn++] = 'n'; memcpy(k + kn, name, n); return kn + (uint32_t)n; }
    k[kn++] = 'k';
    return kn + asd_ksput(k + kn, ks);
}

/* Fills T from its catalog entry. */
static int asd_table_parse(altsql_db *db, const char *name, size_t nlen, const uint8_t *v, uint32_t vn, struct asd_table *T) {
    uint32_t i, nkey;
    if (vn < 8 || (v[0] != ASD_K_TABLE && v[0] != ASD_K_SYNCED)) return ASD_CORRUPT(db, "damaged catalog");
    memset(T, 0, sizeof *T);
    T->kind = v[0];
    T->ks = as_get32(v + 1);
    nkey = v[6];
    T->flags = v[7];
    if (nlen > 31 || !nkey || 8 + nkey > vn || nkey > ALTSQL_DB_MAXCOLS) return ASD_CORRUPT(db, "damaged catalog");
    memcpy(T->name, name, nlen);
    if (asd_cols_parse((const char *)v + 8 + nkey, vn - 8 - nkey, T, 0) || T->ncols != v[5])
        return ASD_CORRUPT(db, "damaged catalog");
    for (i = 0; i < nkey; i++) {
        if (v[8 + i] >= T->ncols) return ASD_CORRUPT(db, "damaged catalog");
        T->key[i] = v[8 + i];
    }
    T->nkey = (uint8_t)nkey;
    return ALTSQL_OK;
}

/* A table by name, from the cache or the catalog. Valid until the next table lookup. */
static int asd_table_get(altsql_db *db, const char *name, size_t nlen, struct asd_table **out) {
    uint8_t k[48];
    uint32_t i, kn, fi, ix;
    int rc;
    if (!nlen || nlen > 31) return asd_err(db, ALTSQL_SCHEMA, "no such table");
    for (i = 0; i < db->ntabs; i++)
        if (strlen(db->tabs[i].name) == nlen && !memcmp(db->tabs[i].name, name, nlen)) { *out = &db->tabs[i]; return ALTSQL_OK; }
    kn = asd_catkey(k, 1, name, nlen, 0);
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_NOTFOUND) return asd_err(db, ALTSQL_SCHEMA, "no such table");
    if (rc) return rc;
    {
        asd_cell c;
        size_t vn;
        asd_leaf_cell(db, ASD_PAGE(db, fi), ix, &c);
        rc = asd_value(db, ASD_PAGE(db, fi), ix, db->rowbuf, ASD_ROWBUF, &vn);
        asd_unpin(db, fi);
        if (rc) return rc;
        if (vn && db->rowbuf[0] == ASD_K_BUCKET) return asd_err(db, ALTSQL_MISUSE, "that name belongs to a bucket, not a table");
        i = db->ntabs < ASD_NTAB ? db->ntabs++ : (db->tpos++ % ASD_NTAB);
        if ((rc = asd_table_parse(db, name, nlen, db->rowbuf, (uint32_t)vn, &db->tabs[i])) != 0) { db->ntabs = 0; return rc; }
    }
    *out = &db->tabs[i];
    return ALTSQL_OK;
}

static int asd_table_byks(altsql_db *db, uint32_t ks, struct asd_table **out) {
    uint8_t k[16];
    uint32_t i, kn, fi, ix;
    size_t vn;
    char name[32];
    int rc;
    for (i = 0; i < db->ntabs; i++) if (db->tabs[i].ks == ks) { *out = &db->tabs[i]; return ALTSQL_OK; }
    kn = asd_catkey(k, 0, 0, 0, ks);
    if ((rc = asd_find(db, k, kn, &fi, &ix)) != 0) return rc == ALTSQL_NOTFOUND ? asd_err(db, ALTSQL_SCHEMA, "no such table") : rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, name, sizeof name - 1, &vn);
    asd_unpin(db, fi);
    if (rc || vn < 2) return rc ? rc : ASD_CORRUPT(db, "damaged catalog");
    return asd_table_get(db, name + 1, vn - 1, out);
}

/* Writes a new table's two catalog entries, in the open write transaction. */
static int asd_table_new(altsql_db *db, struct asd_table *T) {
    uint8_t k[48], *v = db->rowbuf;
    uint32_t kn, vn, i;
    int rc;
    if (db->nextks == ASD_NONE) return asd_err(db, ALTSQL_FULL, "no key space numbers left");
    T->ks = db->nextks++;
    db->mods++;
    v[0] = T->kind;
    as_put32(v + 1, T->ks);
    v[5] = T->ncols;
    v[6] = T->nkey;
    v[7] = T->flags;
    vn = 8;
    for (i = 0; i < T->nkey; i++) v[vn++] = T->key[i];
    for (i = 0; i < T->ncols; i++) {
        size_t cn = strlen(T->cols[i]);
        const char *tn = as_type_name(T->types[i]);
        if (i) v[vn++] = ',';
        memcpy(v + vn, T->cols[i], cn); vn += (uint32_t)cn;
        v[vn++] = ':';
        memcpy(v + vn, tn, strlen(tn)); vn += (uint32_t)strlen(tn);
    }
    kn = asd_catkey(k, 1, T->name, strlen(T->name), 0);
    if ((rc = asd_write(db, 1, k, kn, v, vn)) != 0) return rc;
    kn = asd_catkey(k, 0, 0, 0, T->ks);
    v[0] = T->kind;
    memcpy(v + 1, T->name, strlen(T->name));
    if ((rc = asd_write(db, 1, k, kn, v, 1 + (uint32_t)strlen(T->name))) != 0) return rc;
    db->ntabs = 0;
    return ALTSQL_OK;
}

static int asd_name_ok(const char *name, size_t n) {
    size_t i;
    if (!n || n > 31 || !as_is_ident_start((unsigned char)name[0])) return 0;
    for (i = 1; i < n; i++) if (!as_is_ident_char((unsigned char)name[i])) return 0;
    return 1;
}

int altsql_db_table_create(altsql_db *db, const char *name, const char *columns, const char *key) {
    struct asd_table T;
    size_t i = 0, n;
    uint8_t k[48];
    uint32_t kn, fi, ix;
    int rc, own = 0;
    if (!db || !name || !columns || !key) return ALTSQL_MISUSE;
    n = strlen(name);
    if (!asd_name_ok(name, n)) return asd_err(db, ALTSQL_SYNTAX, "a table name is a letter or _, then letters, digits or _, up to 31");
    memset(&T, 0, sizeof T);
    if (asd_cols_parse(columns, strlen(columns), &T, 0)) return asd_err(db, ALTSQL_SYNTAX, "columns must read name:type,... with types time, int, long, float, real or text");
    while (key[i]) {                                     /* the key's columns, by name */
        size_t a;
        int c;
        while (key[i] == ' ' || key[i] == ',') i++;
        a = i;
        while (key[i] && key[i] != ',' && key[i] != ' ') i++;
        if (i == a) break;
        c = asd_col_index(&T, key + a, i - a);
        if (c < 0 || T.nkey >= ALTSQL_DB_MAXCOLS || memchr(T.key, c, T.nkey)) return asd_err(db, ALTSQL_SCHEMA, "key columns must be columns of the table, each once");
        T.key[T.nkey++] = (uint8_t)c;
    }
    if (!T.nkey) return asd_err(db, ALTSQL_SCHEMA, "a table needs a primary key");
    memcpy(T.name, name, n);
    T.kind = ASD_K_TABLE;
    if ((rc = asd_ready(db)) != 0) return rc;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot create a table");
    kn = asd_catkey(k, 1, name, n, 0);
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_OK) { asd_unpin(db, fi); return asd_err(db, ALTSQL_EXISTS, "that name is taken"); }
    if (rc != ALTSQL_NOTFOUND) return rc;
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    rc = asd_table_new(db, &T);
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    else if (rc) db->failed = 1;
    return rc;
}

int altsql_db_table_info(altsql_db *db, const char *table, altsql_db_tableinfo *o) {
    struct asd_table *T;
    int rc, i;
    if (!db || !table || !o) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    memset(o, 0, sizeof *o);
    o->kind = T->kind;
    o->ncols = T->ncols;
    o->nkey = T->nkey;
    for (i = 0; i < T->ncols; i++) { o->types[i] = T->types[i]; o->names[i] = T->cols[i]; }
    for (i = 0; i < T->nkey; i++) o->key[i] = T->key[i];
    return ALTSQL_OK;
}

/* The tree key of a row: key space, then nkey key values (coerced to their columns' types). */
static int asd_rowkey(altsql_db *db, const struct asd_table *T, const altsql_value *key, int nkey,
                      uint8_t *out, uint32_t room, uint32_t *on) {
    uint32_t n = asd_ksput(out, T->ks), w;
    int i, rc;
    for (i = 0; i < nkey; i++) {
        altsql_value v;
        int t = T->types[T->key[i]];
        if ((rc = asd_coerce(db, t, &key[i], &v)) != 0) return rc;
        if (!(w = asd_kenc(t, &v, out + n, room - n))) return asd_err(db, ALTSQL_TOOBIG, "key too long");
        n += w;
    }
    if (n > db->maxkey) return asd_err(db, ALTSQL_TOOBIG, "key too long");
    *on = n;
    return ALTSQL_OK;
}

/* A row's columns from its tree key and its value (in db->rowbuf). */
static int asd_rowdecode(altsql_db *db, const struct asd_table *T, const uint8_t *tk, uint32_t tkn,
                         const uint8_t *val, uint32_t vn, altsql_value *cols) {
    uint32_t off;
    int64_t x;
    uint32_t w;
    if (T->kind == ASD_K_TABLE)
        return asd_runpack(T->types, T->ncols, val, vn, cols) ? ASD_CORRUPT(db, "damaged row") : ALTSQL_OK;
    { uint8_t tmp[8]; off = asd_ksput(tmp, T->ks); }
    if (!(w = asd_dec_int(tk + off, tkn - off, &x))) return ASD_CORRUPT(db, "damaged row key");
    cols[0].type = ALTSQL_INTEGER; cols[0].len = 0; cols[0].u.i = x;
    off += w;
    if (T->flags & ASD_F_KV) {
        if (vn < 1 || 1u + val[0] > vn) return ASD_CORRUPT(db, "damaged row");
        cols[1].type = ALTSQL_TEXT; cols[1].u.s = (const char *)val + 1; cols[1].len = val[0];
        cols[2].type = ALTSQL_TEXT; cols[2].u.s = (const char *)val + 1 + val[0]; cols[2].len = (int)(vn - 1 - val[0]);
        return ALTSQL_OK;
    }
    if (!(w = asd_dec_int(tk + off, tkn - off, &x))) return ASD_CORRUPT(db, "damaged row key");   /* time */
    off += w;
    if (!asd_dec_int(tk + off, tkn - off, &x)) return ASD_CORRUPT(db, "damaged row key");          /* seq */
    cols[1].type = ALTSQL_INTEGER; cols[1].len = 0; cols[1].u.i = x;
    if (vn < 2 || asd_runpack(T->types + 2, T->ncols - 2u, val + 2, vn - 2, cols + 2)) return ASD_CORRUPT(db, "damaged row");
    return ALTSQL_OK;
}

/* The hidden part of a key for tables without a primary key: the transaction's number and a
 * count within it, so rows keep the order they arrived in. */
static uint32_t asd_rowid(altsql_db *db, uint8_t *p) {
    return asd_enc_int(p, (int64_t)((db->cur << 24) | (db->rowctr++ & 0xFFFFFFu)));
}

/* ---- One way to write a row -----------------------------------------------------------------
 * Every change to a table's rows goes through these two functions, whichever interface asks for
 * it: the row calls, SQL's INSERT, UPDATE and DELETE, DROP TABLE and sync. asd_row_check does all
 * that can refuse a row (the values in their columns' types, the key, and for INSERT the check
 * that no row has the key yet) and writes nothing. asd_row_write writes the row, or deletes it.
 * So a refused row changes nothing, whichever interface sent it, and both interfaces keep the
 * same rules. Secondary indexes, when they come, are kept in asd_row_write, once. */
enum { ASD_OP_INSERT = 1, ASD_OP_REPLACE, ASD_OP_UPDATE, ASD_OP_DELETE, ASD_OP_SYNC };

static int asd_row_check(altsql_db *db, const struct asd_table *T, int op, const altsql_value *row,
                         altsql_value *v, uint8_t *tk, uint32_t room, uint32_t *tkn, const char *dup) {
    altsql_value key[ALTSQL_DB_MAXCOLS];
    uint32_t fi, ix;
    int rc, i;
    for (i = 0; i < T->ncols; i++) if ((rc = asd_coerce(db, T->types[i], &row[i], &v[i])) != 0) return rc;
    for (i = 0; i < T->nkey; i++) key[i] = v[T->key[i]];
    if ((rc = asd_rowkey(db, T, key, T->nkey, tk, room, tkn)) != 0) return rc;
    if (op == ASD_OP_INSERT && !(T->flags & ASD_F_ROWID)) {
        rc = asd_find(db, tk, *tkn, &fi, &ix);
        if (rc == ALTSQL_OK) { asd_unpin(db, fi); return asd_err(db, ALTSQL_EXISTS, dup ? dup : "a row with that key is already there"); }
        if (rc != ALTSQL_NOTFOUND) return rc;
    }
    return ALTSQL_OK;
}

/* v: the row's values, packed into buf; or pv and pvn, the row as stored already (sync keeps a
 * device's payload as it was sent; T is NULL there). A new row of a table without a primary key
 * gets its hidden number here, so tk needs room for it. */
static int asd_row_write(altsql_db *db, const struct asd_table *T, int op, uint8_t *tk, uint32_t tkn,
                         const altsql_value *v, uint8_t *buf, const uint8_t *pv, uint32_t pvn) {
    if (op == ASD_OP_DELETE) return asd_write(db, 0, tk, tkn, 0, 0);
    if (T && (op == ASD_OP_INSERT || op == ASD_OP_REPLACE) && (T->flags & ASD_F_ROWID)) tkn += asd_rowid(db, tk + tkn);
    if (v) { pvn = asd_rpack(T->types, T->ncols, v, buf); pv = buf; }
    return asd_write(db, 1, tk, tkn, pv, pvn);
}

/* row_put and row_insert. */
static int asd_row_call(altsql_db *db, const char *table, const altsql_value *cols, int ncols, int op) {
    struct asd_table *T;
    altsql_value v[ALTSQL_DB_MAXCOLS];
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn;
    int rc, own = 0;
    if (!db || !table || (!cols && ncols)) return ALTSQL_MISUSE;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    if (T->kind != ASD_K_TABLE) return asd_err(db, ALTSQL_MISUSE, "a synced table takes rows only from sync");
    if (ncols != T->ncols) return asd_err(db, ALTSQL_SCHEMA, "wrong number of columns");
    if ((rc = asd_row_check(db, T, op, cols, v, tk, sizeof tk, &tkn, NULL)) != 0) return rc;
    if (!(T->flags & ASD_F_ROWID)) return asd_row_write(db, T, op, tk, tkn, v, db->rowbuf, NULL, 0);
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }   /* the hidden number is the transaction's */
    rc = asd_row_write(db, T, op, tk, tkn, v, db->rowbuf, NULL, 0);
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    return rc;
}

int altsql_db_row_put(altsql_db *db, const char *table, const altsql_value *cols, int ncols) {
    return asd_row_call(db, table, cols, ncols, ASD_OP_REPLACE);
}

int altsql_db_row_insert(altsql_db *db, const char *table, const altsql_value *cols, int ncols) {
    return asd_row_call(db, table, cols, ncols, ASD_OP_INSERT);
}

int altsql_db_row_get(altsql_db *db, const char *table, const altsql_value *key, int nkey,
                      altsql_value *cols, int ncols) {
    struct asd_table *T;
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn, fi, ix;
    size_t vn;
    int rc;
    if (!db || !table || !key || !cols) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    if (T->flags & ASD_F_ROWID) return asd_err(db, ALTSQL_MISUSE, "a table without a primary key is read with row_seek");
    if (nkey != T->nkey) return asd_err(db, ALTSQL_SCHEMA, "give every key column");
    if (ncols < T->ncols) return asd_err(db, ALTSQL_MISUSE, "cols has fewer slots than the table has columns");
    if ((rc = asd_rowkey(db, T, key, nkey, tk, sizeof tk, &tkn)) != 0) return rc;
    if ((rc = asd_find(db, tk, tkn, &fi, &ix)) != 0) return rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, db->rowbuf, ASD_ROWBUF, &vn);
    asd_unpin(db, fi);
    if (rc) return rc == ALTSQL_DB_SHORT ? ASD_CORRUPT(db, "row too large") : rc;
    return asd_rowdecode(db, T, tk, tkn, db->rowbuf, (uint32_t)vn, cols);
}

int altsql_db_row_del(altsql_db *db, const char *table, const altsql_value *key, int nkey) {
    struct asd_table *T;
    uint8_t tk[ALTSQL_DB_MAXKEY + 8];
    uint32_t tkn;
    int rc;
    if (!db || !table || !key) return ALTSQL_MISUSE;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    if (T->flags & ASD_F_ROWID) return asd_err(db, ALTSQL_MISUSE, "a table without a primary key is changed with SQL");
    if (nkey != T->nkey) return asd_err(db, ALTSQL_SCHEMA, "give every key column");
    if ((rc = asd_rowkey(db, T, key, nkey, tk, sizeof tk, &tkn)) != 0) return rc;
    return asd_row_write(db, T, ASD_OP_DELETE, tk, tkn, NULL, NULL, NULL, 0);
}

static int asd_row_start(altsql_db_cursor *c, altsql_db *db, const char *table,
                         const altsql_value *prefix, int nprefix) {
    struct asd_table *T;
    uint32_t n;
    int rc;
    if (!c || !db || !table || (!prefix && nprefix)) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    if (nprefix < 0 || nprefix > T->nkey) return asd_err(db, ALTSQL_SCHEMA, "more prefix values than key columns");
    c->db = db;
    c->space = T->ks;
    c->state = 0;
    c->fi = ASD_NONE;
    if ((rc = asd_rowkey(db, T, prefix, nprefix, c->pre, sizeof c->pre, &n)) != 0) return rc;
    c->plen = (uint16_t)n;
    memcpy(c->key, c->pre, n);
    return ALTSQL_OK;
}

int altsql_db_row_last(altsql_db_cursor *c, altsql_db *db, const char *table,
                       const altsql_value *prefix, int nprefix) {
    int rc = asd_row_start(c, db, table, prefix, nprefix);
    if (rc) return rc;
    return asd_cland(c, asd_clast(c), 3);
}

int altsql_db_row_seek(altsql_db_cursor *c, altsql_db *db, const char *table,
                       const altsql_value *prefix, int nprefix) {
    struct asd_table *T;
    uint32_t n;
    int rc, found;
    if (!c || !db || !table || (!prefix && nprefix)) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &T)) != 0) return rc;
    if (nprefix < 0 || nprefix > T->nkey) return asd_err(db, ALTSQL_SCHEMA, "more prefix values than key columns");
    c->db = db;
    c->space = T->ks;
    c->state = 0;
    c->fi = ASD_NONE;
    if ((rc = asd_rowkey(db, T, prefix, nprefix, c->pre, sizeof c->pre, &n)) != 0) return rc;
    c->plen = (uint16_t)n;
    memcpy(c->key, c->pre, n);
    return asd_cland(c, asd_cseek(c, c->key, n, &found), 2);
}

int altsql_db_row_read(altsql_db_cursor *c, altsql_value *cols, int ncols) {
    struct asd_table *T;
    altsql_db *db;
    size_t vn;
    int rc;
    if (!c || !c->db || !cols) return ALTSQL_MISUSE;
    if (c->state != 1) return ALTSQL_NOTFOUND;
    db = c->db;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_byks(db, c->space, &T)) != 0) return rc;
    if (ncols < T->ncols) return asd_err(db, ALTSQL_MISUSE, "cols has fewer slots than the table has columns");
    if ((rc = altsql_db_value(c, db->rowbuf, ASD_ROWBUF, &vn)) != 0) return rc == ALTSQL_DB_SHORT ? ASD_CORRUPT(db, "row too large") : rc;
    return asd_rowdecode(db, T, c->key, c->klen, db->rowbuf, (uint32_t)vn, cols);
}

int altsql_db_tables(altsql_db *db, int (*cb)(void *ctx, const char *name, int kind), void *ctx) {
    altsql_db_cursor c;
    uint8_t v[40];
    uint32_t n;
    size_t vn;
    int rc, found;
    if (!db || !cb) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0) return rc;
    c.db = db; c.space = ASD_KS_CAT; c.state = 0; c.fi = ASD_NONE;
    n = asd_ksput(c.pre, ASD_KS_CAT);
    c.pre[n++] = 'k';                                    /* the entries by key space: kind, then name */
    c.plen = (uint16_t)n;
    memcpy(c.key, c.pre, n);
    rc = asd_cland(&c, asd_cseek(&c, c.key, n, &found), 2);
    while (rc == ALTSQL_OK) {
        char name[32];
        if ((rc = altsql_db_value(&c, v, sizeof v, &vn)) != 0) return rc == ALTSQL_DB_SHORT ? ASD_CORRUPT(db, "damaged catalog") : rc;
        if (vn >= 2 && vn <= 32 && (v[0] == ASD_K_TABLE || v[0] == ASD_K_SYNCED)) {
            memcpy(name, v + 1, vn - 1);
            name[vn - 1] = 0;
            if ((rc = cb(ctx, name, v[0] == ASD_K_TABLE ? ALTSQL_DB_TABLE : ALTSQL_DB_SYNCED)) != 0) return rc;
        }
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND ? ALTSQL_OK : rc;
}

/* A table's rows, each through asd_row_write, then its two catalog entries, in the open write
 * transaction: DROP TABLE and altsql_db_table_drop both come here. */
static int asd_table_drop_rows(altsql_db *db, const struct asd_table *T, uint64_t *count) {
    altsql_db_cursor c;
    uint8_t k[ALTSQL_DB_MAXKEY + 8];
    uint32_t kn;
    int rc;
    *count = 0;
    for (;;) {                                           /* the first row, again and again */
        rc = altsql_db_row_seek(&c, db, T->name, NULL, 0);
        if (rc == ALTSQL_NOTFOUND) break;
        if (rc) return rc;
        memcpy(k, c.key, c.klen);                       /* the whole tree key, key space included */
        if ((rc = asd_row_write(db, T, ASD_OP_DELETE, k, c.klen, NULL, NULL, NULL, 0)) != 0)
            return rc == ALTSQL_NOTFOUND ? ASD_CORRUPT(db, "a row the cursor found could not be deleted") : rc;
        (*count)++;
    }
    kn = asd_catkey(k, 1, T->name, strlen(T->name), 0);
    if ((rc = asd_write(db, 0, k, kn, 0, 0)) != 0) return rc;
    kn = asd_catkey(k, 0, 0, 0, T->ks);
    rc = asd_write(db, 0, k, kn, 0, 0);
    db->ntabs = 0;
    return rc;
}

int altsql_db_table_drop(altsql_db *db, const char *table) {
    struct asd_table *TP, T;
    uint64_t count, before;
    int rc, own = 0;
    if (!db || !table) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0 || (rc = asd_table_get(db, table, strlen(table), &TP)) != 0) return rc;
    if (TP->kind != ASD_K_TABLE) return asd_err(db, ALTSQL_MISUSE, "a synced table stays while devices send to it: DELETE its rows");
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    T = *TP;
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    before = db->mods;
    rc = asd_table_drop_rows(db, &T, &count);
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    else if (rc && db->mods != before) db->failed = 1;
    return rc;
}

/* ---- Sync from AltSql Core devices ---------------------------------------------------------
 * Key space 2: device, 's' -> the last sequence number applied (LE32);
 *              device, 'm', series id (BE16) -> the key space of its table (LE32). */
static uint32_t asd_skey(uint8_t *k, int64_t device, uint8_t what) {
    uint32_t n = asd_ksput(k, ASD_KS_SYNC);
    n += asd_enc_int(k + n, device);
    k[n++] = what;
    return n;
}

int altsql_db_sync_state(altsql_db *db, int64_t device, uint32_t *last_seq) {
    uint8_t k[16], v[8];
    uint32_t kn, fi, ix;
    size_t vn;
    int rc;
    if (!db || !last_seq) return ALTSQL_MISUSE;
    *last_seq = 0;
    if ((rc = asd_ready(db)) != 0) return rc;
    kn = asd_skey(k, device, 's');
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_NOTFOUND) return ALTSQL_OK;
    if (rc) return rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, v, sizeof v, &vn);
    asd_unpin(db, fi);
    if (rc || vn != 4) return rc ? rc : ASD_CORRUPT(db, "damaged sync state");
    *last_seq = as_get32(v);
    return ALTSQL_OK;
}

/* The table for a device's series, from the cache or the sync state. */
static int asd_sync_map(altsql_db *db, int64_t device, uint32_t sid, uint32_t *ks) {
    uint8_t k[24], v[8];
    uint32_t i, kn, fi, ix;
    size_t vn;
    int rc;
    for (i = 0; i < db->nmaps; i++)
        if (db->maps[i].dev == device && db->maps[i].sid == sid) { *ks = db->maps[i].ks; return ALTSQL_OK; }
    kn = asd_skey(k, device, 'm');
    k[kn++] = (uint8_t)(sid >> 8);
    k[kn++] = (uint8_t)sid;
    if ((rc = asd_find(db, k, kn, &fi, &ix)) != 0) return rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, v, sizeof v, &vn);
    asd_unpin(db, fi);
    if (rc || vn != 4) return rc ? rc : ASD_CORRUPT(db, "damaged sync state");
    *ks = as_get32(v);
    i = db->nmaps < 8 ? db->nmaps++ : (db->mpos++ & 7);
    db->maps[i].dev = device; db->maps[i].sid = sid; db->maps[i].ks = *ks;
    return ALTSQL_OK;
}

/* A series definition from a device: the table of that name, made by the first device. */
static int asd_sync_series(altsql_db *db, int64_t device, const uint8_t *name, uint32_t nlen, uint32_t sid,
                           const uint8_t *text, uint32_t tlen) {
    struct asd_table S, *T;
    uint8_t k[24], v[4];
    uint32_t kn, i;
    int rc;
    char msg[96];
    memset(&S, 0, sizeof S);
    memcpy(S.cols[0], "device", 7); S.types[0] = AS_T_LONG;
    memcpy(S.cols[1], "seq", 4); S.types[1] = AS_T_LONG;
    if (nlen > 31 || asd_cols_parse((const char *)text, tlen, &S, 2) || S.types[2] != AS_T_TIME) {
        snprintf(msg, sizeof msg, "series %.*s: its columns clash with device and seq, or cannot be read", (int)(nlen > 31 ? 31 : nlen), (const char *)name);
        return asd_err(db, ALTSQL_SCHEMA, msg);
    }
    memcpy(S.name, name, nlen);
    S.kind = ASD_K_SYNCED;
    S.nkey = 3; S.key[0] = 0; S.key[1] = 2; S.key[2] = 1;           /* (device, time, seq) */
    rc = asd_table_get(db, S.name, nlen, &T);
    if (rc == ALTSQL_SCHEMA) {
        if ((rc = asd_table_new(db, &S)) != 0) return rc;
    } else if (rc) return rc;
    else {
        int same = T->kind == ASD_K_SYNCED && !(T->flags & ASD_F_KV) && T->ncols == S.ncols;
        for (i = 0; same && i < S.ncols; i++)
            same = T->types[i] == S.types[i] && as_ieq(T->cols[i], strlen(T->cols[i]), S.cols[i], strlen(S.cols[i]));
        if (!same) {
            snprintf(msg, sizeof msg, "series %s: this device's layout differs from the table's", S.name);
            return asd_err(db, ALTSQL_SCHEMA, msg);
        }
        S.ks = T->ks;
    }
    kn = asd_skey(k, device, 'm');
    k[kn++] = (uint8_t)(sid >> 8);
    k[kn++] = (uint8_t)sid;
    as_put32(v, S.ks);
    if ((rc = asd_write(db, 1, k, kn, v, 4)) != 0) return rc;
    db->nmaps = 0;
    return ALTSQL_OK;
}

static int asd_sync_kvtable(altsql_db *db, uint32_t *ks) {
    struct asd_table S, *T;
    int rc = asd_table_get(db, "kv", 2, &T);
    if (rc == ALTSQL_OK) {
        if (!(T->flags & ASD_F_KV)) return asd_err(db, ALTSQL_SCHEMA, "a table named kv is in the way of the devices' key-value pairs");
        *ks = T->ks;
        return ALTSQL_OK;
    }
    if (rc != ALTSQL_SCHEMA) return rc;
    memset(&S, 0, sizeof S);
    memcpy(S.name, "kv", 3);
    S.kind = ASD_K_SYNCED;
    S.flags = ASD_F_KV;
    S.ncols = 3;
    memcpy(S.cols[0], "device", 7); S.types[0] = AS_T_LONG;
    memcpy(S.cols[1], "key", 4); S.types[1] = AS_T_TEXT;
    memcpy(S.cols[2], "value", 6); S.types[2] = AS_T_TEXT;
    S.nkey = 2; S.key[0] = 0; S.key[1] = 1;
    if ((rc = asd_table_new(db, &S)) != 0) return rc;
    *ks = S.ks;
    return ALTSQL_OK;
}

static int asd_sync_rec(altsql_db *db, int64_t device, uint8_t type, uint32_t seq, const uint8_t *pl, uint32_t plen) {
    uint8_t k[ALTSQL_DB_MAXKEY + 8];
    uint32_t kn, ks, klen;
    int rc;
    if (type == AS_R_ROW) {
        uint32_t sid = as_get16(pl);
        rc = asd_sync_map(db, device, sid, &ks);
        if (rc == ALTSQL_NOTFOUND) return asd_err(db, ALTSQL_SCHEMA, "a row of a series this gateway has not seen defined");
        if (rc) return rc;
        kn = asd_ksput(k, ks);
        kn += asd_enc_int(k + kn, device);
        kn += asd_enc_int(k + kn, (int64_t)as_get64(pl + 2));
        kn += asd_enc_int(k + kn, (int64_t)seq);
        return asd_row_write(db, NULL, ASD_OP_SYNC, k, kn, NULL, NULL, pl, plen);
    }
    klen = pl[0];
    if (pl[1] == 0x01) {                                 /* reserved keys */
        if (type == AS_R_PUT && klen >= 2 && pl[2] == 'S')
            return asd_sync_series(db, device, pl + 3, klen - 2, as_get16(pl + 1 + klen),
                                   pl + 1 + klen + 2, plen - 1 - klen - 2);
        return ALTSQL_OK;                                /* other reserved keys stay on the device */
    }
    if ((rc = asd_sync_kvtable(db, &ks)) != 0) return rc;
    kn = asd_ksput(k, ks);
    kn += asd_enc_int(k + kn, device);
    kn += asd_enc_text(k + kn, (const char *)pl + 1, klen);
    if (kn > db->maxkey) return asd_err(db, ALTSQL_TOOBIG, "device key too long for this page size");
    if (type == AS_R_PUT) return asd_row_write(db, NULL, ASD_OP_SYNC, k, kn, NULL, NULL, pl, plen);
    rc = asd_row_write(db, NULL, ASD_OP_DELETE, k, kn, NULL, NULL, NULL, 0);
    return rc == ALTSQL_NOTFOUND ? ALTSQL_OK : rc;
}

int altsql_db_sync_apply(altsql_db *db, int64_t device, uint32_t after_seq,
                         const void *batch, size_t len, uint32_t *last_seq) {
    const uint8_t *p = (const uint8_t *)batch;
    uint8_t k[16], v[4];
    uint32_t kn, last, start;
    size_t off = 0;
    int rc, own = 0, damaged = 0;
    if (!db || (!batch && len)) return ALTSQL_MISUSE;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot apply a batch");
    if ((rc = asd_ready(db)) != 0) return rc;
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    if ((rc = altsql_db_sync_state(db, device, &last)) != 0) goto fail;
    if (after_seq > last) {                    /* records between last and after_seq are missing */
        if (own) altsql_db_rollback(db);       /* nothing was written */
        if (last_seq) *last_seq = last;
        return asd_err(db, ALTSQL_DB_GAP, "the batch starts past the gateway's position: send again from *last_seq");
    }
    start = last;
    while (off < len) {
        const uint8_t *h = p + off;
        uint32_t plen, seq;
        if (len - off < AS_RH || h[0] != AS_MARK) { damaged = 1; break; }
        plen = as_get16(h + 2);
        seq = as_get32(h + 4);
        if (len - off - AS_RH < plen || as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, plen) != as_get32(h + 8) ||
            !as_payload_ok(h[1], h + AS_RH, plen)) { damaged = 1; break; }
        if (seq > last) {
            if ((rc = asd_sync_rec(db, device, h[1], seq, h + AS_RH, plen)) != 0) goto fail;
            last = seq;
        }
        off += AS_RH + plen;
    }
    if (last != start) {
        kn = asd_skey(k, device, 's');
        as_put32(v, last);
        if ((rc = asd_write(db, 1, k, kn, v, 4)) != 0) goto fail;
    }
    if (own && (rc = altsql_db_commit(db)) != 0) return rc;
    if (last_seq) *last_seq = last;
    return damaged ? asd_err(db, ALTSQL_CORRUPT, "the batch is damaged: the records before the damage were kept") : ALTSQL_OK;
fail:
    if (own) {
        char keep[96];
        memcpy(keep, db->err, sizeof keep);
        altsql_db_rollback(db);
        memcpy(db->err, keep, sizeof keep);
    } else db->failed = 1;
    return rc;
}

/* ---- SQL on the tree -----------------------------------------------------------------------
 * Core's own parser and query engine (its as_q and as_expr), over the tree's tables. Only the
 * row source is new: each SELECT, UPDATE and DELETE reads its table through one plan, chosen
 * from WHERE:
 *   point lookup     every key column fixed by =
 *   key list         the first key columns fixed by =, then a list on the next one (IN, or =
 *                    joined by OR): one walk per value, in key order
 *   range scan       the first key columns fixed by =, then a range on the next one
 *   range per device a synced table, a range on time, no device given: one range per device
 *   full scan        anything else
 * WHERE is still checked on every row a plan reads, so a plan only narrows the reading. */
#if ALTSQL_ENABLE_SQL
enum { ASD_P_POINT = 1, ASD_P_RANGE, ASD_P_DEVICES, ASD_P_FULL, ASD_P_LIST };
#define ASD_MAXLIST 64

typedef struct asd_bound { int eq, lo, hi; altsql_value veq, vlo, vhi; } asd_bound;

typedef struct asd_sq {
    as_q q;                                /* Core's query: items, WHERE, groups, order, output */
    struct asd_table T;                    /* the table, copied for the statement            */
    altsql_db *db;
    int plan, neq;
    asd_bound b[ALTSQL_DB_MAXCOLS];        /* what WHERE says of each column                 */
    int lcol, nlist;                       /* a key list: its column and values              */
    altsql_value *list;
    int (*act)(struct asd_sq *s, altsql_value *row);   /* UPDATE, DELETE: each row WHERE keeps */
    void *actx;
    const uint8_t *ck, *resume;            /* the row's key; a prepared SELECT goes on after  */
    uint32_t ckn, rn;                      /* the resume key                                  */
} asd_sq;

/* A constant from WHERE, as a key column's value; 0 when it cannot narrow the reading. */
static int asd_plan_value(asd_sq *s, int col, const as_expr *e, altsql_value *out) {
    as_ctx c;
    altsql_value v;
    int t = s->T.types[col];
    if (!as_is_const(e)) return 0;
    c.row = NULL; c.acc = NULL; c.tmp = &s->q.tmp; c.perm = s->q.A;
    if (as_eval(&c, e, &v) || v.type == ALTSQL_NULL) return 0;
    if ((t == AS_T_TEXT) != (v.type == ALTSQL_TEXT)) return 0;
    if (v.type == ALTSQL_REAL && (v.u.r != v.u.r || ((t != AS_T_FLOAT && t != AS_T_REAL) && !(v.u.r > -9.0e18 && v.u.r < 9.0e18)))) return 0;
    if (t == AS_T_INT && v.type == ALTSQL_INTEGER && (v.u.i < INT32_MIN || v.u.i > INT32_MAX)) return 0;
    if (v.type == ALTSQL_TEXT && (v.len < 0 || v.len > 255)) return 0;
    return asd_coerce(NULL, t, &v, out) == ALTSQL_OK;
}

static void asd_plan_note(asd_sq *s, int col, int op, const as_expr *lit) {
    asd_bound *b = &s->b[col];
    altsql_value v;
    if (!asd_plan_value(s, col, lit, &v)) return;
    if (op == '=') { b->eq = 1; b->veq = v; }
    else if (op == '>' || op == OP_GE) { b->lo = 1; b->vlo = v; }
    else if (op == '<' || op == OP_LE) { b->hi = 1; b->vhi = v; }
}

/* = joined by OR, every term on one column: the values, for a key list. */
static int asd_plan_or(asd_sq *s, const as_expr *e, int *col, altsql_value *vals, int *n) {
    const as_expr *c, *l;
    if (e->k == E_OR) return asd_plan_or(s, e->a, col, vals, n) && asd_plan_or(s, e->b, col, vals, n);
    if (e->k != E_BIN || e->op != '=' || *n >= ASD_MAXLIST) return 0;
    c = e->a->k == E_COL ? e->a : e->b->k == E_COL ? e->b : NULL;
    if (!c) return 0;
    l = c == e->a ? e->b : e->a;
    if (l->k == E_COL || (*col >= 0 && c->idx != *col)) return 0;
    *col = c->idx;
    return asd_plan_value(s, c->idx, l, &vals[(*n)++]);
}

static void asd_plan_where(asd_sq *s, const as_expr *e) {
    if (!e) return;
    if (e->k == E_AND) { asd_plan_where(s, e->a); asd_plan_where(s, e->b); return; }
    if (e->k == E_OR && !s->list) {
        altsql_value vals[ASD_MAXLIST];
        int col = -1, n = 0, i;
        if (!asd_plan_or(s, e, &col, vals, &n) || col < 0) return;
        if (!(s->list = (altsql_value *)as_alloc(s->q.A, sizeof *vals * (size_t)n))) return;
        for (i = 0; i < n; i++) {                       /* text kept apart from the scratch memory */
            s->list[i] = vals[i];
            if (vals[i].type == ALTSQL_TEXT && vals[i].len > 0) {
                char *t = (char *)as_alloc(s->q.A, (size_t)vals[i].len);
                if (!t) { s->list = NULL; return; }
                memcpy(t, vals[i].u.s, (size_t)vals[i].len);
                s->list[i].u.s = t;
            }
        }
        s->lcol = col;
        s->nlist = n;
        return;
    }
    if (e->k == E_BETWEEN && !e->notf && e->a->k == E_COL) {
        asd_plan_note(s, e->a->idx, OP_GE, e->b);
        asd_plan_note(s, e->a->idx, OP_LE, e->c);
    } else if (e->k == E_BIN) {
        int op = e->op;
        if (e->a->k == E_COL && e->b->k != E_COL) asd_plan_note(s, e->a->idx, op, e->b);
        else if (e->b->k == E_COL && e->a->k != E_COL) {
            op = op == '<' ? '>' : op == '>' ? '<' : op == OP_LE ? OP_GE : op == OP_GE ? OP_LE : op;
            asd_plan_note(s, e->b->idx, op, e->a);
        }
    }
}

static void asd_plan_pick(asd_sq *s) {
    const struct asd_table *T = &s->T;
    int i, rowid = (T->flags & ASD_F_ROWID) != 0, next;
    s->neq = 0;
    for (i = 0; i < T->nkey && s->b[T->key[i]].eq; i++) s->neq++;
    next = s->neq < T->nkey ? T->key[s->neq] : -1;
    if (s->neq == T->nkey && !rowid) s->plan = ASD_P_POINT;
    else if (s->list && next >= 0 && s->lcol == next) s->plan = ASD_P_LIST;
    else if (s->neq > 0 || (next >= 0 && (s->b[next].lo || s->b[next].hi))) s->plan = ASD_P_RANGE;
    else if (T->kind == ASD_K_SYNCED && !(T->flags & ASD_F_KV) && (s->b[2].lo || s->b[2].hi || s->b[2].eq)) s->plan = ASD_P_DEVICES;
    else s->plan = ASD_P_FULL;
}

/* A row the plan read: to Core's query, or for UPDATE and DELETE checked by WHERE and acted on. */
static int asd_sq_emit(asd_sq *s, altsql_value *row) {
    as_ctx c;
    altsql_value w;
    int rc;
    s->db->sqlrows++;
    if (!s->act) return as_q_row(&s->q, row);
    s->q.tmp.used = 0;
    if (s->q.where) {
        c.row = row; c.acc = NULL; c.tmp = &s->q.tmp; c.perm = s->q.A;
        if ((rc = as_eval(&c, s->q.where, &w)) != 0) return rc;
        if (!as_truth(&w)) return ALTSQL_OK;
    }
    return s->act(s, row);
}

/* Every row from start up to end (end compared on its own length; NULL: the prefix's end). */
static int asd_sq_range(asd_sq *s, const uint8_t *pre, uint32_t plen, const uint8_t *start, uint32_t sn,
                        const uint8_t *end, uint32_t en) {
    altsql_db *db = s->db;
    altsql_db_cursor c;
    altsql_value row[ALTSQL_DB_MAXCOLS];
    size_t vn;
    int rc, found, past = 0;
    c.db = db; c.space = s->T.ks; c.state = 0; c.fi = ASD_NONE;
    memcpy(c.pre, pre, plen);
    c.plen = (uint16_t)plen;
    memcpy(c.key, start, sn);
    if (s->resume && asd_cmp(s->resume, s->rn, start, sn) >= 0) {   /* rows up to the resume key were given */
        memcpy(c.key, s->resume, s->rn);
        sn = s->rn;
        past = 1;
    }
    rc = asd_cland(&c, asd_cseek(&c, c.key, sn, &found), 2);
    if (rc == ALTSQL_OK && past && found) rc = altsql_db_next(&c);
    while (rc == ALTSQL_OK && !s->q.stop) {
        if (end) {
            uint32_t n = c.klen < en ? c.klen : en;
            int r = memcmp(c.key, end, n);
            if (r > 0) break;
        }
        if ((rc = altsql_db_value(&c, db->rowbuf, ASD_ROWBUF, &vn)) != 0) return rc;
        if ((rc = asd_rowdecode(db, &s->T, c.key, c.klen, db->rowbuf, (uint32_t)vn, row)) != 0) return rc;
        s->ck = c.key;
        s->ckn = c.klen;
        if ((rc = asd_sq_emit(s, row)) != 0) return rc;
        rc = altsql_db_next(&c);
    }
    return rc == ALTSQL_NOTFOUND ? ALTSQL_OK : rc;
}

/* The row under one whole key, if there is one. */
static int asd_sq_point(asd_sq *s, const uint8_t *k, uint32_t kn) {
    altsql_db *db = s->db;
    altsql_value row[ALTSQL_DB_MAXCOLS];
    uint32_t fi, ix;
    size_t vn;
    int rc;
    if (kn > db->maxkey) return ALTSQL_OK;               /* too long to be a key: no such row */
    if (s->resume && asd_cmp(k, kn, s->resume, s->rn) <= 0) return ALTSQL_OK;
    rc = asd_find(db, k, kn, &fi, &ix);
    if (rc == ALTSQL_NOTFOUND) return ALTSQL_OK;
    if (rc) return rc;
    rc = asd_value(db, ASD_PAGE(db, fi), ix, db->rowbuf, ASD_ROWBUF, &vn);
    asd_unpin(db, fi);
    if (rc || (rc = asd_rowdecode(db, &s->T, k, kn, db->rowbuf, (uint32_t)vn, row)) != 0) return rc;
    s->ck = k;
    s->ckn = kn;
    return asd_sq_emit(s, row);
}

/* The key bytes of the first neq key columns, then a bound on the next one (lo, hi or none). */
static uint32_t asd_sq_key(asd_sq *s, uint8_t *k, int extra, const altsql_value *bound, int64_t device, int bydev) {
    uint32_t n = asd_ksput(k, s->T.ks);
    int i;
    if (bydev) n += asd_enc_int(k + n, device);
    for (i = bydev; i < (bydev ? 1 : s->neq); i++) n += asd_kenc(s->T.types[s->T.key[i]], &s->b[s->T.key[i]].veq, k + n, 600);
    if (extra >= 0 && bound) n += asd_kenc(s->T.types[extra], bound, k + n, 600);
    return n;
}

static int asd_sq_scan(asd_sq *s) {
    altsql_db *db = s->db;
    uint8_t pre[ALTSQL_DB_MAXKEY + 600], lo[ALTSQL_DB_MAXKEY + 600], hi[ALTSQL_DB_MAXKEY + 600];
    uint32_t pn, ln, hn;
    int next = s->neq < s->T.nkey ? s->T.key[s->neq] : -1, rc;
    if (s->plan == ASD_P_POINT) return asd_sq_point(s, pre, asd_sq_key(s, pre, -1, NULL, 0, 0));
    if (s->plan == ASD_P_LIST) {                          /* one walk per value, in key order */
        int i, j, whole = s->neq + 1 == s->T.nkey && !(s->T.flags & ASD_F_ROWID);
        altsql_value *v = s->list;
        for (i = 1; i < s->nlist; i++) {                  /* values of one type: their order is the keys' */
            altsql_value x = v[i];
            for (j = i; j > 0 && as_cmp(&v[j - 1], &x) > 0; j--) v[j] = v[j - 1];
            v[j] = x;
        }
        for (i = 0; i < s->nlist && !s->q.stop; i++) {
            if (i && as_cmp(&v[i - 1], &v[i]) == 0) continue;
            pn = asd_sq_key(s, pre, s->lcol, &v[i], 0, 0);
            if (pn > db->maxkey) continue;
            rc = whole ? asd_sq_point(s, pre, pn) : asd_sq_range(s, pre, pn, pre, pn, NULL, 0);
            if (rc) return rc;
        }
        return ALTSQL_OK;
    }
    if (s->plan == ASD_P_DEVICES) {                       /* one range on time for each device */
        altsql_db_cursor d;
        uint8_t k[8];
        int found;
        d.db = db; d.space = ASD_KS_SYNC; d.state = 0; d.fi = ASD_NONE;
        d.plen = (uint16_t)asd_ksput(d.pre, ASD_KS_SYNC);
        memcpy(k, d.pre, d.plen);
        rc = asd_cland(&d, asd_cseek(&d, k, d.plen, &found), 2);
        while (rc == ALTSQL_OK && !s->q.stop) {
            int64_t dev;
            uint32_t w = asd_dec_int(d.key + d.plen, d.klen - d.plen, &dev);
            if (w && d.plen + w < d.klen && d.key[d.plen + w] == 's') {
                const asd_bound *t = &s->b[2];
                pn = asd_sq_key(s, pre, -1, NULL, dev, 1);
                ln = asd_sq_key(s, lo, 2, t->eq ? &t->veq : t->lo ? &t->vlo : NULL, dev, 1);
                hn = asd_sq_key(s, hi, 2, t->eq ? &t->veq : t->hi ? &t->vhi : NULL, dev, 1);
                if ((rc = asd_sq_range(s, pre, pn, lo, ln, (t->eq || t->hi) ? hi : NULL, hn)) != 0) return rc;
            }
            rc = altsql_db_next(&d);
        }
        return rc == ALTSQL_NOTFOUND ? ALTSQL_OK : rc;
    }
    pn = asd_sq_key(s, pre, -1, NULL, 0, 0);
    if (s->plan == ASD_P_FULL || next < 0) return asd_sq_range(s, pre, pn, pre, pn, NULL, 0);
    ln = asd_sq_key(s, lo, next, s->b[next].lo ? &s->b[next].vlo : NULL, 0, 0);
    hn = asd_sq_key(s, hi, next, s->b[next].hi ? &s->b[next].vhi : NULL, 0, 0);
    if (pn > sizeof pre || ln > db->maxkey + 600) return ALTSQL_OK;
    return asd_sq_range(s, pre, pn, lo, ln, s->b[next].hi ? hi : NULL, hn);
}

static int asd_explain(asd_sq *s, altsql_row_cb cb, void *ctx) {
    static const char *const names[2] = { "plan", "detail" };
    char detail[200];
    altsql_value v[2];
    const char *plan = s->plan == ASD_P_POINT ? "point lookup" : s->plan == ASD_P_RANGE ? "range scan"
                     : s->plan == ASD_P_DEVICES ? "range per device" : s->plan == ASD_P_LIST ? "key list" : "full scan";
    size_t n = 0;
    int i;
    n += (size_t)snprintf(detail + n, sizeof detail - n, "%s", s->T.name);
    if (s->neq) {
        n += (size_t)snprintf(detail + n, sizeof detail - n, ", fixed:");
        for (i = 0; i < s->neq && n < sizeof detail; i++)
            n += (size_t)snprintf(detail + n, sizeof detail - n, " %s", s->T.cols[s->T.key[i]]);
    }
    if (s->plan == ASD_P_RANGE && s->neq < s->T.nkey && n < sizeof detail) {
        int k = s->T.key[s->neq];
        if (s->b[k].lo || s->b[k].hi) n += (size_t)snprintf(detail + n, sizeof detail - n, ", range on %s", s->T.cols[k]);
    }
    if (s->plan == ASD_P_DEVICES && n < sizeof detail) n += (size_t)snprintf(detail + n, sizeof detail - n, ", range on time");
    if (s->plan == ASD_P_LIST && n < sizeof detail)
        n += (size_t)snprintf(detail + n, sizeof detail - n, ", list on %s (%d values)", s->T.cols[s->lcol], s->nlist);
    if (n >= sizeof detail) n = sizeof detail - 1;
    v[0].type = ALTSQL_TEXT; v[0].u.s = plan; v[0].len = (int)strlen(plan);
    v[1].type = ALTSQL_TEXT; v[1].u.s = detail; v[1].len = (int)n;
    return cb ? (cb(ctx, 2, v, names) ? ALTSQL_OK : ALTSQL_OK) : ALTSQL_OK;
}

/* SELECT, as Core's as_select reads it, over a table of the tree. */
static int asd_select(altsql_db *db, as_parser *P, altsql_row_cb cb, void *cbctx, int explain) {
    asd_sq *s = (asd_sq *)as_alloc(P->A, sizeof *s);
    as_q *q;
    const char *tname = NULL;
    size_t tlen = 0;
    int i, star = 0, rc;
    if (!s) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    memset(s, 0, sizeof *s);
    s->db = db;
    q = &s->q;
    q->db = P->db;
    q->A = P->A;
    q->limit = -1;
    q->tmin = AS_TIME_NONE;
    q->cb = cb;
    q->cbctx = cbctx;
    q->tmp.base = db->sqlmem;
    q->tmp.cap = AS_TMP_SIZE;
    if (as_accept_op(P, '*')) star = 1;
    else {
        do {
            as_expr *e;
            if (q->nitems >= AS_MAXITEMS) return as_err(P->db, ALTSQL_NOMEM, "too many result columns");
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
            if (q->ngroup >= AS_MAXGROUP) return as_err(P->db, ALTSQL_NOMEM, "too many GROUP BY terms");
            q->group[q->ngroup++] = as_parse_expr(P);
            if (P->rc) return P->rc;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "HAVING")) { q->having = as_parse_expr(P); if (P->rc) return P->rc; }
    if (as_accept_kw(P, "ORDER")) {
        if (!as_expect_kw(P, "BY")) return P->rc;
        do {
            if (q->norder >= AS_MAXGROUP) return as_err(P->db, ALTSQL_NOMEM, "too many ORDER BY terms");
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
    if (tname) {
        struct asd_table *T;
        if ((rc = asd_table_get(db, tname, tlen, &T)) != 0) {
            if (rc == ALTSQL_SCHEMA) return as_err2(P->db, ALTSQL_SCHEMA, "no such table: ", tname, tlen);
            return rc;
        }
        if (T->ncols > AS_MAXCOLS) return as_err(P->db, ALTSQL_SCHEMA, "SQL reads tables of up to 16 columns");
        s->T = *T;
        q->kind = 3;                                    /* a table of the tree */
        q->ncols = s->T.ncols;
        for (i = 0; i < q->ncols; i++) { q->cname[i] = s->T.cols[i]; q->cnlen[i] = strlen(s->T.cols[i]); }
    }
    if (star) {
        if (!q->kind) return as_err(P->db, ALTSQL_SYNTAX, "SELECT * needs a FROM clause");
        for (i = 0; i < q->ncols; i++) {
            as_expr *e = as_node(P, E_COL, q->cname[i]);
            if (!e) return P->rc;
            e->idx = (int16_t)i;
            e->sn = q->cnlen[i];
            q->items[i] = e;
        }
        q->nitems = q->ncols;
    }
    {
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
            if (as_has_agg(q->items[ref])) return as_err(P->db, ALTSQL_SYNTAX, "GROUP BY cannot use an aggregate");
            q->group[i] = q->items[ref];
        } else if (as_resolve(P, q, q->group[i], 0, 0) || as_checked(P, q, q->group[i])) return P->rc;
    }
    for (i = 0; i < q->norder; i++) {
        int ref = as_item_ref(q, q->order[i]);
        if (ref >= 0) q->order[i] = q->items[ref];
        else if (as_resolve(P, q, q->order[i], 1, 0) || as_checked(P, q, q->order[i])) return P->rc;
    }
    if (q->where && (as_resolve(P, q, q->where, 0, 0) || as_checked(P, q, q->where))) return P->rc;
    if (q->having && (as_resolve(P, q, q->having, 1, 0) || as_checked(P, q, q->having))) return P->rc;
    q->grouped = q->ngroup > 0 || q->naggs > 0;
    if (q->having && !q->grouped) return as_err(P->db, ALTSQL_SYNTAX, "HAVING needs GROUP BY or an aggregate");
    for (i = 0; i < q->nitems; i++) {
        const as_expr *e = q->items[i];
        q->names[i] = q->alias[i] ? as_strdup_n(P->A, q->alias[i], q->alen[i]) : as_strdup_n(P->A, e->s0, e->sn);
        if (!q->names[i]) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    }
    q->outv = (altsql_value *)as_alloc(P->A, sizeof(altsql_value) * (size_t)(q->nitems + q->norder + 1));
    if (!q->outv) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    if (q->grouped) {
        q->nbuckets = 4096;
        q->tab = (as_group **)as_alloc(P->A, sizeof(as_group *) * q->nbuckets);
        if (!q->tab) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
        memset(q->tab, 0, sizeof(as_group *) * q->nbuckets);
        if (q->ngroup == 0 && !as_q_newgroup(q, 0, NULL, NULL)) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    }
    if (q->kind) { asd_plan_where(s, q->where); asd_plan_pick(s); }
    if (db->sqldry) return ALTSQL_OK;
    if (explain) {
        if (!q->kind) { s->plan = 0; memcpy(s->T.name, "(no table)", 11); }
        return asd_explain(s, cb, cbctx);
    }
    if (db->curstmt) {                                  /* a prepared statement's run */
        struct altsql_db_stmt *st = db->curstmt;
        st->stream = q->kind == 3 && !q->grouped && !q->norder;
        if (st->stream && st->delivered) {              /* going on after the rows already given */
            if (!st->rkn) return ALTSQL_OK;
            s->resume = st->rk;
            s->rn = st->rkn;
            q->offset = 0;
            if (q->limit >= 0 && (q->limit -= (int64_t)st->delivered) <= 0) return ALTSQL_OK;
        }
        db->cursq = s;
    }
    db->sqlrows = 0;
    rc = q->kind ? asd_sq_scan(s) : as_q_row(q, NULL);
    if (rc) return rc < 0 ? rc : as_err(P->db, rc, "query failed");
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

/* CREATE TABLE [IF NOT EXISTS] name (col type [PRIMARY KEY], ... [, PRIMARY KEY (a, b)]).
 * Without a primary key the first column leads the key and a hidden number keeps rows apart
 * in the order they arrived. */
static int asd_sql_create(altsql_db *db, as_parser *P) {
    struct asd_table T;
    int ifne = 0, rc, own = 0, pkdone = 0;
    if (!as_expect_kw(P, "TABLE")) return P->rc;
    if (as_accept_kw(P, "IF")) {
        if (!as_expect_kw(P, "NOT") || !as_expect_kw(P, "EXISTS")) return P->rc;
        ifne = 1;
    }
    memset(&T, 0, sizeof T);
    if (P->t.k != K_ID || P->t.n > 31) return as_perr(P, "expected a table name of up to 31 characters near ");
    memcpy(T.name, P->t.s, P->t.n);
    as_lex(P);
    if (!as_expect_op(P, '(')) return P->rc;
    do {
        int t;
        size_t j;
        if (as_accept_kw(P, "PRIMARY")) {                /* PRIMARY KEY (a, b, ...) */
            if (!as_expect_kw(P, "KEY") || !as_expect_op(P, '(')) return P->rc;
            if (pkdone || T.nkey) return as_err(P->db, ALTSQL_SYNTAX, "one primary key per table");
            do {
                int c;
                if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
                c = asd_col_index(&T, P->t.s, P->t.n);
                if (c < 0) return as_err2(P->db, ALTSQL_SCHEMA, "no such column: ", P->t.s, P->t.n);
                if (memchr(T.key, c, T.nkey)) return as_err(P->db, ALTSQL_SCHEMA, "a column appears twice in the key");
                T.key[T.nkey++] = (uint8_t)c;
                as_lex(P);
            } while (as_accept_op(P, ','));
            if (!as_expect_op(P, ')')) return P->rc;
            pkdone = 1;
            continue;
        }
        if (P->t.k != K_ID || P->t.n > 31) return as_perr(P, "expected a column name near ");
        if (T.ncols >= AS_MAXCOLS) return as_err(P->db, ALTSQL_SCHEMA, "SQL tables have up to 16 columns");
        for (j = 0; j < T.ncols; j++)
            if (as_ieq(T.cols[j], strlen(T.cols[j]), P->t.s, P->t.n)) return as_err2(P->db, ALTSQL_SCHEMA, "column named twice: ", P->t.s, P->t.n);
        memcpy(T.cols[T.ncols], P->t.s, P->t.n);
        as_lex(P);
        if (P->t.k != K_ID || !(t = as_type_from_sql(P->t.s, P->t.n)))
            return as_perr(P, "expected a type (TIME, INT, INTEGER, FLOAT, REAL, TEXT) near ");
        as_lex(P);
        if (as_accept_op(P, '(')) {                       /* VARCHAR(n): size ignored */
            while (P->t.k != K_END && !as_isop(P, ')')) as_lex(P);
            if (!as_expect_op(P, ')')) return P->rc;
        }
        T.types[T.ncols] = (uint8_t)t;
        if (as_accept_kw(P, "PRIMARY")) {
            if (!as_expect_kw(P, "KEY")) return P->rc;
            if (pkdone || T.nkey) return as_err(P->db, ALTSQL_SYNTAX, "one primary key per table");
            T.key[T.nkey++] = T.ncols;
            pkdone = 1;
        }
        T.ncols++;
    } while (as_accept_op(P, ','));
    if (!as_expect_op(P, ')')) return P->rc;
    if (P->rc) return P->rc;
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    if (!T.ncols) return as_err(P->db, ALTSQL_SYNTAX, "a table needs columns");
    if (!T.nkey) { T.nkey = 1; T.key[0] = 0; T.flags |= ASD_F_ROWID; }
    T.kind = ASD_K_TABLE;
    {
        struct asd_table *X;
        rc = asd_table_get(db, T.name, strlen(T.name), &X);
        if (rc == ALTSQL_OK || rc == ALTSQL_MISUSE) {
            if (ifne && rc == ALTSQL_OK) return ALTSQL_OK;
            return as_err2(P->db, ALTSQL_EXISTS, "that name is taken: ", T.name, strlen(T.name));
        }
        if (rc != ALTSQL_SCHEMA) return rc;
        db->err[0] = 0;
    }
    if (db->sqldry) return ALTSQL_OK;
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    rc = asd_table_new(db, &T);
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    else if (rc) db->failed = 1;
    return rc;
}

/* INSERT INTO t [(cols)] VALUES (...), ...: every row is read and checked before any is
 * written, so the statement is all or nothing. */
static int asd_sql_insert(altsql_db *db, as_parser *P) {
    struct asd_table T, *TP;
    int map[ALTSQL_DB_MAXCOLS], nlist = 0, i, rc, own = 0, replace = 0;
    altsql_value *rows;
    size_t nrows = 0, caprows = 0;
    if (as_accept_kw(P, "OR")) {
        if (!as_expect_kw(P, "REPLACE")) return P->rc;
        replace = 1;
    }
    if (!as_expect_kw(P, "INTO")) return P->rc;
    if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
    if ((rc = asd_table_get(db, P->t.s, P->t.n, &TP)) != 0)
        return rc == ALTSQL_SCHEMA ? as_err2(P->db, ALTSQL_SCHEMA, "no such table: ", P->t.s, P->t.n) : rc;
    T = *TP;
    as_lex(P);
    if (T.kind != ASD_K_TABLE) return as_err(P->db, ALTSQL_MISUSE, "a synced table takes rows only from sync");
    for (i = 0; i < T.ncols; i++) map[i] = i;
    if (as_accept_op(P, '(')) {
        do {
            int c;
            if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
            c = asd_col_index(&T, P->t.s, P->t.n);
            if (c < 0) return as_err2(P->db, ALTSQL_SCHEMA, "no such column: ", P->t.s, P->t.n);
            if (nlist >= T.ncols) return as_err(P->db, ALTSQL_SCHEMA, "too many columns");
            map[nlist++] = c;
            as_lex(P);
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        if (nlist != T.ncols) return as_err(P->db, ALTSQL_SCHEMA, "INSERT must give every column");
    }
    if (!as_expect_kw(P, "VALUES")) return P->rc;
    caprows = 64;
    rows = (altsql_value *)as_alloc(P->A, caprows * T.ncols * sizeof(altsql_value));
    if (!rows) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    do {
        altsql_value in[ALTSQL_DB_MAXCOLS], *v;
        as_ctx c;
        int n = 0;
        c.row = NULL; c.acc = NULL; c.tmp = P->A; c.perm = P->A;
        if (!as_expect_op(P, '(')) return P->rc;
        do {
            as_expr *e;
            if (n >= T.ncols) return as_err(P->db, ALTSQL_SCHEMA, "too many values");
            e = as_parse_expr(P);
            if (!e) return P->rc;
            if (!as_is_const(e)) return as_err2(P->db, ALTSQL_SCHEMA, "values must be constants: ", e->s0, e->sn);
            if ((rc = as_eval(&c, e, &in[n])) != 0) return rc;
            n++;
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        if (n != T.ncols) return as_err(P->db, ALTSQL_SCHEMA, "wrong number of values");
        if (nrows == caprows) {
            altsql_value *more = (altsql_value *)as_alloc(P->A, 2 * caprows * T.ncols * sizeof(altsql_value));
            if (!more) return as_err(P->db, ALTSQL_NOMEM, "too many rows for working memory");
            memcpy(more, rows, caprows * T.ncols * sizeof(altsql_value));
            rows = more;
            caprows *= 2;
        }
        v = rows + nrows * T.ncols;
        for (i = 0; i < T.ncols && !db->sqldry; i++)
            if ((rc = asd_coerce(db, T.types[map[i]], &in[i], &v[map[i]])) != 0) return rc;
        nrows++;
    } while (as_accept_op(P, ','));
    if (P->rc) return P->rc;
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    if (db->sqldry) return ALTSQL_OK;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    {
        uint64_t before = db->mods;
        size_t r;
        uint8_t tk[ALTSQL_DB_MAXKEY + 8];
        rc = ALTSQL_OK;
        /* a key already there, or twice in the statement: nothing is written */
        for (r = 0; r < nrows && !rc && !replace && !(T.flags & ASD_F_ROWID); r++) {
            altsql_value cv[ALTSQL_DB_MAXCOLS];
            uint32_t tkn;
            size_t r2;
            if ((rc = asd_row_check(db, &T, ASD_OP_INSERT, rows + r * T.ncols, cv, tk, sizeof tk, &tkn, NULL)) != 0) break;
            for (r2 = 0; r2 < r && !rc; r2++) {
                int same = 1;
                for (i = 0; i < T.nkey && same; i++)
                    same = as_cmp(&rows[r * T.ncols + T.key[i]], &rows[r2 * T.ncols + T.key[i]]) == 0;
                if (same) rc = asd_err(db, ALTSQL_EXISTS, "the statement gives one key twice");
            }
        }
        for (r = 0; r < nrows && !rc; r++) {
            altsql_value cv[ALTSQL_DB_MAXCOLS];
            uint32_t tkn;
            if ((rc = asd_row_check(db, &T, ASD_OP_REPLACE, rows + r * T.ncols, cv, tk, sizeof tk, &tkn, NULL)) != 0) break;
            rc = asd_row_write(db, &T, replace ? ASD_OP_REPLACE : ASD_OP_INSERT, tk, tkn, cv, db->rowbuf, NULL, 0);
        }
        if (rc && !own && db->mods != before) db->failed = 1;
        db->sqlchanged = rc ? 0 : nrows;
    }
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    return rc;
}

/* The table and Core's query parts that UPDATE and DELETE share with SELECT. */
static asd_sq *asd_sq_new(altsql_db *db, as_parser *P, const struct asd_table *T) {
    asd_sq *s = (asd_sq *)as_alloc(P->A, sizeof *s);
    int i;
    if (!s) { as_err(P->db, ALTSQL_NOMEM, "out of working memory"); return NULL; }
    memset(s, 0, sizeof *s);
    s->db = db;
    s->T = *T;
    s->q.db = P->db;
    s->q.A = P->A;
    s->q.limit = -1;
    s->q.tmin = AS_TIME_NONE;
    s->q.tmp.base = db->sqlmem;
    s->q.tmp.cap = AS_TMP_SIZE;
    s->q.kind = 3;
    s->q.ncols = T->ncols < AS_MAXCOLS ? T->ncols : AS_MAXCOLS;
    for (i = 0; i < s->q.ncols; i++) { s->q.cname[i] = s->T.cols[i]; s->q.cnlen[i] = strlen(s->T.cols[i]); }
    return s;
}

/* What an UPDATE or DELETE does to each row. A change of key columns is kept in store and made
 * after the scan. */
typedef struct asd_chg {
    int n, keyset;
    int col[ALTSQL_DB_MAXCOLS];
    as_expr *e[ALTSQL_DB_MAXCOLS];
    uint8_t *pack, *store;
    size_t used, cap;
    uint64_t count;
} asd_chg;

static int asd_del_act(asd_sq *s, altsql_value *row) {
    asd_chg *u = (asd_chg *)s->actx;
    (void)row;
    u->count++;
    return asd_row_write(s->db, &s->T, ASD_OP_DELETE, (uint8_t *)s->ck, s->ckn, NULL, NULL, NULL, 0);
}

static int asd_upd_act(asd_sq *s, altsql_value *row) {
    asd_chg *u = (asd_chg *)s->actx;
    altsql_value nv[ALTSQL_DB_MAXCOLS];
    as_ctx c;
    uint32_t vn;
    int i, rc;
    memcpy(nv, row, sizeof(altsql_value) * (size_t)s->T.ncols);
    c.row = row; c.acc = NULL; c.tmp = &s->q.tmp; c.perm = s->q.A;
    for (i = 0; i < u->n; i++) {
        altsql_value v;
        if ((rc = as_eval(&c, u->e[i], &v)) != 0) return rc;
        if ((rc = asd_coerce(s->db, s->T.types[u->col[i]], &v, &nv[u->col[i]])) != 0) return rc;
    }
    u->count++;
    if (!u->keyset) return asd_row_write(s->db, &s->T, ASD_OP_UPDATE, (uint8_t *)s->ck, s->ckn, nv, u->pack, NULL, 0);
    vn = asd_rpack(s->T.types, s->T.ncols, nv, u->pack);
    if (u->used + 6 + s->ckn + vn > u->cap)
        return asd_err(s->db, ALTSQL_NOMEM, "an UPDATE of key columns changes more rows than SQL memory holds");
    as_put16(u->store + u->used, s->ckn);
    as_put32(u->store + u->used + 2, vn);
    memcpy(u->store + u->used + 6, s->ck, s->ckn);
    memcpy(u->store + u->used + 6 + s->ckn, u->pack, vn);
    u->used += 6 + s->ckn + vn;
    return ALTSQL_OK;
}

/* Key changes, after the scan: the old rows go, then each new row comes under a key no row has. */
static int asd_upd_move(asd_sq *s, asd_chg *u) {
    altsql_db *db = s->db;
    size_t at;
    int rc;
    for (at = 0; at < u->used; ) {
        uint32_t kn = as_get16(u->store + at), vn = as_get32(u->store + at + 2);
        if ((rc = asd_row_write(db, &s->T, ASD_OP_DELETE, u->store + at + 6, kn, NULL, NULL, NULL, 0)) != 0) return rc;
        at += 6 + kn + vn;
    }
    for (at = 0; at < u->used; ) {
        uint32_t kn = as_get16(u->store + at), vn = as_get32(u->store + at + 2), tkn;
        altsql_value nv[ALTSQL_DB_MAXCOLS], cv[ALTSQL_DB_MAXCOLS];
        uint8_t tk[ALTSQL_DB_MAXKEY + 8];
        const uint8_t *pv = u->store + at + 6 + kn;
        if ((rc = asd_runpack(s->T.types, s->T.ncols, pv, vn, nv)) != 0) return rc;
        if ((rc = asd_row_check(db, &s->T, ASD_OP_INSERT, nv, cv, tk, sizeof tk, &tkn, "the UPDATE gives a row the key of another row")) != 0) return rc;
        if ((rc = asd_row_write(db, &s->T, ASD_OP_INSERT, tk, tkn, cv, u->pack, NULL, 0)) != 0) return rc;
        at += 6 + kn + vn;
    }
    return ALTSQL_OK;
}

/* UPDATE t SET col = expr, ... [WHERE expr] and DELETE FROM t [WHERE expr], through the plans
 * SELECT uses. An error partway fails the transaction; one the statement opened is rolled back. */
static int asd_sql_change(altsql_db *db, as_parser *P, int del) {
    struct asd_table *TP;
    asd_sq *s;
    asd_chg *u;
    uint64_t before;
    int rc, own = 0, i;
    if (del && !as_expect_kw(P, "FROM")) return P->rc;
    if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
    if ((rc = asd_table_get(db, P->t.s, P->t.n, &TP)) != 0)
        return rc == ALTSQL_SCHEMA ? as_err2(P->db, ALTSQL_SCHEMA, "no such table: ", P->t.s, P->t.n) : rc;
    as_lex(P);
    if (!del && TP->kind != ASD_K_TABLE) return as_err(P->db, ALTSQL_MISUSE, "a synced table takes rows only from sync");
    if (TP->ncols > AS_MAXCOLS) return as_err(P->db, ALTSQL_SCHEMA, "SQL reads tables of up to 16 columns");
    if (!(s = asd_sq_new(db, P, TP))) return P->db->err[0] ? ALTSQL_NOMEM : ALTSQL_NOMEM;
    u = (asd_chg *)as_alloc(P->A, sizeof *u);
    if (!u) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    memset(u, 0, sizeof *u);
    if (!(u->pack = (uint8_t *)as_alloc(P->A, ASD_ROWBUF))) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    if (!del) {
        if (!as_expect_kw(P, "SET")) return P->rc;
        do {
            int c;
            if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
            c = asd_col_index(&s->T, P->t.s, P->t.n);
            if (c < 0) return as_err2(P->db, ALTSQL_SCHEMA, "no such column: ", P->t.s, P->t.n);
            for (i = 0; i < u->n; i++)
                if (u->col[i] == c) return as_err2(P->db, ALTSQL_SCHEMA, "column set twice: ", P->t.s, P->t.n);
            as_lex(P);
            if (!as_expect_op(P, '=')) return P->rc;
            u->col[u->n] = c;
            if (!(u->e[u->n] = as_parse_expr(P))) return P->rc;
            if (as_resolve(P, &s->q, u->e[u->n], 0, 0) || as_checked(P, &s->q, u->e[u->n])) return P->rc;
            if (memchr(s->T.key, c, s->T.nkey)) u->keyset = 1;
            u->n++;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "WHERE")) {
        if (!(s->q.where = as_parse_expr(P))) return P->rc;
        if (as_resolve(P, &s->q, s->q.where, 0, 0) || as_checked(P, &s->q, s->q.where)) return P->rc;
    }
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    if (db->sqldry) return ALTSQL_OK;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    if (u->keyset) {                                    /* the rest of SQL memory keeps the changes */
        u->cap = P->A->cap - P->A->used > 64 ? P->A->cap - P->A->used - 64 : 0;
        if (!(u->store = (uint8_t *)as_alloc(P->A, u->cap))) return as_err(P->db, ALTSQL_NOMEM, "out of working memory");
    }
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    before = db->mods;
    asd_plan_where(s, s->q.where);
    asd_plan_pick(s);
    s->act = del ? asd_del_act : asd_upd_act;
    s->actx = u;
    db->sqlrows = 0;
    rc = asd_sq_scan(s);
    if (!rc && u->keyset) rc = asd_upd_move(s, u);
    db->sqlchanged = rc ? 0 : u->count;
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    else if (rc && db->mods != before) db->failed = 1;
    return rc;
}

/* DROP TABLE [IF EXISTS] t: its rows, then its two catalog entries. */
static int asd_sql_drop(altsql_db *db, as_parser *P) {
    struct asd_table *TP, T;
    uint64_t before, count = 0;
    int rc, own = 0, ifex = 0;
    if (!as_expect_kw(P, "TABLE")) return P->rc;
    if (as_accept_kw(P, "IF")) { if (!as_expect_kw(P, "EXISTS")) return P->rc; ifex = 1; }
    if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
    rc = asd_table_get(db, P->t.s, P->t.n, &TP);
    if (rc == ALTSQL_SCHEMA) {
        if (!ifex) return as_err2(P->db, ALTSQL_SCHEMA, "no such table: ", P->t.s, P->t.n);
        db->err[0] = 0;
        as_lex(P);
        return P->t.k != K_END && !as_isop(P, ';') ? as_perr(P, "syntax error near ") : ALTSQL_OK;
    }
    if (rc) return rc;
    as_lex(P);
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    if (TP->kind != ASD_K_TABLE) return as_err(P->db, ALTSQL_MISUSE, "a synced table stays while devices send to it: DELETE its rows");
    if (db->sqldry) return ALTSQL_OK;
    if (db->tx == 1) return asd_err(db, ALTSQL_MISUSE, "a read transaction cannot write");
    T = *TP;
    if (!db->tx) { if ((rc = altsql_db_begin(db, 1)) != 0) return rc; own = 1; }
    before = db->mods;
    rc = asd_table_drop_rows(db, &T, &count);
    db->sqlchanged = rc ? 0 : count;
    if (own) { if (rc) altsql_db_rollback(db); else rc = altsql_db_commit(db); }
    else if (rc && db->mods != before) db->failed = 1;
    return rc;
}

/* ---- The text before Core's parser: IN lists and parameters ---------------------------------
 * Core's parser has neither, so the text is written out first: column IN (v, ...) becomes
 * (column = v OR ...), which the planner reads as a key list, and column NOT IN (v, ...) becomes
 * (column <> v AND ...). Each ? becomes the literal of the value bound to it. Strings and
 * comments are copied as they are. */
typedef struct asd_txt { char *p; size_t n, cap; } asd_txt;

static void asd_tx(asd_txt *o, const char *s, size_t n) {
    if (o->p && o->n + n < o->cap) memcpy(o->p + o->n, s, n);
    o->n += n;
}

static int asd_lit(asd_txt *o, const altsql_value *v) {
    char b[48];
    int n, i;
    switch (v->type) {
    case ALTSQL_NULL: asd_tx(o, "NULL", 4); return ALTSQL_OK;
    case ALTSQL_INTEGER:
        if (v->u.i == INT64_MIN) { asd_tx(o, "(-9223372036854775807-1)", 24); return ALTSQL_OK; }
        n = snprintf(b, sizeof b, v->u.i < 0 ? "(%lld)" : "%lld", (long long)v->u.i);
        asd_tx(o, b, (size_t)n);
        return ALTSQL_OK;
    case ALTSQL_REAL:
        if (v->u.r != v->u.r || v->u.r > 1.7976931348623157e308 || v->u.r < -1.7976931348623157e308) return ALTSQL_MISUSE;
        n = snprintf(b + 1, sizeof b - 4, "%.17g", v->u.r);
        if (!strpbrk(b + 1, ".eE")) { b[1 + n++] = '.'; b[1 + n++] = '0'; }
        if (b[1] == '-') { b[0] = '('; b[1 + n++] = ')'; asd_tx(o, b, (size_t)n + 1); }
        else asd_tx(o, b + 1, (size_t)n);
        return ALTSQL_OK;
    case ALTSQL_TEXT:
        if (v->len < 0) return ALTSQL_MISUSE;
        asd_tx(o, "'", 1);
        for (i = 0; i < v->len; i++) {
            if (!v->u.s[i]) return ALTSQL_MISUSE;
            asd_tx(o, v->u.s + i, 1);
            if (v->u.s[i] == '\'') asd_tx(o, "'", 1);
        }
        asd_tx(o, "'", 1);
        return ALTSQL_OK;
    }
    return ALTSQL_MISUSE;
}

static const char *asd_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static int asd_word(const char *p, const char *w) {
    size_t n = strlen(w);
    return as_ieq(p, n, w, n) && !as_is_ident_char((unsigned char)p[n]);
}

/* One value of an IN list: a number, a string, NULL or ?. Its end, or NULL. */
static const char *asd_item(const char *p) {
    const char *q = p;
    if (*q == '\'') {
        q++;
        while (*q && !(*q == '\'' && q[1] != '\'')) q += *q == '\'' ? 2 : 1;
        return *q ? q + 1 : NULL;
    }
    if (*q == '?') return q + 1;
    if (asd_word(q, "NULL")) return q + 4;
    if (*q == '-' || *q == '+') q++;
    if (!((*q >= '0' && *q <= '9') || *q == '.')) return NULL;
    while ((*q >= '0' && *q <= '9') || *q == '.' || ((*q == 'e' || *q == 'E') && q++ && (*q == '-' || *q == '+' || (*q >= '0' && *q <= '9')))) q++;
    return q;
}

/* mode 0: ? refused (altsql_db_exec); 1: ? kept and counted; 2: ? replaced by the bound
 * values; 3: ? replaced by 0, to check a text. */
static int asd_rewrite(altsql_db *db, const char *src, asd_txt *o, int mode, const altsql_value *par, int *np) {
    const char *p = src;
    int n = 0, rc;
    while (*p) {
        const char *t = p;
        if (*p == '\'') {
            p++;
            while (*p && !(*p == '\'' && p[1] != '\'')) p += *p == '\'' ? 2 : 1;
            if (*p) p++;
            asd_tx(o, t, (size_t)(p - t));
        } else if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            asd_tx(o, t, (size_t)(p - t));
        } else if (*p == '?') {
            p++;
            if (mode == 0) return asd_err(db, ALTSQL_MISUSE, "a parameter (?) needs a prepared statement");
            if (n >= ASD_MAXPARAM) return asd_err(db, ALTSQL_TOOBIG, "a statement takes up to 16 parameters");
            if (mode == 1) asd_tx(o, "?", 1);
            else if (mode == 3) asd_tx(o, "0", 1);
            else if ((rc = asd_lit(o, &par[n])) != 0) return asd_err(db, rc, "a bound value cannot be written as SQL (NaN, infinity, or text with a zero byte)");
            n++;
        } else if (as_is_ident_start((unsigned char)*p)) {
            const char *id = p, *q, *e;
            size_t idn;
            int neg = 0, ok = 1;
            while (as_is_ident_char((unsigned char)*p)) p++;
            idn = (size_t)(p - id);
            q = asd_ws(p);
            if (asd_word(q, "NOT")) { neg = 1; q = asd_ws(q + 3); }
            if (!asd_word(q, "IN") || *(q = asd_ws(q + 2)) != '(') { asd_tx(o, id, idn); continue; }
            for (e = asd_ws(q + 1); ok; ) {              /* literals and parameters only */
                const char *end = asd_item(e);
                if (!end) { ok = 0; break; }
                e = asd_ws(end);
                if (*e == ')') break;
                if (*e != ',') { ok = 0; break; }
                e = asd_ws(e + 1);
            }
            if (!ok) { asd_tx(o, id, idn); continue; }   /* Core's parser reports it */
            asd_tx(o, "(", 1);
            for (e = asd_ws(q + 1); ; ) {
                const char *end = asd_item(e);
                asd_tx(o, id, idn);
                asd_tx(o, neg ? " <> " : " = ", neg ? 4 : 3);
                if (*e == '?') {
                    if (mode == 0) return asd_err(db, ALTSQL_MISUSE, "a parameter (?) needs a prepared statement");
                    if (n >= ASD_MAXPARAM) return asd_err(db, ALTSQL_TOOBIG, "a statement takes up to 16 parameters");
                    if (mode == 1) asd_tx(o, "?", 1);
                    else if (mode == 3) asd_tx(o, "0", 1);
                    else if ((rc = asd_lit(o, &par[n])) != 0) return asd_err(db, rc, "a bound value cannot be written as SQL (NaN, infinity, or text with a zero byte)");
                    n++;
                } else asd_tx(o, e, (size_t)(end - e));
                e = asd_ws(end);
                if (*e == ')') break;
                asd_tx(o, neg ? " AND " : " OR ", neg ? 5 : 4);
                e = asd_ws(e + 1);
            }
            asd_tx(o, ")", 1);
            p = e + 1;
        } else { asd_tx(o, p, 1); p++; }
    }
    if (np) *np = n;
    return ALTSQL_OK;
}

/* No ? and no word IN: the text goes to the parser as it is. */
static int asd_plain(const char *s) {
    const char *p;
    for (p = s; *p; p++)
        if (*p == '?' || ((*p | 32) == 'i' && (p[1] | 32) == 'n' && (p == s || !as_is_ident_char((unsigned char)p[-1])) &&
                          !as_is_ident_char((unsigned char)p[2]))) return 0;
    return 1;
}

static int asd_exec_text(altsql_db *db, const char *sql, size_t resv, altsql_row_cb cb, void *ctx) {
    as_parser P;
    as_arena A;
    int rc = ALTSQL_OK;
    memset(&P, 0, sizeof P);
    P.db = &db->core;
    P.p = sql;
    P.A = &A;
    db->sqlnst = 0;
    as_lex(&P);
    while (!P.rc) {
        while (as_accept_op(&P, ';')) {}
        if (P.rc || P.t.k == K_END) break;
        A.base = db->sqlmem + AS_TMP_SIZE;
        A.cap = db->sqlexec - AS_TMP_SIZE - resv;
        A.used = 0;
        db->sqlnst++;
        if (as_accept_kw(&P, "SELECT")) rc = asd_select(db, &P, cb, ctx, 0);
        else if (as_accept_kw(&P, "EXPLAIN")) rc = as_expect_kw(&P, "SELECT") ? asd_select(db, &P, cb, ctx, 1) : P.rc;
        else if (as_accept_kw(&P, "CREATE")) rc = asd_sql_create(db, &P);
        else if (as_accept_kw(&P, "INSERT")) rc = asd_sql_insert(db, &P);
        else if (as_accept_kw(&P, "UPDATE")) rc = asd_sql_change(db, &P, 0);
        else if (as_accept_kw(&P, "DELETE")) rc = asd_sql_change(db, &P, 1);
        else if (as_accept_kw(&P, "DROP")) rc = asd_sql_drop(db, &P);
        else rc = as_perr(&P, "expected SELECT, INSERT, UPDATE, DELETE, CREATE, DROP or EXPLAIN near ");
        if (rc) break;
        if (P.rc) { rc = P.rc; break; }
        if (P.t.k != K_END && !as_isop(&P, ';')) { rc = as_perr(&P, "syntax error near "); break; }
    }
    if (!rc) rc = P.rc;
    if (rc < 0) {
        if (db->core.err[0]) memcpy(db->err, db->core.err, sizeof db->err);   /* the SQL layer's words name the statement's parts */
        if (!db->err[0]) asd_err(db, rc, rc == ALTSQL_SYNTAX ? "syntax error" : "statement failed");
    }
    return rc;
}

/* The text written out at the top of SQL memory, then run. */
static int asd_run_text(altsql_db *db, const char *src, int mode, const altsql_value *par, altsql_row_cb cb, void *ctx) {
    asd_txt o;
    size_t need;
    int rc;
    memset(&o, 0, sizeof o);
    if ((rc = asd_rewrite(db, src, &o, mode, par, NULL)) != 0) return rc;
    need = (o.n + 1 + 15) & ~(size_t)15;
    if (need + AS_TMP_SIZE * 2 > db->sqlexec) return asd_err(db, ALTSQL_NOMEM, "the statement is too long for SQL memory");
    o.p = (char *)db->sqlmem + db->sqlexec - need;
    o.cap = need;
    o.n = 0;
    asd_rewrite(db, src, &o, mode, par, NULL);
    o.p[o.n] = 0;
    return asd_exec_text(db, o.p, need, cb, ctx);
}

int altsql_db_exec(altsql_db *db, const char *sql, altsql_row_cb cb, void *ctx) {
    int rc;
    if (!db || !sql) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0) return rc;
    if (db->sqlexec < AS_TMP_SIZE * 2) return asd_err(db, ALTSQL_NOMEM, "SQL needs more working memory (sql_mem)");
    db->err[0] = 0;
    db->core.err[0] = 0;
    if (asd_plain(sql)) return asd_exec_text(db, sql, 0, cb, ctx);
    return asd_run_text(db, sql, 0, NULL, cb, ctx);
}

/* ---- Prepared statements ---------------------------------------------------------------------- */
static void asd_st_restart(altsql_db_stmt *st) {
    st->state = 0;
    st->delivered = 0;
    st->rkn = 0;
    st->nrows = st->pos = st->off = st->rused = 0;
    st->more = st->stream = st->err = 0;
}

/* Rows of a run into the statement's batch; a full batch stops the run. */
static int asd_st_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    altsql_db_stmt *st = (altsql_db_stmt *)ctx;
    uint8_t *base = (uint8_t *)st->rows64;
    size_t need = 8 + (size_t)n * sizeof(altsql_value), used = 0;
    altsql_value *dst;
    char *tx;
    int i;
    if (st->skip) { st->skip--; return 0; }
    if (st->ncols != n) {                               /* the column names, once */
        st->ncols = n;
        for (i = 0; i < n && i < AS_MAXITEMS; i++) {
            size_t l = names && names[i] ? strlen(names[i]) : 0;
            if (used + l + 1 > ASD_STMTNAMES) l = 0;
            if (used + 1 > ASD_STMTNAMES) { st->nm[i] = ""; continue; }
            if (l) memcpy(st->names + used, names[i], l);
            st->names[used + l] = 0;
            st->nm[i] = st->names + used;
            used += l + 1;
        }
    }
    for (i = 0; i < n; i++) if (v[i].type == ALTSQL_TEXT && v[i].len > 0) need += (size_t)v[i].len;
    need = (need + 7) & ~(size_t)7;
    if (st->rused + need > ASD_STMTROWS) {
        if (!st->nrows) st->err = ALTSQL_TOOBIG; else st->more = 1;
        return 1;
    }
    as_put32(base + st->rused, (uint32_t)need);
    dst = (altsql_value *)(void *)(base + st->rused + 8);
    tx = (char *)(dst + n);
    for (i = 0; i < n; i++) {
        dst[i] = v[i];
        if (v[i].type == ALTSQL_TEXT) {
            if (v[i].len > 0) memcpy(tx, v[i].u.s, (size_t)v[i].len);
            dst[i].u.s = tx;
            tx += v[i].len > 0 ? v[i].len : 0;
        }
    }
    st->rused += (uint32_t)need;
    st->nrows++;
    if (st->stream && st->db->cursq) {                  /* where a run that goes on starts */
        const asd_sq *s = (const asd_sq *)st->db->cursq;
        memcpy(st->rk, s->ck, s->ckn);
        st->rkn = s->ckn;
    }
    return 0;
}

static int asd_st_run(altsql_db_stmt *st, int mode) {
    altsql_db *db = st->db;
    int rc;
    db->err[0] = 0;
    db->core.err[0] = 0;
    db->sqldry = mode == 3;
    db->curstmt = mode == 2 ? st : NULL;
    db->cursq = NULL;
    rc = asd_run_text(db, st->text, mode, st->par, mode == 2 ? asd_st_cb : NULL, st);
    db->sqldry = 0;
    db->curstmt = NULL;
    db->cursq = NULL;
    return rc;
}

int altsql_db_prepare(altsql_db *db, const char *sql, altsql_db_stmt **out) {
    altsql_db_stmt *st = NULL;
    asd_txt o;
    uint32_t i;
    int rc, np = 0;
    if (!out) return ALTSQL_MISUSE;
    *out = NULL;
    if (!db || !sql) return ALTSQL_MISUSE;
    if ((rc = asd_ready(db)) != 0) return rc;
    db->err[0] = 0;
    db->core.err[0] = 0;
    for (i = 0; i < db->nstmt && !st; i++) if (!db->stmts[i].used) st = &db->stmts[i];
    if (!st) return asd_err(db, ALTSQL_NOMEM, db->nstmt ? "every prepared statement is in use: finalize one"
                                                        : "prepared statements need more SQL memory (sql_mem)");
    st->db = db;
    st->ncols = 0;
    asd_st_restart(st);
    memset(&o, 0, sizeof o);
    o.p = st->text;
    o.cap = ASD_STMTTXT;
    if ((rc = asd_rewrite(db, sql, &o, 1, NULL, &np)) != 0) return rc;
    if (o.n + 1 > o.cap) return asd_err(db, ALTSQL_TOOBIG, "a prepared statement holds up to 4 KB of text, IN lists written out");
    st->text[o.n] = 0;
    st->np = np;
    for (i = 0; i < ASD_MAXPARAM; i++) st->par[i].type = ALTSQL_NULL;
    if ((rc = asd_st_run(st, 3)) != 0) return rc;       /* checked once: parsed, nothing read or written */
    if (db->sqlnst != 1) return asd_err(db, ALTSQL_MISUSE, "a prepared statement holds one statement");
    st->used = 1;
    *out = st;
    return ALTSQL_OK;
}

int altsql_db_bind(altsql_db_stmt *st, int i, const altsql_value *v) {
    altsql_value *p;
    if (!st || !st->used) return ALTSQL_MISUSE;
    if (i < 1 || i > st->np) return asd_err(st->db, ALTSQL_MISUSE, "no parameter with that number");
    p = &st->par[i - 1];
    asd_st_restart(st);
    if (!v || v->type == ALTSQL_NULL) { p->type = ALTSQL_NULL; return ALTSQL_OK; }
    if (v->type == ALTSQL_TEXT) {
        if (v->len < 0 || v->len > 255) return asd_err(st->db, ALTSQL_TOOBIG, "a bound text value holds up to 255 bytes");
        memcpy(st->ptxt[i - 1], v->u.s, (size_t)v->len);
        *p = *v;
        p->u.s = st->ptxt[i - 1];
        return ALTSQL_OK;
    }
    if (v->type != ALTSQL_INTEGER && v->type != ALTSQL_REAL) return asd_err(st->db, ALTSQL_MISUSE, "unknown value type");
    *p = *v;
    return ALTSQL_OK;
}

int altsql_db_step(altsql_db_stmt *st) {
    int rc;
    if (!st || !st->used) return ALTSQL_MISUSE;
    if (st->state == 2) return ALTSQL_DONE;
    if (st->state == 1) {
        if (st->pos + 1 < st->nrows) {
            st->off += as_get32((const uint8_t *)st->rows64 + st->off);
            st->pos++;
            return ALTSQL_OK;
        }
        if (!st->more) { st->state = 2; return ALTSQL_DONE; }
        st->delivered += st->nrows;                     /* the next batch */
    }
    st->skip = st->stream ? 0 : st->delivered;
    st->nrows = st->pos = st->off = st->rused = 0;
    st->more = st->err = 0;
    rc = asd_st_run(st, 2);
    if (!rc && st->err) rc = asd_err(st->db, st->err, "a row is larger than a prepared statement's batch (8 KB)");
    if (rc) { st->state = 2; return rc; }
    if (!st->nrows) { st->state = 2; return ALTSQL_DONE; }
    st->state = 1;
    return ALTSQL_OK;
}

int altsql_db_column_count(altsql_db_stmt *st) { return st && st->used ? st->ncols : 0; }

int altsql_db_column(altsql_db_stmt *st, int i, altsql_value *out) {
    if (!st || !st->used || !out) return ALTSQL_MISUSE;
    if (st->state != 1 || i < 0 || i >= st->ncols) return asd_err(st->db, ALTSQL_MISUSE, "no such column in a current row");
    *out = ((const altsql_value *)(const void *)((const uint8_t *)st->rows64 + st->off + 8))[i];
    return ALTSQL_OK;
}

const char *altsql_db_column_name(altsql_db_stmt *st, int i) {
    return st && st->used && i >= 0 && i < st->ncols && i < AS_MAXITEMS ? st->nm[i] : NULL;
}

int altsql_db_reset(altsql_db_stmt *st) {
    if (!st || !st->used) return ALTSQL_MISUSE;
    asd_st_restart(st);
    return ALTSQL_OK;
}

void altsql_db_finalize(altsql_db_stmt *st) { if (st) st->used = 0; }
#else
int altsql_db_exec(altsql_db *db, const char *sql, altsql_row_cb cb, void *ctx) {
    (void)sql; (void)cb; (void)ctx;
    return asd_err(db, ALTSQL_MISUSE, "built without SQL: AltSql Core's ALTSQL_ENABLE_SQL is 0");
}
int altsql_db_prepare(altsql_db *db, const char *sql, altsql_db_stmt **st) {
    (void)sql;
    if (st) *st = NULL;
    return asd_err(db, ALTSQL_MISUSE, "built without SQL: AltSql Core's ALTSQL_ENABLE_SQL is 0");
}
int  altsql_db_bind(altsql_db_stmt *st, int i, const altsql_value *v) { (void)st; (void)i; (void)v; return ALTSQL_MISUSE; }
int  altsql_db_step(altsql_db_stmt *st) { (void)st; return ALTSQL_MISUSE; }
int  altsql_db_column_count(altsql_db_stmt *st) { (void)st; return 0; }
int  altsql_db_column(altsql_db_stmt *st, int i, altsql_value *out) { (void)st; (void)i; (void)out; return ALTSQL_MISUSE; }
const char *altsql_db_column_name(altsql_db_stmt *st, int i) { (void)st; (void)i; return NULL; }
int  altsql_db_reset(altsql_db_stmt *st) { (void)st; return ALTSQL_MISUSE; }
void altsql_db_finalize(altsql_db_stmt *st) { (void)st; }
#endif

/* ---- Info and checks ----------------------------------------------------------------------- */
int altsql_db_info_get(altsql_db *db, altsql_db_info *o) {
    uint32_t pg, d = 0;
    if (!db || !o) return ALTSQL_MISUSE;
    memset(o, 0, sizeof *o);
    o->page_size = db->ps;
    o->pages = db->npages;
    o->free_pages = db->tx == 2 ? db->nav + db->nho + db->fltail + db->dcount + db->hcount + db->nxleft
                                : db->com.flcount + db->com.nxcount;
    o->txn = db->com.txn;
    o->cache_pages = db->nfr;
    o->mem_used = db->mem_used;
    o->reads = db->nread;
    o->writes = db->nwrite;
    o->syncs = db->nsync;
    o->cache_hits = db->hits;
    o->cache_misses = db->misses;
    o->sql_rows = db->sqlrows;
    o->sql_changed = db->sqlchanged;
    for (pg = db->root; pg && d < ALTSQL_DB_MAXDEPTH; d++) {
        uint32_t fi;
        uint8_t *P;
        if (asd_get(db, pg, &fi)) break;
        P = ASD_PAGE(db, fi);
        pg = P[0] == ASD_BRANCH ? asd_child(P, 0) : 0;
        asd_unpin(db, fi);
    }
    o->depth = d;
    return ALTSQL_OK;
}

typedef struct asd_chk { altsql_db *db; uint8_t *bits; uint32_t npages, leafdepth; uint64_t txn; altsql_db_check_report *r; } asd_chk;

static int asd_mark(asd_chk *k, uint32_t pg) {
    if (pg < 2 || pg >= k->npages || (k->bits[pg >> 3] & (1u << (pg & 7)))) return 0;
    k->bits[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    return 1;
}

static int asd_chk_tree(asd_chk *k, uint32_t pg, uint32_t depth, const uint8_t *lo, uint32_t lon,
                        const uint8_t *hi, uint32_t hin) {
    altsql_db *db = k->db;
    uint32_t fi, n, i;
    uint8_t *P;
    int rc;
    if (depth >= ALTSQL_DB_MAXDEPTH) return ASD_CORRUPT(db, "check: tree too deep");
    if (!asd_mark(k, pg)) return ASD_CORRUPT(db, "check: a tree page is out of range or used twice");
    if ((rc = asd_get(db, pg, &fi)) != 0) return rc;
    P = ASD_PAGE(db, fi);
    n = asd_n(P);
    rc = ALTSQL_OK;
    if (asd_txn(P) > k->txn) { rc = ASD_CORRUPT(db, "check: page newer than its header"); goto out; }
    k->r->tree_pages++;
    if (P[0] == ASD_LEAF) {
        const uint8_t *prev = lo;
        uint32_t prevn = lon;
        if (k->leafdepth == ASD_NONE) k->leafdepth = depth;
        if (k->leafdepth != depth) { rc = ASD_CORRUPT(db, "check: leaves at different depths"); goto out; }
        if (!n && depth) { rc = ASD_CORRUPT(db, "check: empty leaf"); goto out; }
        for (i = 0; i < n; i++) {
            asd_cell c;
            asd_leaf_cell(db, P, i, &c);
            if (prev && asd_cmp(c.k, c.kn, prev, prevn) < (i || !lo ? 1 : 0)) { rc = ASD_CORRUPT(db, "check: keys out of order"); goto out; }
            if (hi && asd_cmp(c.k, c.kn, hi, hin) >= 0) { rc = ASD_CORRUPT(db, "check: key above its separator"); goto out; }
            prev = c.k;
            prevn = c.kn;
            if (c.ov) {
                uint32_t cnt = (c.vn + db->ovd - 1) / db->ovd, j, op = c.ov;
                uint8_t h[ASD_PH];
                for (j = 0; j < cnt; j++) {
                    if (!asd_mark(k, op)) { rc = ASD_CORRUPT(db, "check: an overflow page is out of range or used twice"); goto out; }
                    db->nread++;
                    if (db->f.read(db->f.ctx, (uint64_t)op * db->ps, h, ASD_PH)) { rc = asd_err(db, ALTSQL_IOERR, "read failed"); goto out; }
                    if (h[0] != ASD_OVFL || asd_txn(h) > k->txn) { rc = ASD_CORRUPT(db, "check: damaged overflow chain"); goto out; }
                    op = asd_x(h);
                    k->r->overflow_pages++;
                }
                if (op) { rc = ASD_CORRUPT(db, "check: overflow chain too long"); goto out; }
            }
        }
        k->r->entries += n;
    } else if (P[0] == ASD_BRANCH) {
        for (i = 0; i < n; i++) {
            uint32_t kn, pn;
            const uint8_t *key = asd_bkey(P, i, &kn), *pk = i ? asd_bkey(P, i - 1, &pn) : lo;
            if (!i) pn = lon;
            if (pk && asd_cmp(key, kn, pk, pn) <= 0) { rc = ASD_CORRUPT(db, "check: separators out of order"); goto out; }
            if (hi && asd_cmp(key, kn, hi, hin) >= 0) { rc = ASD_CORRUPT(db, "check: separator above its bound"); goto out; }
        }
        for (i = 0; i <= n; i++) {
            uint32_t ln = lon, hn = hin;
            const uint8_t *l = i ? asd_bkey(P, i - 1, &ln) : lo, *h = i < n ? asd_bkey(P, i, &hn) : hi;
            if ((rc = asd_chk_tree(k, asd_child(P, i), depth + 1, l, ln, h, hn)) != 0) goto out;
        }
    } else rc = ASD_CORRUPT(db, "check: unexpected page in the tree");
out:
    asd_unpin(db, fi);
    return rc;
}

int altsql_db_check(altsql_db *db, int slot, void *mem, size_t mem_size, altsql_db_check_report *rep) {
    asd_chk k;
    asd_hdr x;
    uint32_t ps, pg, i, saved;
    int rc, list;
    if (!db || !mem || !rep || slot < -1 || slot > 1) return ALTSQL_MISUSE;
    if (db->tx == 2) return asd_err(db, ALTSQL_MISUSE, "check runs outside write transactions");
    if (!db->tx && db->reload && (rc = asd_reload(db)) != 0) return rc;
    memset(rep, 0, sizeof *rep);
    if (slot < 0) x = db->com;
    else {
        uint8_t h[ASD_HS];
        db->nread++;
        if (db->f.read(db->f.ctx, (uint64_t)slot * db->ps, h, ASD_HS)) return asd_err(db, ALTSQL_IOERR, "read failed");
        if (!asd_hdr_get(h, &ps, &x) || ps != db->ps || (x.txn & 1) != (uint64_t)slot || x.txn > db->com.txn)
            return asd_err(db, ALTSQL_NOTFOUND, "no valid header in that slot");
    }
    if (mem_size < (size_t)x.npages / 8 + 1) return asd_err(db, ALTSQL_NOMEM, "check needs one bit per page");
    memset(mem, 0, (size_t)x.npages / 8 + 1);
    k.db = db;
    k.bits = (uint8_t *)mem;
    k.npages = x.npages;
    k.leafdepth = ASD_NONE;
    k.txn = x.txn;
    k.r = rep;
    rep->txn = x.txn;
    saved = db->npages;
    if (x.npages > db->npages) db->npages = x.npages;
    rc = ALTSQL_OK;
    if (x.root) {
        rc = asd_chk_tree(&k, x.root, 0, 0, 0, 0, 0);
        rep->depth = k.leafdepth + 1;
    }
    for (list = 0; list < 2 && !rc; list++) {      /* the ready list, then the newest list */
        uint32_t count = 0;
        for (pg = list ? x.nxhead : x.flhead; !rc && pg; ) {
            uint32_t fi, n;
            uint8_t *P;
            if (!asd_mark(&k, pg)) { rc = ASD_CORRUPT(db, "check: a free-list page is out of range or used twice"); break; }
            if ((rc = asd_get(db, pg, &fi)) != 0) break;
            P = ASD_PAGE(db, fi);
            n = asd_n(P);
            if (P[0] != ASD_FREE || asd_txn(P) > x.txn || n > db->epp) { asd_unpin(db, fi); rc = ASD_CORRUPT(db, "check: damaged free list"); break; }
            for (i = 0; i < n && !rc; i++) {
                uint64_t t = as_get64(P + ASD_PH + i * ASD_FLE + 4);
                if (!asd_mark(&k, as_get32(P + ASD_PH + i * ASD_FLE)))
                    rc = ASD_CORRUPT(db, "check: a free page is out of range, used, or listed twice");
                else if (list ? t != x.txn : t && t >= x.txn)
                    rc = ASD_CORRUPT(db, "check: a free page is in the wrong list for when it was freed");
            }
            count += n;
            rep->freelist_pages++;
            pg = asd_x(P);
            asd_unpin(db, fi);
        }
        rep->free_entries += count;
        if (!rc && count != (list ? x.nxcount : x.flcount)) rc = ASD_CORRUPT(db, "check: free-list count differs from the header");
    }
    db->npages = saved;
    if (rc) return rc;
    for (pg = 2; pg < x.npages; pg++)
        if (!(k.bits[pg >> 3] & (1u << (pg & 7)))) return ASD_CORRUPT(db, "check: a page is neither in use nor free");
    return ALTSQL_OK;
}

/* ---- POSIX file port ------------------------------------------------------------------------- */
#ifdef ALTSQL_DB_PORT_FILE
static int asd_px_read(void *ctx, uint64_t off, void *buf, size_t n) {
    altsql_db_posix *p = (altsql_db_posix *)ctx;
    uint8_t *b = (uint8_t *)buf;
    while (n) {
        ssize_t r = pread(p->fd, b, n, (off_t)off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        b += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}
static int asd_px_write(void *ctx, uint64_t off, const void *buf, size_t n) {
    altsql_db_posix *p = (altsql_db_posix *)ctx;
    const uint8_t *b = (const uint8_t *)buf;
    while (n) {
        ssize_t r = pwrite(p->fd, b, n, (off_t)off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        b += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}
static int asd_px_sync(void *ctx) {
    altsql_db_posix *p = (altsql_db_posix *)ctx;
    if (p->nosync) return 0;
#if defined(__APPLE__)
    if (fcntl(p->fd, F_FULLFSYNC) != -1) return 0;
    return fsync(p->fd) ? -1 : 0;
#else
    return fdatasync(p->fd) ? -1 : 0;
#endif
}
static int asd_px_size(void *ctx, uint64_t *size) {
    struct stat st;
    if (fstat(((altsql_db_posix *)ctx)->fd, &st)) return -1;
    *size = (uint64_t)st.st_size;
    return 0;
}
static int asd_px_trunc(void *ctx, uint64_t size) {
    return ftruncate(((altsql_db_posix *)ctx)->fd, (off_t)size) ? -1 : 0;
}
int altsql_db_posix_open(altsql_db_file *f, altsql_db_posix *p, const char *path, int create) {
    struct flock l;
    if (!f || !p || !path) return ALTSQL_MISUSE;
    p->nosync = 0;
    p->fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0644);
    if (p->fd < 0) return errno == ENOENT ? ALTSQL_NOTFOUND : ALTSQL_IOERR;
    memset(&l, 0, sizeof l);
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    if (fcntl(p->fd, F_SETLK, &l) == -1) { close(p->fd); p->fd = -1; return ALTSQL_DB_BUSY; }
    f->ctx = p;
    f->read = asd_px_read;
    f->write = asd_px_write;
    f->sync = asd_px_sync;
    f->size = asd_px_size;
    f->truncate = asd_px_trunc;
    return ALTSQL_OK;
}
void altsql_db_posix_close(altsql_db_posix *p) {
    if (p && p->fd >= 0) { close(p->fd); p->fd = -1; }
}
#endif

/* ---- RAM file port: power cuts and failing calls, for tests and demos ---------------------- */
#ifdef ALTSQL_DB_PORT_RAM
static uint32_t asd_rng(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static int asd_ram_tick(altsql_db_ram *r, int kind) {
    r->calls++;
    if (r->dead) return -1;
    if (r->fail > 0 && --r->fail == 0) { r->fail_kind = kind; r->fails++; return -1; }
    return 0;
}

/* Log record: offset (8), length (4), kind (4: 0 write, 1 truncate, 2 cut write), data. */
static int asd_ram_log(altsql_db_ram *r, uint64_t off, uint64_t len, uint32_t kind, const void *data) {
    uint64_t need = 16 + (kind == 1 ? 0 : len);
    uint8_t *p;
    if (r->logused + need > r->logcap) { r->overflow = 1; return -1; }
    p = r->log + r->logused;
    as_put64(p, off);
    as_put32(p + 8, (uint32_t)len);
    as_put32(p + 12, kind);
    if (kind != 1 && len) memcpy(p + 16, data, (size_t)len);
    r->logused += need;
    return 0;
}

static int asd_ram_read(void *ctx, uint64_t off, void *buf, size_t n) {
    altsql_db_ram *r = (altsql_db_ram *)ctx;
    if (asd_ram_tick(r, 1) || off + n > r->size) return -1;
    memcpy(buf, r->mem + off, n);
    return 0;
}
static int asd_ram_write(void *ctx, uint64_t off, const void *buf, size_t n) {
    altsql_db_ram *r = (altsql_db_ram *)ctx;
    if (asd_ram_tick(r, 2) || off + n > r->cap) return -1;
    r->writes++;
    if (r->cut > 0 && --r->cut == 0) {           /* the power goes during this write */
        asd_ram_log(r, off, n, 2, buf);
        r->dead = 1;
        return -1;
    }
    if (asd_ram_log(r, off, n, 0, buf)) return -1;
    memcpy(r->mem + off, buf, n);
    if (off + n > r->size) r->size = off + n;
    return 0;
}
static int asd_ram_sync(void *ctx) {
    altsql_db_ram *r = (altsql_db_ram *)ctx;
    uint64_t pos = 0;
    if (asd_ram_tick(r, 3)) return -1;
    r->syncs++;
    while (pos < r->logused) {
        uint8_t *p = r->log + pos;
        uint64_t off = as_get64(p);
        uint32_t len = as_get32(p + 8), kind = as_get32(p + 12);
        if (kind == 1) {
            if (off < r->dsize) memset(r->disk + off, 0, (size_t)(r->dsize - off));
            r->dsize = off;
            pos += 16;
        } else {
            memcpy(r->disk + off, p + 16, len);
            if (off + len > r->dsize) r->dsize = off + len;
            pos += 16 + len;
        }
    }
    r->logused = 0;
    return 0;
}
static int asd_ram_size(void *ctx, uint64_t *size) {
    altsql_db_ram *r = (altsql_db_ram *)ctx;
    if (asd_ram_tick(r, 4)) return -1;
    *size = r->size;
    return 0;
}
static int asd_ram_trunc(void *ctx, uint64_t size) {
    altsql_db_ram *r = (altsql_db_ram *)ctx;
    if (asd_ram_tick(r, 5) || size > r->cap) return -1;
    if (asd_ram_log(r, size, 0, 1, 0)) return -1;
    if (size < r->size) memset(r->mem + size, 0, (size_t)(r->size - size));
    r->size = size;
    return 0;
}

void altsql_db_ram_init(altsql_db_file *f, altsql_db_ram *r, uint8_t *mem, uint8_t *disk,
                        uint64_t cap, uint8_t *log, uint64_t logcap) {
    memset(r, 0, sizeof *r);
    r->mem = mem; r->disk = disk; r->cap = cap; r->log = log; r->logcap = logcap;
    memset(mem, 0, (size_t)cap);
    memset(disk, 0, (size_t)cap);
    f->ctx = r;
    f->read = asd_ram_read;
    f->write = asd_ram_write;
    f->sync = asd_ram_sync;
    f->size = asd_ram_size;
    f->truncate = asd_ram_trunc;
}

void altsql_db_ram_powercut(altsql_db_ram *r, uint32_t seed) {
    uint64_t pos = 0, size = r->dsize, top = r->size > r->dsize ? r->size : r->dsize;
    uint32_t s = seed * 2654435761u + 1;
    memcpy(r->mem, r->disk, (size_t)r->dsize);
    if (top > r->dsize) memset(r->mem + r->dsize, 0, (size_t)(top - r->dsize));
    while (pos < r->logused) {
        uint8_t *p = r->log + pos;
        uint64_t off = as_get64(p);
        uint32_t len = as_get32(p + 8), kind = as_get32(p + 12), j;
        if (kind == 1) {
            if (asd_rng(&s) & 1) {
                if (off < size) memset(r->mem + off, 0, (size_t)(size - off));
                size = off;
            }
            pos += 16;
            continue;
        }
        if (kind == 0 && (asd_rng(&s) & 1)) {
            memcpy(r->mem + off, p + 16, len);
            if (off + len > size) size = off + len;
        } else if (kind == 2) {
            for (j = 0; j < len; j += 512) {     /* some sectors of the cut write land */
                uint32_t n = len - j < 512 ? len - j : 512;
                if (asd_rng(&s) & 1) {
                    if (off + j > size) memset(r->mem + size, 0, (size_t)(off + j - size));
                    memcpy(r->mem + off + j, p + 16 + j, n);
                    if (off + j + n > size) size = off + j + n;
                }
            }
        }
        pos += 16 + len;
    }
    r->size = size;
    memcpy(r->disk, r->mem, (size_t)size);
    if (r->dsize > size) memset(r->disk + size, 0, (size_t)(r->dsize - size));
    r->dsize = size;
    r->logused = 0;
    r->dead = 0;
    r->cut = 0;
}
#endif

#endif /* ALTSQL_DB_IMPLEMENTATION */
