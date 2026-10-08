# AltSql DB 0.4.1 (prototype)

The gateway database for a whole fleet. Devices keep running AltSql Core.
A gateway that serves many of them keeps their records in AltSql DB: one
file, a copy-on-write B-tree with two commit headers, every device's
payloads stored byte for byte, read with SQL or through a direct path that
never touches SQL.

Copyright 2026 AltSql.com. Open source under the Apache License 2.0.

## Where it stands

Version 0.4.1 (the engine of 0.3.0-alpha, unchanged), one C file of 5,470 lines, with every figure measured.
AltSql DB and AltSql Core, the engine, are the project's focus for now, both
open source under the Apache License 2.0. 0.3 builds the last two steps of
the design: statement savepoints and secondary indexes.

New in 0.3:

- **Statement savepoints**. Inside the caller's transaction, every SQL
  statement that writes runs under a savepoint. If it fails partway (a
  refused row, a NULL for a column, a value a UNIQUE index already holds, a
  statement out of SQL memory), it is taken back and the transaction goes on;
  0.2 failed the whole transaction. Copy on write never changes a page of an
  earlier commit, so the savepoint keeps only the pages the transaction wrote
  before the statement, the first time the statement changes or frees one,
  in the handle's SQL memory. A statement that succeeds leaves the file
  exactly as it would have without a savepoint. A statement that needs more
  room than half the SQL memory free when it starts, or that the file fails
  (I/O, damage), fails the transaction as in 0.2. A statement in a
  transaction of its own needs no savepoint.
- **Secondary indexes**. `CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON
  table (col, ...)` and `DROP INDEX [IF EXISTS] name`, or
  altsql_db_index_create and altsql_db_index_drop (one internal function).
  Up to 8 a table, 4 columns each; the rows already there are indexed at
  once. Each index is a key space of its own in the tree; an entry's key is
  the indexed values, then the row's key (a UNIQUE index keeps the row's key
  as the value). Every writer keeps them through asd_row_write: the row
  calls, SQL, DROP TABLE and sync, synced tables included (not UNIQUE there).
  UNIQUE refuses a row whose values another row holds (ALTSQL_EXISTS), from
  INSERT, INSERT OR REPLACE of another row, UPDATE and the row calls.
- **Index plans**: index lookup (every indexed column fixed by =) and index
  range (the first fixed, then a range on the next). An index is read when it
  fixes more than the primary key (2 for each column fixed, 1 for a range;
  the primary key wins a tie); an UPDATE doesn't read through an index whose
  columns it sets. EXPLAIN names the index.
- **Index cursors**: altsql_db_index_seek and altsql_db_index_last, by a
  prefix of the indexed values, read with altsql_db_row_read.
  altsql_db_table_info lists a table's indexes.
- **The checker** walks each index against its table under the header it
  checks: every entry names a row whose values give that entry, and there
  are as many entries as rows.
