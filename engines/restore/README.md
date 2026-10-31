# AltSql Restore (module, alpha)

A backup restore verifier. Most backups are never restored until the day
someone needs them, and that's the day you find out last night's dump was cut
short. AltSql Restore takes the backup, restores it into a throwaway sandbox,
counts the rows of every table, takes checksums, and compares the numbers with
the last good run. It alerts when the restore fails or when data shrank past a
threshold you set. Every run writes a dated, sealed report you can hand to an
auditor.

Copyright 2026 AltSql.com. Open source under the Apache License 2.0.

It's one program, `altsql-restore`, in C99 for Linux and macOS. It keeps its
history of runs in an AltSql DB file, the project's own gateway database.

## What it restores

| Backup | How it's restored | Needs |
|---|---|---|
| SQLite database file, or a `.backup` copy | copied into the sandbox and read page by page by a built-in reader of the SQLite file format | nothing |
| tar archive (`.tar`, `.tar.gz`, `.tgz`) | unpacked into the sandbox by a built-in tar reader | nothing |
| folder | copied into the sandbox | nothing |
| PostgreSQL dump (`pg_dump`, plain or custom format) | a throwaway PostgreSQL server in the sandbox; `psql` or `pg_restore` | local PostgreSQL server programs, or Docker |
| MySQL or MariaDB dump (`mysqldump`, `mariadb-dump`) | a throwaway MySQL or MariaDB server in the sandbox; the `mysql` client | local server programs, or Docker |

Any of these files may be gzip-compressed; a built-in reader unpacks it and
checks every CRC-32 and length, so a damaged `.gz` fails.

With `--sandbox auto` (the default) it looks for local server programs first:
`initdb`, `postgres`, `psql` and `pg_restore` for PostgreSQL (on `PATH` and in
the usual places such as `/usr/lib/postgresql/*/bin` and Homebrew), and
`mariadbd` or `mysqld` with the client for MySQL. If they're missing or the
server won't start, it tries Docker (`postgres:16`, `mysql:8.0`; change them
with `--pg-image` and `--mysql-image`). If neither works, the run is
**NOT VERIFIED**: exit status 3, and the report says what was tried and why it
failed. It never reports a backup it couldn't restore as passed.

## Use

```sh
altsql-restore --job shop-db /backups/shop-2026-10-31.sql.gz
```

Run it from cron or your scheduler after the nightly backup, with the same
`--job` name every night. A typical line:

```sh
altsql-restore --job shop-db --history /var/lib/altsql/restore.db \
  --report-dir /var/log/altsql-restore --signer "ops on call" \
  --webhook https://hooks.example.com/backups /backups/shop-latest.sql.gz
```

What a report looks like (a PostgreSQL dump, second run, one table shrank):

```
AltSql Restore report
=====================

Job:            shop-db
Run:            2
Date:           2026-10-31 09:21:09 UTC
Backup:         shop_pg_shrunk.sql
Backup size:    208,715 bytes
Backup SHA-256: 5c0d...
Kind:           PostgreSQL plain dump
Restored with:  local PostgreSQL server: postgres (PostgreSQL) 16.15 (/usr/lib/postgresql/16/bin/postgres)
Restore time:   3.1 s
Threshold:      alert when an item loses more than 10% of its rows
Compared with:  run 1 of 2026-10-31 09:21:02 UTC (PASSED)

Table                        rows         previous    change  checksum
public.audit_log              300              300     +0.0%  e3f8a293e06e0b08  same
public.customers            1,200            1,200     +0.0%  ec86aa07f03e8f27  same
sales.orders                4,000            5,000    -20.0%  00efb449a33d2edc  changed

Verdict: ALERT
  - sales.orders shrank from 5,000 to 4,000 rows (-20.0%), more than the 10% allowed

Webhook:        sent to https://hooks.example.com/backups
Signed off by:  ops on call
Seal (SHA-256 of the lines above): ce9c6ced...
```

### Options

