#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Seeds and a dictionary for tests/fuzz_db.c: one file per seed, its first
byte the mode (0 SQL, 1 prepared, 2 damaged file, 3 and 4 sync batches).
Usage: python3 tools/fuzz_seeds.py <seed dir> <dict out> <Core's sql.dict>"""
import os, random, sys
out, dict_out, core_dict = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
sql = [
    "SELECT * FROM t1 WHERE a = 1 AND b IN (1, 2)",
    "SELECT a, COUNT(*), AVG(c) FROM t1 GROUP BY a HAVING COUNT(*) > 0 ORDER BY a DESC LIMIT 3 OFFSET 1",
    "SELECT k, v * 2 - w FROM t2 WHERE k NOT IN ('k1', 'x') OR v BETWEEN -5 AND 5",
    "SELECT * FROM t3 WHERE x >= 50 ORDER BY y",
    "EXPLAIN SELECT * FROM t1 WHERE a = 3 AND b > 2",
    "INSERT INTO t1 VALUES (9, 9, 9.5, 'nine'), (9, 10, 0, '')",
    "INSERT OR REPLACE INTO t2 (k, v, w) VALUES ('k1', 2, 3)",
    "UPDATE t1 SET c = c * 2, d = UPPER(d) WHERE a IN (1, 2)",
    "UPDATE t1 SET b = b + 100 WHERE a = 1",
    "UPDATE t3 SET x = x + 1",
    "DELETE FROM t2 WHERE v < 0",
    "DROP TABLE t3; CREATE TABLE t3 (x TIME, y LONG); INSERT INTO t3 VALUES (1, 2)",
    "DROP TABLE IF EXISTS nosuch; SELECT LENGTH(d), LOWER(d), ABS(c), ROUND(c, 1) FROM t1",
    "CREATE TABLE IF NOT EXISTS t4 (p TEXT, q INT, PRIMARY KEY (q, p)); INSERT INTO t4 VALUES ('a', 1)",
]
prep = [
    ("SELECT * FROM t1 WHERE a = ? AND b = ?", [(0, 1), (0, 2)]),
    ("SELECT * FROM t2 WHERE k IN (?, ?, 'zz')", [(2, 0), (2, 1)]),
    ("INSERT INTO t2 VALUES (?, ?, ?)", [(2, 5), (0, 7), (1, 0)]),
    ("UPDATE t1 SET c = ? WHERE a = ?", [(1, 3), (0, 1)]),
    ("DELETE FROM t3 WHERE y > ?", [(0, 1)]),
    ("SELECT a, SUM(c) FROM t1 WHERE b <> ? GROUP BY a ORDER BY a LIMIT ?", [(0, 0), (0, 2)]),
]
i = 0
for q in sql:
    open(os.path.join(out, "sql%02d" % i), "wb").write(b"\x00" + q.encode()); i += 1
for q, binds in prep:
    b = b"\x01" + q.encode() + b"\x00"
    for t, v in binds:
        b += bytes([t]) + (v.to_bytes(8, "little") if t != 1 else (v * 1.5).hex().encode()[:8].ljust(8, b"0"))
    open(os.path.join(out, "prep%02d" % i), "wb").write(b); i += 1
rnd = random.Random(7)
for k in range(6):
    open(os.path.join(out, "file%02d" % k), "wb").write(b"\x02" + bytes(rnd.randrange(256) for _ in range(3 * (k + 1))))
for k in range(4):
    open(os.path.join(out, "sync%02d" % k), "wb").write(bytes([3 + k % 2]) + bytes(rnd.randrange(256) for _ in range(40 + 30 * k)))
words = ["UPDATE", "DELETE", "DROP", "SET", "IN", "EXPLAIN", "REPLACE", "PRIMARY", "KEY", "?", "t1", "t2", "t3", "temps", "kv",
         "a", "b", "c", "d", "k", "v", "w", "x", "y", "z", "device", "seq"]
with open(dict_out, "w") as f:
    f.write(open(core_dict).read())
    for w in words:
        f.write('"%s"\n' % w)
print("%d seeds in %s" % (len(os.listdir(out)), out))
