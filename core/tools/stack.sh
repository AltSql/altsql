#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Stack depth of every public call, Cortex-M4 (Thumb-2), clang -Os:
# the sensor build (key-value, time-series, sync) and the gateway build.
set -e
cd "$(dirname "$0")/.."
python3 tools/amalgamate.py >/dev/null
mkdir -p build/stack
printf '#define ALTSQL_IMPLEMENTATION\n#include "altsql.h"\n' > build/stack/impl.c
CC="clang --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -Os -ffreestanding -ffunction-sections -std=c99 -Itools/shim -Idist -fstack-usage"
$CC -DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_TEXT=0 -c build/stack/impl.c -o build/stack/sensor.o
$CC -c build/stack/impl.c -o build/stack/gateway.o
echo "Sensor build (key-value, time-series, sync): deepest stack per call"
python3 tools/stack_depth.py build/stack/sensor.o build/stack/sensor.su > build/stack/sensor.txt
grep '^altsql_' build/stack/sensor.txt | sort -k2 -n -r
grep -v '^altsql_' build/stack/sensor.txt
echo ""
echo "Gateway build (everything): deepest stack per call"
python3 tools/stack_depth.py build/stack/gateway.o build/stack/gateway.su > build/stack/gateway.txt
grep '^altsql_' build/stack/gateway.txt | sort -k2 -n -r
grep -v '^altsql_' build/stack/gateway.txt
