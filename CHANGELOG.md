# Changelog

## 0.4.0 (8 October 2026)

The first AltSql release published with ready-built binaries for every
system, made and checked by the release workflow. The engine's behaviour is
unchanged from 0.3.0-alpha; file format version 3 is unchanged too.

- **One version for both.** AltSql Core and AltSql DB now share one version
  number, 0.4.0. Core was 0.1.0-alpha and DB 0.3.0-alpha; `altsql-db
  --version` prints both, and both now say 0.4.0.
- **Binaries for Linux and macOS.** `altsql` and `altsql-db` for Linux x86-64
  and ARM64 (static) and for macOS on Apple silicon and Intel, as
  `altsql_<system>_<arch>.tar.gz` on the release page. Each build runs the
  end-to-end shell test before the release goes out. The names carry no
  version, so the `releases/latest/download/` links stay the same.
- **macOS.** AltSql DB built with `-D_POSIX_C_SOURCE` alone couldn't see
  `F_FULLFSYNC` and didn't compile. The sync now falls back to `fsync` when
  `F_FULLFSYNC` is hidden, and both Makefiles add `-D_DARWIN_C_SOURCE` on
  macOS, so a sync there reaches the disk.
- **Tests on every push.** Core's tests, DB's tests and the shell test run on
  Linux x86-64, Linux ARM and macOS (`.github/workflows/test.yml`). A green
  run on `main` with a new version is what publishes a release; see
  [RELEASING.md](RELEASING.md).
- **The browser demo** in `demos/db/` is rebuilt at 0.4.0
  (`app/engine-db.v4.js`).
- Releases now live on the GitHub release page. `releases/` keeps the
  0.2.0-alpha and 0.3.0-alpha folders as they were.

## After 0.3.0-alpha (5 October 2026)

- **The speed figures run again**, three times each on one CPU of the same
  shared machine: Gate 1 (`tools/gate1_report.py`) on 0.3, and Gate 2
  through the new `tools/gate2_report.py` (the one-million-row benchmark and
  the savepoint benchmark). Gate 1's ordered scan came to 0.74x SQLite
  against a mark of 0.8x; `tools/gate1_versions.py` and
  `tools/scan_count.c` compare it with 0.1 and 0.2: the same instructions,
  about 9% slower on this machine. `engines/db/README.md` has the figures.
  The engine's code is unchanged.

## 0.3.0-alpha (5 October 2026)

- **AltSql DB 0.3.0-alpha.**
  - **Statement savepoints.** Inside the caller's transaction, a statement
    that fails partway (a refused row, a NULL for a column, a value a UNIQUE
    index already has, a statement out of SQL memory) is taken back, and the
    transaction goes on. 0.2 failed the whole transaction. The savepoint keeps
    the pages the statement changes that the transaction wrote before it, in
    the handle's SQL memory; a statement that succeeds leaves the file exactly
    as it would have without one. If a statement needs more room than half
    the SQL memory free when it starts, or the file fails it (I/O, damage),
    the failure fails the transaction, as in 0.2. Statements in a
    transaction of their own need no savepoint and cost nothing more.
  - **Secondary indexes.** `CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON
    table (col, ...)` and `DROP INDEX [IF EXISTS] name` in SQL;
    `altsql_db_index_create` and `altsql_db_index_drop` directly (both call
    one internal function). Up to 8 indexes a table, 4 columns an index. The
    rows already in the table are indexed when the index is made. Every
    writer keeps them through the one row-write path: the row calls, SQL,
    DROP TABLE and sync, synced tables included (indexes there are not
    UNIQUE). A UNIQUE index refuses a row whose values another row has, from
    INSERT, INSERT OR REPLACE of another row, UPDATE and the row calls
    alike (`ALTSQL_EXISTS`).
  - **Two new plans:** index lookup (every indexed column fixed by =) and
    index range (the first ones fixed, then a range on the next). An index is
    read when it fixes more than the primary key; the primary key wins a
    tie; an UPDATE doesn't read through an index whose columns it sets.
    EXPLAIN names the index.
  - **Index cursors:** `altsql_db_index_seek` and `altsql_db_index_last` give
    rows in index order, read with `altsql_db_row_read`. `altsql_db_table_info`
    lists a table's indexes.
  - **The checker** (`altsql_db_check`) walks each index against its table,
    both ways, under the header it checks; the report counts indexes and
    their entries.
  - **File format version 3** (the catalog knows indexes). 0.3 opens version
    2 files and writes version 3.
  - **Fixed:** an SQL read whose fixed text key values came to more than
    about 60 bytes could stop early and miss rows; a plan built from very long
    text values could write past its buffer.
  - **API changes:** `altsql_db_cursor`, `altsql_db_check_report` and
    `altsql_db_tableinfo` gain fields: recompile.
  - **Tests:** `test_savepoint` and `test_index` are new; the interface test
    runs with indexes made both ways; the crash matrix and fault injection
    gain an SQL workload with three indexes and a failing statement in every
    transaction; the SQLite comparison runs with indexes; the fuzzer makes
    indexes and runs statements inside transactions; 56 planted bugs (19 new),
    all caught.
  - **The shell:** `.schema` shows a table's indexes, `.check` reports them.
