#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Regenerates every log in results/ on the current code, one step after another
# (nothing else should run meanwhile: Gate 1 measures speed). The fuzzing and planted-bug
# logs come from make fuzz and make mutants: results/fuzz.log and results/mutants.log.
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
run test_crash ./build/test_crash
run test_fault ./build/test_fault
run test_vs_sqlite make -s vs-sqlite
run sanitize make sanitize
run valgrind make valgrind
run strict make strict
run size sh tools/size.sh ../../../third/sqlite
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O1 -I../../core/dist -I. -Itests -o build/minmem tools/minmem.c && run minmem ./build/minmem
T=../../../third
cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -O2 -g -I../../core/dist -I. -I$T/sqlite -I$T/lmdb \
   -o build/gate1 tools/gate1.c build/sqlite3.o build/mdb.o build/midl.o -lpthread -lm
mkdir -p "$S/gdb" "$S/bench"
run gate1_sqlcheck gdb -batch -x tools/sqlcheck.gdb --args ./build/gate1 altsql "$S/gdb" 50000
run gate1_driver taskset -c 1 python3 tools/gate1_report.py "$S" 1000000
cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -O2 -g -DBENCH_SQLITE -I../../core/dist -I. -I../../../third/sqlite -o build/bench tools/bench.c build/sqlite3.o -lm -lpthread
run bench taskset -c 1 ./build/bench 1000000 "$S/bench"
# the demo: built for the browser, run under Node, then in headless Chromium at desktop and phone sizes
run demo_build sh ../../demos/db/build.sh
run demo_facts node ../../demos/db/tools/facts.js ../../demos/db/app/engine-db.v1.js
mkdir -p ../../demos/db/build/shots
run demo_browser env NODE_PATH="$(npm root -g)" node ../../demos/db/tools/headless.js "file://$(cd ../../demos/db && pwd)/app/index.html" ../../demos/db/build/shots
grep -h "^exit" results/*.log | sort | uniq -c
