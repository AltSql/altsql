/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* PostgreSQL and MySQL/MariaDB dumps for AltSql Restore.
 *
 * A throwaway server starts inside the sandbox, from the server programs installed on this
 * machine or from a Docker image, the dump is restored into it with the engine's own client
 * (psql, pg_restore, mysql), stopping at the first error, and every table is counted and
 * checksummed:
 *   PostgreSQL: count(*) and the sum of the first 60 bits of md5(row::text) over the rows,
 *               which doesn't depend on row order;
 *   MySQL:      COUNT(*) and CHECKSUM TABLE.
 * With --sandbox auto, local programs are tried first, then Docker. When neither can run a
 * server, the result is "unavailable": the run is NOT VERIFIED and the report says why. */

#define ENG_OK      0
#define ENG_FAILED  (-1)        /* the backup didn't restore                    */
#define ENG_UNAVAIL (-3)        /* no engine to restore it into                 */

enum { SB_AUTO = 0, SB_LOCAL = 1, SB_DOCKER = 2 };
enum { K_PG = 1, K_MY = 2 };

typedef struct rctx {
    const char *sandbox;
    const char *dump;           /* the dump, unpacked, inside the sandbox */
    int         pg_custom;      /* pg_dump -Fc archive                    */
    double      timeout;
    int         mode;
    const char *docker;
    const char *pg_image, *my_image;
    items      *out;
    strs       *notes;
    strs       *unavail;        /* why each way of running an engine failed */
    char        engine[512];    /* what restored it, for the report       */
} rctx;

typedef struct dbs {
    int   kind, docker, mariadb, drop;
    uid_t uid;
    gid_t gid;
    char  user[64];
    char  server[AR_PATH], client[AR_PATH], tool[AR_PATH];   /* tool: pg_restore, or the install program */
    char  dockerprog[AR_PATH], container[128];
    char  srv[AR_PATH], sock[AR_PATH], data[AR_PATH];
    pid_t pid;
    char  version[200];
} dbs;

static char g_out[AR_PATH];            /* sandbox/out.txt: the output of the last command */

static void sbpath(const rctx *r, const char *name, char *out) { snprintf(out, AR_PATH, "%s/%s", r->sandbox, name); }

/* The client command line, up to (not including) the SQL source. */
static void client_args(const dbs *d, strs *a, const char *db) {
    if (d->docker) { strs_add(a, d->dockerprog); strs_add(a, "exec"); strs_add(a, "-i"); strs_add(a, d->container); }
    if (d->kind == K_PG) {
        strs_add(a, d->docker ? "psql" : d->client);
        strs_add(a, "-X"); strs_add(a, "-q"); strs_add(a, "-A"); strs_add(a, "-t");
        strs_add(a, "-F"); strs_add(a, "\x1f");
        strs_add(a, "-h"); strs_add(a, d->docker ? "127.0.0.1" : d->srv);
        strs_add(a, "-p"); strs_add(a, "5432");
        strs_add(a, "-U"); strs_add(a, d->docker ? "postgres" : "altsql");
        strs_add(a, "-d"); strs_add(a, db);
    } else {
        if (d->docker) { strs_add(a, "mysql"); strs_add(a, "-h127.0.0.1"); strs_add(a, "--protocol=TCP"); }
        else { strs_add(a, d->client); strs_add(a, "--no-defaults"); strs_add(a, "-S"); strs_add(a, d->sock); }
        strs_add(a, "-uroot"); strs_add(a, "-N"); strs_add(a, "-B");
        if (db) strs_add(a, db);
    }
}

/* Runs SQL text through the client; output in g_out. */
static int client_sql(const rctx *r, const dbs *d, const char *db, const char *sql, double timeout, int stop_on_error) {
    char f[AR_PATH];
    strs a = { 0 };
    runopt o;
    int rc;
    sbpath(r, "q.sql", f);
    if (write_file(f, sql, strlen(sql))) return RUN_SPAWN;
    client_args(d, &a, db);
    if (d->kind == K_PG && stop_on_error) { strs_add(&a, "-v"); strs_add(&a, "ON_ERROR_STOP=1"); }
    if (d->kind == K_MY && !stop_on_error) strs_add(&a, "--force");
    memset(&o, 0, sizeof o);
    o.in = f;
    o.out = g_out;
    o.timeout = timeout;
    rc = run_list(&a, &o);
    strs_free(&a);
    return rc;
}

