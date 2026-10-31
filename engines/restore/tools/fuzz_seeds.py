# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
#
# Seed files for tests/fuzz_readers.c: small SQLite databases that use the parts of the file
# format the reader walks (interior pages, overflow chains, indexes, WITHOUT ROWID tables, a
# free list, page sizes 512, 1024 and 4096), tar archives with GNU long names and
# pax headers, and the gzip test vectors.
#   python3 tools/fuzz_seeds.py OUTDIR
import io, os, shutil, sqlite3, sys, tarfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))


def db(name, page_size, rows=300, big=False, free=False):
    path = os.path.join(out, name)
    if os.path.exists(path):
        os.remove(path)
    c = sqlite3.connect(path)
    c.execute("PRAGMA page_size=%d" % page_size)
    c.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, v REAL, b BLOB)")
    c.execute("CREATE INDEX t_name ON t(name)")
    c.execute("CREATE TABLE w (k TEXT PRIMARY KEY, n INTEGER) WITHOUT ROWID")
    for i in range(rows):
        payload = ("x" * (3000 if big and i % 7 == 0 else 20)) + str(i)
        c.execute("INSERT INTO t VALUES (?, ?, ?, ?)", (i, payload, i / 3.0, bytes([i % 256]) * (i % 50)))
        c.execute("INSERT INTO w VALUES (?, ?)", ("key%05d" % i, i))
    if free:
        c.execute("DELETE FROM t WHERE id % 3 = 0")
    c.commit()
    c.close()


db("small_512.sqlite", 512, rows=60)
db("big_4096.sqlite", 4096, rows=400, big=True)
db("free_1024.sqlite", 1024, rows=200, big=True, free=True)


def tar(name, entries, fmt):
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=fmt) as t:
        for path, data in entries:
            if data is None:
                ti = tarfile.TarInfo(path)
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            else:
                ti = tarfile.TarInfo(path)
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
    open(os.path.join(out, name), "wb").write(bio.getvalue())


long = "site/" + "deep/" * 30 + "file.txt"
tar("ustar.tar", [("site", None), ("site/a.txt", b"hello\n"), ("site/b/c.bin", bytes(range(256)) * 3)], tarfile.USTAR_FORMAT)
tar("gnu_long.tar", [("site/a.txt", b"x" * 700), (long, b"long name\n")], tarfile.GNU_FORMAT)
tar("pax.tar", [("site/a.txt", b"y" * 1300), (long + ".pax", b"pax name\n")], tarfile.PAX_FORMAT)
for v in ("vec_fixed.gz", "vec_multi.gz", "vec_stored.gz"):
    shutil.copy(os.path.join(here, "..", "tests", "fixtures", v), os.path.join(out, v))
print("seeds in", out)