- **File format version 3**. 0.3 opens version 2 files and writes version 3.
- **Fixed**: an SQL read whose fixed text key values came to more than about
  60 bytes could stop early and miss rows (the cursor's 64-byte prefix); a
  plan key built from very long text values could write past its buffer.
- **API changes**: altsql_db_cursor, altsql_db_check_report (indexes,
  index_entries) and altsql_db_tableinfo (nindex, index[]) gain fields.

What 0.2 built: one way to write a row (asd_row_check, asd_row_write) for
every writer; altsql_db_row_insert, altsql_db_table_drop, altsql_db_tables;
sync in order with after_seq and ALTSQL_DB_GAP; the interface test; the
altsql-db shell.

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
  and DROP TABLE [IF EXISTS], CREATE and DROP INDEX (0.3), INSERT [OR
  REPLACE], UPDATE, DELETE, SELECT with everything Core's gateway SQL
  accepts, column [NOT] IN (values), and EXPLAIN. Seven plans: point lookup,
  key list, range scan, range per device, index lookup and index range (0.3),
  full scan. Prepared statements with ? parameters, up to four at a time.
- **Ports**: a POSIX file with an exclusive lock, and a file in RAM that can
  lose power at any write or fail any call. **No malloc**: one block of
  memory from the caller.

## Build and test

    make test        unit and model tests, tables, sync, SQL, the interface test, savepoints, indexes, crash matrix, fault injection
    make shell       the altsql-db shell, build/altsql-db
    make test-shell  both shells, a device file to a gateway file, every line compared
    make sanitize    the same under AddressSanitizer and UBSan (quick settings)
    make valgrind    unit, model, table, sync, SQL, interface, savepoint and index tests under Valgrind (quick settings)
    make strict      every warning an error, gcc and clang
    make vs-sqlite   random SQL on AltSql DB and SQLite, answers compared (fetches SQLite)
    make fuzz        45 minutes of coverage-guided fuzzing (clang with libFuzzer; 0.3's logged run took 10)
    make mutants     plants deliberate bugs and checks that the tests catch them
    make gate1       Gate 1 against SQLite and LMDB (fetches them; idle machine)
    make size        code size beside AltSql Core and SQLite
    sh tools/all_logs.sh <dir>   every log in results/ but fuzz.log, mutants.log and gate1_versions.log
    python3 tools/gate1_versions.py <dir>   Gate 1's direct path on 0.1, 0.2 and this code, side by side

What the tests check (logs in `results/`, made on 0.3.0-alpha unless marked 0.1):

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
- **test_same**: the interface test. One random workload runs twice, as SQL
  text and as direct calls: 4 seeds of 25,000 steps (51,704 writes, 18,032
  reads, 27,019 refusals, 2,245 tables and indexes dropped or made, 7,265
  transactions). Since 0.3 the tables carry indexes made both ways, one of
  them UNIQUE, and SQL's index plans are read against the index cursors. The
  same answers and refusals code for code after every step, the same rows in
  2,945 whole-table comparisons, and files identical byte for byte at the end.
- **test_savepoint** (new): statements made to fail partway inside
  transactions (a NULL at a random row after the rows before it changed;
  keys moved onto a row that stays; overflow pages freed and taken again; an
  UPDATE through an index, in another order than the leaves'; every row of a
  table), on one file of two that run the same work otherwise. 8 seeds of 60
  rounds: 30,794 statements on both files, 761 failed on one and taken back
  (126 of them over every row). After every failure both files hold the same
  rows; with a cache that holds the run they end byte for byte the same, and
  with a cache of 31 pages of 512 bytes, 357 of the failures read pages back
  from the file. A savepoint too small fails the transaction, and a rollback
  recovers.
- **test_index** (new): 6 runs on three tables (an int key, a text and int
  key, no key), pages of 512 bytes to 4 KB, caches large and small: 72,000
  random steps through every writer, 8,425 transactions, 4,877 indexes made
  and dropped, 3,711 statements failed partway inside transactions (tables
  and indexes as before each time). 33,574 queries through their plans
  against full scans, 13,102 of them through an index, all the same; 4,248
  checks; 307,670 rows walked by index cursors forwards and backwards; a
  prepared SELECT through an index read batch after batch. Then UNIQUE,
  names, limits and DDL in transactions one by one, a synced table with an
  index through 18 batches and a retention DELETE, and a version 2 file.
- **test_shell**: both shells carry records from a device file to a gateway
  file; gaps, resends, rows by key, refusals, indexes and checks; every line
  compared with tests/test_shell.expected.
- **test_sql**: 30 fixed and 1,000 random statements on Core's
  gateway SQL and on AltSql DB, the same answers; plans checked with EXPLAIN
  and rows read; 1,500 random narrowings against full scans; IN lists,
  UPDATE, DELETE, DROP TABLE and prepared statements.
- **test_vs_sqlite**: 4 seeds of 25,000 random statements on SQLite 3.53.4
  and AltSql DB, both with the same six indexes (one UNIQUE, over a key
  column, so it never refuses on one engine only), one dropped and made again
  every 1,000 statements: 75,070 SELECTs (8,427 through an index) and 24,930
  writes after 3,600 INSERTs that loaded the tables, the whole table compared
  after each write, 850,033 rows compared in all, none different.
- **test_crash**: a cut at every write of a run, three ways of losing the
  unsynced writes, the cut write torn by 512-byte sectors: 3,354 cuts at
  1,118 writes, the file always opens at the last commit or the one under
  way. New in 0.3: the same on SQL with three indexes and a statement that
  fails in each transaction (2,860 taken back on the way), 909 cuts, the
  indexes checked against the table under both headers after each.
- **test_fault**: the Nth file call fails, for every N: 1,091 runs, and 258
  more on the SQL workload with indexes, checked after every run and reopen.
- **fuzz**: 10 minutes on 0.3, 629,155 inputs, no failure: SQL on tables
  with indexes, one of them UNIQUE (half the SQL inputs run inside a
  transaction, a statement at a time, so that savepoints take failures
  back); prepared statements, damaged files, sync batches. 0.1's 45-minute run (3,253,055 inputs) found two bugs in damaged
  files, both fixed (a large value pointing to page 0; cells that overlap);
  their inputs are in tests/fuzz_regressions and replayed first.
- **mutants**: 56 of 56 planted bugs caught: 0.1's 37, one in sync in order
  (0.2), 8 in statement savepoints and 10 in secondary indexes (0.3).

## Gate 1 (measured again on 0.3)

`tools/gate1.c` runs the same one million keys and values through AltSql
DB's direct path, SQLite 3.53.4 (built with its recommended options) and
LMDB 0.9.35, each in its own process, on real files with real syncs;
`tools/gate1_report.py` runs everything three times. Middle of three runs on
0.3, on 5 October, pinned to one CPU (0.1's of 2 October in brackets):

| Pass mark | Result | |
|---|---|---|
| Reads in the cache, 2x SQLite's best through SQL | 4.42x (5.00x) | pass |
| The same with whole-number keys, 2x SQLite's blob path | 1.22x (1.21x) | fail, accepted |
| Ordered scan, 0.8x SQLite | 0.74x (0.88x) | FAIL |
| 10,000-write transactions, 0.8x SQLite | 2.58x (3.32x) | pass |
| Read runs never enter SQL code | no call into Core's parser or executor (gdb) | pass |
| Three runs within 5% | reads_cached 3.4%, scan 5.9%, writes_tx10000 13.7%, reads_4x 9.5%; SQLite's and LMDB's own runs up to 28.6% | FAIL |

The scan: AltSql DB scanned 13.7 million entries a second (14.6 on 0.1)
and SQLite with mmap 18.5 million (16.6 on 2 October; its own three runs
spread 28.6%). The scan's code is the same as in 0.2 and runs no more
instructions: callgrind counts 73.1 million for two scans of 100,000
entries on 0.3 and 73.3 million on 0.2 (`tools/scan_count.c`). Yet in six
rounds of the 0.1, 0.2 and 0.3 builds run in turn
(`tools/gate1_versions.py`, `results/gate1_versions.log`), 0.3 scanned 14.4
million entries a second at the middle against 15.3 and 15.7 million; its
reads, writes and load were level with the other two or ahead. Building 0.3 with its code
aligned to 64 bytes did not close the gap (13.7 million). Finding the cause
needs a quiet machine with hardware counters.

The decision of 2 October: Gate 1 counts as passed against SQLite's SQL
path; the blob path stays on record as a gap; the spread needs a quiet
machine. The scan result of 5 October came after it and is not decided.

## Gate 2: measurements

`tools/bench.c`, Core's one-million-row benchmark and its five queries, each
database in a file with real syncs and 8 MB of memory. Since 0.3 it also
makes a secondary index over the million rows, asks two questions through
it on AltSql DB and on SQLite, and loads the rows again into a table that
keeps an index. `tools/gate2_report.py` runs it three times on one CPU,
with the savepoint benchmark; the tables give the middle of three
(`results/gate2_summary.log`, each run in `results/gate2_run*.log`; the
machine in `results/machine.log`). 28 of the 45 figures spread more than 5%
across the three runs, so they are indicative; both marks below held in
every run.

| | AltSql Core | AltSql DB 0.3 | SQLite |
|---|---|---|---|
| SELECT time / 3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) FROM readings GROUP BY hour | 195.1 ms | 153.8 ms | 282.2 ms |
| SELECT machine, AVG(temp) FROM readings GROUP BY machine ORDER BY machine | 153.5 ms | 120.7 ms | 245.8 ms |
| SELECT COUNT(*) FROM readings WHERE temp > 29.5 | 141.0 ms | 107.8 ms | 34.4 ms |
| SELECT time, temp FROM readings WHERE time >= 1768222000 AND machine = 3 | 0.63 ms | 0.40 ms | 0.10 ms |
| SELECT time, temp FROM readings ORDER BY temp DESC LIMIT 5 | 142.3 ms | 105.9 ms | 45.7 ms |
| Load, rows/s | 5,804,910 | 2,879,473 (direct path) | 1,848,920 |
| Bytes per row | 32.3 on flash | 31.4 | 21.0 |
| Reopen | scans the log | 0.03 ms | |

AltSql DB is faster than AltSql Core on all five queries, the Alpha's mark.

A secondary index on the same million rows (new in 0.3):

| | AltSql DB 0.3 | SQLite |
|---|---|---|
| CREATE INDEX readings_temp ON readings (temp) | 0.60 s, 21.3 more bytes per row | 0.45 s, 18.0 more bytes per row |
| SELECT COUNT(*), MIN(time) FROM readings WHERE temp = 25.5 (1,000 rows) | 0.51 ms; 125.8 ms as a full scan: 238x | 0.05 ms |
| SELECT time, temp FROM readings WHERE temp BETWEEN 25.5 AND 25.52 AND machine = 6 (500 rows) | 1.45 ms; 131.0 ms as a full scan: 92x | 1.56 ms |
| Load into a table with an index on (machine, temp), rows/s | 256,052; 76.8 bytes per row, 22% of the file's pages free for reuse | 143,639; 42.9 bytes per row |

Each ratio is the middle of the three runs' own ratios. The design mark,
an index lookup on a million rows at least 10 times faster than a full
scan, is met (231x, 252x and 238x in the three runs).
AltSql DB reads each row the index names from the table; SQLite counts
from its index alone. Keeping an index costs a second write for each row,
and with copy on write every commit copies the index pages it changed:
hence the free pages after the indexed load.

What statement savepoints cost (`tools/bench_sp.c`, in the same three runs,
`results/bench_sp_run*.log`): SQL statements on a file in RAM, so the
engine's own time is all there is, built on 0.3 and on 0.2's header.
Statements a second, middle of three:

| | 0.2 | 0.3 | |
|---|---|---|---|
| INSERT, one row a statement, 1,000 statements a transaction (a savepoint each in 0.3) | 1,068,729 | 948,778 | -11% |
| UPDATE by key, 1,000 statements a transaction (a savepoint each in 0.3) | 283,387 | 259,365 | -8% |
| UPDATE by key, each statement its own transaction (no savepoint) | 123,033 | 136,580 | +11% |

Sync: one million readings from 100 Core devices, batches of up to 4 KB, one
commit each: 309,232 readings/s, 35.1 bytes per reading on disk (0.2, one
run: 326,699). 0.1's best of three: 279,094 readings/s; the queries 161.4,
131.8, 118.4, 0.5 and 118.3 ms on AltSql DB.

Code size at -O2: 122,912 bytes for AltSql DB (96,821 in 0.2), 126,472
with both ports; with AltSql Core's gateway build (68,180) that is 194,652
bytes, 19.8% of SQLite's 982,281 (computed), inside the design's limit of
a fifth (196,456 bytes, Gate 3). At -Os: 88,713 and 91,275, against
SQLite's 662,732. Smallest working memory: 165,568 bytes with 4 KB pages,
39,200 with 512-byte pages (0.1: 164,480 and 38,112). A cursor takes 1,248
bytes, a handle 1,304 (`results/size.log`, `results/minmem.log`).

## The demo

`demos/db`: twelve devices running AltSql Core sync into one AltSql DB file
in the browser (WebAssembly, 225,693 bytes, app/engine-db.v3.js); SQL with
its plan beside the direct path; since 0.3 an index on (machine, temp), kept
by every batch, and a preset question that reads through it; the stored
bytes beside the device's; a power cut in the middle of a commit. `V=3 sh
build.sh` builds it (needs `pip install ziglang`); `node tools/facts.js`
runs it under Node; `tools/headless.js` checks it in headless Chromium at
desktop and phone sizes (`results/demo_*.log`).

## Known limits

- Savepoints are for statements only: there is no SAVEPOINT or RELEASE in
  SQL. A statement that needs to keep more pages than half the SQL memory
  free when it starts, or that the file fails (I/O, damage), still fails the
  caller's transaction, as every failure did in 0.2.
- Indexes: up to 8 a table and 4 columns each. An index plan takes = on the
  index's first columns and one range after them; a key list (IN, or =
  joined by OR) is read only on the primary key, and ORDER BY never reads
  through an index. An index on a synced table can't be UNIQUE. An entry
  holds the indexed values and the row's key, so it must fit a key (1,005
  bytes with 4 KB pages, 109 with 512-byte pages): long text in an indexed
  column is refused (ALTSQL_TOOBIG). Every index slows the writes that touch
  it, and a load into an indexed table leaves more free pages behind.
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