static void db_stop(const rctx *r, dbs *d) {
    char f[AR_PATH];
    if (d->pid > 0) {
        stop_group(d->pid, d->kind == K_PG ? SIGINT : SIGTERM);
        d->pid = 0;
        g_server = 0;
    }
    sbpath(r, "server", f);
    unlink(f);
    if (d->container[0]) {
        strs a = { 0 };
        runopt o;
        memset(&o, 0, sizeof o);
        o.timeout = 60;
        strs_add(&a, d->dockerprog); strs_add(&a, "rm"); strs_add(&a, "-f"); strs_add(&a, d->container);
        run_list(&a, &o);
        strs_free(&a);
        d->container[0] = 0;
        sbpath(r, "container", f);
        unlink(f);
    }
    if (d->srv[0]) rmtree(d->srv);
}

/* Waits until a query answers; also watches a local server's process. */
static int wait_ready(const rctx *r, dbs *d, double seconds, const char *log) {
    double end = now_mono() + seconds;
    while (now_mono() < end) {
        int st;
        if (client_sql(r, d, d->kind == K_PG ? "postgres" : NULL, "SELECT 1;", 20, 1) == 0) return 0;
        if (d->pid > 0 && waitpid(d->pid, &st, WNOHANG) == d->pid) {
            char t[1024];
            d->pid = 0;
            g_server = 0;
            tail_lines(log, 6, t, sizeof t);
            return seterr("the server stopped while starting: %s", t);
        }
        sleep_ms(250);
    }
    {
        char t[1024];
        tail_lines(log, 4, t, sizeof t);
        return seterr("the server didn't answer within %.0f seconds: %s", seconds, t);
    }
}

static int write_server_file(const rctx *r, pid_t pid, int sig) {
    char f[AR_PATH], line[64];
    sbpath(r, "server", f);
    snprintf(line, sizeof line, "%d %d\n", (int)pid, sig);
    return write_file(f, line, strlen(line));
}

/* A folder for the server's files, owned by the user it runs as. */
static int make_srv(const rctx *r, dbs *d) {
    sbpath(r, "srv", d->srv);
    snprintf(d->data, sizeof d->data, "%s/data", d->srv);
    if (mkdir(d->srv, 0700) && errno != EEXIST) return seterr("cannot make %s: %s", d->srv, strerror(errno));
    if (d->drop && chown(d->srv, d->uid, d->gid)) return seterr("cannot hand %s to %s: %s", d->srv, d->user, strerror(errno));
    return 0;
}

static void first_line(const char *path, char *out, size_t cap) {
    char *s = read_all(path, NULL), *nl;
    out[0] = 0;
    if (!s) return;
    if ((nl = strchr(s, '\n')) != NULL) *nl = 0;
    snprintf(out, cap, "%s", s);
    free(s);
}

static int prog_version(const char *prog, char *out, size_t cap) {
    strs a = { 0 };
    runopt o;
    int rc;
    memset(&o, 0, sizeof o);
    o.out = g_out;
    o.timeout = 30;
    strs_add(&a, prog); strs_add(&a, "--version");
    rc = run_list(&a, &o);
    strs_free(&a);
    first_line(g_out, out, cap);
    return rc;
}

/* ---- starting a server: local ----------------------------------------------------------- */
static const char *const pg_dirs[] = { "/usr/lib/postgresql/*/bin", "/usr/pgsql-*/bin", "/usr/local/pgsql/bin",
    "/opt/homebrew/opt/postgresql@*/bin", "/usr/local/opt/postgresql@*/bin", "/opt/homebrew/bin", "/usr/local/bin",
    "/Applications/Postgres.app/Contents/Versions/latest/bin", NULL };
static const char *const my_dirs[] = { "/usr/sbin", "/usr/local/sbin", "/usr/local/mysql/bin", "/usr/local/mariadb/bin",
    "/opt/homebrew/opt/mysql/bin", "/opt/homebrew/opt/mariadb/bin", "/usr/local/opt/mysql/bin", "/usr/local/opt/mariadb/bin",
    "/opt/homebrew/bin", "/usr/local/bin", NULL };

