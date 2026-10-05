/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* altsql-db: command-line shell for AltSql DB, the gateway database kept in one file.
 *
 *   altsql-db FILE                   interactive shell; creates FILE when it does not exist
 *   altsql-db FILE ARG [ARG ...]     run each ARG (SQL or a dot command) in order, then exit
 *   altsql-db --version
 *
 * Without ARGs the shell reads stdin, line by line. End SQL with ';'. Dot commands: .help
 * Exit status 1 when a command failed; with ARGs the first failure ends the run. */
#define ALTSQL_IMPLEMENTATION
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "altsql.h"
#define ALTSQL_DB_IMPLEMENTATION
#define ALTSQL_DB_PORT_FILE
#include "altsql_db.h"

enum { CMD_OK = 0, CMD_FAIL = 1, CMD_QUIT = 2 };
#define MAXWORDS (3 + ALTSQL_DB_MAXCOLS + 1)

static altsql_db *db;
static uint8_t work[8u << 20];          /* working memory for the engine */

/* ---- errors ----------------------------------------------------------------------- */
/* An error line on stderr, after everything printed so far; returns CMD_FAIL. */
static int fail(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return CMD_FAIL;
}

/* Reports a result code of the engine; CMD_FAIL for an error or a key that is not there. */
static int report(int rc) {
    const char *m;
    if (rc == ALTSQL_NOTFOUND) {
        fflush(stdout);
        fputs("not found\n", stderr);
        return CMD_FAIL;
    }
    if (rc >= 0) return CMD_OK;
    m = altsql_db_errmsg(db);
    return m && *m ? fail("%s", m) : fail("code %d", rc);
}

/* ---- printing values, as core/tools/cli.c does ------------------------------------ */
static void print_real(FILE *f, double r) {
    char b[40];
    int p;
    if (r >= -3.4e38 && r <= 3.4e38 && (double)(float)r == r) {       /* a float column */
        for (p = 6; p <= 9; p++) {
            snprintf(b, sizeof b, "%.*g", p, r);
            if ((float)strtod(b, NULL) == (float)r) break;
        }
    } else {
        snprintf(b, sizeof b, "%.15g", r);
    }
    if (!strpbrk(b, ".eEn")) strcat(b, ".0");
    fputs(b, f);
}

static void print_value(FILE *f, const altsql_value *v) {
    switch (v->type) {
    case ALTSQL_NULL: fputs("NULL", f); break;
    case ALTSQL_INTEGER: fprintf(f, "%lld", (long long)v->u.i); break;
    case ALTSQL_REAL: print_real(f, v->u.r); break;
    default: fwrite(v->u.s, 1, (size_t)v->len, f);
    }
}

typedef struct { long rows; } result;

/* A header line of column names before the first row, then each row, values joined by '|'. */
static int row_cb(void *ctx, int n, const altsql_value *v, const char *const *names) {
    result *r = (result *)ctx;
    int i;
    if (r->rows++ == 0 && names) {
        for (i = 0; i < n; i++) printf("%s%s", i ? "|" : "", names[i]);
        printf("\n");
    }
    for (i = 0; i < n; i++) {
        if (i) putchar('|');
        print_value(stdout, &v[i]);
    }
    putchar('\n');
    return 0;
}

/* ---- reading what the user typed ---------------------------------------------------- */
/* Cuts a line into words, in place. A word may be quoted with ' or ". The word at index
 * rest (-1: none) is the whole rest of the line. Returns the number of words, or -1 when
 * there are more than max. */
static int split(char *s, char **w, int max, int rest) {
    int n = 0;
    for (;;) {
        char q = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) return n;
        if (n == max) return -1;
        if (n == rest) {
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            w[n++] = s;
            return n;
        }
        if (*s == '\'' || *s == '"') q = *s++;
        w[n++] = s;
        while (*s && (q ? *s != q : (*s != ' ' && *s != '\t'))) s++;
        if (*s) *s++ = 0;
    }
}

/* A whole number in decimal. */
static int getnum(const char *s, int64_t *out) {
    char *e;
    errno = 0;
    *out = (int64_t)strtoll(s, &e, 10);
    return *s && !*e && !errno;
}

/* The names of the column types, as altsql_db_tableinfo numbers them. */
static const char *const tname[] = { "?", "time", "int", "long", "float", "real", "text" };

/* A word as a value, the way its column wants it: time, int and long as whole numbers,
 * float and real as doubles, text as it is. Returns nonzero (after an error line) when
 * the word is not a number. */
