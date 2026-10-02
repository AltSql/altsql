#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Coverage-guided fuzzing of the four entry points that take outside input.
# Needs clang with libFuzzer (on Ubuntu: apt install clang libclang-rt-18-dev).
# Usage: sh tools/fuzz.sh [seconds per target, default 60] [jobs, default 1]
set -e
cd "$(dirname "$0")/.."
SECS=${1:-60}
JOBS=${2:-1}
python3 tools/amalgamate.py >/dev/null
mkdir -p build/fuzz
FLAGS="-g -O1 -std=c99 -Idist -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -D_POSIX_C_SOURCE=200809L"
for t in sql sync import mount; do
    T=$(echo $t | tr a-z A-Z)
    clang $FLAGS -DFUZZ_$T tests/fuzz/fuzz.c -o build/fuzz/fuzz_$t
done
cc -std=c99 -O1 -Idist tests/fuzz/seeds.c -o build/fuzz/make_seeds
rm -rf build/fuzz/seeds && for t in sql sync import mount; do mkdir -p build/fuzz/seeds/$t build/fuzz/corpus/$t; done
./build/fuzz/make_seeds build/fuzz/seeds >/dev/null
for t in sql sync import mount; do
    case $t in
        sql)    extra="-dict=tests/fuzz/sql.dict -max_len=4096" ;;
        sync)   extra="-max_len=4096" ;;
        import) extra="-max_len=8192" ;;
        mount)  extra="-max_len=2049" ;;
    esac
    echo "== $t: $SECS s"
    mkdir -p tests/fuzz/regressions/$t
    ./build/fuzz/fuzz_$t build/fuzz/corpus/$t build/fuzz/seeds/$t tests/fuzz/regressions/$t $extra -max_total_time=$SECS \
        -jobs=$JOBS -workers=$JOBS -artifact_prefix=build/fuzz/$t- -print_final_stats=1 2>&1 \
        | grep -E "^(stat::number_of_executed_units|stat::peak_rss_mb|#[0-9]+ +DONE)|ERROR|rule broken|SUMMARY" || true
done
ls build/fuzz/ | grep -E -- "-(crash|leak|timeout|oom)-" && { echo "FAILURES FOUND (inputs saved in build/fuzz/)"; exit 1; } || echo "no failures"
