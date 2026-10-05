#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Gate 2 for AltSql DB, three times: the one-million-row benchmark (build/bench) and what
statement savepoints cost (build/bench_sp, built on 0.3, and build/bench_sp02, built on 0.2's
header), each run three times on one core, then the middle of three and the spread.

    python3 tools/gate2_report.py <scratch dir> [rows]

Writes results/gate2_run1.log .. run3.log, results/bench_sp_run1.log .. run3.log and
results/gate2_summary.log. tools/all_logs.sh builds the three programs first.
Pass marks: AltSql DB faster than AltSql Core on all five of Core's queries (the Alpha's mark),
and a lookup through an index at least 10 times faster than a full scan (0.3's design mark).
A spread is (largest - smallest) / largest over the three runs; above 5% it is marked.
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scratch = sys.argv[1]
rows = sys.argv[2] if len(sys.argv) > 2 else "1000000"
os.makedirs(scratch, exist_ok=True)
CPU = ["taskset", "-c", "1"]


def parse_bench(text):
    """Every figure in one run of build/bench, keyed by what it measures."""
    res = {}
    last_query = None
    for line in text.splitlines():
        m = re.match(r"^(\w+)\s+query\s+([\d.]+) ms\s+\d+ rows\s+(.*)$", line)
        if m:
            last_query = (m.group(1), m.group(3))
            res[("query", m.group(1), m.group(3))] = float(m.group(2))
            continue
        m = re.match(r"^(\w+)\s+load\s+(\d+) rows/s(.*?), ([\d.]+) bytes per row", line)
        if m:
            kind = "indexed" if "with an index" in m.group(3) else "plain"
            res[("load", m.group(1), kind)] = float(m.group(2))
            res[("bytes_per_row", m.group(1), kind)] = float(m.group(4))
            continue
        m = re.match(r"^(\w+)\s+index \S+ \(temp\) made over \d+ rows in ([\d.]+) s; ([\d.]+) more bytes", line)
        if m:
            res[("index_build_s", m.group(1), "")] = float(m.group(2))
            res[("index_bytes_per_row", m.group(1), "")] = float(m.group(3))
            continue
        m = re.search(r"through the index (\d+) times faster", line)
        if m and last_query:
            res[("index_speedup", last_query[0], last_query[1])] = float(m.group(1))
            continue
        m = re.search(r"(\d+)% of the file's pages free", line)
        if m:
            res[("free_pages_pct", "altsqldb", "indexed")] = float(m.group(1))
            continue
        m = re.match(r"^altsqldb reopen ([\d.]+) ms", line)
        if m:
            res[("reopen_ms", "altsqldb", "")] = float(m.group(1))
            continue
        m = re.match(r"^sync\s+(\d+) readings/s", line)
        if m:
            res[("sync_readings_per_s", "sync", "")] = float(m.group(1))
    return res


def parse_sp(text):
    res = {}
    for line in text.splitlines():
        m = re.match(r"^\s+(.*?):\s+(\d+) statements/s", line)
        if m:
            res[m.group(1)] = float(m.group(2))
    return res


def run(cmd):
    return subprocess.run(CPU + cmd, capture_output=True, text=True, check=True, cwd=here).stdout


bench_runs, sp02_runs, sp03_runs = [], [], []
for r in range(3):
    out = run([os.path.join(here, "build", "bench"), rows, scratch])
    with open(os.path.join(here, "results", "gate2_run%d.log" % (r + 1)), "w") as f:
        f.write(out)
    bench_runs.append(parse_bench(out))
    a = run([os.path.join(here, "build", "bench_sp02")])
    b = run([os.path.join(here, "build", "bench_sp")])
    with open(os.path.join(here, "results", "bench_sp_run%d.log" % (r + 1)), "w") as f:
        f.write(a + b)
    sp02_runs.append(parse_sp(a))
    sp03_runs.append(parse_sp(b))


def mid(vals):
    return sorted(vals)[1]


def spread(vals):
    return (max(vals) - min(vals)) / max(vals) if max(vals) else 0.0


def fmt(v):
    if v >= 1000:
        return "{:,.0f}".format(v)
    if v >= 10:
        return "{:.1f}".format(v)
    return "{:.2f}".format(v)


out = []
w = out.append
w("AltSql DB Gate 2: %s rows, 8 MB of memory each, files with real syncs; three runs on one core, the middle of three" % rows)
w("(query times are each run's best of three; a spread above 5% is marked *)")
w("")
w("%-9s %-100s %12s %12s %12s %12s %8s" % ("engine", "measure", "run 1", "run 2", "run 3", "middle", "spread"))
keys = [k for k in bench_runs[0] if all(k in b for b in bench_runs)]
order = {"load": 0, "bytes_per_row": 1, "query": 2, "index_build_s": 3, "index_bytes_per_row": 4,
         "index_speedup": 5, "reopen_ms": 6, "free_pages_pct": 7, "sync_readings_per_s": 8}
eng_order = {"core": 0, "altsqldb": 1, "sqlite": 2, "sync": 3, "synced": 4}
keys.sort(key=lambda k: (eng_order.get(k[1], 9), order.get(k[0], 9)))
spreads = {}
for k in keys:
    vals = [b[k] for b in bench_runs]
    s = spread(vals)
    spreads[k] = s
    name = k[0] if not k[2] else (k[0] + ": " + k[2])
    if len(name) > 100:
        name = name[:97] + "..."
    w("%-9s %-100s %12s %12s %12s %12s %7.1f%%%s" % (k[1], name, fmt(vals[0]), fmt(vals[1]), fmt(vals[2]),
                                                 fmt(mid(vals)), 100 * s, " *" if s > 0.05 else ""))
w("")
w("What statement savepoints cost (a file in RAM; statements a second, middle of three)")
w("%-66s %12s %12s %8s %14s" % ("", "0.2", "0.3", "change", "spread 0.2/0.3"))
for k in sp03_runs[0]:
    v2 = [x[k] for x in sp02_runs]
    v3 = [x[k] for x in sp03_runs]
    w("%-66s %12s %12s %+7.0f%% %6.1f%% %5.1f%%" % (k[:66], fmt(mid(v2)), fmt(mid(v3)),
                                                  100 * (mid(v3) / mid(v2) - 1), 100 * spread(v2), 100 * spread(v3)))
w("")


def m(k):
    return mid([b[k] for b in bench_runs])


queries = [k[2] for k in keys if k[0] == "query" and k[1] == "core"]
w("Pass marks")
faster = [(q, m(("query", "altsqldb", q)), m(("query", "core", q))) for q in queries]
for q, a, c in faster:
    w("  %-100s AltSql DB %8.2f ms, Core %8.2f ms  %s" % (q[:100], a, c, "faster" if a < c else "NOT faster"))
w("  AltSql DB faster than AltSql Core on all five of Core's queries: %s"
  % ("PASS" if faster and all(a < c for _, a, c in faster) else "FAIL"))
sp = [k for k in keys if k[0] == "index_speedup" and "temp = 25.5)" in k[2]]
if sp:
    look = mid([b[sp[0]] for b in bench_runs])
    w("  a lookup through an index at least 10 times faster than a full scan: %.0fx  %s" % (look, "PASS" if look >= 10 else "FAIL"))
big = [k for k in keys if spreads[k] > 0.05]
w("  figures with a spread above 5%%: %d of %d (marked * above)" % (len(big), len(keys)))
text = "\n".join(out) + "\n"
with open(os.path.join(here, "results", "gate2_summary.log"), "w") as f:
    f.write(text)
print(text)
