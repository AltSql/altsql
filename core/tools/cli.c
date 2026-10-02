/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* altsql: command-line shell for AltSql databases kept in a file.
 *
 *   altsql new FILE [sectors [sector_size [align]]]   create an empty database
 *   altsql FILE                                        interactive shell
 *   altsql FILE "SELECT ..."                           run SQL or a dot command
 *
 * In the shell, end SQL with ';'. Dot commands: .help                      */
#define ALTSQL_IMPLEMENTATION
#define ALTSQL_PORT_FILE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "altsql.h"

static altsql *db;
static altsql_flash fl;

/* ---- printing values ------------------------------------------------------------ */
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

/* ---- dot commands ---------------------------------------------------------------- */
static int series_cb(void *ctx, const char *name, const char *schema) {
    const char *want = (const char *)ctx;
    if (!want || strcmp(want, name) == 0) printf("%s (%s)\n", name, schema);
    return 0;
}

static int file_writer(void *ctx, const char *data, size_t len) {
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : 1;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    size_t cap = 0, n = 0, k;
    if (!f) return NULL;
    do {
        if (n == cap) { cap = cap ? cap * 2 : 65536; p = (char *)realloc(p, cap); }
        k = fread(p + n, 1, cap - n, f);
        n += k;
    } while (k > 0);
    fclose(f);
    *len = n;
    return p;
}

static void report(int rc) {
    if (rc < 0) fprintf(stderr, "error %d: %s\n", rc, altsql_errmsg(db));
    else if (rc == ALTSQL_NOTFOUND) fprintf(stderr, "not found\n");
}

static void help(void) {
    printf(".tables                 list tables (time-series) and the kv table\n"
           ".schema [NAME]          show table definitions\n"
           ".info                   storage and memory use\n"
           ".put KEY VALUE          set a key\n"
           ".get KEY                read a key\n"
           ".del KEY                delete a key\n"
           ".export [FILE]          write the database as text (default: screen)\n"
           ".import FILE            read text written by .export\n"
           ".quit                   leave\n"
           "SQL: CREATE TABLE, INSERT, SELECT ... WHERE / GROUP BY / HAVING / ORDER BY / LIMIT;\n");
}

static int dot(char *line) {
    char *cmd = strtok(line, " \t\r\n"), *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, "\r\n");
    int rc = 0;
    if (!cmd) return 0;
    if (!strcmp(cmd, ".quit") || !strcmp(cmd, ".exit")) return 1;
    if (!strcmp(cmd, ".help")) help();
    else if (!strcmp(cmd, ".tables")) { rc = altsql_series_each(db, series_cb, NULL); printf("kv (key, value)\n"); }
    else if (!strcmp(cmd, ".schema")) rc = altsql_series_each(db, series_cb, a);
    else if (!strcmp(cmd, ".info")) {
        altsql_info i;
        altsql_info_get(db, &i);
        printf("sectors %u x %u bytes: %u in use, %u free\nnewest record #%u, %u keys indexed\n"
               "since open: %u sectors reclaimed, %u rows rolled over\nengine memory %u bytes\n",
               i.sectors, i.sector_size, i.used_sectors, i.free_sectors, i.last_seq, i.kv_keys,
               i.gc_runs, i.rows_dropped, i.mem_used);
    } else if (!strcmp(cmd, ".put") && a) rc = altsql_put(db, a, b ? b : "", b ? strlen(b) : 0);
    else if (!strcmp(cmd, ".get") && a) {
        static char v[65536];
        size_t n;
        rc = altsql_get(db, a, v, sizeof v, &n);
        if (!rc) { fwrite(v, 1, n < sizeof v ? n : sizeof v, stdout); putchar('\n'); }
    } else if (!strcmp(cmd, ".del") && a) rc = altsql_del(db, a);
    else if (!strcmp(cmd, ".export")) {
        FILE *f = a ? fopen(a, "wb") : stdout;
        if (!f) { perror(a); return 0; }
        rc = altsql_export(db, file_writer, f);
        if (a) fclose(f);
    } else if (!strcmp(cmd, ".import") && a) {
        size_t n;
        char *text = read_file(a, &n);
        if (!text) { perror(a); return 0; }
        rc = altsql_import(db, text, n);
        free(text);
    } else {
        fprintf(stderr, "unknown or incomplete command: %s (try .help)\n", cmd);
    }
    report(rc);
    return 0;
}

static void run_sql(const char *sql) {
    result r = { 0 };
    report(altsql_exec(db, sql, row_cb, &r));
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

static void shell(void) {
    char line[4096], *buf = NULL;
    size_t len = 0;
    int tty = isatty(0);
    if (tty) printf("AltSql %s. End SQL with ';'. Type .help for commands.\n", ALTSQL_VERSION);
    for (;;) {
        size_t n;
        if (tty) { fputs(len ? "   ...> " : "altsql> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        if (!len && line[0] == '.') { if (dot(line)) break; continue; }
        n = strlen(line);
        buf = (char *)realloc(buf, len + n + 1);
        memcpy(buf + len, line, n + 1);
        len += n;
        if (complete(buf)) { run_sql(buf); len = 0; }
    }
    if (len) run_sql(buf);
    free(buf);
}

static int usage(void) {
    fprintf(stderr, "usage: altsql new FILE [sectors [sector_size [align]]]\n"
                    "       altsql FILE [\"SQL or .command\"]\n");
    return 2;
}

int main(int argc, char **argv) {
    altsql_config cfg;
    size_t memsz = 32u << 20;
    int rc, creating = argc >= 3 && !strcmp(argv[1], "new");
    const char *path;
    if (argc < 2) return usage();
    path = creating ? argv[2] : argv[1];
    if (creating) {
        uint32_t sc = argc > 3 ? (uint32_t)atol(argv[3]) : 256;
        uint32_t ss = argc > 4 ? (uint32_t)atol(argv[4]) : 4096;
        uint32_t al = argc > 5 ? (uint32_t)atol(argv[5]) : 4;
        if (access(path, F_OK) == 0) { fprintf(stderr, "%s already exists\n", path); return 1; }
        if (altsql_file_flash_open(&fl, path, ss, sc, al, 1)) { perror(path); return 1; }
    } else if (altsql_file_flash_open(&fl, path, 0, 0, 0, 0)) {
        fprintf(stderr, "cannot open %s (create it with: altsql new %s)\n", path, path);
        return 1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = malloc(memsz);
    cfg.mem_size = memsz;
    cfg.kv_slots = 1u << 16;
    cfg.max_series = 64;
    cfg.max_record = 4096;
    cfg.create = (uint8_t)creating;
    rc = altsql_open(&db, &fl, &cfg);
    if (rc) {
        fprintf(stderr, "cannot open database: %s\n", db ? altsql_errmsg(db) : "bad settings");
        return 1;
    }
    if (creating) {
        printf("created %s: %u sectors of %u bytes\n", path, fl.sector_count, fl.sector_size);
    } else if (argc > 2) {
        char *cmd = argv[2];
        if (cmd[0] == '.') dot(cmd);
        else run_sql(cmd);
    } else {
        shell();
    }
    altsql_close(db);
    altsql_file_flash_close(&fl);
    free(cfg.mem);
    return 0;
}
