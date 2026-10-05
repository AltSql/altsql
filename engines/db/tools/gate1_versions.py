#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Gate 1's direct path on 0.1, 0.2 and the current code, side by side on one machine.

    python3 tools/gate1_versions.py <scratch dir> [rounds] [entries]

Builds tools/gate1.c on the headers of releases v0.1.0-alpha and v0.2.0-alpha (git show), on the
current header, and on the current header with code aligned to 64 bytes; runs the four in turn on
CPU 1, round after round (AltSql DB only, 1,000,000 entries by default, 6 rounds). Then counts
the instructions of the scan alone (tools/scan_count.c, two scans of 100,000 entries) under
valgrind --tool=callgrind on 0.2 and on the current code. Writes results/gate1_versions.log.
Needs build/sqlite3.o, build/mdb.o and build/midl.o (make gate1 fetches and builds them).
"""
import os
import re
import statistics
import subprocess
import sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scratch = sys.argv[1]
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 6
entries = sys.argv[3] if len(sys.argv) > 3 else "1000000"
T = os.path.join(here, "..", "..", "..", "third")
B = os.path.join(here, "build")
os.makedirs(scratch, exist_ok=True)


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, cwd=here, capture_output=True, text=True, **kw).stdout


def version_of(inc):
    with open(os.path.join(inc, "altsql_db.h")) as f:
        return re.search(r'#define ALTSQL_DB_VERSION "([^"]+)"', f.read()).group(1)


heads = {}
for tag in ("v0.1.0-alpha", "v0.2.0-alpha"):
    d = os.path.join(B, "hdr-" + tag)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "altsql_db.h"), "w") as f:
        f.write(sh(["git", "show", tag + ":engines/db/altsql_db.h"]))
    heads[tag] = d
cc = ["cc", "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-O2", "-g"]
link = [os.path.join(B, "sqlite3.o"), os.path.join(B, "mdb.o"), os.path.join(B, "midl.o"), "-lpthread", "-lm"]
inc = ["-I../../core/dist", "-I.", "-I" + os.path.join(T, "sqlite"), "-I" + os.path.join(T, "lmdb")]
builds = [
    ("0.1", ["-I" + heads["v0.1.0-alpha"]], "gate1_v01"),
    ("0.2", ["-I" + heads["v0.2.0-alpha"]], "gate1_v02"),
    ("0.3", [], "gate1_vcur"),
    ("0.3, code aligned", ["-falign-functions=64", "-falign-loops=32"], "gate1_vcura"),
]
labels = []
for label, extra, name in builds:
    sh(cc + extra + inc + ["-o", os.path.join(B, name), "tools/gate1.c"] + link)
    labels.append(label)
cur = version_of(here)

runs = {label: {} for label in labels}
for r in range(rounds):
    for label, _, name in builds:
        out = sh(["taskset", "-c", "1", os.path.join(B, name), "altsql", scratch, entries])
        for line in out.splitlines():
            p = line.split()
            if len(p) == 4 and p[1] in ("reads_cached", "reads_4x", "scan", "writes_tx10000", "load"):
                runs[label].setdefault(p[1], []).append(float(p[2]))
        for f in os.listdir(scratch):
            if f.startswith("gate1.altsql"):
                os.remove(os.path.join(scratch, f))

out = []
w = out.append
w("AltSql DB Gate 1, the direct path of 0.1, 0.2 and %s side by side: tools/gate1.c built on each version's" % cur)
w("header, run in turn on CPU 1, %d rounds of %s entries; middle = median of the rounds," % (rounds, "{:,}".format(int(entries))))
w("spread = (largest - smallest) / largest")
w("")
w("%-15s %-18s %s %12s %8s" % ("measure", "code", " ".join("%11s" % ("round %d" % (i + 1)) for i in range(rounds)), "middle", "spread"))
units = {"reads_cached": "reads/s", "reads_4x": "reads/s", "scan": "entries/s", "writes_tx10000": "writes/s", "load": "entries/s"}
for m in ("reads_cached", "reads_4x", "scan", "writes_tx10000", "load"):
    for label in labels:
        xs = runs[label][m]
        w("%-15s %-18s %s %12s %7.1f%%" % (m, label, " ".join("%11s" % "{:,.0f}".format(x) for x in xs),
                                         "{:,.0f}".format(statistics.median(xs)), 100 * (max(xs) - min(xs)) / max(xs)))
    w("")
w("units: " + ", ".join("%s %s" % (k, v) for k, v in units.items()))

# instructions of the scan alone, under callgrind
w("")
w("Instructions of the scan alone (tools/scan_count.c: two scans of 100,000 entries; valgrind --tool=callgrind,")
w("inclusive count of scan())")
for label, extra in (("0.2", ["-I" + heads["v0.2.0-alpha"]]), (cur, [])):
    exe = os.path.join(B, "scan_count_" + label.replace(".", ""))
    sh(cc + extra + ["-I../../core/dist", "-I.", "-o", exe, "tools/scan_count.c", "-lm"])
    cg = os.path.join(scratch, "callgrind." + label)
    subprocess.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=" + cg, exe, "100000"], check=True,
                   cwd=here, capture_output=True)
    ann = sh(["callgrind_annotate", "--inclusive=yes", cg])
    m = re.search(r"^\s*([\d,]+) \([^)]*\)\s+\S*scan_count\.c:scan ", ann, re.M)
    w("  %-18s %s instructions" % (label, m.group(1) if m else "not found"))
text = "\n".join(out) + "\n"
with open(os.path.join(here, "results", "gate1_versions.log"), "w") as f:
    f.write(text)
print(text)
