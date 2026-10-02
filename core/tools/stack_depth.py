# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Worst-case stack depth of each public AltSql call on a Cortex-M4.

Reads clang's per-function stack report (-fstack-usage) and the calls
between functions (from the object file's relocations), then adds up the
frames along the deepest chain of calls behind every altsql_* function. A
function may appear at most twice in a chain, which covers the one place
where the engine calls itself (making room while reclaiming). Recursion in
the SQL parser is bounded separately, by its 200-level limit.
Usage: python3 tools/stack_depth.py file.o file.su   (see tools/stack.sh)
"""
import re, subprocess, sys
obj, su = sys.argv[1], sys.argv[2]
frames = {}
for line in open(su):
    parts = line.rstrip('\n').split('\t')
    name = parts[0].split(':')[-1]
    frames[name] = int(parts[1])
dis = subprocess.run(['llvm-objdump', '-dr', obj], capture_output=True, text=True).stdout
edges, cur = {}, None
for line in dis.splitlines():
    m = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
    if m:
        cur = m.group(1); edges.setdefault(cur, set()); continue
    m = re.search(r'R_ARM_THM_(?:CALL|JUMP24)\s+(\S+)', line)
    if m and cur:
        tgt = m.group(1)
        if tgt.startswith('.text.'): tgt = tgt[6:]
        edges[cur].add(tgt)
memo = {}
def worst(f, path):
    if path.count(f) >= 2: return 0, []
    best, bpath = 0, []
    for g in edges.get(f, ()):
        d, p = worst(g, path + [f])
        if d > best: best, bpath = d, p
    return frames.get(f, 0) + best, [f] + bpath
apis = sorted(n for n in frames if n.startswith('altsql_'))
for a in apis:
    d, p = worst(a, [])
    print("%-22s %5d bytes   %s" % (a, d, ' > '.join(x for x in p if frames.get(x, 0) or x in edges)))
libc = sorted({g for f in edges for g in edges[f] if g not in frames})
print("calls outside the engine (libc):", ', '.join(libc))
