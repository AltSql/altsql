#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Gate 1 for AltSql DB: runs every engine three times and checks the pass marks.

    python3 tools/gate1_report.py <scratch dir> [entries]

Writes results/gate1_run1.log .. run3.log and results/gate1_summary.log.
Pass marks (set in AltSql DB's design):
  - random reads in the cache at least twice the rate of SQLite's best
  - ordered scans and 10,000-write transactions at least 0.8 times SQLite's rate
  - the read runs never enter the SQL code (checked separately with gdb)
  - a clean crash matrix (tests/test_crash)
  - three runs within 5% of each other
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scratch = sys.argv[1]
entries = sys.argv[2] if len(sys.argv) > 2 else "1000000"
engines = ["altsql", "altsql-int", "sqlite", "sqlite-mmap", "sqlite-blob", "lmdb"]
os.makedirs(scratch, exist_ok=True)

runs = []
for r in range(3):
    res = {}
    log = []
    for e in engines:
        out = subprocess.run([os.path.join(here, "build", "gate1"), e, scratch, entries],
                             capture_output=True, text=True, check=True).stdout
        log.append(out)
        for line in out.splitlines():
            eng, name, value, unit = line.split()
            res[(eng, name)] = (float(value), unit)
    with open(os.path.join(here, "results", "gate1_run%d.log" % (r + 1)), "w") as f:
        f.write("".join(log))
    runs.append(res)

def med(key):
    vals = sorted(run[key][0] for run in runs)
    return vals[1]

def spread(key):
    vals = [run[key][0] for run in runs]
    return (max(vals) - min(vals)) / max(vals)

out = []
w = out.append
w("AltSql DB Gate 1: %s entries, 12-byte keys (device, time), 14-byte values; three runs, medians" % entries)
w("")
measures = [("reads_cached", "reads/s"), ("reads_4x", "reads/s"), ("scan", "entries/s"),
            ("writes_tx1", "writes/s"), ("writes_tx100", "writes/s"), ("writes_tx10000", "writes/s"),
            ("load", "entries/s"), ("bytes_on_disk", "bytes"), ("bytes_per_entry", "bytes"), ("peak_memory", "KB")]
w("%-16s" % "measure" + "".join("%14s" % e for e in engines))
for m, unit in measures:
    row = "%-16s" % m
    for e in engines:
        row += "%14s" % ("{:,.0f}".format(med((e, m))) if (e, m) in runs[0] else "n/a")
    w(row)
w("")
sq_sql = max(med(("sqlite", "reads_cached")), med(("sqlite-mmap", "reads_cached")))
sq_blob = med(("sqlite-blob", "reads_cached"))
a = med(("altsql", "reads_cached"))
ai = med(("altsql-int", "reads_cached"))
scan_sq = max(med(("sqlite", "scan")), med(("sqlite-mmap", "scan")))
scan_a = med(("altsql", "scan"))
tx_sq = max(med(("sqlite", "writes_tx10000")), med(("sqlite-mmap", "writes_tx10000")))
tx_a = med(("altsql", "writes_tx10000"))
spreads = {m: spread(("altsql", m)) for m in ("reads_cached", "scan", "writes_tx10000", "reads_4x")}

def mark(ok):
    return "PASS" if ok else "FAIL"

w("Pass marks")
w("  reads in the cache vs SQLite's best through SQL (WITHOUT ROWID, mmap off or on): %.2fx  (need 2.0)  %s"
  % (a / sq_sql, mark(a / sq_sql >= 2.0)))
w("  reads in the cache, whole-number keys, vs SQLite's blob path (sqlite3_blob_reopen): %.2fx  (need 2.0)  %s"
  % (ai / sq_blob, mark(ai / sq_blob >= 2.0)))
w("  ordered scan vs SQLite: %.2fx  (need 0.8)  %s" % (scan_a / scan_sq, mark(scan_a / scan_sq >= 0.8)))
w("  10,000-write transactions vs SQLite: %.2fx  (need 0.8)  %s" % (tx_a / tx_sq, mark(tx_a / tx_sq >= 0.8)))
w("  three runs within 5%: " + ", ".join("%s %.1f%%" % (m, 100 * v) for m, v in spreads.items())
  + "  " + mark(max(spreads.values()) <= 0.05))
w("")
w("For reference: single-write transactions, AltSql DB %.0f/s vs SQLite %.0f/s (two syncs per commit vs one);"
  % (med(("altsql", "writes_tx1")), med(("sqlite", "writes_tx1"))))
w("LMDB reads in the cache %.0f/s." % med(("lmdb", "reads_cached")))
text = "\n".join(out) + "\n"
with open(os.path.join(here, "results", "gate1_summary.log"), "w") as f:
    f.write(text)
print(text)
