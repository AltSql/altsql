#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
#
# End to end test of the two shells. A Core device (altsql) keeps a time series, a
# text series and a key-value pair, and writes two sync batches with .sync. The
# gateway shell (altsql-db) applies them: out of order, twice, and in order. Then it is
# asked with SQL and with the direct path (.row, .get). Everything the shells print goes
# into one log, which is compared with test_shell.expected.
#
#   make test-shell                    build both shells and run this
#   sh tests/test_shell.sh             run it, with the shells built already
#   UPDATE=1 sh tests/test_shell.sh    write test_shell.expected from this run; read it first
#
# ALTSQL_SHELL and ALTSQL_DB_SHELL name the two programs (full paths).
# One line of the log is made steady before the comparison: .check prints the entries
# and pages of the file, which follow the layout of the engine, so those numbers become N
# (the index entries it prints after them are one for each row, so they stay).

here=$(cd "$(dirname "$0")" && pwd) || exit 1
CORE_SH=${ALTSQL_SHELL:-$here/../../../core/build/altsql}
DB_SH=${ALTSQL_DB_SHELL:-$here/../build/altsql-db}
for f in "$CORE_SH" "$DB_SH"; do
    [ -x "$f" ] || { echo "test_shell: $f is not built (run: make test-shell)" >&2; exit 1; }
done

T=$(mktemp -d "${TMPDIR:-/tmp}/test_shell.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
trap 'exit 1' HUP INT TERM
OUT=$T/log
cd "$T" || exit 1
fail=0

# ---- helpers ------------------------------------------------------------------------------

# check WHAT COMMAND...: a plain assertion for what the log cannot show.
check() {
    what=$1; shift
    "$@" || { echo "test_shell: FAILED: $what" >&2; fail=1; }
}

say() { printf '\n## %s\n' "$*" >>"$OUT"; }

# show PROGRAM ARG...: the command line as the log shows it, each argument in [ ].
show() {
    if [ "$1" = "$CORE_SH" ]; then printf '$ altsql'; else printf '$ altsql-db'; fi
    shift
    for a in "$@"; do printf ' [%s]' "$a"; done
    printf '\n'
}

# finish STATUS: the output of the last run (in $LAST), then its exit status when not 0.
finish() {
    [ -z "$LAST" ] || printf '%s\n' "$LAST" >>"$OUT"
    [ "$1" -eq 0 ] || printf '(exit %s)\n' "$1" >>"$OUT"
}

# run PROGRAM ARG...: run it, log the command, what it printed (standard output and
# errors in order) and its exit status.
run() {
    show "$@" >>"$OUT"
    LAST=$("$@" 2>&1)
    finish $?
}

# runin PROGRAM SCRIPT ARG...: the same, with SCRIPT on standard input.
runin() {
    prog=$1; script=$2; shift 2
    { show "$prog" "$@"; printf '%s\n' "$script" | sed 's/^/> /'; } >>"$OUT"
    LAST=$(printf '%s\n' "$script" | "$prog" "$@" 2>&1)
    finish $?
}

dev() { run "$CORE_SH" dev.db "$@"; }
gw() { run "$DB_SH" gw.db "$@"; }

# The position a batch reaches, from "batch: N bytes, after A up to P".
upto() { printf '%s\n' "$LAST" | sed -n 's/.* up to \([0-9][0-9]*\).*/\1/p'; }

# seqof TABLE TIME: the sequence number the gateway gave the row of that time (device 7).
seqof() { "$DB_SH" gw.db "SELECT seq FROM $1 WHERE device = 7 AND time = $2;" | sed -n 2p; }

# Everything the gateway holds of device 7, for comparing before and after.
snap() {
    printf '.tables\n.state 7\nSELECT * FROM temps ORDER BY time;\nSELECT * FROM status ORDER BY time;\nSELECT * FROM kv;\n' |
        "$DB_SH" gw.db 2>&1
}

# ---- the program itself ---------------------------------------------------------------------

want="altsql-db $(sed -n 's/^#define ALTSQL_DB_VERSION "\(.*\)"$/\1/p' "$here/../altsql_db.h")"
want="$want (AltSql Core $(sed -n 's/^#define ALTSQL_VERSION "\(.*\)"$/\1/p' "$here/../../../core/dist/altsql.h"))"
check "--version prints '$want'" test "$("$DB_SH" --version)" = "$want"

say "usage"
run "$DB_SH"

# ---- a Core device writes two batches ---------------------------------------------------------

say "a Core device keeps two series and a pair"
run "$CORE_SH" new dev.db
runin "$CORE_SH" "CREATE TABLE temps (time TIME, machine INT, temp FLOAT);
CREATE TABLE status (time TIME, code INT, msg TEXT);
INSERT INTO temps VALUES (1700000001, 1, 21.5), (1700000002, 2, 22.25), (1700000003, 1, 21.75);
INSERT INTO status VALUES (1700000002, 3, 'door open');
.put site plant-a" dev.db

say "batch 1, everything so far"
dev ".sync 0 b1.bin"
S1=$(upto)

say "more records, then batch 2, and a call with nothing new to send"
runin "$CORE_SH" "INSERT INTO temps VALUES (1700000004, 2, 22.5), (1700000005, 1, 22);
INSERT INTO status VALUES (1700000005, 0, 'door closed');
.put site plant-b
.put owner ops" dev.db
dev ".sync $S1 b2.bin"
S2=$(upto)
dev ".sync $S2 b3.bin"

# A device with more records than one 64 KB batch holds: 3000 rows are about 2520 records
# in the first batch ("more to send") and the rest in the second.
say "a device with more records than one batch holds"
run "$CORE_SH" new big.db
awk 'BEGIN { print "CREATE TABLE big (time TIME, n INT);"
             for (i = 0; i < 3000; i++) printf "INSERT INTO big VALUES (%d, %d);\n", 1700001000 + i, i % 100 }' >big.sql
