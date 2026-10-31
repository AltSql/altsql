#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Regenerates every log in results/ on the current code, one after another. Any figure
# quoted about AltSql Restore comes from these logs.
#   sh tools/all_logs.sh
cd "$(dirname "$0")/.." || exit 1
mkdir -p results
make -s all
run() { name=$1; shift; "$@" > results/$name.log 2>&1; echo "exit $?" >> results/$name.log; }
run test_units ./build/test_units
run test_restore sh tests/test_restore.sh
run sanitize make -s sanitize
run strict make -s strict
run bench sh tools/bench.sh
grep -h "^exit" results/*.log | sort | uniq -c
