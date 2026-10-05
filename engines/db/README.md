# AltSql DB 0.2.0-alpha (prototype)

The gateway database for a whole fleet. Devices keep running AltSql Core.
A gateway that serves many of them keeps their records in AltSql DB: one
file, a copy-on-write B-tree with two commit headers, every device's
payloads stored byte for byte, read with SQL or through a direct path that
never touches SQL.

Copyright 2026 AltSql.com. Open source under the Apache License 2.0.

## Where it stands

Version 0.2.0-alpha, one C file of 4,504 lines, with every figure measured.
AltSql DB and AltSql Core, the engine, are the project's focus for now, both
open source under the Apache License 2.0.

New in 0.2:

- **One way to write a row**. Every change to a table's rows goes through
  two internal functions, whichever interface asks for it: asd_row_check
  (the values in their columns' types, the key, and for an insert the check
  that no row has the key yet; it writes nothing) and asd_row_write (writes
  the row or deletes it). The row calls, SQL's INSERT, UPDATE and DELETE,
  DROP TABLE and sync all use them, so both interfaces keep the same rules.
  The 0.1 test suites print the same logs as before, apart from the version
  line.
- **New calls**: altsql_db_row_insert, the direct twin of INSERT (refuses a
  key that is already there with ALTSQL_EXISTS); altsql_db_table_drop, the
  direct twin of DROP TABLE (both call one internal function);
  altsql_db_tables, which lists the tables.
- **Sync in order**. altsql_db_sync_apply takes after_seq, the position the
  device read the batch from. A batch that starts past the gateway's position
  is refused with ALTSQL_DB_GAP (-24) and the gateway's position in
  *last_seq, so a device may send several batches ahead over a link that
  loses, repeats or reorders them, and every record still arrives once. This
  is an API change: callers pass the position they read the batch from.
- **The shell**, altsql-db (tools/shell.c, `make shell`): SQL, rows by key
  with no SQL step, buckets, sync batches from a file, tables and keys,
  checks. AltSql Core's shell gained `.sync AFTER FILE` to write the batches.

Moved to 0.3: statement savepoints and secondary indexes.

What 0.1 built, all still there:

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
  row; row_put, row_insert, row_get, row_del, row_seek, row_last, row_read,
  table_create, table_drop, tables.
- **Sync from Core devices**. altsql_db_sync_apply takes the bytes
  altsql_sync_read gave a device, unchanged, and the position it read them
  from: one transaction per batch with the device's new position, resends
  skipped, a gap refused, a damaged record ends the batch, a series with
  another layout refuses it.
- **SQL**. Core's own parser and query engine over the tree: CREATE TABLE
  and DROP TABLE [IF EXISTS], INSERT [OR REPLACE], UPDATE, DELETE, SELECT
  with everything Core's gateway SQL accepts, column [NOT] IN (values), and
  EXPLAIN. Five plans: point lookup, key list, range scan, range per device,
  full scan. Prepared statements with ? parameters, up to four at a time.
- **Ports**: a POSIX file with an exclusive lock, and a file in RAM that can
  lose power at any write or fail any call. **No malloc**: one block of
  memory from the caller.

