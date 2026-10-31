/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* The sandbox: a private folder for one run, the programs it runs, and its clean-up.
 *
 * A sandbox is WORKDIR/altsql-restore-PID-XXXXXX. It holds an "owner" file with the pid of
 * the run, and, while a database server runs in it, "server" (the server's pid and the
 * signal that stops it) or "container" (the Docker program and the container's name).
 *
 * Clean-up happens three ways, so the sandbox goes even when the run doesn't end well:
 *   1. at the end of the run, and from the handler of SIGINT, SIGTERM and SIGHUP;
 *   2. a watchdog process, started with the sandbox, waits on a pipe from the run; when the
 *      run is gone for any reason (kill -9 included) the pipe closes and the watchdog
 *      stops the server or container and removes the folder;
 *   3. every run first sweeps WORKDIR for sandboxes whose run and watchdog are both gone. */
#include <glob.h>
#include <pwd.h>

#define SB_PREFIX "altsql-restore-"

static char   g_sandbox[AR_PATH];       /* the current sandbox, "" when none       */
static volatile pid_t g_child = 0;      /* the command running now (its group)     */
static volatile pid_t g_server = 0;     /* a server started by this run (its group) */
static int    g_wd_fd = -1;
static pid_t  g_wd_pid = 0;

/* ---- running programs ----------------------------------------------------------------- */
typedef struct runopt {
    const char *in;           /* stdin from this file, else /dev/null                   */
    const char *out;          /* stdout and stderr to this file (truncated), else /dev/null */
    double      timeout;      /* seconds; 0: none                                        */
    int         drop;         /* run as uid/gid (when this program runs as root)         */
    uid_t       uid;
    gid_t       gid;
    int         background;   /* start it in a session of its own and return its pid    */
} runopt;

#define RUN_SPAWN   (-1)      /* could not start it                                      */
#define RUN_TIMEOUT (-2)      /* killed when its time ran out                            */

static void child_setup(const runopt *o) {
    int fd;
    if (o->background) setsid(); else setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    fd = open(o->in ? o->in : "/dev/null", O_RDONLY);
    if (fd >= 0) { dup2(fd, 0); if (fd > 0) close(fd); }
    fd = o->out ? open(o->out, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open("/dev/null", O_WRONLY);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    if (o->drop) {
        if (setgid(o->gid) || setuid(o->uid)) { fprintf(stderr, "cannot switch to uid %d\n", (int)o->uid); _exit(126); }
    }
}

/* Runs argv. Returns its exit status (0-255), 128+N when signal N ended it, RUN_SPAWN or
 * RUN_TIMEOUT; with background set, the pid (> 0) or RUN_SPAWN. */
static int run_cmd(char *const argv[], const runopt *o) {
    pid_t pid;
    int st;
    double end = o->timeout > 0 ? now_mono() + o->timeout : 0;
    long nap = 5;
    fflush(NULL);
    pid = fork();
    if (pid < 0) return RUN_SPAWN;
    if (pid == 0) {
        child_setup(o);
        execv(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (o->background) return (int)pid;
    setpgid(pid, pid);
    g_child = pid;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { g_child = 0; return RUN_SPAWN; }
        if (end && now_mono() > end) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            g_child = 0;
            return RUN_TIMEOUT;
        }
        sleep_ms(nap);
        if (nap < 100) nap *= 2;
    }
    g_child = 0;
    kill(-pid, SIGKILL);                      /* anything it left running in its group */
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return RUN_SPAWN;
}

/* argv from a string list, NULL-terminated (pointers into the list). */
static char **argv_of(strs *L) {
    char **v = (char **)xmalloc(((size_t)L->n + 1) * sizeof *v);
    int i;
    for (i = 0; i < L->n; i++) v[i] = L->v[i];
    v[L->n] = NULL;
    return v;
}
static int run_list(strs *L, const runopt *o) {
    char **v = argv_of(L);
    int rc = run_cmd(v, o);
    free(v);
    return rc;
}

/* The last lines of a file, for messages: up to max lines, joined with " | ". */
static void tail_lines(const char *path, int max, char *out, size_t cap) {
    size_t n, i, start;
    int lines = 0;
    char *s = read_all(path, &n);
    out[0] = 0;
    if (!s) return;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
    start = n;
    while (start > 0) {
        if (s[start - 1] == '\n' && ++lines >= max) break;
        start--;
    }
    while (start < n && (s[start] == '\n' || s[start] == ' ' || s[start] == '\r')) start++;
    {
        size_t o = 0;
        for (i = start; i < n && o + 4 < cap; i++) {
            if (s[i] == '\n') { if (o + 4 < cap) { memcpy(out + o, " | ", 3); o += 3; } }
            else if (s[i] != '\r') out[o++] = s[i];
        }
        out[o] = 0;
    }
    free(s);
}

/* ---- finding programs ------------------------------------------------------------------- */
static strs g_engine_path;               /* --engine-path: when set, the only places looked in */

/* Folders to look in: --engine-path when given, else PATH and then the extra patterns
 * (glob patterns, newest version first). */
static void search_dirs(strs *out, const char *const *extra) {
    int i;
    if (g_engine_path.n) { for (i = 0; i < g_engine_path.n; i++) strs_add(out, g_engine_path.v[i]); return; }
    {
        const char *p = getenv("PATH");
        char tmp[AR_PATH];
        while (p && *p) {
            const char *c = strchr(p, ':');
            size_t k = c ? (size_t)(c - p) : strlen(p);
            if (k && k < sizeof tmp) { memcpy(tmp, p, k); tmp[k] = 0; if (!strs_has(out, tmp)) strs_add(out, tmp); }
            p = c ? c + 1 : NULL;
        }
    }
    for (i = 0; extra && extra[i]; i++) {
        glob_t g;
        if (glob(extra[i], 0, NULL, &g) == 0) {
            size_t j;
            for (j = g.gl_pathc; j > 0; j--) if (!strs_has(out, g.gl_pathv[j - 1])) strs_add(out, g.gl_pathv[j - 1]);
        }
        globfree(&g);
    }
}
static int find_in(const strs *dirs, const char *name, char *out, size_t cap) {
    int i;
    for (i = 0; i < dirs->n; i++) {
        snprintf(out, cap, "%s/%s", dirs->v[i], name);
        if (is_exec(out)) return 1;
    }
    out[0] = 0;
    return 0;
}

/* ---- the sandbox -------------------------------------------------------------------------- */
/* Is pid running? A zombie (dead, not yet reaped: its parent died and PID 1 doesn't reap,
 * as in some containers) counts as gone; Linux shows that in /proc. */
static int pid_alive(pid_t pid) {
    char p[64], *s, *e;
    int alive;
    if (pid <= 0) return 0;
    alive = kill(pid, 0) == 0 || errno == EPERM;
    if (!alive) return 0;
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    if ((s = read_all(p, NULL)) != NULL) {
        if ((e = strrchr(s, ')')) != NULL && e[1] == ' ' && (e[2] == 'Z' || e[2] == 'X')) alive = 0;
        free(s);
    }
    return alive;
}

/* Stops the process group of a server and waits for it, up to a few seconds. */
static void stop_group(pid_t pid, int sig) {
    double end = now_mono() + 15;
    int st;
    if (pid <= 1) return;
    kill(-pid, sig);
    kill(pid, sig);
    while (now_mono() < end) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD && !pid_alive(pid))) break;
        sleep_ms(50);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &st, WNOHANG) > 0) {}
}

