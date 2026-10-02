# AltSql DB (prototype, Alpha)

The gateway database for a whole fleet. Devices keep running AltSql Core.
A gateway that serves many of them keeps their records in AltSql DB: one
file, a copy-on-write B-tree with two commit headers, every device's
payloads stored byte for byte, read with SQL or through a direct path that
never touches SQL.

Copyright 2026 AltSql.com. Open source under the Apache License 2.0.

## Where it stands

The 0.1 Alpha is built:
version 0.1.0-alpha, one C file of 4,399 lines, with every figure measured.

- **Tree storage**. One file of pages (512 bytes to 64 KB, 4 KB by default).
  A copy-on-write B-tree, two 64-byte commit headers synced in order, large
  values on overflow pages, no journal and no write-ahead log. Two free
  lists: the ready list, which the next transaction may use in full, and the
  newest list, the pages the last commit freed, which wait one more commit.
  No transaction walks past entries it cannot use, so the file does not
  creep.
- **Transactions, catalog, buckets, the direct path**. begin, commit,
  rollback; named buckets; get, put, delete; cursors that survive writes.
- **Tables**. Primary keys of one or more columns, written so that keys
  compare as bytes in the values' order; rows packed as Core packs a series
  row; row_put, row_get, row_del, row_seek, row_last, row_read.
- **Sync from Core devices**. altsql_db_sync_apply takes the bytes
  altsql_sync_read gave a device, unchanged: one transaction per batch with
  the device's new position, resends skipped, a damaged record ends the
  batch, a series with another layout refuses it.
- **SQL**. Core's own parser and query engine over the tree: CREATE TABLE
  and DROP TABLE [IF EXISTS], INSERT [OR REPLACE], UPDATE, DELETE, SELECT
  with everything Core's gateway SQL accepts, column [NOT] IN (values), and
  EXPLAIN. Five plans: point lookup, key list, range scan, range per device,
  full scan. Prepared statements with ? parameters, up to four at a time.
- **Ports**: a POSIX file with an exclusive lock, and a file in RAM that can
  lose power at any write or fail any call. **No malloc**: one block of
  memory from the caller.

## Build and test

    make test        unit and model tests, tables, sync, SQL, crash matrix, fault injection
    make sanitize    the same under AddressSanitizer and UBSan (quick settings)
    make valgrind    unit, model, table, sync and SQL tests under Valgrind (quick settings)
    make strict      every warning an error, gcc and clang
    make vs-sqlite   random SQL on AltSql DB and SQLite, answers compared (fetches SQLite)
    make fuzz        45 minutes of coverage-guided fuzzing (clang with libFuzzer)
    make mutants     plants deliberate bugs and checks that the tests catch them
    make gate1       Gate 1 against SQLite and LMDB (fetches them; idle machine)
    make size        code size beside AltSql Core and SQLite
    sh tools/all_logs.sh <dir>   every log in results/ but fuzz.log and mutants.log

What the tests check (logs in `results/`):

- **test_db**: unit tests; 180 model-test runs of put, get, delete,
  scans with writes in between, commit, rollback and reopen against a plain
  in-memory model, checked against it as they go and in full at intervals,
  and checked page by page; ten
  rewrites of 4,000 entries (2,521 pages after round 3, 2,587 after
  round 6, 2,588 after round 10); each of bytes 16 to 59 of the newest
  commit header (44 bytes) damaged in turn, the file opening at the
  commit before.
- **test_rows**: key encodings sort as their values and come back as they
  went in; 40 model runs of tables against a model.
- **test_sync**: six real Core devices, 40 rounds, every row checked byte for
  byte as sent (6,116 rows in all); resent, stale, cut and damaged
  batches; a different layout refused; then three devices and a power cut at
  each of the gateway's 623 writes in a sync session.
- **test_sql**: 30 fixed and 1,000 random statements on Core's
  gateway SQL and on AltSql DB, the same answers; plans checked with EXPLAIN
  and rows read; 1,500 random narrowings against full scans; IN lists,
  UPDATE, DELETE, DROP TABLE and prepared statements.
- **test_vs_sqlite**: 4 seeds of 25,000 random statements on
  SQLite 3.53.4 and AltSql DB: 75,093 SELECTs and
  24,907 writes after 3,600 INSERTs that loaded the tables, the
  whole table compared after each write, 850,354 rows compared in all.
- **test_crash**: a cut at every write of a run, three ways of losing the
  unsynced writes, the cut write torn by 512-byte sectors: 3,354 cuts at
  1,118 writes,
  the file always opens at the last commit or the one under way.
