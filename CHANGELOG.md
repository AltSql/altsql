# Changelog

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
- **Binaries:** both shells for Linux x86-64, statically linked, attached to
  the GitHub release.
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
