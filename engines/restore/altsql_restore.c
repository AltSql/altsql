/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* altsql-restore: AltSql Restore, the backup restore verifier.
 *
 *   altsql-restore [options] BACKUP           restore BACKUP into a sandbox, count, compare, report
 *   altsql-restore --list [--job NAME]        the runs in the history
 *   altsql-restore --verify-report FILE       check a report's seal
 *   altsql-restore --help | --version
 *
 * BACKUP is a PostgreSQL dump (pg_dump plain or custom format), a MySQL or MariaDB dump
 * (mysqldump), a SQLite database file (or a .backup copy of one), a tar archive, or a
 * folder; any file may be gzip-compressed. Exit status: 0 verified, 1 alert (the restore
 * failed, or data shrank past the threshold), 2 usage or internal error, 3 not verified (no
 * engine to restore into). The history of runs is kept in an AltSql DB file.
 *
 * One program from one C file: this file includes AltSql Core, AltSql DB and the parts
 * in src/. Build: make -C engines/restore (needs only a C99 compiler and POSIX). */
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_IMPLEMENTATION
#define ALTSQL_DB_PORT_FILE
#include "altsql_db.h"

#include "src/common.c"
#include "src/sha256.c"
#include "src/gz.c"
#include "src/tar.c"
#include "src/sqlite.c"
#include "src/sandbox.c"
#include "src/engines.c"

#define ALTSQL_RESTORE_VERSION ALTSQL_DB_VERSION

enum { EXIT_VERIFIED = 0, EXIT_ALERT = 1, EXIT_USAGE = 2, EXIT_NOTVERIFIED = 3 };

/* ---- options ----------------------------------------------------------------------------- */
typedef struct opts {
    const char *backup, *job, *kind, *history, *workdir, *docker, *pg_image, *my_image;
    const char *report, *report_dir, *signer, *sign_key, *webhook, *verify, *list_job;
    double      max_shrink, timeout;
    int         mode, accept, webhook_always, end_check, quiet, list;
} opts;

static void usage(FILE *f) {
    fprintf(f,
"altsql-restore %s: AltSql Restore, a backup restore verifier.\n"
"\n"
"  altsql-restore [options] BACKUP\n"
"  altsql-restore --list [--job NAME] [--history FILE]\n"
"  altsql-restore --verify-report FILE [--sign-key FILE]\n"
"\n"
"BACKUP: a PostgreSQL dump (plain or custom format), a MySQL or MariaDB dump, a SQLite\n"
"database file, a tar archive or a folder; files may be gzip-compressed.\n"
"\n"
"  --job NAME           the name the history keeps this backup under (default: default)\n"
"  --kind KIND          auto, postgres, mysql, sqlite or archive (default: auto)\n"
"  --history FILE       the history, an AltSql DB file (default: altsql-restore.db)\n"
"  --max-shrink PCT     alert when a table loses more than PCT%% of its rows (default: 10)\n"
"  --accept             take this run as the new baseline even if it shrank\n"
"  --sandbox MODE       auto, local or docker: where database servers come from (default: auto)\n"
"  --engine-path DIRS   look for server programs only in these folders (colon-separated)\n"
"  --docker PROG        the Docker program (default: docker)\n"
"  --pg-image IMAGE     Docker image for PostgreSQL (default: postgres:16)\n"
"  --mysql-image IMAGE  Docker image for MySQL (default: mysql:8.0)\n"
"  --work-dir DIR       where sandboxes are made (default: $TMPDIR or /tmp)\n"
"  --timeout SEC        stop a restore that runs longer (default: 3600)\n"
"  --no-end-check       accept a plain dump without its dump tool's last line\n"
"  --report FILE        write the report to FILE\n"
"  --report-dir DIR     write the report to DIR/JOB-DATE-runN.txt\n"
"  --signer NAME        who signs off the report\n"
"  --sign-key FILE      seal the report with HMAC-SHA256 under this key\n"
"  --webhook URL        POST a JSON summary there when a run doesn't pass (needs curl)\n"
"  --webhook-always     POST after every run\n"
"  -q, --quiet          don't print the report\n"
"\n"
"Exit status: 0 verified, 1 alert, 2 usage or internal error, 3 not verified.\n", ALTSQL_RESTORE_VERSION);
}

/* ---- the history: an AltSql DB file ------------------------------------------------------- */
static uint8_t *g_dbmem;
typedef struct hist { altsql_db *db; altsql_db_posix px; altsql_db_file file; } hist;