static int pg_start_local(const rctx *r, dbs *d) {
    strs dirs = { 0 }, a = { 0 };
    char initdb[AR_PATH] = "", log[AR_PATH];
    int i, rc;
    runopt o;
    search_dirs(&dirs, pg_dirs);
    for (i = 0; i < dirs.n; i++) {                      /* initdb and postgres from one folder */
        char p1[AR_PATH], p2[AR_PATH];
        snprintf(p1, sizeof p1, "%s/initdb", dirs.v[i]);
        snprintf(p2, sizeof p2, "%s/postgres", dirs.v[i]);
        if (is_exec(p1) && is_exec(p2)) {
            snprintf(initdb, sizeof initdb, "%s", p1);
            snprintf(d->server, sizeof d->server, "%s", p2);
            snprintf(d->client, sizeof d->client, "%s/psql", dirs.v[i]);
            snprintf(d->tool, sizeof d->tool, "%s/pg_restore", dirs.v[i]);
            if (!is_exec(d->client)) find_in(&dirs, "psql", d->client, sizeof d->client);
            if (!is_exec(d->tool)) find_in(&dirs, "pg_restore", d->tool, sizeof d->tool);
            break;
        }
    }
    strs_free(&dirs);
    if (!initdb[0]) return seterr("no local PostgreSQL server programs (initdb and postgres) were found");
    if (!is_exec(d->client)) return seterr("PostgreSQL's client psql was not found beside %s", d->server);
    if (r->pg_custom && !is_exec(d->tool)) return seterr("pg_restore was not found beside %s", d->server);
    if (geteuid() == 0) {
        static const char *const users[] = { "postgres", "nobody", NULL };
        if (!service_user(users, &d->uid, &d->gid, d->user, sizeof d->user)) return seterr("running as root, and no user postgres or nobody to run PostgreSQL as");
        d->drop = 1;
    }
    if (make_srv(r, d)) return -1;
    prog_version(d->server, d->version, sizeof d->version);
    memset(&o, 0, sizeof o);
    o.drop = d->drop; o.uid = d->uid; o.gid = d->gid;
    sbpath(r, "initdb.log", log);
    o.out = log;
    o.timeout = 300;
    strs_add(&a, initdb); strs_add(&a, "-D"); strs_add(&a, d->data); strs_add(&a, "-U"); strs_add(&a, "altsql");
    strs_add(&a, "--auth=trust"); strs_add(&a, "-E"); strs_add(&a, "UTF8"); strs_add(&a, "--locale=C"); strs_add(&a, "-N");
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc != 0) {
        char t[1024];
        tail_lines(log, 4, t, sizeof t);
        return seterr("initdb failed (%d): %s%s", rc, t, d->drop ? " (running as root, the server runs as the user "
                      "postgres or nobody, which must be able to enter --work-dir)" : "");
    }
    sbpath(r, "server.log", log);
    o.out = log;
    o.timeout = 0;
    o.background = 1;
    strs_add(&a, d->server); strs_add(&a, "-D"); strs_add(&a, d->data); strs_add(&a, "-k"); strs_add(&a, d->srv);
    strs_add(&a, "-p"); strs_add(&a, "5432"); strs_add(&a, "-c"); strs_add(&a, "listen_addresses=");
    strs_add(&a, "-c"); strs_add(&a, "fsync=off"); strs_add(&a, "-c"); strs_add(&a, "synchronous_commit=off");
    strs_add(&a, "-c"); strs_add(&a, "full_page_writes=off");
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc <= 0) return seterr("cannot start %s", d->server);
    d->pid = (pid_t)rc;
    g_server = d->pid;
    write_server_file(r, d->pid, SIGINT);
    return wait_ready(r, d, 90, log);
}