- **AltSql Core 0.1.0-alpha**, the engine, is unchanged.
- **The browser demo** in `demos/db/` keeps an index on (machine, temp) as the
  devices sync, and asks one machine's readings through it
  (`app/engine-db.v3.js`).
- **Binaries:** both shells for Linux x86-64, statically linked, in
  `releases/v0.3.0-alpha/` with the release notes and checksums.

## 0.2.0-alpha (5 October 2026)

AltSql now focuses on two products, both open source under the Apache
License 2.0: the engine, AltSql Core, and the database, AltSql DB.

- **AltSql DB 0.2.0-alpha.**
  - One way to write a row: every change to a table's rows goes through the
    same internal check and write, whichever interface asks for it (the row
    calls, SQL's INSERT, UPDATE and DELETE, DROP TABLE and sync). The 0.1
    test suites print the same logs as before, apart from the version line.
  - New calls: `altsql_db_row_insert` (the direct twin of INSERT; refuses a
    key that is already there), `altsql_db_table_drop` (the direct twin of
    DROP TABLE) and `altsql_db_tables` (lists the tables).
  - Sync in order: `altsql_db_sync_apply` takes `after_seq`, the position the
    device read the batch from. A batch that starts past the gateway's
    position is refused with `ALTSQL_DB_GAP` (-24) and the gateway's
    position, so a device may send several batches ahead over a link that
    loses, repeats or reorders them, and every record still arrives once.
    This closes 0.1's known limit on batches that arrive out of order.
    **API change:** callers pass the position they read the batch from.
  - The interface test (`tests/test_same.c`): one random workload as SQL text
    and as direct calls, the same answers and refusals at every step, and
    files identical byte for byte at the end.
  - `altsql-db`, a command-line shell for AltSql DB (`make shell`), with an
    end-to-end test that carries records from a device file to a gateway
    file (`make test-shell`).
- **AltSql Core 0.1.0-alpha**, the engine, is unchanged. Its shell gains
  `.sync AFTER FILE`, which writes a sync batch to a file.
- **The browser demo** in `demos/db/` is rebuilt on AltSql DB 0.2
  (`app/engine-db.v2.js`).
- **Binaries:** both shells for Linux x86-64, statically linked, in
  `releases/v0.2.0-alpha/` with the release notes and checksums.
- Statement savepoints and secondary indexes move to 0.3.

## 0.1.0-alpha (October 2026)

First public release.

- **AltSql Core 0.1.0-alpha**, the engine: key-value and time-series on the
  device, SQL on the gateway, and the same records on both, synced byte for
  byte.
- **AltSql DB 0.1.0-alpha**, the database for a gateway that serves a whole
  fleet: one file for every device, read by key with no SQL step, or with
  SQL over the same records.
- **The AltSql DB browser demo** in `demos/db/`.
- License: Apache 2.0.