| Option | What it does |
|---|---|
| `--job NAME` | the name the history keeps this backup under (default `default`) |
| `--kind KIND` | `auto`, `postgres`, `mysql`, `sqlite` or `archive`; auto reads the first bytes |
| `--history FILE` | the history, an AltSql DB file (default `altsql-restore.db`) |
| `--max-shrink PCT` | alert when a table loses more than PCT% of its rows (default 10) |
| `--accept` | take this run as the new baseline even if it shrank (after a planned clean-up) |
| `--sandbox MODE` | `auto`, `local` or `docker` |
| `--engine-path DIRS` | look for server programs only in these folders (colon-separated) |
| `--docker PROG` | the Docker program (default `docker`) |
| `--pg-image`, `--mysql-image` | the Docker images (default `postgres:16`, `mysql:8.0`) |
| `--work-dir DIR` | where sandboxes are made (default `$TMPDIR`, else `/tmp`) |
| `--timeout SEC` | stop a restore that runs longer, and fail it (default 3600) |
| `--no-end-check` | accept a plain dump that doesn't end with its dump tool's last line |
| `--report FILE` | also write the report to FILE |
| `--report-dir DIR` | also write it to `DIR/JOB-YYYYMMDD-HHMMSS-runN.txt` |
| `--signer NAME` | who signs off the report |
| `--sign-key FILE` | seal the report with HMAC-SHA256 under the key in FILE |
| `--webhook URL` | POST a JSON summary when a run doesn't pass (needs `curl`) |
| `--webhook-always` | POST after every run |
| `-q` | don't print the report |
| `--list [--job NAME]` | the runs in the history, with each report's seal |
| `--verify-report FILE [--sign-key FILE]` | check that a report is as it was written |

Exit status: **0** verified (PASSED or ACCEPTED), **1** alert (FAILED or
ALERT), **2** usage or internal error, **3** NOT VERIFIED.

## What counts as a failure

The restore **fails** when:

- a gzip file is cut short or damaged (CRC-32, length, bad codes);
- a SQLite file is shorter than its header says, isn't a whole number of
  pages, or has a page of the wrong kind, a cell or payload out of its page, a
  page reached twice, a page number past the end, a broken overflow chain, a
  free list that doesn't add up, or schema records that don't decode;
- a tar archive has a header that fails its checksum, ends inside a file, has
  no end blocks, or holds an absolute path or `..` (nothing is ever written
  outside the sandbox);