static int my_start_local(const rctx *r, dbs *d) {
    strs dirs = { 0 }, a = { 0 };
    char log[AR_PATH], outlog[AR_PATH], arg[AR_PATH + 32];
    int rc;
    runopt o;
    search_dirs(&dirs, my_dirs);
    if (!find_in(&dirs, "mariadbd", d->server, sizeof d->server)) find_in(&dirs, "mysqld", d->server, sizeof d->server);
    if (!find_in(&dirs, "mariadb", d->client, sizeof d->client)) find_in(&dirs, "mysql", d->client, sizeof d->client);
    if (!d->server[0]) { strs_free(&dirs); return seterr("no local MySQL or MariaDB server program (mysqld or mariadbd) was found"); }
    if (!d->client[0]) { strs_free(&dirs); return seterr("no MySQL client (mysql or mariadb) was found"); }
    prog_version(d->server, d->version, sizeof d->version);
    d->mariadb = strstr(d->version, "MariaDB") != NULL;
    if (d->mariadb && !find_in(&dirs, "mariadb-install-db", d->tool, sizeof d->tool))
        find_in(&dirs, "mysql_install_db", d->tool, sizeof d->tool);
    strs_free(&dirs);
    if (d->mariadb && !d->tool[0]) return seterr("MariaDB's mariadb-install-db was not found");
    if (geteuid() == 0) {
        static const char *const users[] = { "mysql", "nobody", NULL };
        if (!service_user(users, &d->uid, &d->gid, d->user, sizeof d->user)) return seterr("running as root, and no user mysql or nobody to run the server as");
        d->drop = 1;
    }
    if (make_srv(r, d)) return -1;
    snprintf(d->sock, sizeof d->sock, "%s/mysql.sock", d->srv);
    if (strlen(d->sock) > 100) return seterr("the sandbox path is too long for a server socket; use a shorter --work-dir");
    memset(&o, 0, sizeof o);
    sbpath(r, "initdb.log", log);
    o.out = log;
    o.timeout = 300;
    if (d->mariadb) {
        strs_add(&a, d->tool); strs_add(&a, "--no-defaults");
        snprintf(arg, sizeof arg, "--datadir=%s", d->data); strs_add(&a, arg);
        strs_add(&a, "--auth-root-authentication-method=normal"); strs_add(&a, "--skip-test-db");
    } else {
        strs_add(&a, d->server); strs_add(&a, "--no-defaults"); strs_add(&a, "--initialize-insecure");
        snprintf(arg, sizeof arg, "--datadir=%s", d->data); strs_add(&a, arg);
    }
    if (d->drop) { snprintf(arg, sizeof arg, "--user=%s", d->user); strs_add(&a, arg); }
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc != 0) {
        char t[1024];
        tail_lines(log, 4, t, sizeof t);
        return seterr("the database setup failed (%d): %s%s", rc, t, d->drop ? " (running as root, the server runs as the user "
                      "mysql or nobody, which must be able to enter --work-dir)" : "");
    }
    sbpath(r, "server.log", outlog);
    o.out = outlog;
    o.timeout = 0;
    o.background = 1;
    snprintf(log, sizeof log, "%s/error.log", d->srv);     /* written by the server, as its own user */
    strs_add(&a, d->server); strs_add(&a, "--no-defaults");
    snprintf(arg, sizeof arg, "--datadir=%s", d->data); strs_add(&a, arg);
    snprintf(arg, sizeof arg, "--socket=%s", d->sock); strs_add(&a, arg);
    snprintf(arg, sizeof arg, "--pid-file=%s/mysqld.pid", d->srv); strs_add(&a, arg);
    snprintf(arg, sizeof arg, "--log-error=%s", log); strs_add(&a, arg);
    strs_add(&a, "--skip-networking"); strs_add(&a, "--innodb-flush-log-at-trx-commit=0");
    if (!d->mariadb) strs_add(&a, "--mysqlx=OFF");
    if (d->drop) { snprintf(arg, sizeof arg, "--user=%s", d->user); strs_add(&a, arg); }
    if (d->drop) chmod(r->sandbox, 0755);
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc <= 0) return seterr("cannot start %s", d->server);
    d->pid = (pid_t)rc;
    g_server = d->pid;
    write_server_file(r, d->pid, SIGTERM);
    return wait_ready(r, d, 120, log);
}

