# Notes for Claude

Releases are automated. To release, raise the version and push to `main`;
never ask the owner to publish one. [RELEASING.md](RELEASING.md) has the
whole procedure, the checks to run afterwards, and how to copy it to a new
project.

## In progress: AltSql Restore (started 31 October 2026)

**What and why.** AltSql Restore is a new module, a backup restore verifier.
Most backups are never tested until the day they're needed. `altsql-restore`
takes last night's backup (a PostgreSQL dump, a MySQL/MariaDB dump, a SQLite
database file or `.backup` copy, or a plain file/folder archive), restores it
into a throwaway sandbox, counts rows per table and takes checksums, compares
them with the previous run kept in a small history, and alerts (non-zero exit,
a clear report, an optional webhook) when the restore fails or the data shrinks
beyond a set threshold. Each run writes a dated, sealed report that ops teams
can hand to auditors. Site messaging rule: the native hybrid comes first, then
the engine, then the database; modules are secondary. Restore gets a post and a
place in the module list, never the home page hero.

**Design decisions so far**
- Lives in `engines/restore/` beside `engines/db/`, C99 like the rest of the
  repo, one program `tools/altsql-restore` built from `engines/restore/altsql_restore.c`.
  POSIX only (fork/exec, like the repo's other code); no Windows build, the
  same as RELEASING.md says for the shells.
- History is kept in **AltSql DB** (the project's own storage, a natural fit):
  one file, `--history FILE` (default `altsql-restore.db`), tables `runs` and
  `counts`, keyed by job name. Runs that couldn't be verified are recorded but
  never become the baseline; a run compares with the last verified run.
- SQLite and archives need **no outside software**: a built-in read-only SQLite
  file reader (walks every table b-tree, counts rows, hashes each record, checks
  page types, ranges, loops and the header's page count, so a truncated or
  damaged file fails), and a built-in tar reader with a built-in gzip inflater
  (CRC32 and length checked). The backup is copied or unpacked into the sandbox
  directory first, then verified there. A folder is read file by file.
- PostgreSQL and MySQL: `--sandbox auto|local|docker`. auto tries local server
  binaries first (initdb/postgres/psql/pg_restore; mariadbd or mysqld plus the
  client), then Docker. A server runs on a unix socket inside the sandbox
  directory. Plain dumps must end with the dump tool's completion line, so a
  truncated dump fails; restores stop at the first error. Checksums: Postgres,
  an order-independent md5 over each row's text; MySQL, CHECKSUM TABLE.
- When no engine can be found or started the verdict is **NOT VERIFIED**, exit
  code 3, and the report says why. It never passes silently.
- Exit codes: 0 verified, 1 alert (restore failed, or a table lost more rows
  than `--max-shrink PCT`, default 10, or disappeared), 2 usage or internal
  error, 3 not verified.
- Sandbox clean-up: removed at exit and on SIGINT/SIGTERM/SIGHUP; each sandbox
  holds its owner's pid, and every run first sweeps sandboxes (and their server
  processes or Docker containers) left by a process that's gone, so a crash
  (kill -9) leaves nothing behind after the next run.
- Report: plain text, UTC date, backup SHA-256, sandbox used, per-table rows and
  checksums beside the previous run, verdict, `--signer NAME`, and a SHA-256
  seal of the report (HMAC-SHA256 with `--sign-key FILE` for a keyed seal).
  Webhook: JSON POST through the `curl` program when present; when it's
  missing the report says the webhook wasn't sent.
- Tests: `engines/restore/tests/test_restore.sh` (fixtures made with python3's
  sqlite3, tarfile and gzip), failure tests for the main promise (corrupt and
  truncated dumps fail; a shrink beyond the threshold alerts; a missing engine
  is NOT VERIFIED; the sandbox is cleaned up, also after kill -9). Logs go to
  `engines/restore/results/` through `tools/all_logs.sh`, the pattern
  `engines/db` uses. Postgres and MySQL tests run where the engine exists and
  say SKIPPED otherwise; the log records which engines were covered.
- Release: `altsql-restore` joins the release archives; version goes to 0.5.0
  (minor bump) in all three version lines when it ships.

**Environment notes (the session of 31 October).** Local PostgreSQL 16 server
binaries are present; MariaDB 10.11 was installed with apt; Docker's daemon
starts but its registry is blocked, so Docker sandboxes can't be tested here.

**Steps** (update as they finish)
- [x] Read the repo, decide the design (this section)
- [x] SHA-256, CRC32, gzip inflate, tar reader (`engines/restore/src/`)
- [x] SQLite file reader
- [x] History in AltSql DB, comparison, report, seal, webhook (webhook not yet tested)
- [x] Sandbox, clean-up and stale sweep; Postgres and MySQL via local binaries or Docker (checked by hand: PG 16 and MariaDB 10.11 local; kill -9 with a live server cleaned up by the watchdog)
- [x] Tests and failure tests; results logs (`engines/restore/results/`, via `tools/all_logs.sh`: unit 22 checks, end-to-end 116 checks with local PG 16 and MariaDB 10.11)
- [x] CI: build and test the module; release workflow adds the binary (pushed, not yet run: first CI run comes with the version bump)
- [x] README (manual), engines/restore/README.md, CHANGELOG, NOTICE/licences check (no new third-party code; THIRD-PARTY-LICENSES.txt names altsql-restore)
- [x] Review of the diff (no second agent available: a second pass of my own), fixes, logs recorded again. Fixed: pg_dump -Ft was checked as a file archive (now restored with pg_restore); --list created an empty history; history fd leaked into servers; on macOS the sweep could kill a reused pid (now checks the command line with ps). Fuzzed 700 damaged SQLite/tar files under ASan/UBSan: no crash.
- [ ] Version 0.5.0, push, watch CI and the release, check files and a downloaded binary (version raised and pushed; watching CI)
- [ ] Site: post, module list, download/reference pages; check the deploy

Notes from building: when run as root, servers drop to the postgres/mysql
user, so `--work-dir` must be a folder that user can enter (/tmp is fine).
Test fixtures (real pg_dump and mariadb-dump output) go in
`engines/restore/tests/fixtures/`.

**Where to start if this session stops:** read this section, then
`git log --oneline` for what is pushed; continue at the first unchecked step.
