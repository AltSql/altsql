#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Code size of AltSql builds for a Cortex-M4 (ARMv7E-M, Thumb-2) and four
# other chip families, clang -Os. Counts only AltSql's own code and constants:
# the C library routines it calls and the compiler's floating-point helpers
# are not included. Every build uses -Wall -Wextra -Wshadow -pedantic -Werror,
# so this script also shows that each one compiles without a warning.
set -e
cd "$(dirname "$0")/.."
python3 tools/amalgamate.py >/dev/null
mkdir -p build/size
printf '#define ALTSQL_IMPLEMENTATION\n#include "altsql.h"\n' > build/size/impl.c
W="-std=c99 -pedantic -Wall -Wextra -Wshadow -Werror"
CC="clang --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -Os -ffreestanding -ffunction-sections $W -Itools/shim -Idist"
row() {
    $CC $2 -c build/size/impl.c -o build/size/$1.o
    llvm-size -A build/size/$1.o | awk -v name="$1" -v desc="$3" '
        /^\.text/ { t += $2 } /^\.rodata/ { r += $2 } /^\.data/ { d += $2 } /^\.bss/ { b += $2 }
        END { printf "  %-10s %6d bytes code + %5d bytes constants   %s\n", name, t, r, desc }'
}
echo "Every build below: clang -Os $W (fails on any warning)"
echo ""
echo "AltSql code size, Cortex-M4 Thumb-2, clang -Os:"
row kv      "-DALTSQL_ENABLE_TS=0 -DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_SYNC=0 -DALTSQL_ENABLE_TEXT=0" "key-value only"
row sensor  "-DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_SYNC=0 -DALTSQL_ENABLE_TEXT=0" "key-value + time-series"
row sensor+ "-DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_TEXT=0" "key-value + time-series + sync"
row gateway "" "everything, including SQL and text export"
# The same builds for other chip families (code + constants, bytes)
total() {
    $1 $2 -c build/size/impl.c -o build/size/x.o
    llvm-size -A build/size/x.o | awk '/^\.text/ { t += $2 } /^\.rodata/ { r += $2 } END { printf "%8d", t + r }'
}
echo ""
echo "Same code, other chip families (code + constants, bytes):"
echo "  target                          key-value   sensor (+ts +sync)   gateway (all)"
for t in \
  "Cortex-M0+ (ARMv6-M)|--target=thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb" \
  "Cortex-M4 (ARMv7E-M)|--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb" \
  "Cortex-M33 (ARMv8-M)|--target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mthumb" \
  "RISC-V RV32IMC|--target=riscv32-unknown-elf -march=rv32imc -mabi=ilp32" \
  "x86-64 (gateway, PC)|--target=x86_64-linux-gnu" ; do
    name=${t%%|*}; flags=${t#*|}
    C="clang $flags -Os -ffreestanding -ffunction-sections $W -Itools/shim -Idist"
    printf "  %-30s" "$name"
    total "$C" "-DALTSQL_ENABLE_TS=0 -DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_SYNC=0 -DALTSQL_ENABLE_TEXT=0"
    printf "        "
    total "$C" "-DALTSQL_ENABLE_SQL=0 -DALTSQL_ENABLE_TEXT=0"
    printf "          "
    total "$C" ""
    echo ""
done
echo ""

# RAM: the engine's own structures on a 32-bit target, read back from a compiled table
printf '#define ALTSQL_IMPLEMENTATION\n#include "altsql.h"\nconst unsigned as_sizes[4] = { sizeof(struct altsql), sizeof(as_sector), sizeof(as_slot), sizeof(as_series) };\n' > build/size/sizes.c
$CC -fdata-sections -c build/size/sizes.c -o build/size/sizes.o
llvm-objcopy -O binary --only-section=.rodata.as_sizes build/size/sizes.o build/size/sizes.bin
python3 - <<'PY'
import struct
a, s, k, e = struct.unpack("<4I", open("build/size/sizes.bin", "rb").read()[:16])
print("RAM on a 32-bit MCU: %d bytes + %d per flash sector + %d per key slot + %d per series + the record buffer" % (a, s, k, e))
n = a + 16 * s + 32 * k + 4 * e + 128
print("  example: 64 KB flash as 16 x 4 KB sectors, 32 key slots, 4 series, 128-byte records = %d bytes" % n)
PY