/* Is pid a server of this sandbox? On Linux its command line names the sandbox; elsewhere
 * the pid is trusted. */
static int pid_in_sandbox(pid_t pid, const char *dir) {
    char p[64], *cmd;
    size_t n, i;
    int hit;
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    if (!is_dir("/proc/self")) return pid_alive(pid);
    if (!(cmd = read_all(p, &n))) return 0;
    for (i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
    hit = strstr(cmd, dir) != NULL;
    free(cmd);
    return hit;
}

/* Stops whatever runs in a sandbox and removes the folder. Safe to call twice. */
static int sandbox_remove(const char *dir) {
    char p[AR_PATH], *s;
    if (!dir[0]) return 0;
    snprintf(p, sizeof p, "%s/server", dir);
    if ((s = read_all(p, NULL)) != NULL) {
        int pid = 0, sig = SIGTERM;
        if (sscanf(s, "%d %d", &pid, &sig) >= 1 && pid > 1 && pid_in_sandbox((pid_t)pid, dir)) stop_group((pid_t)pid, sig);
        free(s);
    }
    snprintf(p, sizeof p, "%s/container", dir);
    if ((s = read_all(p, NULL)) != NULL) {
        char *nl = strchr(s, '\n');
        if (nl) {
            runopt o;
            char *argv[5];
            *nl = 0;
            memset(&o, 0, sizeof o);
            o.timeout = 60;
            argv[0] = s; argv[1] = (char *)"rm"; argv[2] = (char *)"-f"; argv[3] = nl + 1; argv[4] = NULL;
            { char *e = strchr(nl + 1, '\n'); if (e) *e = 0; }
            run_cmd(argv, &o);
        }
        free(s);
    }
    return rmtree(dir);
}

/* The handler of SIGINT, SIGTERM and SIGHUP: stop what runs, then leave; the watchdog
 * removes the folder at once, and the folder's server is stopped by it too. */
static void on_signal(int sig) {
    if (g_child > 0) kill(-g_child, SIGKILL);
    if (g_server > 0) kill(-g_server, SIGKILL);
    _exit(128 + sig);
}

static void watchdog_start(const char *dir) {
    int p[2];
    pid_t pid;
    if (pipe(p)) return;
    fflush(NULL);
    pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return; }
    if (pid == 0) {
        char c;
        int fd;
        setsid();
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        close(p[1]);
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); if (fd > 2) close(fd); }
        while (read(p[0], &c, 1) < 0 && errno == EINTR) {}
        sandbox_remove(dir);
        _exit(0);
    }
    close(p[0]);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    g_wd_fd = p[1];
    g_wd_pid = pid;
    {
        char f[AR_PATH], line[32];
        snprintf(f, sizeof f, "%s/watchdog", dir);
        snprintf(line, sizeof line, "%d\n", (int)pid);
        write_file(f, line, strlen(line));
    }
}

