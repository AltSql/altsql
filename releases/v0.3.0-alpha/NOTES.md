# AltSql 0.3.0-alpha (5 October 2026)

AltSql DB gets statement savepoints and secondary indexes. AltSql Core, the engine, is unchanged. Both are open source under the Apache License 2.0.

**What's in it**

- **AltSql DB 0.3.0-alpha**, the database for a gateway that serves a whole fleet:
  - Statement savepoints. Inside your transaction, a statement that fails partway (a refused row, a NULL for a column, a value a UNIQUE index already has) is taken back, and the transaction goes on. In 0.2 the whole transaction failed. A statement that succeeds leaves the file exactly as it would have without a savepoint.
  - Secondary indexes. `CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON table (col, ...)` and `DROP INDEX [IF EXISTS] name`, or `altsql_db_index_create` and `altsql_db_index_drop`. Up to 8 a table, 4 columns each. Every writer keeps them through the one row-write path, sync on synced tables included. Two new plans, index lookup and index range, which EXPLAIN names. `altsql_db_index_seek` and `altsql_db_index_last` read rows in index order with no SQL step.
  - `altsql_db_check` walks each index against its table.
  - File format version 3. 0.3 opens version 2 files and writes version 3.
  - Fixed: an SQL read whose fixed text key values came to more than about 60 bytes could stop early and miss rows.
  - **API change:** `altsql_db_cursor`, `altsql_db_check_report` and `altsql_db_tableinfo` gain fields. Recompile.
- **AltSql Core 0.1.0-alpha**, unchanged. Its shell binary is the same, byte for byte, as in 0.2.0-alpha.
- **The browser demo** in `demos/db/` keeps an index on (machine, temp) as the devices sync, and asks one machine's readings through it.
- **Binaries:** `altsql-0.3.0-alpha-linux-x86_64.tar.gz`, in this folder, holds both shells for Linux x86-64, statically linked, with LICENSE, NOTICE and a README. `SHA256SUMS` lists the checksums of the archive and of both programs.

**Measured** (one run on a shared cloud machine; details in `engines/db/README.md`): on a million rows, a lookup through an index took 0.7 ms against 132.2 ms for a full scan (198x), and a range 1.5 ms against 141.1 ms (93x). A load into a table that keeps an index ran 232,978 rows/s (SQLite with the same index: 142,455). Statements under a savepoint ran 5% to 10% slower than on 0.2. AltSql Core and AltSql DB together: 194,652 bytes of code at -O2, 19.8% of SQLite.

**Tested:** two new suites (savepoints, indexes), the interface test with indexes, a crash matrix and fault injection on SQL with indexes, 100,000 random statements compared with SQLite, 10 minutes of fuzzing, and 56 of 56 planted bugs caught.

**Status:** alpha. Built and tested on a PC; nothing has run on a microcontroller or a gateway yet. File formats may still change. Not for production.

**Try it:** `cd core && make test`, then `cd engines/db && make test && make shell && make test-shell`. Or use the binaries:

```
curl -LO https://github.com/AltSql/altsql/raw/main/releases/v0.3.0-alpha/altsql-0.3.0-alpha-linux-x86_64.tar.gz
sha256sum altsql-0.3.0-alpha-linux-x86_64.tar.gz      # 50892462ef783bf9...
tar xzf altsql-0.3.0-alpha-linux-x86_64.tar.gz && cd altsql-0.3.0-alpha-linux-x86_64
```

```
./altsql new dev.db
./altsql dev.db "CREATE TABLE temps (time TIME, machine INT, temp FLOAT)"
./altsql dev.db "INSERT INTO temps VALUES (1700000001, 1, 21.5), (1700000002, 2, 22.25)"
./altsql dev.db ".sync 0 batch.bin"
./altsql-db gw.db ".sync 7 0 batch.bin" "CREATE INDEX tm ON temps (machine);"
./altsql-db gw.db "EXPLAIN SELECT * FROM temps WHERE machine = 2;" "SELECT * FROM temps WHERE machine = 2;"
```

**License:** Apache 2.0. Copyright 2026 AltSql.com.