- **test_fault**: the Nth file call fails, for every N: 1,091 runs.
- **fuzz**: 45 minutes, 3,253,055 inputs: SQL, prepared statements,
  damaged files, sync batches. Two bugs found in damaged files and fixed (a
  large value pointing to page 0; cells that overlap); their inputs are in
  tests/fuzz_regressions and replayed first. The logged run is the one after
  both fixes.
- **mutants**: 37 of 37 planted bugs caught.

## Gate 1

`tools/gate1.c` runs the same one million keys and values through AltSql
DB's direct path, SQLite 3.53.4 (built with its recommended options) and
LMDB 0.9.35, each in its own process, on real files with real syncs;
`tools/gate1_report.py` runs everything three times. Middle of three runs:

| Pass mark | Result | |
|---|---|---|
| Reads in the cache, 2x SQLite's best through SQL | 5.00x | pass |
| The same with whole-number keys, 2x SQLite's blob path | 1.21x | fail, accepted |
| Ordered scan, 0.8x SQLite | 0.88x | pass |
| 10,000-write transactions, 0.8x SQLite | 3.32x | pass |
| Read runs never enter SQL code | no call into Core's parser or executor (gdb) | pass |
| Three runs within 5% | reads_cached 2.8%, scan 3.6%, writes_tx10000 7.7%, reads_4x 4.8%; SQLite's and LMDB's own runs up to 42.9% | FAIL |

The decision (2 October): Gate 1 counts as passed against SQLite's SQL
path; the blob path stays on record as a gap; the spread needs a quiet
machine.

## Gate 2: measurements

`tools/bench.c`, Core's one-million-row benchmark and its five queries, each
database in a file with real syncs and 8 MB of memory; best of three runs
(`results/bench.log`):

| | AltSql Core | AltSql DB | SQLite |
|---|---|---|---|
| SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour | 204.7 ms | 161.4 ms | 303.8 ms |
| SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine | 165.4 ms | 131.8 ms | 270.2 ms |
| SELECT COUNT(*) FROM readings WHERE temp > 29.5 | 150.4 ms | 118.4 ms | 38.4 ms |
| SELECT time, temp FROM readings WHERE time >= 1768222000 AND machine = 3 | 0.6 ms | 0.5 ms | 0.1 ms |
| SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5 | 154.4 ms | 118.3 ms | 51.0 ms |
| Load, rows/s | 5,159,827 | 2,790,180 (direct path) | 1,937,974 |
| Bytes per row | 32.3 on flash | 31.4 | 21.0 |
| Reopen | scans the log | 0.03 ms | |

Sync: one million readings from 100 Core devices, batches of up to 4 KB, one
commit each: 279,094 readings/s (2,065 commits/s), 35.1 bytes
per reading on disk. Code size: 94,925 bytes at -O2 (68,203 at -Os)
for the engine, against SQLite's 982,281 (662,732). Smallest
working memory: 164,480 bytes with 4 KB pages, 38,112 with
512-byte pages. A cursor takes 1,248 bytes.

## The demo

`demos/db`: twelve devices running AltSql Core sync into one AltSql DB file
in the browser (WebAssembly, 202,403 bytes); SQL with its plan beside the
direct path; the stored bytes beside the device's; a power cut in the middle
of a commit. `sh build.sh` builds it (needs `pip install ziglang`); `node
tools/facts.js` runs it under Node; `tools/headless.js` checks it in
headless Chromium at desktop and phone sizes.

## Known limits

- Batches must arrive in the order a device sent them: each is applied past
  the confirmed position, so a batch that arrives ahead of a lost one moves
  the position past the lost records, and the device drops them.
- A commit syncs twice, so single-write transactions are slower than
  SQLite in WAL mode, which syncs once.
- After a bulk load in random order the file is larger than SQLite's: copy
  on write keeps the last versions of every page a big transaction touched,
  and there is no compaction yet.
- SQL reads tables of up to 16 columns (Core's query engine). ORDER BY ...
  DESC with LIMIT reads its whole range and sorts. A prepared statement is
  parsed again at each run, and a sorted result longer than its 8 KB batch
  runs again for each batch.
- Where SQL differs from SQLite: text for a number column, a number for a
  text column, or a NULL is refused, and a real stored in a whole-number
  column loses its fraction; % on reals keeps the fraction; ORDER BY a number sorts by that
  constant; an UPDATE of keys is checked when the statement ends; minus zero
  comes back as -0.
- The POSIX lock is a fcntl lock: it stops other processes, not a second
  open in the same process. The page checksum field is reserved and zero.