echo '$ altsql [big.db] < big.sql  (CREATE TABLE and 3000 INSERTs)' >>"$OUT"
"$CORE_SH" big.db <big.sql >/dev/null 2>&1 || { echo "test_shell: FAILED: loading big.db" >&2; fail=1; }
run "$CORE_SH" big.db ".sync 0 big1.bin"
B1=$(upto)
run "$CORE_SH" big.db ".sync $B1 big2.bin"
B2=$(upto)

if [ -z "$S1" ] || [ -z "$S2" ] || [ -z "$B1" ] || [ -z "$B2" ]; then
    echo "test_shell: the device gave no positions; see the log:" >&2
    cat "$OUT" >&2
    exit 1
fi

# ---- the gateway ---------------------------------------------------------------------------------

say "a new file is created on first use"
check "gw.db does not exist yet" test ! -e gw.db
gw .tables
check "gw.db was created" test -s gw.db

say "batch 2 before batch 1: a gap, and nothing changes"
snap >snap0
gw ".sync 7 $S1 b2.bin" ".state 7" .tables
snap >snap1
check "a gap changes nothing" cmp -s snap0 snap1

say "batch 1, then the tables the series made"
gw ".sync 7 0 b1.bin" ".state 7" .tables .schema
gw "SELECT * FROM temps;" "SELECT * FROM status;" "SELECT * FROM kv;"

say "batch 2"
gw ".sync 7 $S1 b2.bin" ".state 7"
gw "SELECT * FROM temps ORDER BY time;" "SELECT * FROM status ORDER BY time;" "SELECT * FROM kv;"
gw "SELECT machine, COUNT(*), AVG(temp) FROM temps GROUP BY machine ORDER BY machine;"
gw ".sync 7 $S2 b3.bin" ".state 7"

say "the same batches again change nothing"
snap >snap0
gw ".sync 7 0 b1.bin" ".sync 7 $S1 b2.bin" ".state 7"
snap >snap1
check "applying batches again changes nothing" cmp -s snap0 snap1

say "rows by key, with no SQL step"
T3=$(seqof temps 1700000003)
T5=$(seqof status 1700000005)
gw ".row get temps 7 1700000003 $T3" ".row get status 7 1700000005 $T5" ".row get kv 7 site" ".row get kv 7 owner"
gw ".row get temps 7 1700000003 999"
gw ".row get kv 7"
gw ".row put kv 7 site x"

say "a second device sends the same batches"
gw ".sync 9 0 b1.bin" ".sync 9 $S1 b2.bin" ".state 9" ".state 7"
gw "SELECT device, COUNT(*), MAX(time) FROM temps GROUP BY device ORDER BY device;"
gw "SELECT * FROM kv ORDER BY device, key;"