static int parse_value(const char *s, int type, altsql_value *v) {
    char *e = NULL;
    memset(v, 0, sizeof *v);
    if (type == 6) {
        v->type = ALTSQL_TEXT;
        v->u.s = s;
        v->len = (int)strlen(s);
        return 0;
    }
    errno = 0;
    if (type >= 4) {
        v->type = ALTSQL_REAL;
        v->u.r = strtod(s, &e);
    } else {
        v->type = ALTSQL_INTEGER;
        v->u.i = (int64_t)strtoll(s, &e, 10);
    }
    if (e == s || *e || errno) return fail("%s is not a number", s);
    return 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *p = NULL, *q;
    size_t cap = 0, n = 0, k;
    if (!f) return NULL;
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            if (!(q = (char *)realloc(p, cap))) { free(p); fclose(f); return NULL; }
            p = q;
        }
        k = fread(p + n, 1, cap - n, f);
        n += k;
    } while (k > 0);
    if (ferror(f)) { free(p); p = NULL; }
    fclose(f);
    *len = n;
    return p;
}

/* ---- tables ------------------------------------------------------------------------ */
static int print_table(void *ctx, const char *name, int kind) {
    (void)ctx;
    printf("%s%s\n", name, kind == ALTSQL_DB_SYNCED ? " (synced)" : "");
    return 0;
}

/* The name of table number want in the catalog. */
typedef struct { int want, seen; char name[40]; } pick;

static int pick_table(void *ctx, const char *name, int kind) {
    pick *p = (pick *)ctx;
    (void)kind;
    if (p->seen++ == p->want) snprintf(p->name, sizeof p->name, "%s", name);
    return 0;
}

static int do_schema(const char *name) {
    altsql_db_tableinfo ti;
    int i, rc = altsql_db_table_info(db, name, &ti);
    if (rc) return report(rc);
    printf("%s (", name);
    for (i = 0; i < ti.ncols; i++)
        printf("%s%s:%s", i ? "," : "", ti.names[i], tname[(unsigned)ti.types[i] <= 6 ? ti.types[i] : 0]);
    printf(") key (");
    for (i = 0; i < ti.nkey; i++) printf("%s%s", i ? "," : "", ti.names[ti.key[i]]);
    printf(")%s\n", ti.kind == ALTSQL_DB_SYNCED ? " synced" : "");
    for (i = 0; i < ti.nindex; i++) {
        int j;
        printf("  %sindex %s (", ti.index[i].unique ? "unique " : "", ti.index[i].name);
        for (j = 0; j < ti.index[i].ncols; j++) printf("%s%s", j ? "," : "", ti.names[ti.index[i].cols[j]]);
        printf(")\n");
    }
    return CMD_OK;
}

/* Every table: the catalog is listed again for each, so no list has to be kept. */
static int do_schema_all(void) {
    pick p;
    int i;
    for (i = 0;; i++) {
        p.want = i;
        p.seen = 0;
        if (report(altsql_db_tables(db, pick_table, &p))) return CMD_FAIL;
        if (p.seen <= i) return CMD_OK;
        if (do_schema(p.name)) return CMD_FAIL;
    }
}

/* .row get|put|insert|del TABLE VALUE ...: a is the word after .row, n the words from there. */
static int do_row(char **a, int n) {
    altsql_db_tableinfo ti;
    altsql_value val[ALTSQL_DB_MAXCOLS];
    int i, rc, want, get = !strcmp(a[0], "get"), del = !strcmp(a[0], "del"), keyed;
    if (!get && !del && strcmp(a[0], "put") && strcmp(a[0], "insert"))
        return fail("unknown or incomplete command: .row %s (try .help)", a[0]);
    if ((rc = altsql_db_table_info(db, a[1], &ti)) != 0) return report(rc);
    keyed = get || del;                                    /* key values, not whole rows */
    want = keyed ? ti.nkey : ti.ncols;
    if (n - 2 != want) return fail("%s takes %d %s%s", a[1], want, keyed ? "key value" : "value", want == 1 ? "" : "s");
    for (i = 0; i < want; i++)
        if (parse_value(a[2 + i], ti.types[keyed ? ti.key[i] : i], &val[i])) return CMD_FAIL;
    if (get) {
        altsql_value row[ALTSQL_DB_MAXCOLS];
        char nm[ALTSQL_DB_MAXCOLS][32];                    /* the names, copied: they last one call */
        const char *np[ALTSQL_DB_MAXCOLS];
        result r = { 0 };
        for (i = 0; i < ti.ncols; i++) {
            snprintf(nm[i], sizeof nm[i], "%s", ti.names[i]);
            np[i] = nm[i];
        }
        if ((rc = altsql_db_row_get(db, a[1], val, want, row, ALTSQL_DB_MAXCOLS)) != 0) return report(rc);
        row_cb(&r, ti.ncols, row, np);
        return CMD_OK;
    }
    if (del) rc = altsql_db_row_del(db, a[1], val, want);
    else if (!strcmp(a[0], "put")) rc = altsql_db_row_put(db, a[1], val, want);
    else rc = altsql_db_row_insert(db, a[1], val, want);
    return report(rc);
}