/* ---- starting a server: Docker ------------------------------------------------------------ */
static int docker_start(const rctx *r, dbs *d) {
    strs dirs = { 0 }, a = { 0 };
    runopt o;
    char f[AR_PATH], line[AR_PATH + 160], label[64], t[1024];
    int rc;
    if (strchr(r->docker, '/')) snprintf(d->dockerprog, sizeof d->dockerprog, "%s", r->docker);
    else {
        static const char *const none[] = { NULL };
        strs saved = g_engine_path;                      /* Docker is looked for in PATH */
        memset(&g_engine_path, 0, sizeof g_engine_path);
        search_dirs(&dirs, none);
        g_engine_path = saved;
        find_in(&dirs, r->docker, d->dockerprog, sizeof d->dockerprog);
        strs_free(&dirs);
    }
    if (!d->dockerprog[0] || !is_exec(d->dockerprog)) return seterr("Docker was not found (%s)", r->docker);
    memset(&o, 0, sizeof o);
    o.out = g_out;
    o.timeout = 30;
    strs_add(&a, d->dockerprog); strs_add(&a, "info"); strs_add(&a, "--format"); strs_add(&a, "{{.ServerVersion}}");
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc != 0) { tail_lines(g_out, 2, t, sizeof t); return seterr("Docker is installed but its daemon doesn't answer: %s", t); }
    d->docker = 1;
    snprintf(d->container, sizeof d->container, "%s", base_name(r->sandbox));
    sbpath(r, "container", f);
    snprintf(line, sizeof line, "%s\n%s\n", d->dockerprog, d->container);
    if (write_file(f, line, strlen(line))) return -1;
    snprintf(label, sizeof label, "altsql-restore.owner=%d", (int)getpid());
    o.timeout = 900;                                     /* the image may have to be pulled */
    strs_add(&a, d->dockerprog); strs_add(&a, "run"); strs_add(&a, "-d"); strs_add(&a, "--name"); strs_add(&a, d->container);
    strs_add(&a, "--label"); strs_add(&a, label);
    if (d->kind == K_PG) { strs_add(&a, "-e"); strs_add(&a, "POSTGRES_HOST_AUTH_METHOD=trust"); strs_add(&a, r->pg_image); }
    else {
        strs_add(&a, "-e"); strs_add(&a, "MYSQL_ALLOW_EMPTY_PASSWORD=yes");
        strs_add(&a, "-e"); strs_add(&a, "MARIADB_ALLOW_EMPTY_ROOT_PASSWORD=yes");
        strs_add(&a, r->my_image);
    }
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc != 0) { tail_lines(g_out, 3, t, sizeof t); return seterr("docker run %s failed: %s", d->kind == K_PG ? r->pg_image : r->my_image, rc == RUN_TIMEOUT ? "timed out" : t); }
    if (wait_ready(r, d, d->kind == K_PG ? 180 : 300, g_out)) return -1;
    strs_add(&a, d->dockerprog); strs_add(&a, "exec"); strs_add(&a, d->container);
    strs_add(&a, d->kind == K_PG ? "postgres" : "mysqld"); strs_add(&a, "--version");
    o.timeout = 30;
    run_list(&a, &o);
    strs_free(&a);
    first_line(g_out, t, sizeof t);
    snprintf(d->version, sizeof d->version, "%s", t);
    return 0;
}

/* ---- PostgreSQL: restore and count -------------------------------------------------------- */
/* Role names a plain dump hands objects to ("OWNER TO x", "GRANT ... TO x"), so the restore
 * can make them first, as a real restore would. Only plain identifiers or "quoted" ones. */
static void pg_roles(const char *dump, strs *roles) {
    FILE *f = fopen(dump, "r");
    char line[8192];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = NULL;
        if (!strncmp(line, "ALTER ", 6)) { p = strstr(line, " OWNER TO "); if (p) p += 10; }
        else if (!strncmp(line, "GRANT ", 6)) { p = strstr(line, " TO "); if (p) p += 4; }
        while (p && *p) {
            char name[256];
            size_t n = 0;
            while (*p == ' ') p++;
            if (*p == '"') {
                name[n++] = *p++;
                while (*p && *p != '"' && n < sizeof name - 2) name[n++] = *p++;
                if (*p != '"') break;
                name[n++] = *p++;
            } else {
                while ((*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '$') && n < sizeof name - 1) name[n++] = *p++;
            }
            name[n] = 0;
            if (n && strcmp(name, "PUBLIC") && strcmp(name, "CURRENT_USER") && strcmp(name, "SESSION_USER") && !strs_has(roles, name)) strs_add(roles, name);
            while (*p == ' ') p++;
            if (*p == ',') { p++; continue; }
            break;
        }
    }
    fclose(f);
}

static int dump_has_line(const char *dump, const char *prefix) {
    FILE *f = fopen(dump, "r");
    char line[4096];
    size_t n = strlen(prefix);
    int hit = 0;
    if (!f) return 0;
    while (!hit && fgets(line, sizeof line, f)) if (!strncmp(line, prefix, n)) hit = 1;
    fclose(f);
    return hit;
}

/* Splits text into lines in place. */
static int split_lines(char *s, char **v, int max) {
    int n = 0;
    while (*s && n < max) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        if (e && e > s && e[-1] == '\r') e[-1] = 0;
        v[n++] = s;
        if (!e) break;
        s = e + 1;
    }
    return n;
}