say "a device with many records: two batches"
gw ".sync 12 0 big1.bin" ".sync 12 $B1 big2.bin" ".state 12"
gw "SELECT COUNT(*), MIN(time), MAX(time), SUM(n) FROM big;"

say "sync errors"
printf 'not a batch\n' >junk.bin
gw ".sync 7 0 nosuch.bin"
gw ".sync x 0 b1.bin"
gw ".sync 15 0 junk.bin"
gw ".sync 15 0"

# ---- tables of the gateway's own ----------------------------------------------------------------

say "a table of the gateway's own"
gw "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT, score FLOAT);" ".schema notes"
gw ".row put notes 1 first 1.5" ".row put notes 2 'second note' 2" ".row insert notes 3 third 3.25"
gw "SELECT * FROM notes;"
gw ".row put notes 1 'first, replaced' 1.75" ".row get notes 1"
gw ".row insert notes 3 again 9"
gw "SELECT * FROM notes;"
gw ".row del notes 2" ".row get notes 2"
gw ".row put notes 4 only"
gw ".row put notes four x 1"
gw ".row get nosuch 1"

say "secondary indexes: made, read, kept, refused, checked, dropped"
gw "CREATE INDEX notes_score ON notes (score);" "CREATE UNIQUE INDEX notes_body ON notes (body);" ".schema notes"
gw "EXPLAIN SELECT * FROM notes WHERE score > 1.6;" "SELECT * FROM notes WHERE score > 1.6;"
gw "INSERT INTO notes VALUES (9, 'third', 0.5);"
gw ".row put notes 9 third 0.5"
gw ".row put notes 9 ninth 0.5" "SELECT * FROM notes WHERE body = 'ninth';"
gw "CREATE INDEX temps_machine ON temps (machine);" "EXPLAIN SELECT * FROM temps WHERE machine = 1;"
gw "SELECT device, time, temp FROM temps WHERE machine = 1 ORDER BY device, time;"
gw .check
gw "DROP INDEX notes_score;" ".schema notes"
gw "DROP INDEX notes_score;"
gw ".drop temps"
gw ".drop notes" ".tables"
gw ".row get notes 1"

# ---- buckets and transactions --------------------------------------------------------------------

say "buckets: the direct path"
gw ".put cfg mode fast" ".put cfg name two words here" ".get cfg mode" ".get cfg name" ".keys cfg"
gw ".del cfg mode" ".get cfg mode"
gw ".get nobucket k"
gw ".keys nobucket"

say "transactions"
gw .begin ".put cfg tmp 1" ".keys cfg" .rollback ".keys cfg"
gw .begin ".put cfg kept 1" .commit ".keys cfg"
gw .begin ".put cfg left-open 1"
gw ".keys cfg"
gw .rollback
gw .commit

# ---- standard input -----------------------------------------------------------------------------

say "standard input: SQL over several lines, an error does not stop the run, .quit does"
runin "$DB_SH" "SELECT COUNT(*)
  FROM temps
  WHERE device = 7;

SELECT * FROM nope;
.bogus
.state 7
SELECT 1 + 1 AS two;
.quit
SELECT 'not reached';" gw.db

say "arguments stop at the first error"
gw "SELECT * FROM nope;" ".tables"

# ---- the file ----------------------------------------------------------------------------------------

say "the file is sound, and .help lists the commands"
gw .check
gw .help

# .info has figures that change from run to run, so the log does not hold them.
case $("$DB_SH" gw.db .info) in
page_size*pages*) ;;
*) echo "test_shell: FAILED: .info lists page_size and pages" >&2; fail=1 ;;
esac

# ---- compare ---------------------------------------------------------------------------------------

sed 's/^ok, [0-9][0-9]* entries, [0-9][0-9]* pages/ok, N entries, N pages/' "$OUT" >"$T/log.norm"
if [ -n "${UPDATE:-}" ]; then
    cp "$T/log.norm" "$here/test_shell.expected" || exit 1
    echo "test_shell: wrote $here/test_shell.expected"
    exit 0
fi
diff -u "$here/test_shell.expected" "$T/log.norm" >&2 || fail=1
if [ "$fail" -ne 0 ]; then
    echo "test_shell: FAILED" >&2
    exit 1
fi
echo "test_shell: all checks passed"