/* ---- buckets: the direct path ----------------------------------------------------- */
static int do_get(const char *bucket, const char *key) {
    static char small[65536];
    char *buf = small, *big = NULL;
    size_t vn = 0;
    uint32_t b;
    int rc = altsql_db_bucket(db, bucket, 0, &b);
    if (rc) return report(rc);
    rc = altsql_db_get(db, b, key, strlen(key), buf, sizeof small, &vn);
    if (rc == ALTSQL_DB_SHORT && (big = (char *)malloc(vn)) != NULL) {     /* a longer value */
        buf = big;
        rc = altsql_db_get(db, b, key, strlen(key), buf, vn, &vn);
    }
    if (rc == 0) {
        fwrite(buf, 1, vn, stdout);
        putchar('\n');
    }
    free(big);
    return report(rc);
}

static int do_put(const char *bucket, const char *key, const char *val) {
    uint32_t b;
    int rc = altsql_db_bucket(db, bucket, 1, &b);
    if (rc) return report(rc);
    return report(altsql_db_put(db, b, key, strlen(key), val, strlen(val)));
}

static int do_del(const char *bucket, const char *key) {
    uint32_t b;
    int rc = altsql_db_bucket(db, bucket, 0, &b);
    if (rc) return report(rc);
    return report(altsql_db_del(db, b, key, strlen(key)));
}

static int do_keys(const char *bucket) {
    altsql_db_cursor c;
    const void *k;
    size_t kn;
    uint32_t b;
    int rc = altsql_db_bucket(db, bucket, 0, &b);
    if (rc) return report(rc);
    for (rc = altsql_db_seek(&c, db, b, NULL, 0); rc == ALTSQL_OK; rc = altsql_db_next(&c)) {
        if ((rc = altsql_db_key(&c, &k, &kn)) != 0) break;
        fwrite(k, 1, kn, stdout);
        putchar('\n');
    }
    return rc == ALTSQL_NOTFOUND ? CMD_OK : report(rc);      /* the end of the bucket is not an error */
}

/* ---- sync, info, check --------------------------------------------------------------- */
static int do_sync(const char *device, const char *after, const char *path) {
    int64_t d, a;
    uint32_t last = 0;
    size_t len = 0;
    char *batch;
    int rc;
    if (!getnum(device, &d) || !getnum(after, &a) || a < 0 || a > (int64_t)UINT32_MAX)
        return fail("DEVICE and AFTER_SEQ must be whole numbers");
    if (!(batch = read_file(path, &len))) return fail("cannot read %s: %s", path, strerror(errno));
    rc = altsql_db_sync_apply(db, d, (uint32_t)a, batch, len, &last);
    free(batch);
    if (rc == ALTSQL_DB_GAP) {                              /* not an error: the device sends again */
        printf("gap: send again from %lu\n", (unsigned long)last);
        return CMD_OK;
    }
    if (rc) return report(rc);
    printf("applied, position %lu\n", (unsigned long)last);
    return CMD_OK;
}

static int do_state(const char *device) {
    int64_t d;
    uint32_t last;
    int rc;
    if (!getnum(device, &d)) return fail("DEVICE must be a whole number");
    if ((rc = altsql_db_sync_state(db, d, &last)) != 0) return report(rc);
    printf("%lu\n", (unsigned long)last);
    return CMD_OK;
}

static int do_info(void) {
    altsql_db_info i;
    int rc = altsql_db_info_get(db, &i);
    if (rc) return report(rc);
#define F(f) printf("%-13s %llu\n", #f, (unsigned long long)i.f)
    F(page_size); F(pages); F(free_pages); F(depth); F(cache_pages); F(txn); F(mem_used);
    F(reads); F(writes); F(syncs); F(cache_hits); F(cache_misses); F(sql_rows); F(sql_changed);
#undef F
    return CMD_OK;
}

