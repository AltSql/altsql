#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Regenerates every log in results/ on the current code, one step after another
# (nothing else should run meanwhile: Gates 1 and 2 measure speed). The fuzzing and planted-bug
# logs come from make fuzz and make mutants: results/fuzz.log and results/mutants.log; the
# side-by-side run of 0.1, 0.2 and this code from tools/gate1_versions.py.
#   sh tools/all_logs.sh <scratch dir for Gate 1 files>
cd "$(dirname "$0")/.."
S=${1:-build/gate1-files}
mkdir -p results "$S"
make -s all
run() { name=$1; shift; "$@" > results/$name.log 2>&1; echo "exit $?" >> results/$name.log; }
run test_db ./build/test_db
run test_rows ./build/test_rows
run test_sync ./build/test_sync
run test_sql ./build/test_sql
run test_same ./build/test_same
run test_savepoint ./build/test_savepoint
run test_index ./build/test_index
run test_crash ./build/test_crash
run test_fault ./build/test_fault
run test_vs_sqlite make -s vs-sqlite
run sanitize make sanitize
run valgrind make valgrind
run strict make strict
run test_shell make -s test-shell
run size sh tools/size.sh ../../../third/sqlite
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O1 -I../../core/dist -I. -Itests -o build/minmem tools/minmem.c && run minmem ./build/minmem
T=../../../third
cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -O2 -g -I../../core/dist -I. -I$T/sqlite -I$T/lmdb \
   -o build/gate1 tools/gate1.c build/sqlite3.o build/mdb.o build/midl.o -lpthread -lm
mkdir -p "$S/gdb"
# the machine the speed figures run on, and how much CPU time its host took meanwhile
stat0=$(head -1 /proc/stat)
{
  echo "The machine the speed figures ran on ($(date '+%d %B %Y, %H:%M %Z'))"
  echo "  $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'), $(nproc) virtual CPUs, $(free -m | awk '/Mem:/ {print $2}') MB of memory"
  echo "  Linux $(uname -r); the runs pinned to CPU 1"
  echo "  load before the runs: $(cut -d' ' -f1-3 /proc/loadavg)"
} > results/machine.log
run gate1_sqlcheck gdb -batch -x tools/sqlcheck.gdb --args ./build/gate1 altsql "$S/gdb" 50000
run gate1_driver taskset -c 1 python3 tools/gate1_report.py "$S" 1000000
cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -O2 -g -DBENCH_SQLITE -I../../core/dist -I. -I../../../third/sqlite -o build/bench tools/bench.c build/sqlite3.o -lm -lpthread
# what statement savepoints cost: the same statements on 0.3 and on 0.2's header (git show v0.2.0-alpha:engines/db/altsql_db.h)
mkdir -p build/v02 && git show v0.2.0-alpha:engines/db/altsql_db.h > build/v02/altsql_db.h 2>/dev/null || git show 35edd98:engines/db/altsql_db.h > build/v02/altsql_db.h
cc -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -I../../core/dist -I. -o build/bench_sp tools/bench_sp.c -lm
cc -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -Ibuild/v02 -I../../core/dist -I. -o build/bench_sp02 tools/bench_sp.c -lm
# Gate 2: the benchmark and the savepoint cost, three runs each on one core (gate2_run*.log, bench_sp_run*.log, gate2_summary.log)
mkdir -p "$S/gate2"
run gate2_driver taskset -c 1 python3 tools/gate2_report.py "$S/gate2" 1000000
python3 - "$stat0" "$(head -1 /proc/stat)" >> results/machine.log <<'PY'
import sys
a = [int(x) for x in sys.argv[1].split()[1:]]
b = [int(x) for x in sys.argv[2].split()[1:]]
d = [y - x for x, y in zip(a, b)]
total = sum(d[:8])
print("  over the runs, CPU time the host took for itself (steal): %.2f%% of both CPUs' time; idle %.1f%%; waiting for the disk %.1f%%"
      % (100.0 * d[7] / total, 100.0 * d[3] / total, 100.0 * d[4] / total))
PY
echo "  load after the runs: $(cut -d' ' -f1-3 /proc/loadavg)" >> results/machine.log
# the demo: built for the browser, run under Node, then in headless Chromium at desktop and phone sizes
run demo_build env V=3 sh ../../demos/db/build.sh
run demo_facts node ../../demos/db/tools/facts.js ../../demos/db/app/engine-db.v3.js
mkdir -p ../../demos/db/build/shots
run demo_browser env NODE_PATH="$(npm root -g)" node ../../demos/db/tools/headless.js "file://$(cd ../../demos/db && pwd)/app/index.html" ../../demos/db/build/shots
grep -h "^exit" results/*.log | sort | uniq -c