static int pg_count_db(const rctx *r, const dbs *d, const char *db, int prefix_db) {
    char *s, **lines, **qn = NULL;
    size_t len;
    int n, i, ntab = 0, rc = 0;
    buf q = { 0 };
    static const char *const list =
        "SELECT n.nspname || '.' || c.relname, format('%I.%I', n.nspname, c.relname) FROM pg_class c "
        "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relkind = 'r' "
        "AND n.nspname NOT IN ('pg_catalog', 'information_schema') AND n.nspname NOT LIKE 'pg\\_%' ORDER BY 1;";
    if (client_sql(r, d, db, list, 600, 1) != 0) { char t[1024]; tail_lines(g_out, 4, t, sizeof t); return seterr("cannot list the tables: %s", t); }
    if (!(s = read_all(g_out, &len))) return seterr("cannot read the table list");
    lines = (char **)xmalloc((len + 2) * sizeof *lines);
    n = split_lines(s, lines, (int)len + 1);
    qn = (char **)xmalloc(((size_t)n + 1) * 2 * sizeof *qn);
    for (i = 0; i < n; i++) {
        char *sep = strchr(lines[i], '\x1f');
        if (!sep || !lines[i][0]) continue;
        *sep = 0;
        qn[2 * ntab] = lines[i];
        qn[2 * ntab + 1] = sep + 1;
        buf_printf(&q, "SELECT %d, count(*), coalesce(sum(('x' || substr(md5(t::text), 1, 15))::bit(60)::bigint), 0) FROM ONLY %s t;\n",
                   ntab, sep + 1);
        ntab++;
    }
    if (ntab) {
        char *s2, **l2;
        size_t len2;
        int n2;
        buf full = { 0 };
        /* the same text for the same rows on any machine: dates, times and floats don't
         * follow the server's time zone or settings */
        buf_printf(&full, "SET TimeZone = 'UTC';\nSET DateStyle = 'ISO, YMD';\nSET IntervalStyle = 'postgres';\n"
                          "SET extra_float_digits = 1;\nSET bytea_output = 'hex';\n%s", buf_str(&q));
        buf_free(&q);
        q = full;
        if (client_sql(r, d, db, buf_str(&q), r->timeout, 1) != 0) {
            char t[1024];
            tail_lines(g_out, 4, t, sizeof t);
            rc = seterr("cannot count the rows: %s", t);
        } else if (!(s2 = read_all(g_out, &len2))) {
            rc = seterr("cannot read the counts");
        } else {
            l2 = (char **)xmalloc((len2 + 2) * sizeof *l2);
            n2 = split_lines(s2, l2, (int)len2 + 1);
            for (i = 0; i < n2; i++) {
                int k;
                long long cnt;
                char sum[128], name[1200], hex[65], h[200];
                if (sscanf(l2[i], "%d\x1f%lld\x1f%127s", &k, &cnt, sum) != 3 || k < 0 || k >= ntab) continue;
                snprintf(h, sizeof h, "pg:%lld:%s", cnt, sum);
                sha256_hex(h, strlen(h), hex);
                if (prefix_db) snprintf(name, sizeof name, "%s:%s", db, qn[2 * k]);
                else snprintf(name, sizeof name, "%s", qn[2 * k]);
                items_add(r->out, name, cnt, hex);
            }
            free(l2);
            free(s2);
        }
    }
    free(qn);
    free(lines);
    free(s);
    buf_free(&q);
    return rc;
}

