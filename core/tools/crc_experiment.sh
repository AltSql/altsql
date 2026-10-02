#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# The two checksum experiments: the benchmark run with
# (1) a table-driven CRC-32 (256 entries, 1 KB) and (2) no checksum check at
# all on reads. Each changes one function in a temporary copy of the engine;
# the engine in dist/ is not touched. Experiment 2 is unsafe by design and
# exists only to show where the scan time goes.
set -e
cd "$(dirname "$0")/.."
python3 tools/amalgamate.py >/dev/null
T=build/crc_experiment
mkdir -p $T/table $T/nocheck
python3 - "$T" <<'PY'
import sys
T = sys.argv[1]
h = open('dist/altsql.h').read()
old_check = "if (as_crc32(as_crc32(0, p + 1, 7), p + AS_RH, len) != as_get32(p + 8)) {"
assert h.count(old_check) == 1
open(T + '/nocheck/altsql.h', 'w').write(h.replace(old_check, "if (0) {"))
start = h.index("static uint32_t as_crc32(uint32_t crc, const void *data, size_t n) {")
end = h.index("static uint32_t as_hash(")
table = '''static uint32_t as_crc_t[256];
static uint32_t as_crc32(uint32_t crc, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    if (!as_crc_t[1]) {
        uint32_t i, j, c;
        for (i = 0; i < 256; i++) { c = i; for (j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); as_crc_t[i] = c; }
    }
    crc = ~crc;
    while (n--) crc = (crc >> 8) ^ as_crc_t[(crc ^ *p++) & 0xFFu];
    return ~crc;
}

'''
open(T + '/table/altsql.h', 'w').write(h[:start] + table + h[end:])
PY
for v in table nocheck; do
    cc -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -I$T/$v -o $T/$v/bench tests/bench.c
    echo "== $v"
    $T/$v/bench
done
