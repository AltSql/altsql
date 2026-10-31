# Notes for Claude

Releases are automated. To release, raise the version and push to `main`;
never ask the owner to publish one. [RELEASING.md](RELEASING.md) has the
whole procedure, the checks to run afterwards, and how to copy it to a new
project.

## Done: independent review of AltSql Restore, fixes in 0.5.1 (31 October 2026)

A reviewer that didn't write the code went through `engines/restore/`. Five
bugs were confirmed, each with a test that failed on 0.5.0, and fixed
(CHANGELOG 0.5.1):
- High: a plain dump's client commands (`\!`, `\o`, `\connect host=`,
  mysql `system`/`source`/`tee`) ran on the host, as root, and the run
  PASSED. psql now runs with `\restrict KEY` (random key) after
  `pg_dump_commands()` rewrites the dump's own `\restrict`/`\connect`
  lines; with an older psql that reader is the only guard. mysql runs with
  `--binary-mode --local-infile=0`. Clients run as the server's user when
  root.
- High: a SQLite file copied mid-transaction PASSED (case study 3). The
  reader now checks index entries against table rows (partial indexes: not
  more), rising rowids, and that every page is used.
- Medium: MariaDB `SYSTEM VERSIONED` tables weren't counted.
- Medium: the webhook's files used predictable names in the work folder and
  followed links; now a mkdtemp folder.
- Low: the tar reader's pax parser read up to 5 bytes past its buffer.
Found sound: the gzip inflater, the SQLite walker's bounds, tar paths (no
links are made, `..` and absolute paths refused), shrink logic, exit codes,
seals (the last "Seal (" line is checked; a plain SHA-256 seal can be
recomputed by anyone, as the manual says), sandbox sockets (unix socket, no
port), the watchdog's pid checks. `make fuzz` (30,000 damaged files under
ASan/UBSan, `results/fuzz.log`): no crash after the pax fix.

**Still open (not bugs in the promise, recorded in the manual):** the dump's
SQL runs as superuser of the sandbox server and can reach the host through
`COPY ... PROGRAM` or `INTO OUTFILE` as the server's user (Docker mode
contains that); `pg_dump --clean` dumps and tables in tablespaces fail;
no size limit on unpacked data (a gzip bomb fills the work folder before
it fails); item names over 250 bytes are cut and could collide (archives
only).

## Done: AltSql Restore case studies (31 October 2026)

Real runs of the released v0.5.0 binary (SHA-256 checked) on Pagila and
Chinook, local PostgreSQL 16.15 and MariaDB 10.11.14, no Docker. Scripts,
outputs, sealed reports and histories in
`engines/restore/results/case-studies/` (`sudo sh run_all.sh` reruns all;
downloads go to the ignored `work/`). Site: three case study posts, a use
examples post, links from the Restore post and the modules page.

**Found during case studies:** a SQLite file copied with cp in the middle
of a write transaction (rollback journal, journal not copied) PASSED;
`PRAGMA integrity_check` lists 100 missing index entries, and the invoice
table walks to 469 rows where its index holds 412 (57 rows of an
uncommitted transaction). The reader walks indexes but doesn't compare an
index's entry count with its table's rows. See `3-sqlite-mid-write/`.
Fixed in 0.5.1: the copy now FAILS; the case study was rerun with 0.5.1 and
the site post has a section saying so.

## Done: AltSql Restore, released in 0.5.0 (31 October 2026)

**What.** A new module, `engines/restore/`, one C99 program `altsql-restore`
(POSIX: Linux and macOS). It restores a backup into a throwaway sandbox,
counts and checksums every table, compares with the last PASSED or ACCEPTED
run, and alerts. Backups: PostgreSQL dumps (pg_dump plain, custom, tar
format, with or without -C), MySQL/MariaDB dumps, SQLite files, tar(.gz)
archives, folders; any file may be gzipped. The manual is
`engines/restore/README.md`; the root README links it.

**How it's built.** `altsql_restore.c` includes Core, AltSql DB and
`src/*.c`: `common.c`, `sha256.c` (SHA-256, HMAC), `gz.c` (CRC-32, inflater,
gzip), `tar.c` (tar reader, folder copy, items), `sqlite.c` (read-only
SQLite file walker), `sandbox.c` (run_cmd, watchdog, sweep), `engines.c`
(PostgreSQL and MySQL via local server programs, then Docker). History:
AltSql DB file, tables `runs` and `counts`. Exit codes 0 verified, 1 alert,
2 usage, 3 NOT VERIFIED (no engine; never a pass). Reports end with a
SHA-256 or HMAC-SHA256 seal; `--verify-report` checks one; the history
keeps every seal. Clean-up: end of run, signal handler, a watchdog process
on a pipe (kill -9), and a sweep by the next run; foreground commands and
servers are recorded in the sandbox (`child`, `server`, `container`).

**Status.** Released as v0.5.0 (tag on cc14eaa); CI green on Linux x86-64,
Linux ARM and macOS; the release has altsql-restore in all four archives,
smoke-tested. Tests: 22 unit checks, 123 end-to-end checks (local PG 16.15
and MariaDB 10.11.14 here; CI also covers Docker postgres:16 and mysql:8.0
on ubuntu-latest, and whatever local servers the runners start). Logs:
`engines/restore/results/` via `tools/all_logs.sh`. Site: post
"AltSql Restore Proves Your Nightly Backup Restores Before You Need It",
the modules page and the download page.

**Things to know.** Run as root, servers drop to postgres/mysql/nobody, so
`--work-dir` must be enterable by them (/tmp is). PostgreSQL checksums pin
TimeZone=UTC and output styles so they match across machines. Tests must
not hard-code checksums. Don't push to main between a version bump and its
release: the release only publishes the commit main still points at.

**What comes next (not started).**
- Record the fuzz pass (700 damaged SQLite/tar files under ASan, no crash,
  done by hand on 31 October) as a script and a results log.
- Docker mode was only tested in CI (the registry is blocked in the cloud
  session); a local Docker run on a laptop would be worth a log.
- A slow shrink under the threshold each night never alerts (each run is
  compared with the last good run); a "compare with N runs ago" option.
- zip archives, pg_dumpall, directory-format pg dumps are not supported.
- The browser demo still says 0.4.1 (its build is unchanged).

- Oct 31 2026, owner decision: dump SQL running as superuser of the throwaway server (COPY ... TO PROGRAM, INTO OUTFILE reaching the host as the server user) stays as documented in the module manual; Docker mode contains it. Not to be changed without the owner.