static int hist_open(hist *h, const char *path) {
    altsql_db_config cfg;
    altsql_db_tableinfo ti;
    int rc = altsql_db_posix_open(&h->file, &h->px, path, 1);
    if (rc == ALTSQL_DB_BUSY) return seterr("the history %s is in use by another run", path);
    if (rc) return seterr("cannot open the history %s: %s", path, strerror(errno));
    g_dbmem = (uint8_t *)xmalloc(4u << 20);
    memset(&cfg, 0, sizeof cfg);
    cfg.mem = g_dbmem;
    cfg.mem_size = 4u << 20;
    cfg.create = 1;
    if ((rc = altsql_db_open(&h->db, &h->file, &cfg)) != 0) {
        seterr("cannot open the history %s: %s", path, h->db ? altsql_db_errmsg(h->db) : "bad settings");
        altsql_db_posix_close(&h->px);
        h->db = NULL;
        return -1;
    }
    if (altsql_db_table_info(h->db, "runs", &ti) != 0 &&
        altsql_db_table_create(h->db, "runs", "job:text,run:int,at:long,verdict:text,kind:text,backup:text,sha:text,seal:text", "job,run"))
        return seterr("cannot set up the history: %s", altsql_db_errmsg(h->db));
    if (altsql_db_table_info(h->db, "counts", &ti) != 0 &&
        altsql_db_table_create(h->db, "counts", "job:text,run:int,item:text,rows:long,checksum:text", "job,run,item"))
        return seterr("cannot set up the history: %s", altsql_db_errmsg(h->db));
    return 0;
}
static void hist_close(hist *h) {
    if (h->db) altsql_db_close(h->db);
    if (h->db) altsql_db_posix_close(&h->px);
    h->db = NULL;
}
static void vtext(altsql_value *v, const char *s) {
    v->type = ALTSQL_TEXT;
    v->u.s = s;
    v->len = (int)strlen(s) > 255 ? 255 : (int)strlen(s);
}
static void vint(altsql_value *v, int64_t i) { v->type = ALTSQL_INTEGER; v->u.i = i; v->len = 0; }
static void copy_text(char *out, size_t cap, const altsql_value *v) {
    size_t n = v->type == ALTSQL_TEXT ? (size_t)v->len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, v->u.s, n);
    out[n] = 0;
}

typedef struct runrec { int64_t run, at; char verdict[32], kind[64], backup[256], sha[65], seal[65]; } runrec;

static int read_run(altsql_db_cursor *c, runrec *r) {
    altsql_value v[8];
    int rc = altsql_db_row_read(c, v, 8);
    if (rc) return rc;
    r->run = v[1].u.i;
    r->at = v[2].u.i;
    copy_text(r->verdict, sizeof r->verdict, &v[3]);
    copy_text(r->kind, sizeof r->kind, &v[4]);
    copy_text(r->backup, sizeof r->backup, &v[5]);
    copy_text(r->sha, sizeof r->sha, &v[6]);
    copy_text(r->seal, sizeof r->seal, &v[7]);
    return 0;
}

/* The number this run gets, and the last run that passed or was accepted (base->run 0: none). */
static int hist_lookup(hist *h, const char *job, int64_t *next, runrec *base, items *base_items) {
    altsql_db_cursor c;
    altsql_value key[2];
    int rc;
    *next = 1;
    memset(base, 0, sizeof *base);
    vtext(&key[0], job);
    rc = altsql_db_row_last(&c, h->db, "runs", key, 1);
    if (rc == ALTSQL_NOTFOUND) return 0;
    while (rc == 0) {
        runrec r;
        if ((rc = read_run(&c, &r)) != 0) break;
        if (*next == 1) *next = r.run + 1;
        if (!strcmp(r.verdict, "PASSED") || !strcmp(r.verdict, "ACCEPTED")) { *base = r; break; }
        rc = altsql_db_prev(&c);
    }
    if (rc != 0 && rc != ALTSQL_NOTFOUND) return seterr("cannot read the history: %s", altsql_db_errmsg(h->db));
    if (base->run) {
        vint(&key[1], base->run);
        rc = altsql_db_row_seek(&c, h->db, "counts", key, 2);
        while (rc == 0) {
            altsql_value v[5];
            char name[256], sum[65];
            if ((rc = altsql_db_row_read(&c, v, 5)) != 0) break;
            copy_text(name, sizeof name, &v[2]);
            copy_text(sum, sizeof sum, &v[4]);
            items_add(base_items, name, v[3].u.i, sum);
            rc = altsql_db_next(&c);
        }
        if (rc != ALTSQL_NOTFOUND) return seterr("cannot read the history: %s", altsql_db_errmsg(h->db));
    }
    return 0;
}

static int hist_record(hist *h, const char *job, int64_t run, int64_t at, const char *verdict, const char *kind,
                       const char *backup, const char *sha, const char *seal, const items *L, int with_counts) {
    altsql_value v[8];
    int i, rc;
    if ((rc = altsql_db_begin(h->db, 1)) != 0) return seterr("cannot write the history: %s", altsql_db_errmsg(h->db));
    vtext(&v[0], job); vint(&v[1], run); vint(&v[2], at); vtext(&v[3], verdict);
    vtext(&v[4], kind); vtext(&v[5], backup); vtext(&v[6], sha); vtext(&v[7], seal);
    rc = altsql_db_row_insert(h->db, "runs", v, 8);
    for (i = 0; rc == 0 && with_counts && i < L->n; i++) {
        altsql_value c[5];
        vtext(&c[0], job); vint(&c[1], run); vtext(&c[2], L->v[i].name); vint(&c[3], L->v[i].rows); vtext(&c[4], L->v[i].sum);
        rc = altsql_db_row_put(h->db, "counts", c, 5);
    }
    if (rc == 0) rc = altsql_db_commit(h->db);
    if (rc) { seterr("cannot write the history: %s", altsql_db_errmsg(h->db)); altsql_db_rollback(h->db); return -1; }
    return 0;
}