- a plain PostgreSQL dump doesn't end with `PostgreSQL database dump
  complete`, or a MySQL dump with `-- Dump completed` (both tools write that
  line last, so a dump cut short is caught before anything runs);
- `psql`, `pg_restore` or `mysql` stops at an error (they run with
  stop-on-first-error; a plain PostgreSQL dump runs in one transaction);
- the restore runs past `--timeout`.

The run **alerts** when the restore worked but a table (or, for archives, a
top-level folder or the total bytes) lost more than `--max-shrink` percent of
its rows since the last good run, or disappeared, or the restore holds no
tables at all. Tables that grew, new tables and changed checksums are shown
in the report but don't alert: data changes every day.

The baseline is the last run that **PASSED** or was **ACCEPTED**. An ALERT
doesn't move it, so the alert repeats every night until the data comes back
or someone runs `--accept`. Each run is compared with the last good run, not
with the first: a table that loses 5% a night for a month never crosses a 10%
threshold. Read the "previous" column, or keep the threshold low.

## Counts and checksums

- **SQLite**: rows counted by walking every table's b-tree (rowid tables and
  WITHOUT ROWID tables), indexes walked too. The checksum is SHA-256 over every
  row's rowid and record bytes, in key order.
- **PostgreSQL**: `count(*)`, and the sum of the first 60 bits of
  `md5(row::text)` over the rows, so the order rows come back in doesn't
  matter. Every table outside `pg_catalog` and `information_schema`.
- **MySQL**: `COUNT(*)` and `CHECKSUM TABLE`. `CHECKSUM TABLE` values depend
  on the server version, so moving the sandbox to another MySQL version shows
  every table as "changed" once (counts are unaffected).
- **Archives and folders**: one item per top-level entry (or per entry below a
  single top folder, as `tar czf site.tgz site` makes): its number of files
  and a SHA-256 over each file's path, size and SHA-256. Plus `(total bytes)`.

A plain dump and a custom-format dump of the same PostgreSQL data give the
same checksums; so do a folder, its `.tar` and its `.tar.gz`.

## The report and its seal

The report ends with a seal: the SHA-256 of every line above it, or with
`--sign-key`, an HMAC-SHA256 under your key. The history keeps the seal of
every run, and `--list` shows it, so a report produced later can be checked
two ways: `altsql-restore --verify-report FILE` recomputes the seal from the
text, and the seal should match the one in the history. Anyone can recompute a
plain SHA-256 seal after editing a report; a keyed seal can't be recomputed
without the key, which is the one to use for auditors.

## The history

An AltSql DB file with two tables: `runs` (job, run, time, verdict, kind,
backup name, backup SHA-256, report seal) and `counts` (job, run, item, rows,
checksum), keyed by job and run. A run holds an exclusive lock on it, so a
second run on the same history stops at once with a message. Open it with the
`altsql-db` shell to query it with SQL:

```sh
altsql-db altsql-restore.db "SELECT job, run, verdict FROM runs WHERE verdict <> 'PASSED';"
```

## Sandboxes and clean-up

Each run works in `WORKDIR/altsql-restore-PID-XXXXXX`. Database servers run
there on a unix socket with no network port, so they can't clash with a real
server on the same machine. The sandbox goes away:

1. at the end of the run, and on SIGINT, SIGTERM and SIGHUP;
2. when the run dies any other way (kill -9, a crash): a small watchdog
   process started with the sandbox notices at once, stops the server or
   removes the Docker container, and deletes the folder;
3. if the watchdog died too, the next run finds the sandbox, sees that its run
   is gone, and removes it, saying so in its report.

**Running as root.** PostgreSQL refuses to run as root, so the server runs as
the user `postgres` (or `nobody`), and MySQL as `mysql`. That user must be
able to enter `--work-dir`; `/tmp` works, a folder under a private home
folder may not, and the report says so when initdb is refused.

## Tests and figures

    make             build/altsql-restore and the unit tests
    make test        unit tests, then the end-to-end tests
    make sanitize    the same under AddressSanitizer and UBSan
    make strict      every warning an error, gcc and clang
    sh tools/all_logs.sh   every log in results/
    sh tools/bench.sh      timings, three runs each

From the logs in `results/` (31 October 2026, a 2-CPU Linux VM):

- **test_units.log**: 22 checks. SHA-256 and HMAC-SHA256 against published
  vectors, CRC-32, the inflater on stored, fixed and dynamic Huffman blocks
  and two-member files. 2,756 cut-short gzip files: every one fails (a cut
  exactly at the end of the first of two gzip members is a whole gzip file;
  the formats inside catch that one). 2,000 single flipped bits in a gzip
  file: all 2,000 fail.
- **test_restore.log**: 121 checks, all passed, with a local PostgreSQL 16.15
  and a local MariaDB 10.11.14. SQLite files cut in half, short by one page,
  short by 100 bytes, with a page header overwritten, with a broken header, as
  a cut or damaged `.gz`: all fail. Tar archives cut in the middle, without
  their end blocks, with a broken header checksum, with a `../` path: all
  fail. PostgreSQL and MySQL dumps cut short, missing only their last lines,
  with a bad row, and a cut-short custom-format dump: all fail. Plain,
  custom-format and tar-format dumps of the same PostgreSQL data give the
  same checksums; a `pg_dump -C` dump is counted in the database it makes. 20% fewer rows
  alerts, 4% doesn't, a dropped table alerts. PostgreSQL and MySQL with no
  server programs and no Docker: NOT VERIFIED. kill -9 of a run with a live
  PostgreSQL or MariaDB server in its sandbox: server stopped and sandbox
  removed; the same for SIGTERM and for a restore past `--timeout`; with the
  watchdog killed too, the next run sweeps the sandbox.
- **sanitize.log**: all the above under AddressSanitizer and UBSan.
- **bench.log**: a SQLite file of 1,000,000 rows (45 MB) checked in 0.6 to
  0.8 s; a tar.gz of 5,000 files in 0.4 s; the 6,500-row PostgreSQL test dump
  through a fresh local server in 1.5 to 1.8 s, the MySQL one in 2.0 to 2.2 s
  (most of it starting the server). Three runs each.

CI runs `make test` on Linux x86-64, Linux ARM and macOS on every push, with
whatever servers the runner has, and through Docker on ubuntu-latest. The last
lines of the log name the engines each run covered.

## Known limits

- An alpha. The report format and the history's tables may still change.
- SQLite keeps no page checksums: damage that leaves the structure valid shows
  only as changed counts and checksums. A `-wal` file beside the database is
  not part of the backup; checkpoint before copying, or use `.backup`.
  Virtual tables have no rows in the file and are skipped.
- PostgreSQL: `pg_dump` plain and custom formats. Not `pg_dumpall`, not the
  directory or tar formats. Roles the dump hands objects to are made as
  NOLOGIN roles first. A dump from a newer `pg_dump` may need a newer server
  than the one found; pick the Docker image with `--pg-image`.
- Docker mode depends on pulling the image; the first run is slow.
- POSIX only (Linux and macOS); no Windows build.
- A run's pid in the sandbox's owner file decides whether a sandbox is stale;
  a reused pid makes the sweep wait for a later run.

## License

Apache License 2.0. See [LICENSE](../../LICENSE) and [NOTICE](../../NOTICE).
The SQLite, tar and gzip readers, SHA-256 and HMAC are AltSql's own code,
written from the published formats; nothing third-party is compiled in.