static int pg_restore_into(const rctx *r, dbs *d) {
    strs a = { 0 }, roles = { 0 };
    runopt o;
    int rc, created = 0, i;
    char t[1500];
    if (client_sql(r, d, "postgres", "CREATE DATABASE restore_check;", 120, 1) != 0) {
        tail_lines(g_out, 3, t, sizeof t);
        return seterr("cannot make the database to restore into: %s", t);
    }
    if (!r->pg_custom) {
        buf q = { 0 };
        created = dump_has_line(r->dump, "CREATE DATABASE ");
        pg_roles(r->dump, &roles);
        for (i = 0; i < roles.n; i++) buf_printf(&q, "CREATE ROLE %s NOLOGIN;\n", roles.v[i]);
        if (roles.n) {
            client_sql(r, d, "postgres", buf_str(&q), 120, 0);          /* roles already there: fine */
            strs_addf(r->notes, "made %d role%s the dump hands objects to", roles.n, roles.n == 1 ? "" : "s");
        }
        buf_free(&q);
        strs_free(&roles);
    }
    memset(&o, 0, sizeof o);
    o.out = g_out;
    o.timeout = r->timeout;
    if (r->pg_custom) {
        if (d->docker) {
            strs_add(&a, d->dockerprog); strs_add(&a, "exec"); strs_add(&a, "-i"); strs_add(&a, d->container);
            strs_add(&a, "pg_restore"); strs_add(&a, "-h"); strs_add(&a, "127.0.0.1"); strs_add(&a, "-U"); strs_add(&a, "postgres");
            o.in = r->dump;
        } else {
            strs_add(&a, d->tool); strs_add(&a, "-h"); strs_add(&a, d->srv); strs_add(&a, "-p"); strs_add(&a, "5432");
            strs_add(&a, "-U"); strs_add(&a, "altsql");
        }
        strs_add(&a, "-d"); strs_add(&a, "restore_check"); strs_add(&a, "--exit-on-error");
        strs_add(&a, "--no-owner"); strs_add(&a, "--no-privileges"); strs_add(&a, "--single-transaction");
        if (!d->docker) strs_add(&a, r->dump);
    } else {
        client_args(d, &a, created ? "postgres" : "restore_check");
        strs_add(&a, "-v"); strs_add(&a, "ON_ERROR_STOP=1");
        if (!created) strs_add(&a, "--single-transaction");
        if (d->docker) o.in = r->dump;
        else { strs_add(&a, "-f"); strs_add(&a, r->dump); }
    }
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc == RUN_TIMEOUT) return seterr("the restore took longer than %.0f seconds and was stopped", r->timeout);
    if (rc != 0) {
        tail_lines(g_out, 4, t, sizeof t);
        return seterr("%s stopped with an error (exit %d): %s", r->pg_custom ? "pg_restore" : "psql", rc, t[0] ? t : "no message");
    }
    if (created) {
        char *s, *lines[512];
        size_t len;
        int n;
        if (client_sql(r, d, "postgres", "SELECT datname FROM pg_database WHERE datname NOT IN ('template0', 'template1') ORDER BY 1;", 120, 1) != 0)
            return seterr("cannot list the databases");
        if (!(s = read_all(g_out, &len))) return seterr("cannot list the databases");
        n = split_lines(s, lines, 512);
        for (i = 0; i < n; i++) {
            if (!lines[i][0]) continue;
            if (pg_count_db(r, d, lines[i], strcmp(lines[i], "restore_check") != 0)) { free(s); return -1; }
        }
        free(s);
        strs_add(r->notes, "the dump makes its own database (pg_dump -C); every database was counted");
        return 0;
    }
    return pg_count_db(r, d, "restore_check", 0);
}

/* ---- MySQL: restore and count ------------------------------------------------------------- */
static void my_quote(buf *q, const char *name) {
    buf_add(q, "`", 1);
    for (; *name; name++) { if (*name == '`') buf_add(q, "`", 1); buf_add(q, name, 1); }
    buf_add(q, "`", 1);
}

