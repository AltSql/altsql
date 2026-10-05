# AltSql 0.2.0-alpha (5 October 2026)

AltSql now focuses on two products, both open source under the Apache License 2.0: the engine, **AltSql Core**, and the database, **AltSql DB**.

**What's in it**

- **AltSql DB 0.2.0-alpha**, the database for a gateway that serves a whole fleet:
  - One way to write a row. Every change to a table's rows goes through the same internal check and write, whichever interface asks for it: the row calls, SQL's INSERT, UPDATE and DELETE, DROP TABLE and sync. The 0.1 test suites print the same logs as before, apart from the version line.
  - New calls: `altsql_db_row_insert` (the direct twin of INSERT), `altsql_db_table_drop` (the direct twin of DROP TABLE) and `altsql_db_tables`.
  - Sync in order. `altsql_db_sync_apply` now takes `after_seq`, the position the device read the batch from, and refuses a batch that starts past the gateway's position with `ALTSQL_DB_GAP` and the gateway's position. A device may send several batches ahead over a link that loses, repeats or reorders them, and every record still arrives once. **API change:** callers pass the position they read the batch from.
  - The interface test: one random workload run as SQL text and as direct calls, 4 seeds of 25,000 steps, with the same answers and refusals at every step and files identical byte for byte at the end.
  - `altsql-db`, a command-line shell for the database.
- **AltSql Core 0.1.0-alpha**, the engine, unchanged. Its shell, `altsql`, gains `.sync AFTER FILE`, which writes a sync batch to a file.
- **The browser demo** in `demos/db/`, rebuilt on AltSql DB 0.2.
- **Binaries:** `altsql-0.2.0-alpha-linux-x86_64.tar.gz`, in this folder, holds both shells for Linux x86-64, statically linked, with LICENSE, NOTICE and a README. `SHA256SUMS` lists the checksums of the archive and of both programs.

Statement savepoints and secondary indexes move to 0.3.

**Status:** alpha. Built and tested on a PC; nothing has run on a microcontroller or a gateway yet. File formats may still change. Not for production.

**Try it:** `cd core && make test`, then `cd engines/db && make test && make shell && make test-shell`. Or use the binaries:

```
curl -LO https://github.com/AltSql/altsql/raw/main/releases/v0.2.0-alpha/altsql-0.2.0-alpha-linux-x86_64.tar.gz
sha256sum altsql-0.2.0-alpha-linux-x86_64.tar.gz      # e9dddea8476023f3...
tar xzf altsql-0.2.0-alpha-linux-x86_64.tar.gz && cd altsql-0.2.0-alpha-linux-x86_64
```

```
./altsql new dev.db
./altsql dev.db "CREATE TABLE temps (time TIME, machine INT, temp FLOAT)"
./altsql dev.db "INSERT INTO temps VALUES (1700000001, 1, 21.5), (1700000002, 2, 22.25)"
./altsql dev.db ".sync 0 batch.bin"
./altsql-db gw.db ".sync 7 0 batch.bin" "SELECT * FROM temps;"
```

**License:** Apache 2.0. Copyright 2026 AltSql.com.
