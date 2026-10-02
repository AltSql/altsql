#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Mutation check: plants one deliberate bug at a time in the engine and
runs the test suite against it. A good test suite catches every one.

Each mutant is a small, realistic mistake (a check dropped, a bound off by
one, a step skipped). The test suite runs with shorter settings than
`make test` to keep this quick; a mutant counts as caught when any test
fails, crashes or runs for more than two minutes.

Usage: python3 tools/mutants.py            (from the repository root)
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

MUTANTS = [
    ("storage", "a record's checksum is not checked when reading",
     "if (as_crc32(as_crc32(0, p + 1, 7), p + AS_RH, len) != as_get32(p + 8)) {", "if (0) {"),
    ("storage", "after a power cut, writing resumes over a torn record",
     "db->head_off = valid_end > last ? valid_end : last;", "db->head_off = valid_end;"),
    ("storage", "a sector is not erased before it is reused",
     "rc = as_erase(db, s);", "rc = 0;"),
    ("storage", "reclaiming skips live key-value records instead of copying them",
     "if (!live) continue;", "if (live) continue;"),
    ("storage", "a sector opened while reclaiming is not marked as such",
     "db->gc_of = db->sec[t].seq;", "db->gc_of = 0;"),
    ("storage", "an interrupted reclaim is not undone at start-up",
     "if (gc_of && db->used > 1 && gc_of == best - db->used + 1) {", "if (0) {"),
    ("storage", "a reclaimed sector is not retired",
     "rc = as_write(db, t * db->ss, z, db->align > 4u ? db->align : 4u);", "rc = 0;"),
    ("key-value", "a reclaimed delete leaves a hole in the key index",
     "as_kv_repoint(db, &r, AS_TOMB); continue;", "as_kv_repoint(db, &r, 0); continue;"),
    ("sync", "a delete the gateway has not confirmed is dropped when reclaiming",
     "return !db->replica && r->seq > db->synced;", "return 0;"),
    ("sync", "the gateway skips the record right after the ones it has",
     "if (seq >= db->next_rseq) {", "if (seq > db->next_rseq) {"),
    ("sync", "the device skips a sector it still has to send",
     "if (nf && nf <= after_seq + 1u) continue;", "if (nf && nf <= after_seq + 2u) continue;"),
    ("time-series", "a time-range scan skips a sector whose newest row is exactly the start",
     "if (db->sec[as_run_sector(db, k)].max_time < from) continue;",
     "if (db->sec[as_run_sector(db, k)].max_time <= from) continue;"),
    ("time-series", "float values read back with binary noise (21.530000686645508)",
     "if ((float)r == (float)a) return d < 0 ? -r : r;", "if (0) return d < 0 ? -r : r;"),
    ("SQL", "WHERE time >= N drops the rows at exactly N",
     "if (L > q->tmin) q->tmin = L;", "if (L > q->tmin) q->tmin = L + 1;"),
    ("SQL", "OFFSET skips one row too many",
     "if (q->skipped < q->offset) { q->skipped++; return 0; }",
     "if (q->skipped <= q->offset) { q->skipped++; return 0; }"),
    ("text", "export does not escape quotes and backslashes",
     "if (c == '\"' || c == '\\\\') { e[0] = '\\\\'; e[1] = (char)c; as_wr_put(w, e, 2); }",
     "if (c == '\"' || c == '\\\\') { e[0] = (char)c; as_wr_put(w, e, 1); }"),
]

TESTS = [("test_basic", []), ("test_powercut", ["3000"]), ("test_sync", []), ("test_fuzz", ["3000"])]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    subprocess.run([sys.executable, "tools/amalgamate.py"], check=True, stdout=subprocess.DEVNULL)
    header = open("dist/altsql.h").read()
    tmp = tempfile.mkdtemp(prefix="altsql-mutants-")
    caught = 0
    print("%-3s %-12s %-72s %s" % ("#", "area", "planted bug", "result"))
    try:
        for i, (area, what, old, new) in enumerate(MUTANTS, 1):
            n = header.count(old)
            if n != 1:
                print("%-3d %-12s %-72s NOT APPLIED (%d matches)" % (i, area, what, n))
                continue
            with open(os.path.join(tmp, "altsql.h"), "w") as f:
                f.write(header.replace(old, new))
            verdict = "missed"
            t0 = time.time()
            for name, args in TESTS:
                exe = os.path.join(tmp, name)
                b = subprocess.run(["cc", "-std=c99", "-O2", "-D_POSIX_C_SOURCE=200809L", "-I" + tmp, "-Itests",
                                    "-o", exe, "tests/%s.c" % name], capture_output=True, text=True)
                if b.returncode != 0:
                    verdict = "caught (does not compile)"
                    break
                try:
                    r = subprocess.run([exe] + args, capture_output=True, text=True, timeout=120)
                except subprocess.TimeoutExpired:
                    verdict = "caught by %s (hangs)" % name
                    break
                if r.returncode != 0:
                    verdict = "caught by %s" % name + (" (crash)" if r.returncode < 0 else "")
                    break
            if verdict.startswith("caught"):
                caught += 1
            print("%-3d %-12s %-72s %s  [%.0f s]" % (i, area, what, verdict, time.time() - t0), flush=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d of %d planted bugs caught" % (caught, len(MUTANTS)))
    return 0 if caught == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