## Build and test

    make test        unit and model tests, tables, sync, SQL, the interface test, crash matrix, fault injection
    make shell       the altsql-db shell, build/altsql-db
    make test-shell  both shells, a device file to a gateway file, every line compared
    make sanitize    the same under AddressSanitizer and UBSan (quick settings)
    make valgrind    unit, model, table, sync and SQL tests under Valgrind (quick settings)
    make strict      every warning an error, gcc and clang
    make vs-sqlite   random SQL on AltSql DB and SQLite, answers compared (fetches SQLite)
    make fuzz        45 minutes of coverage-guided fuzzing (clang with libFuzzer; 0.2's logged run took 10)
    make mutants     plants deliberate bugs and checks that the tests catch them
    make gate1       Gate 1 against SQLite and LMDB (fetches them; idle machine)
    make size        code size beside AltSql Core and SQLite
    sh tools/all_logs.sh <dir>   every log in results/ but fuzz.log and mutants.log

What the tests check (logs in `results/`, made on 0.2.0-alpha unless marked 0.1):

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
  each of the gateway's 623 writes in a sync session. New in 0.2, sync in
  order: 8 runs of four devices that send up to four batches ahead over a
  link that loses, repeats and reorders them (8,888 batches sent, 507 lost,
  527 delivered twice, 3,529 refused as gaps): every record applied exactly
  once. The same runs with every batch claiming position 0, as 0.1 assumed,
  lost 16,109 records.
- **test_same** (new): the interface test. One random workload runs twice,
  as SQL text and as direct calls: 4 seeds of 25,000 steps (52,259 writes,
  15,092 reads, 32,331 refusals, 1,944 tables dropped or made, 7,375
  transactions). The same answers and refusals code for code after every
  step, the same rows in 2,996 whole-table comparisons, and files identical
  byte for byte at the end.
- **test_shell** (new): both shells carry records from a device file to a
  gateway file; gaps, resends, rows by key, refusals and checks; every line
  compared with tests/test_shell.expected.
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
- **fuzz**: 10 minutes on 0.2, 745,022 inputs, no failure: SQL, prepared statements,
  damaged files, sync batches. 0.1's 45-minute run (3,253,055 inputs) found
  two bugs in damaged files, both fixed (a large value pointing to page 0;
  cells that overlap); their inputs are in tests/fuzz_regressions and
  replayed first.
- **mutants** (0.1): 37 of 37 planted bugs caught. Not run again for 0.2.

## Gate 1 (measured on 0.1)

Not run again for 0.2. `tools/gate1.c` runs the same one million keys and values through AltSql
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
database in a file with real syncs and 8 MB of memory (`results/bench.log`).
0.2's run is one run on a shared cloud machine, so its figures are
indicative; 0.1's were the best of three.

| | AltSql Core | AltSql DB 0.2 | SQLite |
|---|---|---|---|
| SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour | 200.9 ms | 163.4 ms | 305.0 ms |
| SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine | 166.1 ms | 131.6 ms | 284.8 ms |
| SELECT COUNT(*) FROM readings WHERE temp > 29.5 | 150.4 ms | 122.2 ms | 39.5 ms |
| SELECT time, temp FROM readings WHERE time >= 1768222000 AND machine = 3 | 0.6 ms | 0.5 ms | 0.1 ms |
| SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5 | 149.5 ms | 120.7 ms | 49.5 ms |
| Load, rows/s | 5,307,888 | 2,953,613 (direct path) | 1,987,340 |
| Bytes per row | 32.3 on flash | 31.4 | 21.0 |
| Reopen | scans the log | 0.03 ms | |

Sync: one million readings from 100 Core devices, batches of up to 4 KB, one
commit each: 326,699 readings/s (2,418 commits/s), 35.1 bytes per reading on
disk. 0.1's best of three: 279,094 readings/s; the queries 161.4, 131.8,
118.4, 0.5 and 118.3 ms on AltSql DB.

Code size at -O2: 96,821 bytes for AltSql DB (94,925 in 0.1), 100,383 with
both ports; with AltSql Core's gateway build (68,180) that is 168,563 bytes,
17.2% of SQLite's 982,281 (computed). At -Os: 69,610 and 72,172, against
SQLite's 662,732. Smallest working memory (0.1): 164,480 bytes with 4 KB
pages, 38,112 with 512-byte pages. A cursor takes 1,248 bytes.

## The demo

`demos/db`: twelve devices running AltSql Core sync into one AltSql DB file
in the browser (WebAssembly, 201,700 bytes, app/engine-db.v2.js); SQL with its plan beside the
direct path; the stored bytes beside the device's; a power cut in the middle
of a commit. `sh build.sh` builds it (needs `pip install ziglang`); `node
tools/facts.js` runs it under Node; `tools/headless.js` checks it in
headless Chromium at desktop and phone sizes.

## Known limits

- No statement savepoints yet: an UPDATE or DELETE that fails partway
  inside the caller's transaction fails that transaction, where a refused
  direct call leaves it usable. Primary keys only: no secondary indexes yet.
  Both come in 0.3.
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