static int my_restore_into(const rctx *r, dbs *d) {
    strs a = { 0 };
    runopt o;
    int rc, n, i, ntab = 0;
    char t[1500], *s, **lines;
    size_t len;
    buf q = { 0 };
    char **nm;
    if (client_sql(r, d, NULL, "CREATE DATABASE restore_check;", 120, 1) != 0) {
        tail_lines(g_out, 3, t, sizeof t);
        return seterr("cannot make the database to restore into: %s", t);
    }
    client_args(d, &a, "restore_check");
    memset(&o, 0, sizeof o);
    o.in = r->dump;
    o.out = g_out;
    o.timeout = r->timeout;
    rc = run_list(&a, &o);
    strs_free(&a);
    if (rc == RUN_TIMEOUT) return seterr("the restore took longer than %.0f seconds and was stopped", r->timeout);
    if (rc != 0) { tail_lines(g_out, 4, t, sizeof t); return seterr("the mysql client stopped with an error (exit %d): %s", rc, t[0] ? t : "no message"); }
    if (client_sql(r, d, NULL, "SELECT table_schema, table_name FROM information_schema.tables WHERE table_type = 'BASE TABLE' "
                   "AND table_schema NOT IN ('mysql', 'information_schema', 'performance_schema', 'sys') ORDER BY 1, 2;", 600, 1) != 0) {
        tail_lines(g_out, 3, t, sizeof t);
        return seterr("cannot list the tables: %s", t);
    }
    if (!(s = read_all(g_out, &len))) return seterr("cannot read the table list");
    lines = (char **)xmalloc((len + 2) * sizeof *lines);
    n = split_lines(s, lines, (int)len + 1);
    nm = (char **)xmalloc(((size_t)n + 1) * 2 * sizeof *nm);
    for (i = 0; i < n; i++) {
        char *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        *tab = 0;
        nm[2 * ntab] = lines[i];
        nm[2 * ntab + 1] = tab + 1;
        buf_printf(&q, "SELECT %d, COUNT(*) FROM ", ntab);
        my_quote(&q, lines[i]); buf_add(&q, ".", 1); my_quote(&q, tab + 1);
        buf_add(&q, ";\nCHECKSUM TABLE ", 16);
        my_quote(&q, lines[i]); buf_add(&q, ".", 1); my_quote(&q, tab + 1);
        buf_add(&q, ";\n", 2);
        ntab++;
    }
    rc = 0;
    if (ntab) {
        if (client_sql(r, d, NULL, buf_str(&q), r->timeout, 1) != 0) {
            tail_lines(g_out, 3, t, sizeof t);
            rc = seterr("cannot count the rows: %s", t);
        } else {
            size_t len2;
            char *s2 = read_all(g_out, &len2), **l2;
            int n2, k = -1;
            long long cnt = 0;
            if (!s2) rc = seterr("cannot read the counts");
            else {
                l2 = (char **)xmalloc((len2 + 2) * sizeof *l2);
                n2 = split_lines(s2, l2, (int)len2 + 1);
                for (i = 0; i < n2; i++) {
                    int kk;
                    long long c;
                    char *tab = strrchr(l2[i], '\t');
                    if (k < 0) {
                        if (sscanf(l2[i], "%d\t%lld", &kk, &c) == 2 && kk >= 0 && kk < ntab) { k = kk; cnt = c; }
                    } else if (tab) {
                        char h[200], hex[65], name[600];
                        snprintf(h, sizeof h, "my:%lld:%s", cnt, tab + 1);
                        sha256_hex(h, strlen(h), hex);
                        if (!strcmp(nm[2 * k], "restore_check")) snprintf(name, sizeof name, "%s", nm[2 * k + 1]);
                        else snprintf(name, sizeof name, "%s.%s", nm[2 * k], nm[2 * k + 1]);
                        items_add(r->out, name, cnt, hex);
                        k = -1;
                    }
                }
                free(l2);
                free(s2);
            }
        }
    }
    free(nm);
    free(lines);
    free(s);
    buf_free(&q);
    return rc;
}

/* ---- the whole thing ------------------------------------------------------------------------ */
static int db_verify(rctx *r, int kind) {
    int tries[2], nt = 0, i;
    const char *ename = kind == K_PG ? "PostgreSQL" : "MySQL";
    snprintf(g_out, sizeof g_out, "%s/out.txt", r->sandbox);
    if (r->mode == SB_AUTO || r->mode == SB_LOCAL) tries[nt++] = SB_LOCAL;
    if (r->mode == SB_AUTO || r->mode == SB_DOCKER) tries[nt++] = SB_DOCKER;
    for (i = 0; i < nt; i++) {
        dbs d;
        int rc;
        memset(&d, 0, sizeof d);
        d.kind = kind;
        g_err[0] = 0;
        rc = tries[i] == SB_LOCAL ? (kind == K_PG ? pg_start_local(r, &d) : my_start_local(r, &d)) : docker_start(r, &d);
        if (rc) {
            strs_addf(r->unavail, "%s %s: %s", tries[i] == SB_LOCAL ? "local" : "Docker", ename, g_err);
            g_err[0] = 0;
            db_stop(r, &d);
            continue;
        }
        if (kind == K_MY && d.mariadb) ename = "MariaDB";
        if (tries[i] == SB_LOCAL) snprintf(r->engine, sizeof r->engine, "local %s server: %s (%s)", ename, d.version, d.server);
        else snprintf(r->engine, sizeof r->engine, "Docker image %s: %s", kind == K_PG ? r->pg_image : r->my_image, d.version);
        rc = kind == K_PG ? pg_restore_into(r, &d) : my_restore_into(r, &d);
        db_stop(r, &d);
        return rc ? ENG_FAILED : ENG_OK;
    }
    return ENG_UNAVAIL;
}
