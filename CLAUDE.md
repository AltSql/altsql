# Notes for Claude

Releases are automated. To release, raise the version and push to `main`;
never ask the owner to publish one. [RELEASING.md](RELEASING.md) has the
whole procedure, the checks to run afterwards, and how to copy it to a new
project.

## In progress: independent review of AltSql Restore

A reviewer that did not write the code is checking `engines/restore/` for
real bugs: the SQLite, tar and gzip readers, partial restores reported as
success, table enumeration, checksum determinism, injection, sandbox
isolation and clean-up, the history file and report seals. Each bug gets a
failing test first. A fixed bug means a 0.5.1 release.

## In progress: AltSql Restore case studies (31 October 2026)

Case studies and use examples for altsql.com, from real runs of the
released binary (releases/latest, checksum-checked). Only new files under
`engines/restore/results/case-studies/`; the module's code is not touched.
Rerun everything with `sudo sh engines/restore/results/case-studies/run_all.sh`.

- [x] Release v0.5.0 downloaded, SHA-256 checked (`fetch.sh`, `versions.txt`).
- [x] Pagila (PostgreSQL) and Chinook (PostgreSQL, MariaDB, SQLite), pinned
      by SHA-256; local PostgreSQL 16.15 and MariaDB 10.11.14, no Docker.
- [x] 1-pagila-postgres: verified, upload cut short (FAILED), archiving job
      with the wrong year (ALERT, rental -71.7% and 36 partitions emptied),
      alert repeats, rows put back (PASSED).
- [x] 2-restore-scripts: Latin-1 copy of a dump, upstream Pagila on PG 16,
      gunzip | psql of a cut file: psql exits 0 each time; Restore FAILED.
- [x] 3-sqlite-mid-write: cp during a transaction PASSED (see below).
- [x] 4-use-examples: cron, --max-shrink, --accept, altsql-db history,
      --sign-key/--verify-report, webhook, NOT VERIFIED.
- [ ] Site: case study posts, a use examples post, links.

**Found during case studies:** a SQLite file copied with cp in the middle
of a write transaction (rollback journal, the journal not copied) PASSED;
`PRAGMA integrity_check` lists 100 index entries missing and SQLite counts
412 invoices where Restore's table walk counts 469 (rows of an uncommitted
transaction). The reader walks indexes but doesn't compare an index's entry
count with its table's row count. Recorded in
`results/case-studies/3-sqlite-mid-write/`. Not fixed here.

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