static int list_runs(const opts *o) {
    hist h;
    altsql_db_cursor c;
    altsql_value key[1];
    int rc;
    memset(&h, 0, sizeof h);
    if (hist_open(&h, o->history)) { fprintf(stderr, "altsql-restore: %s\n", g_err); return EXIT_USAGE; }
    if (o->list_job) vtext(&key[0], o->list_job);
    rc = altsql_db_row_seek(&c, h.db, "runs", key, o->list_job ? 1 : 0);
    printf("%-20s %5s  %-20s  %-12s  %-24s  %s\n", "job", "run", "date (UTC)", "verdict", "kind", "backup");
    while (rc == 0) {
        altsql_value v[8];
        char job[256], when[32];
        runrec r;
        time_t t;
        if ((rc = altsql_db_row_read(&c, v, 8)) != 0) break;
        copy_text(job, sizeof job, &v[0]);
        if (read_run(&c, &r)) break;
        t = (time_t)r.at;
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", gmtime(&t));
        printf("%-20s %5lld  %-20s  %-12s  %-24s  %s\n", job, (long long)r.run, when, r.verdict, r.kind, r.backup);
        rc = altsql_db_next(&c);
    }
    hist_close(&h);
    return rc == ALTSQL_NOTFOUND ? 0 : EXIT_USAGE;
}

/* ---- seal ------------------------------------------------------------------------------------ */
static int load_key(const char *path, uint8_t **key, size_t *kn) {
    char *s = read_all(path, kn);
    if (!s) return seterr("cannot read the key file %s", path);
    while (*kn && (s[*kn - 1] == '\n' || s[*kn - 1] == '\r')) (*kn)--;
    if (!*kn) { free(s); return seterr("the key file %s is empty", path); }
    *key = (uint8_t *)s;
    return 0;
}
static int seal_text(const char *text, size_t n, const char *keyfile, char *line, size_t cap, char hex[65]) {
    if (keyfile) {
        uint8_t *key;
        size_t kn;
        if (load_key(keyfile, &key, &kn)) return -1;
        hmac_sha256_hex(key, kn, text, n, hex);
        free(key);
        snprintf(line, cap, "Seal (HMAC-SHA256 of the lines above, keyed): %s\n", hex);
    } else {
        sha256_hex(text, n, hex);
        snprintf(line, cap, "Seal (SHA-256 of the lines above): %s\n", hex);
    }
    return 0;
}
static int verify_report(const opts *o) {
    size_t n;
    char *s = read_all(o->verify, &n), *p, *last = NULL, line[256], hex[65];
    if (!s) { fprintf(stderr, "altsql-restore: cannot read %s\n", o->verify); return EXIT_USAGE; }
    for (p = s; (p = strstr(p, "\nSeal (")) != NULL; p++) last = p + 1;
    if (!last && !strncmp(s, "Seal (", 6)) last = s;
    if (!last) { printf("%s: no seal line\n", o->verify); free(s); return EXIT_ALERT; }
    if (seal_text(s, (size_t)(last - s), o->sign_key, line, sizeof line, hex)) { fprintf(stderr, "altsql-restore: %s\n", g_err); free(s); return EXIT_USAGE; }
    if (strncmp(last, line, strlen(line)) || last[strlen(line)] != 0) {
        printf("%s: the seal does NOT match%s; the report was changed%s\n", o->verify,
               o->sign_key ? " under this key" : "", o->sign_key ? " or the key is another" : " (or it was sealed with a key: give --sign-key)");
        free(s);
        return EXIT_ALERT;
    }
    printf("%s: the seal matches; the report is as it was written\n", o->verify);
    free(s);
    return 0;
}