static int do_check(void) {
    altsql_db_info i;
    altsql_db_check_report rep;
    size_t room;
    void *bits;
    int rc = altsql_db_info_get(db, &i);
    if (rc) return report(rc);
    room = (size_t)i.pages / 8 + 1;                        /* one bit for each page of the file */
    if (!(bits = malloc(room))) return fail("out of memory");
    rc = altsql_db_check(db, -1, bits, room, &rep);
    free(bits);
    if (rc) return report(rc);
    printf("ok, %llu entries, %lu pages", (unsigned long long)rep.entries, (unsigned long)i.pages);
    if (rep.indexes) printf(", %lu %s with %llu entries, each one's row there", (unsigned long)rep.indexes,
                            rep.indexes == 1 ? "index" : "indexes", (unsigned long long)rep.index_entries);
    printf("\n");
    return CMD_OK;
}

/* ---- dot commands --------------------------------------------------------------------- */
static void version(void) {
    printf("altsql-db %s (AltSql Core %s)\n", ALTSQL_DB_VERSION, ALTSQL_VERSION);
}

static void help(void) {
    printf(".help                           list the commands\n"
           ".quit                           leave\n"
           ".exit                           leave\n"
           ".version                        show the versions\n"
           ".tables                         list the tables; (synced) marks those filled by sync\n"
           ".schema [TABLE]                 columns with their types, the primary key and the indexes\n"
           ".info                           the file, the cache and the memory in figures\n"
           ".check                          check every page of the file, and each index against its table\n"
           ".begin                          start a write transaction\n"
           ".commit                         make the transaction final\n"
           ".rollback                       undo the transaction\n"
           ".get BUCKET KEY                 read a key (the direct path, no SQL)\n"
           ".put BUCKET KEY VALUE           set a key; the bucket is created\n"
           ".del BUCKET KEY                 delete a key\n"
           ".keys BUCKET                    list the keys of a bucket\n"
           ".row get TABLE K1 [K2 ...]      read a row by its primary key, with no SQL step\n"
           ".row put TABLE V1 V2 ...        insert or replace a row, values in column order\n"
           ".row insert TABLE V1 V2 ...     insert a row; refuse a key that is already there\n"
           ".row del TABLE K1 [K2 ...]      delete a row by its primary key\n"
           ".drop TABLE                     drop a table with its rows\n"
           ".sync DEVICE AFTER_SEQ FILE     apply a sync batch file that a Core device wrote\n"
           ".state DEVICE                   the last sequence number applied for a device\n"
           "SQL: CREATE TABLE, DROP TABLE, CREATE [UNIQUE] INDEX, DROP INDEX, INSERT, UPDATE, DELETE,\n"
           "     SELECT ... WHERE / GROUP BY / HAVING / ORDER BY / LIMIT, EXPLAIN SELECT; end with ';'.\n"
           "     Words with spaces: 'like this' or \"this\".\n");
}

/* Runs one dot command (the line is cut up in place). CMD_QUIT for .quit. */
static int dot(char *line) {
    char *w[MAXWORDS];
    int n, rest;
    line[strcspn(line, "\r\n")] = 0;
    rest = (!strncmp(line, ".put ", 5) || !strncmp(line, ".put\t", 5)) ? 3 : -1;    /* the value: the rest of the line */
    if ((n = split(line, w, MAXWORDS, rest)) < 0) return fail("too many words in the command");
    if (n == 0) return CMD_OK;
    if (!strcmp(w[0], ".quit") || !strcmp(w[0], ".exit")) return CMD_QUIT;
    if (!strcmp(w[0], ".help") && n == 1) { help(); return CMD_OK; }
    if (!strcmp(w[0], ".version") && n == 1) { version(); return CMD_OK; }
    if (!strcmp(w[0], ".tables") && n == 1) return report(altsql_db_tables(db, print_table, NULL));
    if (!strcmp(w[0], ".schema") && n <= 2) return n == 2 ? do_schema(w[1]) : do_schema_all();
    if (!strcmp(w[0], ".info") && n == 1) return do_info();
    if (!strcmp(w[0], ".check") && n == 1) return do_check();
    if (!strcmp(w[0], ".begin") && n == 1) return report(altsql_db_begin(db, 1));
    if (!strcmp(w[0], ".commit") && n == 1) return report(altsql_db_commit(db));
    if (!strcmp(w[0], ".rollback") && n == 1) return report(altsql_db_rollback(db));
    if (!strcmp(w[0], ".get") && n == 3) return do_get(w[1], w[2]);
    if (!strcmp(w[0], ".put") && (n == 3 || n == 4)) return do_put(w[1], w[2], n == 4 ? w[3] : "");
    if (!strcmp(w[0], ".del") && n == 3) return do_del(w[1], w[2]);
    if (!strcmp(w[0], ".keys") && n == 2) return do_keys(w[1]);
    if (!strcmp(w[0], ".row") && n >= 4) return do_row(w + 1, n - 1);
    if (!strcmp(w[0], ".drop") && n == 2) return report(altsql_db_table_drop(db, w[1]));
    if (!strcmp(w[0], ".sync") && n == 4) return do_sync(w[1], w[2], w[3]);
    if (!strcmp(w[0], ".state") && n == 2) return do_state(w[1]);
    return fail("unknown or incomplete command: %s (try .help)", w[0]);
}

