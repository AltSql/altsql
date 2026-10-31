#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# How long a verification takes, three runs each: a SQLite file of 1,000,000 rows (built-in
# reader), a tar.gz of 5,000 files (built-in tar and gzip), and the PostgreSQL and MySQL test
# dumps through local servers when there are any. Prints the machine first.
#   sh tools/bench.sh          (with build/altsql-restore built)
cd "$(dirname "$0")/.." || exit 1
R=$PWD/build/altsql-restore
T=$(mktemp -d /tmp/ar_bench.XXXXXX) || exit 1
chmod 755 "$T"
trap 'rm -rf "$T"' EXIT
echo "machine: $(uname -sm), $( (grep -m1 'model name' /proc/cpuinfo 2> /dev/null | sed 's/.*: //') || sysctl -n machdep.cpu.brand_string 2> /dev/null), $(nproc 2> /dev/null || sysctl -n hw.ncpu) CPUs"
echo "altsql-restore: $("$R" --version), $(wc -c < "$R" | tr -d ' ') bytes"
python3 - "$T" << 'PY'
import sqlite3, sys, os, tarfile
T = sys.argv[1]
c = sqlite3.connect(T + "/big.db")
c.execute("create table readings(id integer primary key, device int, at int, value real, note text)")
c.execute("create index readings_device on readings(device, at)")
c.executemany("insert into readings values(?,?,?,?,?)", ((i, i % 500, 1_700_000_000 + i, (i % 977) / 7.0, "ok" if i % 9 else "check") for i in range(1, 1_000_001)))
c.commit(); c.close()
os.makedirs(T + "/files", exist_ok=True)
for i in range(5000):
    d = T + "/files/d%02d" % (i % 50); os.makedirs(d, exist_ok=True)
    open(d + "/f%04d.txt" % i, "w").write(("line %d\n" % i) * (i % 200 + 1))
with tarfile.open(T + "/files.tar.gz", "w:gz") as t: t.add(T + "/files", arcname="files")
PY
run() {
    label=$1; shift
    for n in 1 2 3; do
        s=$(date +%s.%N 2> /dev/null || date +%s)
        "$R" --history "$T/h.db" --work-dir "$T" -q "$@" > /dev/null 2>&1
        rc=$?
        e=$(date +%s.%N 2> /dev/null || date +%s)
        printf '%-46s run %d: %6.2f s (exit %d)\n' "$label" "$n" "$(echo "$e - $s" | bc)" "$rc"
    done
}
echo "sizes: SQLite file $(wc -c < "$T/big.db" | tr -d ' ') bytes; tar.gz $(wc -c < "$T/files.tar.gz" | tr -d ' ') bytes"
run "SQLite file, 1,000,000 rows" --job big "$T/big.db"
run "tar.gz, 5,000 files in 50 folders" --job files "$T/files.tar.gz"
run "PostgreSQL plain dump, 6,500 rows (local)" --job pg --sandbox local tests/fixtures/shop_pg.sql.gz
run "MySQL dump, 6,500 rows (local)" --job my --sandbox local tests/fixtures/shop_my.sql.gz