/* ---- webhook ------------------------------------------------------------------------------------ */
static void json_str(buf *b, const char *s) {
    buf_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf_add(b, "\\", 1); buf_add(b, s, 1); }
        else if (c == '\n') buf_add(b, "\\n", 2);
        else if (c < 0x20) buf_printf(b, "\\u%04x", c);
        else buf_add(b, s, 1);
    }
    buf_add(b, "\"", 1);
}
static void send_webhook(const opts *o, const char *workdir, const char *json, char *result, size_t cap) {
    strs dirs = { 0 }, a = { 0 };
    static const char *const none[] = { NULL };
    char curl[AR_PATH], f[AR_PATH], out[AR_PATH], t[512];
    runopt ro;
    int rc;
    strs saved = g_engine_path;
    memset(&g_engine_path, 0, sizeof g_engine_path);
    search_dirs(&dirs, none);
    g_engine_path = saved;
    if (!find_in(&dirs, "curl", curl, sizeof curl)) { strs_free(&dirs); snprintf(result, cap, "NOT sent: the curl program was not found"); return; }
    strs_free(&dirs);
    snprintf(f, sizeof f, "%s/altsql-restore-webhook-%d.json", workdir, (int)getpid());
    snprintf(out, sizeof out, "%s/altsql-restore-webhook-%d.out", workdir, (int)getpid());
    if (write_file(f, json, strlen(json))) { g_err[0] = 0; snprintf(result, cap, "NOT sent: cannot write its body"); return; }
    memset(&ro, 0, sizeof ro);
    ro.out = out;
    ro.timeout = 30;
    strs_add(&a, curl); strs_add(&a, "-sS"); strs_add(&a, "-f"); strs_add(&a, "-m"); strs_add(&a, "20");
    strs_add(&a, "-H"); strs_add(&a, "Content-Type: application/json");
    strs_add(&a, "--data-binary"); strs_addf(&a, "@%s", f); strs_add(&a, o->webhook);
    rc = run_list(&a, &ro);
    strs_free(&a);
    tail_lines(out, 2, t, sizeof t);
    unlink(f);
    unlink(out);
    if (rc == 0) snprintf(result, cap, "sent to %s", o->webhook);
    else snprintf(result, cap, "FAILED to send to %s (curl exit %d%s%s)", o->webhook, rc, t[0] ? ": " : "", t);
}

/* ---- the run ------------------------------------------------------------------------------------ */
static const char *kind_name(const char *k, int custom, int gz) {
    static char out[80];
    const char *n = !strcmp(k, "postgres") ? (custom ? "PostgreSQL custom-format dump" : "PostgreSQL plain dump")
                  : !strcmp(k, "mysql") ? "MySQL dump" : !strcmp(k, "sqlite") ? "SQLite database file"
                  : !strcmp(k, "folder") ? "folder" : "tar archive";
    snprintf(out, sizeof out, "%s%s", n, gz ? " (gzip)" : "");
    return out;
}

/* Guesses the kind from the first bytes, unpacked. */
static const char *guess_kind(const peeker *k, int *custom) {
    char text[4097];
    *custom = 0;
    if (k->n >= 16 && !memcmp(k->b, "SQLite format 3", 16)) return "sqlite";
    if (k->n >= 5 && !memcmp(k->b, "PGDMP", 5)) { *custom = 1; return "postgres"; }
    if (k->n >= 262 && !memcmp(k->b + 257, "ustar", 5)) return "archive";
    memcpy(text, k->b, k->n);
    text[k->n] = 0;
    { size_t i; for (i = 0; i < k->n; i++) if (!text[i]) text[i] = ' '; }
    if (strstr(text, "PostgreSQL database dump")) return "postgres";
    if (strstr(text, "MySQL dump") || strstr(text, "MariaDB dump")) return "mysql";
    return NULL;
}

