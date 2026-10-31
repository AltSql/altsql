#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# A quick check of a built altsql-restore, for the release builds: a good SQLite file and a
# good tar.gz pass, cut-short ones fail, a PostgreSQL dump with no engine is NOT VERIFIED.
#   sh engines/restore/tests/smoke.sh /path/to/altsql-restore
R=$1
here=$(cd "$(dirname "$0")" && pwd) || exit 1
[ -x "$R" ] || { echo "smoke: $R is not a program" >&2; exit 1; }
T=$(mktemp -d /tmp/ar_smoke.XXXXXX) || exit 1
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
"$R" --version || exit 1
python3 -c "
import sqlite3
c = sqlite3.connect('s.db'); c.execute('create table t(a integer primary key, b text)')
c.executemany('insert into t values(?,?)', [(i, 'row %d' % i) for i in range(20000)]); c.commit()
d = open('s.db', 'rb').read(); open('cut.db', 'wb').write(d[:len(d) // 2])" || exit 1
mkdir -p files/a && echo hello > files/a/x.txt && echo world > files/b.txt && tar czf f.tgz files && head -c 60 f.tgz > cut.tgz
fail=0
check() { want=$1; shift; "$R" --history h.db --work-dir "$T" -q "$@"; got=$?; [ "$got" -eq "$want" ] || { echo "smoke: $* gave $got, wanted $want" >&2; fail=1; }; }
check 0 --job s s.db
check 1 --job s cut.db
check 0 --job f f.tgz
check 1 --job f cut.tgz
check 3 --job p --engine-path /nonexistent --docker /nonexistent "$here/fixtures/shop_pg.sql.gz"
[ "$(ls "$T" | grep -c '^altsql-restore-')" -eq 0 ] || { echo "smoke: a sandbox was left" >&2; fail=1; }
[ $fail -eq 0 ] && echo "smoke: altsql-restore works"
exit $fail