static int sandbox_create(const char *workdir) {
    char tmpl[AR_PATH], f[AR_PATH], line[64];
    struct sigaction sa;
    if (snprintf(tmpl, sizeof tmpl, "%s/" SB_PREFIX "%d-XXXXXX", workdir, (int)getpid()) >= (int)sizeof tmpl)
        return seterr("the work folder's path is too long");
    if (!mkdtemp(tmpl)) return seterr("cannot make a sandbox in %s: %s", workdir, strerror(errno));
    chmod(tmpl, 0755);
    snprintf(g_sandbox, sizeof g_sandbox, "%s", tmpl);
    snprintf(f, sizeof f, "%s/owner", tmpl);
    snprintf(line, sizeof line, "%d\n", (int)getpid());
    if (write_file(f, line, strlen(line))) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    watchdog_start(tmpl);
    return 0;
}

static void sandbox_finish(void) {
    if (g_server > 0) g_server = 0;
    if (g_sandbox[0]) sandbox_remove(g_sandbox);
    g_sandbox[0] = 0;
    if (g_wd_fd >= 0) { close(g_wd_fd); g_wd_fd = -1; }
    if (g_wd_pid > 0) {
        int st;
        double end = now_mono() + 10;
        while (now_mono() < end && waitpid(g_wd_pid, &st, WNOHANG) == 0) sleep_ms(10);
        g_wd_pid = 0;
    }
}

/* Removes sandboxes in workdir whose run and watchdog are both gone. Returns how many. */
static int sandbox_sweep(const char *workdir, strs *notes) {
    DIR *d = opendir(workdir);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char dir[AR_PATH], f[AR_PATH], *s;
        int owner = 0, wd = 0;
        if (strncmp(e->d_name, SB_PREFIX, strlen(SB_PREFIX))) continue;
        if (snprintf(dir, sizeof dir, "%s/%s", workdir, e->d_name) >= (int)sizeof dir || !is_dir(dir)) continue;
        snprintf(f, sizeof f, "%s/owner", dir);
        if ((s = read_all(f, NULL)) != NULL) { owner = atoi(s); free(s); }
        snprintf(f, sizeof f, "%s/watchdog", dir);
        if ((s = read_all(f, NULL)) != NULL) { wd = atoi(s); free(s); }
        if (!owner) {
            /* made a moment ago and not written yet, or not ours: leave it unless its name says it's ours and old */
            struct stat st;
            if (stat(dir, &st) || time(NULL) - st.st_mtime < 3600) continue;
        } else if (pid_alive((pid_t)owner) || pid_alive((pid_t)wd)) {
            continue;
        }
        if (sandbox_remove(dir) == 0) {
            n++;
            strs_addf(notes, "removed a sandbox left by an earlier run that stopped (pid %d): %s", owner, e->d_name);
        }
    }
    closedir(d);
    return n;
}

/* The user servers run as when this program runs as root (PostgreSQL refuses root). */
static int service_user(const char *const *names, uid_t *uid, gid_t *gid, char *name, size_t cap) {
    int i;
    for (i = 0; names[i]; i++) {
        struct passwd *pw = getpwnam(names[i]);
        if (pw) { *uid = pw->pw_uid; *gid = pw->pw_gid; snprintf(name, cap, "%s", names[i]); return 1; }
    }
    return 0;
}