int main(int argc, char **argv) {
    opts o;
    hist h;
    items cur = { 0 }, base = { 0 };
    strs notes = { 0 }, unavail = { 0 }, reasons = { 0 }, fresh = { 0 };
    runrec b;
    int64_t run = 0;
    int i, restore_rc = 0, custom = 0, gz = 0, exitc;
    char sha[65] = "", hexseal[65], when[64], whenfile[32], engine[600] = "", restore_err[2100] = "";
    const char *kind = NULL, *verdict, *unit = "rows";
    uint64_t size = 0;
    time_t now = time(NULL);
    double t0, secs = 0;
    buf rep = { 0 };

    memset(&o, 0, sizeof o);
    o.job = "default"; o.kind = "auto"; o.history = "altsql-restore.db"; o.docker = "docker";
    o.pg_image = "postgres:16"; o.my_image = "mysql:8.0"; o.max_shrink = 10; o.timeout = 3600; o.end_check = 1;
    o.workdir = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "altsql-restore: %s needs a value\n", a), exit(EXIT_USAGE), (char *)NULL))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--version")) {
            printf("altsql-restore %s (AltSql DB %s, AltSql Core %s)\n", ALTSQL_RESTORE_VERSION, ALTSQL_DB_VERSION, ALTSQL_VERSION);
            return 0;
        }
        else if (!strcmp(a, "--job")) o.job = NEXT();
        else if (!strcmp(a, "--kind")) o.kind = NEXT();
        else if (!strcmp(a, "--history")) o.history = NEXT();
        else if (!strcmp(a, "--max-shrink")) o.max_shrink = atof(NEXT());
        else if (!strcmp(a, "--accept")) o.accept = 1;
        else if (!strcmp(a, "--sandbox")) {
            const char *m = NEXT();
            if (!strcmp(m, "auto")) o.mode = SB_AUTO; else if (!strcmp(m, "local")) o.mode = SB_LOCAL;
            else if (!strcmp(m, "docker")) o.mode = SB_DOCKER;
            else { fprintf(stderr, "altsql-restore: --sandbox takes auto, local or docker\n"); return EXIT_USAGE; }
        }
        else if (!strcmp(a, "--engine-path")) {
            char *p = NEXT(), *s;
            for (s = strtok(p, ":"); s; s = strtok(NULL, ":")) strs_add(&g_engine_path, s);
            if (!g_engine_path.n) strs_add(&g_engine_path, "/nonexistent");
        }
        else if (!strcmp(a, "--docker")) o.docker = NEXT();
        else if (!strcmp(a, "--pg-image")) o.pg_image = NEXT();
        else if (!strcmp(a, "--mysql-image")) o.my_image = NEXT();
        else if (!strcmp(a, "--work-dir")) o.workdir = NEXT();
        else if (!strcmp(a, "--timeout")) o.timeout = atof(NEXT());
        else if (!strcmp(a, "--no-end-check")) o.end_check = 0;
        else if (!strcmp(a, "--report")) o.report = NEXT();
        else if (!strcmp(a, "--report-dir")) o.report_dir = NEXT();
        else if (!strcmp(a, "--signer")) o.signer = NEXT();
        else if (!strcmp(a, "--sign-key")) o.sign_key = NEXT();
        else if (!strcmp(a, "--webhook")) o.webhook = NEXT();
        else if (!strcmp(a, "--webhook-always")) o.webhook_always = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) o.quiet = 1;
        else if (!strcmp(a, "--list")) o.list = 1;
        else if (!strcmp(a, "--verify-report")) o.verify = NEXT();
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "altsql-restore: unknown option %s (try --help)\n", a); return EXIT_USAGE; }
        else if (!o.backup) o.backup = a;
        else { fprintf(stderr, "altsql-restore: one backup at a time (%s and %s)\n", o.backup, a); return EXIT_USAGE; }
    }
    if (o.verify) return verify_report(&o);
    if (o.list) {
        for (i = 1; i < argc; i++) if (!strcmp(argv[i], "--job")) o.list_job = o.job;
        return list_runs(&o);
    }
    if (!o.backup) { usage(stderr); return EXIT_USAGE; }
    if (o.max_shrink < 0 || o.max_shrink > 100) { fprintf(stderr, "altsql-restore: --max-shrink takes 0 to 100\n"); return EXIT_USAGE; }
    if (o.timeout <= 0) o.timeout = 3600;
    if (strcmp(o.kind, "auto") && strcmp(o.kind, "postgres") && strcmp(o.kind, "mysql") && strcmp(o.kind, "sqlite") && strcmp(o.kind, "archive")) {
        fprintf(stderr, "altsql-restore: --kind takes auto, postgres, mysql, sqlite or archive\n");
        return EXIT_USAGE;
    }
    if (!*o.job || strlen(o.job) > 200) { fprintf(stderr, "altsql-restore: --job takes a name of 1 to 200 bytes\n"); return EXIT_USAGE; }
    if (o.sign_key) { uint8_t *k; size_t kn; if (load_key(o.sign_key, &k, &kn)) { fprintf(stderr, "altsql-restore: %s\n", g_err); return EXIT_USAGE; } free(k); }
    if (access(o.backup, R_OK)) { fprintf(stderr, "altsql-restore: cannot read %s: %s\n", o.backup, strerror(errno)); return EXIT_USAGE; }
    if (!is_dir(o.workdir)) { fprintf(stderr, "altsql-restore: the work folder %s doesn't exist\n", o.workdir); return EXIT_USAGE; }
    signal(SIGPIPE, SIG_IGN);
    crc_init();

    memset(&h, 0, sizeof h);
    if (hist_open(&h, o.history) || hist_lookup(&h, o.job, &run, &b, &base)) {
        fprintf(stderr, "altsql-restore: %s\n", g_err);
        hist_close(&h);
        return EXIT_USAGE;
    }
    sandbox_sweep(o.workdir, &notes);

    /* ---- restore into the sandbox ---- */
    t0 = now_mono();
    if (is_dir(o.backup)) { kind = "folder"; snprintf(sha, sizeof sha, "(a folder: see the checksums below)"); }
    else if (sha256_file(o.backup, sha, &size)) { snprintf(restore_err, sizeof restore_err, "%s", g_err); restore_rc = ENG_FAILED; }
    if (!restore_rc && sandbox_create(o.workdir)) {
        fprintf(stderr, "altsql-restore: %s\n", g_err);
        hist_close(&h);
        return EXIT_USAGE;
    }
    if (!restore_rc && !kind) {
        peeker pk;
        const char *g;
        pk.n = 0;
        if (stream_file(o.backup, peek_cb, &pk, &gz)) { snprintf(restore_err, sizeof restore_err, "%s", g_err); restore_rc = ENG_FAILED; }
        else {
            g = guess_kind(&pk, &custom);
            if (!strcmp(o.kind, "auto")) {
                if (!g) { snprintf(restore_err, sizeof restore_err, "cannot tell what kind of backup %s is; give --kind", base_name(o.backup)); restore_rc = ENG_FAILED; }
                else kind = g;
            } else {
                kind = o.kind;
                if (!strcmp(kind, "postgres") && g && strcmp(g, "postgres")) custom = 0;
            }
        }
    }
    if (!restore_rc) {
        char dst[AR_PATH];
        g_err[0] = 0;
        if (!strcmp(kind, "folder") || !strcmp(kind, "archive")) {
            fentries files = { 0 };
            unit = "files";
            snprintf(dst, sizeof dst, "%s/files", g_sandbox);
            if (mkdirs(dst, 0755) == 0 &&
                (!strcmp(kind, "folder") ? dir_restore(o.backup, dst, "", &files) : tar_restore(o.backup, dst, &files, &gz)) == 0) {
                long nf = 0, q;
                for (q = 0; q < files.n; q++) if (files.v[q].type != 'd') nf++;
                fent_items(&files, &cur);
                strs_addf(&notes, "%s entries %s into the sandbox, every file hashed with SHA-256 as it was written",
                          commas(nf), !strcmp(kind, "folder") ? "copied" : "unpacked");
                snprintf(engine, sizeof engine, "built-in %s reader (no outside software)", !strcmp(kind, "folder") ? "folder" : "tar and gzip");
            } else restore_rc = ENG_FAILED;
            fent_free(&files);
        } else {
            copier cp;
            memset(&cp, 0, sizeof cp);
            snprintf(dst, sizeof dst, "%s/%s", g_sandbox, !strcmp(kind, "sqlite") ? "backup.sqlite" : "dump");
            cp.outpath = dst;
            if (!(cp.out = fopen(dst, "wb"))) { seterr("cannot write %s: %s", dst, strerror(errno)); restore_rc = ENG_FAILED; }
            else {
                int r = stream_file(o.backup, copier_cb, &cp, &gz);
                if (fclose(cp.out) && !r) r = seterr("cannot write %s", dst);
                if (r) restore_rc = ENG_FAILED;
            }
            chmod(dst, 0644);
            if (!restore_rc && !strcmp(kind, "sqlite")) {
                if (sqlite_verify(dst, &cur, &notes)) restore_rc = ENG_FAILED;
                snprintf(engine, sizeof engine, "built-in SQLite file reader (no outside software)");
            } else if (!restore_rc) {
                char tail[8193];
                int pg = !strcmp(kind, "postgres");
                copier_tail(&cp, tail);
                if (pg && cp.hn >= 5 && !memcmp(cp.head, "PGDMP", 5)) custom = 1;
                if (o.end_check && !custom) {
                    const char *mark = pg ? "PostgreSQL database dump complete" : "-- Dump completed";
                    if (!strstr(tail, mark)) {
                        seterr("the dump doesn't end with \"%s\": it was cut short, or written without comments (--no-end-check accepts that)", mark);
                        restore_rc = ENG_FAILED;
                    }
                }
                if (!restore_rc) {
                    rctx r;
                    memset(&r, 0, sizeof r);
                    r.sandbox = g_sandbox; r.dump = dst; r.pg_custom = custom; r.timeout = o.timeout; r.mode = o.mode;
                    r.docker = o.docker; r.pg_image = o.pg_image; r.my_image = o.my_image;
                    r.out = &cur; r.notes = &notes; r.unavail = &unavail;
                    restore_rc = db_verify(&r, pg ? K_PG : K_MY);
                    snprintf(engine, sizeof engine, "%s", r.engine);
                }
            }
        }
        if (restore_rc == ENG_FAILED) snprintf(restore_err, sizeof restore_err, "%s", g_err[0] ? g_err : "the restore failed");
    }
    {
        const char *pause = getenv("ALTSQL_RESTORE_TEST_PAUSE");    /* tests: time to kill a run mid-way */
        if (pause && g_sandbox[0]) sleep_ms(atol(pause) * 1000L);
    }
    secs = now_mono() - t0;
    sandbox_finish();
    items_sort(&cur);

    /* ---- compare with the baseline and decide ---- */
    if (restore_rc == ENG_UNAVAIL) verdict = "NOT VERIFIED";
    else if (restore_rc) { verdict = "FAILED"; strs_addf(&reasons, "the restore failed: %s", restore_err); }
    else {
        int n_alert = 0;
        for (i = 0; i < base.n; i++) {
            item *bi = &base.v[i], *ci = items_find(&cur, bi->name);
            if (!ci) { strs_addf(&reasons, "%s is gone (it had %s %s)", bi->name, commas(bi->rows), unit); n_alert++; }
            else if (bi->rows > 0 && (double)ci->rows < (double)bi->rows * (1.0 - o.max_shrink / 100.0)) {
                strs_addf(&reasons, "%s shrank from %s to %s %s (%.1f%%), more than the %g%% allowed", bi->name,
                          commas(bi->rows), commas(ci->rows), unit, 100.0 * (double)(ci->rows - bi->rows) / (double)bi->rows, o.max_shrink);
                n_alert++;
            }
        }
        for (i = 0; i < cur.n; i++) if (base.n && !items_find(&base, cur.v[i].name)) strs_add(&fresh, cur.v[i].name);
        if (cur.n == 0 || (!strcmp(unit, "files") && cur.n == 1)) {
            strs_addf(&reasons, "the restore holds no %s", !strcmp(unit, "files") ? "files" : "tables");
            n_alert++;
        }
        if (n_alert && o.accept) verdict = "ACCEPTED";
        else if (n_alert) verdict = "ALERT";
        else verdict = o.accept && base.n ? "ACCEPTED" : "PASSED";
    }
    if (!kind) kind = "unknown";

    /* ---- the report ---- */
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));
    strftime(whenfile, sizeof whenfile, "%Y%m%d-%H%M%S", gmtime(&now));
    buf_printf(&rep, "AltSql Restore report\n=====================\n\n");
    buf_printf(&rep, "Job:            %s\n", o.job);
    buf_printf(&rep, "Run:            %lld\n", (long long)run);
    buf_printf(&rep, "Date:           %s\n", when);
    buf_printf(&rep, "Backup:         %s\n", o.backup);
    if (strcmp(kind, "folder")) buf_printf(&rep, "Backup size:    %s bytes\n", commas((int64_t)size));
    buf_printf(&rep, "Backup SHA-256: %s\n", sha[0] ? sha : "(not read)");
    buf_printf(&rep, "Kind:           %s\n", strcmp(kind, "unknown") ? kind_name(kind, custom, gz) : "unknown");
    buf_printf(&rep, "Restored with:  %s\n", engine[0] ? engine : restore_rc == ENG_UNAVAIL ? "nothing: no engine available" : "-");
    buf_printf(&rep, "Restore time:   %.1f s\n", secs);
    buf_printf(&rep, "Threshold:      alert when an item loses more than %g%% of its %s\n", o.max_shrink, unit);
    if (b.run) {
        char bw[32];
        time_t bt = (time_t)b.at;
        strftime(bw, sizeof bw, "%Y-%m-%d %H:%M:%S UTC", gmtime(&bt));
        buf_printf(&rep, "Compared with:  run %lld of %s (%s)\n\n", (long long)b.run, bw, b.verdict);
    } else buf_printf(&rep, "Compared with:  nothing: the first verified run of this job\n\n");
    if (cur.n) {
        int w = 5;
        for (i = 0; i < cur.n; i++) if ((int)strlen(cur.v[i].name) > w) w = (int)strlen(cur.v[i].name);
        for (i = 0; i < base.n; i++) if (!items_find(&cur, base.v[i].name) && (int)strlen(base.v[i].name) > w) w = (int)strlen(base.v[i].name);
        if (w > 48) w = 48;
        buf_printf(&rep, "%-*s  %15s  %15s  %8s  %-16s  %s\n", w, !strcmp(unit, "files") ? "Item" : "Table", unit, "previous", "change", "checksum", "");
        for (i = 0; i < cur.n; i++) {
            item *ci = &cur.v[i], *bi = items_find(&base, ci->name);
            char ch[32] = "", flag[32] = "";
            if (bi) {
                if (bi->rows) snprintf(ch, sizeof ch, "%+.1f%%", 100.0 * (double)(ci->rows - bi->rows) / (double)bi->rows);
                else snprintf(ch, sizeof ch, ci->rows ? "new rows" : "0.0%%");
                snprintf(flag, sizeof flag, "%s", strcmp(bi->sum, ci->sum) ? "changed" : "same");
            } else if (b.run) snprintf(flag, sizeof flag, "new");
            buf_printf(&rep, "%-*.*s  %15s  %15s  %8s  %.16s  %s\n", w, w, ci->name, commas(ci->rows), bi ? commas(bi->rows) : "-", ch, ci->sum, flag);
        }
        for (i = 0; i < base.n; i++) if (!items_find(&cur, base.v[i].name))
            buf_printf(&rep, "%-*.*s  %15s  %15s  %8s  %-16s  %s\n", w, w, base.v[i].name, "-", commas(base.v[i].rows), "-100.0%", "", "GONE");
        buf_add(&rep, "\n", 1);
    }
    if (notes.n) {
        buf_printf(&rep, "Checks:\n");
        for (i = 0; i < notes.n; i++) buf_printf(&rep, "  - %s\n", notes.v[i]);
        buf_add(&rep, "\n", 1);
    }
    buf_printf(&rep, "Verdict: %s\n", verdict);
    if (!strcmp(verdict, "NOT VERIFIED")) {
        buf_printf(&rep, "  This backup was NOT restored, so it is not verified: no engine could run it here.\n");
        for (i = 0; i < unavail.n; i++) buf_printf(&rep, "  - %s\n", unavail.v[i]);
    }
    for (i = 0; i < reasons.n; i++) buf_printf(&rep, "  - %s\n", reasons.v[i]);
    if (!strcmp(verdict, "PASSED")) buf_printf(&rep, "  The backup restored, and no %s shrank past the threshold%s.\n",
                                                !strcmp(unit, "files") ? "item" : "table", b.run ? "" : " (first run: this is now the baseline)");
    if (!strcmp(verdict, "ACCEPTED")) buf_printf(&rep, "  Accepted by the operator (--accept): this run is the new baseline.\n");
    if (fresh.n) {
        buf_printf(&rep, "  New since the baseline:");
        for (i = 0; i < fresh.n && i < 20; i++) buf_printf(&rep, "%s %s", i ? "," : "", fresh.v[i]);
        buf_printf(&rep, "%s\n", fresh.n > 20 ? ", ..." : "");
    }
    buf_add(&rep, "\n", 1);

    /* the webhook goes before the seal, so the report can say what happened to it */
    {
        char wres[1200];
        int send = o.webhook && (o.webhook_always || strcmp(verdict, "PASSED"));
        if (send) {
            buf j = { 0 };
            buf_printf(&j, "{\"tool\":\"altsql-restore\",\"version\":\"%s\",\"job\":", ALTSQL_RESTORE_VERSION);
            json_str(&j, o.job);
            buf_printf(&j, ",\"run\":%lld,\"date\":\"%s\",\"verdict\":\"%s\",\"backup\":", (long long)run, when, verdict);
            json_str(&j, o.backup);
            buf_printf(&j, ",\"backup_sha256\":\"%s\",\"reasons\":[", sha);
            for (i = 0; i < reasons.n + unavail.n; i++) { if (i) buf_add(&j, ",", 1); json_str(&j, i < reasons.n ? reasons.v[i] : unavail.v[i - reasons.n]); }
            buf_printf(&j, "],\"items\":%d}", cur.n);
            send_webhook(&o, o.workdir, buf_str(&j), wres, sizeof wres);
            buf_free(&j);
        } else snprintf(wres, sizeof wres, "%s", o.webhook ? "not needed (the run passed)" : "none set");
        buf_printf(&rep, "Webhook:        %s\n", wres);
    }
    buf_printf(&rep, "Signed off by:  %s\n", o.signer ? o.signer : "(no signer given)");
    {
        char line[256];
        if (seal_text((const char *)rep.p, rep.n, o.sign_key, line, sizeof line, hexseal)) { fprintf(stderr, "altsql-restore: %s\n", g_err); return EXIT_USAGE; }
        buf_printf(&rep, "%s", line);
    }

    /* ---- record, then write the report ---- */
    exitc = !strcmp(verdict, "PASSED") || !strcmp(verdict, "ACCEPTED") ? EXIT_VERIFIED
          : !strcmp(verdict, "NOT VERIFIED") ? EXIT_NOTVERIFIED : EXIT_ALERT;
    g_err[0] = 0;
    if (hist_record(&h, o.job, run, (int64_t)now, verdict, kind, base_name(o.backup), sha, hexseal, &cur,
                    !strcmp(verdict, "PASSED") || !strcmp(verdict, "ACCEPTED") || !strcmp(verdict, "ALERT"))) {
        fprintf(stderr, "altsql-restore: %s\n", g_err);
        exitc = EXIT_USAGE;
    }
    hist_close(&h);
    if (!o.quiet) fwrite(rep.p, 1, rep.n, stdout);
    if (o.report && write_file(o.report, rep.p, rep.n)) { fprintf(stderr, "altsql-restore: %s\n", g_err); exitc = EXIT_USAGE; }
    if (o.report_dir) {
        char path[AR_PATH], safe[201];
        size_t k;
        for (k = 0; o.job[k] && k < 200; k++) safe[k] = (char)((o.job[k] == '/' || o.job[k] == ' ') ? '_' : o.job[k]);
        safe[k] = 0;
        snprintf(path, sizeof path, "%s/%s-%s-run%lld.txt", o.report_dir, safe, whenfile, (long long)run);
        g_err[0] = 0;
        if (mkdirs(o.report_dir, 0755) || write_file(path, rep.p, rep.n)) { fprintf(stderr, "altsql-restore: %s\n", g_err); exitc = EXIT_USAGE; }
        else if (!o.quiet) printf("Report written to %s\n", path);
    }
    buf_free(&rep);
    items_free(&cur);
    items_free(&base);
    strs_free(&notes); strs_free(&unavail); strs_free(&reasons); strs_free(&fresh);
    free(g_dbmem);
    return exitc;
}