/* ---- SQL and the read loop ----------------------------------------------------------- */
static int run_sql(const char *sql) {
    result r = { 0 };
    return report(altsql_db_exec(db, sql, row_cb, &r));
}

/* A statement is complete when it ends with ';' outside a string. */
static int complete(const char *s) {
    int inq = 0;
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == '\'') inq = !inq;
        if (!inq && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') last = s;
    }
    return last && *last == ';' && !inq;
}

static int blank(const char *s) {
    return !s[strspn(s, " \t\r\n")];
}

/* Reads stdin line by line. Returns 1 when a command failed. */
static int shell(void) {
    static char line[65536];
    char *buf = NULL, *q;
    size_t len = 0;
    int tty = isatty(0), status = 0, r;
    if (tty) printf("AltSql DB %s. End SQL with ';'. Type .help for commands.\n", ALTSQL_DB_VERSION);
    for (;;) {
        size_t n;
        if (tty) {
            fputs(len ? "   ...> " : "altsql-db> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, stdin)) {
            if (tty) putchar('\n');                       /* end of input at a prompt */
            break;
        }
        n = strlen(line);
        if (!len && line[0] == '.') {                     /* a dot command is one line */
            if (n == sizeof line - 1 && line[n - 1] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                status |= fail("the line is too long");
                continue;
            }
            r = dot(line);
            if (r == CMD_QUIT) break;
            status |= r;
            continue;
        }
        if (!len && blank(line)) continue;
        if (!(q = (char *)realloc(buf, len + n + 1))) { free(buf); return fail("out of memory"); }
        buf = q;
        memcpy(buf + len, line, n + 1);
        len += n;
        if (complete(buf)) {
            status |= run_sql(buf);
            len = 0;
        }
    }
    if (len && !blank(buf)) status |= run_sql(buf);      /* SQL left without its ';' */
    free(buf);
    return status;
}

static int usage(void) {
    fprintf(stderr, "usage: altsql-db FILE                 interactive shell; creates FILE when it is missing\n"
                    "       altsql-db FILE ARG [ARG ...]   run each ARG (SQL or a dot command), then exit\n"
                    "       altsql-db --version\n");
    return 2;
}

int main(int argc, char **argv) {
    altsql_db_posix px;
    altsql_db_file file;
    altsql_db_config cfg;
    int i, rc, status = 0;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        version();
        return 0;
    }
    if (argc < 2 || argv[1][0] == '-') return usage();     /* no file named like an option is made by mistake */
    rc = altsql_db_posix_open(&file, &px, argv[1], 1);
    if (rc == ALTSQL_DB_BUSY) return fail("%s is in use by another process", argv[1]);
    if (rc) return fail("cannot open %s: %s", argv[1], strerror(errno));
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = work;
    cfg.mem_size = sizeof work;
    cfg.create = 1;                                        /* page_size 0: the default */
    rc = altsql_db_open(&db, &file, &cfg);
    if (rc) {
        fail("cannot open %s: %s", argv[1], db ? altsql_db_errmsg(db) : "bad settings");
        altsql_db_posix_close(&px);
        return 1;
    }
    if (argc > 2) {
        for (i = 2; i < argc && !status; i++) {
            rc = argv[i][0] == '.' ? dot(argv[i]) : run_sql(argv[i]);
            if (rc == CMD_QUIT) break;
            status = rc;
        }
    } else {
        status = shell();
    }
    altsql_db_close(db);                                   /* rolls back a transaction left open */
    altsql_db_posix_close(&px);
    fflush(stdout);
    return status;
}
