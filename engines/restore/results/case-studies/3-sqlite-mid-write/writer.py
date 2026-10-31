"""The application's nightly import: 20,000 invoices with one line each, in
one transaction, into the Chinook SQLite database (rollback journal, the
default). A small page cache makes SQLite write changed pages into the
database file before the commit, as any large transaction does. Half-way the
writer stops and waits, so the backup copy is taken at a known moment:

  python3 -I writer.py DB PAUSE_FILE GO_FILE
"""
import os, sqlite3, sys, time
db_path, pause, go = sys.argv[1:4]
db = sqlite3.connect(db_path, isolation_level=None)
db.execute("pragma cache_size=50")
db.execute("begin")
n0 = db.execute("select max(InvoiceId) from Invoice").fetchone()[0]
for i in range(1, 20001):
    inv = n0 + i
    db.execute("insert into Invoice (InvoiceId, CustomerId, InvoiceDate, BillingCountry, Total)"
               " values (?, ?, '2026-10-30 00:00:00', 'Brazil', 1.98)", (inv, i % 59 + 1))
    db.execute("insert into InvoiceLine (InvoiceLineId, InvoiceId, TrackId, UnitPrice, Quantity)"
               " values (?, ?, ?, 0.99, 2)", (100000 + i, inv, i % 3503 + 1))
    if i == 10000:
        open(pause, "w").close()
        while not os.path.exists(go):
            time.sleep(0.05)
db.execute("commit")
