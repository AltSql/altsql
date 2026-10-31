#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests of altsql-restore, aimed at its promise:
#   a corrupt or cut-short backup always fails;
#   a table that lost rows past the threshold always alerts;
#   a missing engine is reported as NOT VERIFIED, never as passed;
#   the sandbox is always cleaned up, also after kill -9.
# SQLite files, tar archives and folders need nothing else and are always tested. The
# PostgreSQL and MySQL/MariaDB tests run when a local server can start (or with Docker when
# ALTSQL_RESTORE_TEST_DOCKER=1); otherwise they print SKIPPED and why. The last lines say
# which engines were covered.
#
#   make test                 builds altsql-restore and runs this
#   sh tests/test_restore.sh  with the program built (ALTSQL_RESTORE names another build)
# Needs python3 (with its sqlite3 module) to make the SQLite and tar fixtures.

here=$(cd "$(dirname "$0")" && pwd) || exit 1
R=${ALTSQL_RESTORE:-$here/../build/altsql-restore}
FX=$here/fixtures
[ -x "$R" ] || { echo "test_restore: $R is not built (run: make)" >&2; exit 1; }
command -v python3 > /dev/null || { echo "test_restore: needs python3" >&2; exit 1; }

# /tmp, not $TMPDIR: a server started as root runs as its own user and must reach the
# sandbox, and a socket path must stay short.
T=$(mktemp -d /tmp/ar_test.XXXXXX) || exit 1
chmod 755 "$T"
WK=$T/work
H=$T/history.db
mkdir -p "$WK"
trap 'rm -rf "$T"' EXIT
trap 'exit 1' HUP INT TERM
pass=0
fail=0
cov_pg="no"
cov_my="no"
cov_docker="SKIPPED (set ALTSQL_RESTORE_TEST_DOCKER=1)"

ok() { pass=$((pass + 1)); echo "ok    $*"; }
bad() { fail=$((fail + 1)); echo "FAIL  $*"; }
show() { tail -n 18 "$T/last.out" | sed 's/^/      | /'; }
# expect CODE WHAT ARGS...: runs altsql-restore and checks its exit status; output in last.out
expect() {
    want=$1; what=$2; shift 2
    "$R" --history "$H" --work-dir "$WK" "$@" > "$T/last.out" 2>&1
    got=$?
    if [ "$got" -eq "$want" ]; then ok "$what (exit $got)"; else bad "$what: exit $got, wanted $want"; show; fi
}
# has TEXT WHAT: the last report holds TEXT
has() {
    if grep -q -- "$1" "$T/last.out"; then ok "$2"; else bad "$2: no \"$1\" in the report"; show; fi
}
hasnt() {
    if grep -q -- "$1" "$T/last.out"; then bad "$2: \"$1\" is in the report"; show; else ok "$2"; fi
}
sandboxes() { ls "$WK" | grep -c '^altsql-restore-'; }
servers() { pgrep -f "$WK/altsql-restore-" 2> /dev/null | wc -l | tr -d ' '; }
# wait_gone SECONDS: until no sandbox and no server of one is left
wait_gone() {
    i=0
    while [ "$i" -lt $(($1 * 10)) ]; do
        [ "$(sandboxes)" -eq 0 ] && [ "$(servers)" -eq 0 ] && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}
# wait_sandbox: until a sandbox exists (and, with an argument, a server runs in it)
wait_sandbox() {
    i=0
    while [ "$i" -lt 600 ]; do
        if [ "$(sandboxes)" -gt 0 ]; then
            [ -z "$1" ] && return 0
            [ "$(servers)" -gt 0 ] && return 0
        fi
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

echo "AltSql Restore: end-to-end tests ($("$R" --version))"

# ---- fixtures ---------------------------------------------------------------------------------
cd "$T" || exit 1
python3 - << 'PY' || { echo "test_restore: cannot make the fixtures" >&2; exit 1; }
import sqlite3, tarfile, gzip, os, random, shutil
random.seed(7)
def shop(path, orders=5000, notes=50, drop_notes=False):
    if os.path.exists(path): os.remove(path)
    c = sqlite3.connect(path)
    c.execute("create table orders(id integer primary key, item text, qty int)")
    c.execute("create index orders_item on orders(item)")
    if not drop_notes:
        c.execute("create table notes(k text primary key, v blob) without rowid")
        c.executemany("insert into notes values(?,?)", [("k%03d" % i, bytes(random.getrandbits(8) for _ in range(3000))) for i in range(notes)])
    c.executemany("insert into orders(id,item,qty) values(?,?,?)", [(i, "item-%d" % (i * 7919 % 10007), i % 13) for i in range(1, orders + 1)])
    c.commit(); c.close()
shop("shop.db")
shop("shop_small.db", orders=4000)          # 20% fewer orders
shop("shop_bit_small.db", orders=4800)      # 4% fewer
shop("shop_nonotes.db", drop_notes=True)
with open("shop.db", "rb") as f, gzip.open("shop.db.gz", "wb") as g: g.write(f.read())
d = open("shop.db", "rb").read()
open("trunc_half.db", "wb").write(d[:len(d) // 2])
open("trunc_page.db", "wb").write(d[:len(d) - 4096])
open("trunc_odd.db", "wb").write(d[:len(d) - 100])
b = bytearray(d)
for i in range(4096 * 3, 4096 * 3 + 64): b[i] = 0xAB        # a b-tree page header overwritten
open("corrupt_page.db", "wb").write(bytes(b))
b = bytearray(d); b[0:16] = b"Not SQLite at all"[:16]
open("corrupt_header.db", "wb").write(bytes(b))
g = open("shop.db.gz", "rb").read()
open("trunc.db.gz", "wb").write(g[:len(g) * 2 // 3])
b = bytearray(g); b[len(b) // 2] ^= 0x10
open("corrupt.db.gz", "wb").write(bytes(b))
# a folder and its archives
for sub, n in (("www", 40), ("conf", 3), ("data", 10)):
    os.makedirs("site/" + sub, exist_ok=True)
    for i in range(n): open("site/%s/f%02d.txt" % (sub, i), "w").write("x" * (i * 37 + 1))
with tarfile.open("site.tar.gz", "w:gz") as t: t.add("site")
with tarfile.open("site.tar", "w") as t: t.add("site")
shutil.copytree("site", "site_small")
for i in range(20): os.remove("site_small/www/f%02d.txt" % i)                # half of www gone
with tarfile.open("site_small.tar.gz", "w:gz") as t: t.add("site_small", arcname="site")
tb = open("site.tar", "rb").read()
open("trunc_mid.tar", "wb").write(tb[:len(tb) // 2 + 100])
open("trunc_end.tar", "wb").write(tb[:len(tb) - 10240] if len(tb) > 20480 else tb[:len(tb) // 2])
tg = open("site.tar.gz", "rb").read()
open("trunc.tar.gz", "wb").write(tg[:len(tg) - 30])
b = bytearray(tb); b[512 * 3 + 148] ^= 0x01                                 # a header checksum broken
open("corrupt.tar", "wb").write(bytes(b))
import io
with tarfile.open("evil.tar", "w") as t:
    data = b"pwned\n"; info = tarfile.TarInfo("../escaped.txt"); info.size = len(data); t.addfile(info, io.BytesIO(data))
open("garbage.bin", "wb").write(os.urandom(5000))
open("hmac.key", "w").write("a key the auditors keep\n")
PY

# ---- SQLite ----------------------------------------------------------------------------------
echo "-- SQLite files (built-in reader)"
expect 0 "a SQLite file restores and passes" --job sq shop.db
has "Verdict: PASSED" "first run passes and becomes the baseline"
has "orders  *5,000" "orders counted: 5,000 rows"
has "notes  *50 " "a WITHOUT ROWID table counted: 50 rows"
expect 0 "the same file gzipped passes" --job sq shop.db.gz
has "same" "the checksums match the baseline"
for f in trunc_half.db trunc_page.db trunc_odd.db corrupt_page.db corrupt_header.db trunc.db.gz corrupt.db.gz; do
    expect 1 "damaged backup $f fails" --job sq "$f"
    has "Verdict: FAILED" "$f: verdict FAILED"
done
expect 1 "garbage given as SQLite fails" --job sq --kind sqlite garbage.bin
expect 1 "garbage of unknown kind fails" --job sq garbage.bin
has "cannot tell what kind" "it says the kind is unknown"
expect 0 "4% fewer rows is within the 10% threshold" --job sq shop_bit_small.db
expect 1 "20% fewer rows alerts" --job sq shop_small.db
has "orders shrank from 4,800 to 4,000 rows (-16.7%)" "the alert names the table and the numbers (against the last good run, 4,800)"
expect 1 "the next run alerts again: an alert is not a new baseline" --job sq shop_small.db
expect 0 "the same with --max-shrink 25 passes" --job sq --max-shrink 25 shop_small.db
expect 1 "a table that disappeared alerts" --job sq shop_nonotes.db
has "notes is gone" "the alert says the table is gone"
expect 0 "--accept takes the smaller data as the baseline" --job sq --accept shop_small.db
has "Verdict: ACCEPTED" "verdict ACCEPTED"
expect 0 "after --accept the smaller file passes" --job sq shop_small.db
# a file copied with cp in the middle of a big write transaction (rollback journal, not
# copied): SQLite had already written part of the transaction into the file, so the table
# walks to more rows than its index holds. It PASSED before 0.5.1 (found by the case studies).
# Valid files of every shape the new checks touch must still pass.
python3 - << 'PY'
import os, random, shutil, sqlite3
for f in ("torn_app.db", "torn.db"):
    if os.path.exists(f): os.remove(f)
db = sqlite3.connect("torn_app.db", isolation_level=None)
db.execute("create table inv (id integer primary key, cust int, note text)")
db.execute("create index inv_cust on inv(cust)")
db.execute("begin")
for i in range(1, 401): db.execute("insert into inv values (?, ?, ?)", (i, i % 50, "committed row %d" % i))
db.execute("commit")
db.execute("pragma cache_size=20")                # small cache: pages reach the file before the commit
db.execute("begin")
for i in range(401, 10401):
    db.execute("insert into inv values (?, ?, ?)", (i, i % 50, "import row %d" % i))
    if i == 5400: shutil.copyfile("torn_app.db", "torn.db")
db.execute("rollback")
db.close()
random.seed(7)
def mk(name, ps, av=None, vac=False):
    if os.path.exists(name): os.remove(name)
    c = sqlite3.connect(name, isolation_level=None)
    c.execute("pragma page_size=%d" % ps)
    if av: c.execute("pragma auto_vacuum=%s" % av)
    c.execute("create table t (id integer primary key, a int, b text, c blob)")
    c.execute("create index t_a on t(a)")
    c.execute("create index t_part on t(b) where a > 50")
    c.execute("create index t_expr on t(lower(b), a * 2)")
    c.execute("create table w (k text primary key, v int, z text unique) without rowid")
    c.execute("create index w_v on w(v)")
    c.execute('create table "Mixed Case" (x)')
    c.execute('create index mc on "mixed case"(x)')
    c.execute("begin")
    for i in range(2000):
        c.execute("insert into t values (?, ?, ?, ?)", (i * 3, random.randint(0, 100), "s%d" % i * random.randint(1, 30), bytes(random.randint(0, 3000))))
        c.execute("insert into w values (?, ?, ?)", ("k%06d" % i, i % 77, "z%d" % i))
        c.execute('insert into "Mixed Case" values (?)', (i,))
    c.execute("commit")
    c.execute("delete from t where id % 7 = 0")
    c.execute("delete from w where v = 3")
    if av == "incremental": c.execute("pragma incremental_vacuum(5)")
    if vac: c.execute("vacuum")
    c.close()
mk("shape_512.db", 512); mk("shape_65536.db", 65536); mk("shape_autovac.db", 4096, "full")
mk("shape_incr.db", 1024, "incremental"); mk("shape_vacuumed.db", 4096, vac=True)
PY
expect 1 "a SQLite file copied in the middle of a write transaction fails" --job sq-torn torn.db
has "table inv has 470 rows, its index inv_cust has 400 entries" "it names the table and the index that disagree"
for f in shape_512.db shape_65536.db shape_autovac.db shape_incr.db shape_vacuumed.db; do
    expect 0 "a valid file passes the index, rowid and page checks ($f)" --job "sq-$f" "$f"
done
has "partial indexes: no more entries than rows" "a partial index is checked as partial"

# ---- archives ---------------------------------------------------------------------------------
echo "-- tar archives and folders (built-in tar and gzip readers)"
expect 0 "a tar.gz restores and passes" --job ar site.tar.gz
has "site/www  *40" "files counted per folder: site/www has 40"
expect 0 "the same files as a plain tar pass, same checksums" --job ar site.tar
has "same" "the checksums match"
expect 0 "the same files as a folder pass" --job ar site
expect 1 "half of a folder's files gone alerts" --job ar site_small.tar.gz
has "site/www shrank from 40 to 20 files" "the alert names the folder"
for f in trunc_mid.tar trunc_end.tar trunc.tar.gz corrupt.tar; do
    expect 1 "damaged archive $f fails" --job ar "$f"
done
expect 1 "an archive with a ../ path fails" --job ar evil.tar
has "\"..\"" "it names the unsafe path"
if [ -e "$WK/escaped.txt" ] || [ -e "$T/escaped.txt" ]; then bad "a file was written outside the sandbox"; else ok "nothing was written outside the sandbox"; fi

# ---- the report, the seal and the history ------------------------------------------------------
echo "-- reports, seals, history, webhook"
expect 0 "a report written to a file" --job rp --signer "ops on duty" --report "$T/r1.txt" shop.db
has "Signed off by:  ops on duty" "the signer is named"
"$R" --verify-report "$T/r1.txt" > "$T/v.out" 2>&1 && ok "the seal of an untouched report matches" || bad "the seal of an untouched report doesn't match"
sed 's/orders  *5,000/orders           5,900/' "$T/r1.txt" > "$T/r1bad.txt"
cmp -s "$T/r1.txt" "$T/r1bad.txt" && bad "the report edit didn't take"
"$R" --verify-report "$T/r1bad.txt" > "$T/v.out" 2>&1 && bad "an edited report still matches its seal" || ok "an edited report fails its seal"
expect 0 "a keyed seal" --job rp --sign-key hmac.key --report "$T/r2.txt" shop.db
has "HMAC-SHA256" "the report says the seal is keyed"
"$R" --verify-report "$T/r2.txt" --sign-key hmac.key > /dev/null 2>&1 && ok "the keyed seal matches with the key" || bad "the keyed seal doesn't match with the key"
echo "another key" > "$T/k2"
"$R" --verify-report "$T/r2.txt" --sign-key "$T/k2" > /dev/null 2>&1 && bad "the keyed seal matches under another key" || ok "the keyed seal fails under another key"
expect 0 "a report into a folder" --job rp --report-dir "$T/reports" shop.db
[ "$(ls "$T/reports" | grep -c '^rp-.*-run3\.txt$')" -eq 1 ] && ok "the report file is named by job, date and run" || bad "no report file rp-...-run3.txt in $T/reports"
"$R" --history "$H" --list --job sq > "$T/last.out" 2>&1
has "ACCEPTED" "--list shows the runs of a job"
[ "$(grep -c '^sq ' "$T/last.out")" -ge 10 ] && ok "--list shows every run" || { bad "--list shows too few runs"; show; }
if command -v curl > /dev/null; then
    cat > "$T/hook.py" << 'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers["Content-Length"]); open(sys.argv[2], "wb").write(self.rfile.read(n))
        self.send_response(200); self.end_headers()
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1] + ".tmp", "w").write(str(s.server_port)); import os; os.rename(sys.argv[1] + ".tmp", sys.argv[1])
s.timeout = 30; s.handle_request()
PY
    python3 "$T/hook.py" "$T/port" "$T/hook.json" &
    hp=$!
    i=0; while [ ! -s "$T/port" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    NO_PROXY=127.0.0.1 no_proxy=127.0.0.1 expect 1 "an alert posts to the webhook" --job sq --webhook "http://127.0.0.1:$(cat "$T/port")/hook" shop_nonotes.db
    wait $hp
    has "Webhook:        sent to" "the report says the webhook was sent"
    grep -q '"verdict":"ALERT"' "$T/hook.json" 2> /dev/null && ok "the webhook body says ALERT" || bad "the webhook body: $(cat "$T/hook.json" 2> /dev/null)"
    NO_PROXY=127.0.0.1 no_proxy=127.0.0.1 expect 1 "a webhook that can't be reached doesn't hide the alert" --job sq --webhook "http://127.0.0.1:9/none" shop_nonotes.db
    has "FAILED to send" "the report says the webhook failed"
    expect 0 "a passing run sends nothing without --webhook-always" --job rp --webhook "http://127.0.0.1:9/none" shop.db
    has "not needed (the run passed)" "the report says so"
    # the webhook's body and curl's output went to predictable names in the work folder
    # (often /tmp), through any link someone left there, before 0.5.1
    echo precious > "$T/victim"
    sh -c 'ln -s "$1" "$2/altsql-restore-webhook-$$.json"; ln -s "$1" "$2/altsql-restore-webhook-$$.out"; exec "$3" --history "$4" --work-dir "$2" -q --job rp --webhook-always --webhook http://127.0.0.1:9/none shop.db' \
        x "$T/victim" "$WK" "$R" "$H" > /dev/null 2>&1
    [ "$(cat "$T/victim")" = precious ] && ok "the webhook follows no link left in the work folder" || bad "a link in the work folder was followed: $(head -c 80 "$T/victim")"
    rm -f "$WK"/altsql-restore-webhook-*
else
    echo "SKIPPED the webhook tests: no curl"
fi

# ---- engines that aren't there ----------------------------------------------------------------
echo "-- missing engines are NOT VERIFIED"
expect 3 "PostgreSQL dump, no server programs, local only" --job nv --sandbox local --engine-path /nonexistent "$FX/shop_pg.sql.gz"
has "Verdict: NOT VERIFIED" "verdict NOT VERIFIED"
hasnt "PASSED" "it never says PASSED"
expect 3 "PostgreSQL dump, no server programs and no Docker" --job nv --engine-path /nonexistent --docker /nonexistent/docker "$FX/shop_pg.sql.gz"
has "Docker was not found" "the report says Docker is missing too"
expect 3 "MySQL dump, no server programs and no Docker" --job nvm --engine-path /nonexistent --docker /nonexistent/docker "$FX/shop_my.sql.gz"
has "no local MySQL or MariaDB server" "the report says the server is missing"
"$R" --history "$H" --list --job nv > "$T/last.out" 2>&1
has "NOT VERIFIED" "the history keeps NOT VERIFIED runs"

# ---- clean-up ---------------------------------------------------------------------------------
echo "-- the sandbox is cleaned up"
[ "$(sandboxes)" -eq 0 ] && ok "no sandbox left after all the runs above" || bad "$(sandboxes) sandboxes left"
ALTSQL_RESTORE_TEST_PAUSE=30 "$R" --history "$T/h_kill.db" --work-dir "$WK" -q shop.db > /dev/null 2>&1 &
p=$!
wait_sandbox && ok "a run has its sandbox" || bad "no sandbox appeared"
kill -9 $p; wait $p 2> /dev/null
wait_gone 10 && ok "kill -9: the watchdog removed the sandbox" || bad "kill -9: the sandbox is still there"
ALTSQL_RESTORE_TEST_PAUSE=30 "$R" --history "$T/h_kill.db" --work-dir "$WK" -q shop.db > /dev/null 2>&1 &
p=$!
wait_sandbox
i=0; while ! cat "$WK"/altsql-restore-*/watchdog > /dev/null 2>&1 && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
kill -9 "$(cat "$WK"/altsql-restore-*/watchdog)" $p 2> /dev/null; wait $p 2> /dev/null
sleep 0.5
[ "$(sandboxes)" -eq 1 ] && ok "kill -9 of the run and its watchdog leaves a sandbox, for the sweep" || bad "expected 1 sandbox left, found $(sandboxes)"
expect 0 "the next run sweeps it" --job cl shop.db
has "removed a sandbox left by an earlier run" "the report says it removed it"
[ "$(sandboxes)" -eq 0 ] && ok "nothing left after the sweep" || bad "$(sandboxes) sandboxes left after the sweep"
ALTSQL_RESTORE_TEST_PAUSE=30 "$R" --history "$T/h_kill.db" --work-dir "$WK" -q shop.db > /dev/null 2>&1 &
p=$!
wait_sandbox
kill -TERM $p; wait $p 2> /dev/null
wait_gone 10 && ok "SIGTERM: the sandbox is removed" || bad "SIGTERM: the sandbox is still there"

# ---- PostgreSQL and MySQL ----------------------------------------------------------------------
# engine_tests NAME KIND SANDBOX: the dumps of one engine, through one way of running it
engine_tests() {
    name=$1; kind=$2; mode=$3; job=$kind-$mode
    if [ "$kind" = pg ]; then full=shop_pg.sql.gz; shrunk=shop_pg_shrunk.sql.gz; t1=public.customers; t2=sales.orders; done_line="PostgreSQL database dump complete"
    else full=shop_my.sql.gz; shrunk=shop_my_shrunk.sql.gz; t1=customers; t2=orders; done_line="-- Dump completed"; fi
    "$R" --history "$H" --work-dir "$WK" --job "$job" --sandbox "$mode" "$FX/$full" > "$T/last.out" 2>&1
    got=$?
    if [ $got -eq 3 ]; then
        why=$(grep -A3 '^Verdict' "$T/last.out" | grep '  - ' | head -n 2 | sed 's/^ *- //' | tr '\n' ' ')
        echo "SKIPPED $name ($mode): $why"
        return 1
    fi
    [ $got -eq 0 ] && ok "$name ($mode): the dump restores and passes" || { bad "$name ($mode): exit $got, wanted 0"; show; return 0; }
    has "$t1  *1,200" "$name ($mode): $t1 has 1,200 rows"
    has "$t2  *5,000" "$name ($mode): $t2 has 5,000 rows"
    grep '^Restored with' "$T/last.out" | sed 's/^Restored with: *//' > "$T/ver_$job"
    echo "      restored with: $(cat "$T/ver_$job")"
    if [ "$kind" = pg ]; then
        expect 0 "$name ($mode): the custom-format dump of the same data passes" --job "$job" --sandbox "$mode" "$FX/shop_pg.dump"
        has "same" "$name ($mode): the plain and custom dumps give the same checksums"
        expect 0 "$name ($mode): the tar-format dump (pg_dump -Ft) of the same data passes" --job "$job" --sandbox "$mode" "$FX/shop_pg.tar"
        has "archive dump" "$name ($mode): it is restored as a PostgreSQL dump, not checked as a file archive"
        has "sales.orders  *5,000  *5,000  *+0.0%  [0-9a-f]*  same" "$name ($mode): the tar-format dump gives the same checksums"
        expect 0 "$name ($mode): a dump that makes its own database (pg_dump -C) passes" --job "$job-create" --sandbox "$mode" "$FX/shop_pg_create.sql.gz"
        has "shop:sales.orders  *5,000" "$name ($mode): its tables are counted in the database it made"
        python3 -c "import sys;d=open(sys.argv[1],'rb').read();open(sys.argv[2],'wb').write(d[:len(d)//2])" "$FX/shop_pg.dump" "$T/trunc.dump"
        expect 1 "$name ($mode): a cut-short custom dump fails" --job "$job" --sandbox "$mode" "$T/trunc.dump"
    fi
    expect 1 "$name ($mode): 20% fewer orders alerts" --job "$job" --sandbox "$mode" "$FX/$shrunk"
    has "$t2 shrank from 5,000 to 4,000 rows" "$name ($mode): the alert names the table"
    python3 - "$FX/$full" "$T" "$kind" << 'PY'
import gzip, sys
src, T, kind = sys.argv[1], sys.argv[2], sys.argv[3]
s = gzip.open(src, "rt").read()
open(T + "/trunc.sql", "w").write(s[: len(s) * 3 // 5])
mark = "-- PostgreSQL database dump complete" if kind == "pg" else "-- Dump completed"
open(T + "/trunc_end.sql", "w").write(s[: s.rindex(mark)])                      # only the last lines gone
if kind == "pg":
    i = s.index("COPY sales.orders"); j = s.index("\n", s.index("\n", i) + 1)
    open(T + "/bad.sql", "w").write(s[:j] + "\nnot a row at all\n" + s[j + 1:])
    i = s.index("COPY public.audit_log")
    open(T + "/slow.sql", "w").write(s[:i] + "SELECT pg_sleep(60);\n" + s[i:])
else:
    i = s.index("INSERT INTO `orders`")
    open(T + "/bad.sql", "w").write(s[:i] + "INSERT INTO orders VALUES (not valid sql;\n" + s[i:])
    i = s.index("INSERT INTO `audit_log`")
    open(T + "/slow.sql", "w").write(s[:i] + "SELECT SLEEP(60);\n" + s[i:])
PY
    expect 1 "$name ($mode): a dump cut short fails" --job "$job" --sandbox "$mode" "$T/trunc.sql"
    has "doesn't end with \"$done_line\"" "$name ($mode): it says the last line is missing"
    expect 1 "$name ($mode): a dump missing only its last lines fails" --job "$job" --sandbox "$mode" "$T/trunc_end.sql"
    expect 1 "$name ($mode): a damaged dump fails at the first error" --job "$job" --sandbox "$mode" "$T/bad.sql"
    has "stopped with an error" "$name ($mode): it quotes the error"
    expect 1 "$name ($mode): a restore past --timeout is stopped and fails" --job "$job" --sandbox "$mode" --timeout 3 "$T/slow.sql"
    has "took longer than 3 seconds" "$name ($mode): it says the restore was stopped"
    # the client's own commands (\! runs a shell command) must never run on this machine: they
    # ran as root, and the run passed, before 0.5.1. Markers go in a folder anyone can write,
    # so a command run as the server's user would leave one too.
    M=$T/markers; mkdir -p "$M"; chmod 1777 "$M"
    python3 - "$FX/$full" "$T" "$kind" "$M" << 'PY'
import gzip, sys
src, T, kind, M = sys.argv[1:5]
s = gzip.open(src, "rt").read()
if kind == "pg":
    s = "\n".join(l for l in s.split("\n") if not l.startswith("\\restrict") and not l.startswith("\\unrestrict"))
    i = s.index("COPY public.audit_log")
    open(T + "/cmd1.sql", "w").write(s[:i] + "\\! touch %s/cmd1\n" % M + s[i:])
    open(T + "/cmd2.sql", "w").write(s[:i] + "SELECT 1; \\o %s/cmd2\n" % M + s[i:])
    # psql reads quotes by standard_conforming_strings, which SQL can change on the fly: no
    # reading of the text can follow that, psql's own lock (\restrict) must catch it
    open(T + "/cmd3.sql", "w").write(s[:i] + "SET standard_conforming_strings = off;\n"
         "SELECT set_config('standard_' || 'conforming_strings', 'on', false);\n"
         "SELECT 'a\\'; \\! touch %s/cmd3\n-- ';\n" % M + s[i:])
    open(T + "/cmd4.sql", "w").write(s[:i] + "\\connect \"host=127.0.0.1 dbname=postgres\"\n" + s[i:])
    open(T + "/nocmd.sql", "w").write(s[:i] + "CREATE FUNCTION public.f(t text) RETURNS text LANGUAGE sql AS $fn$ SELECT regexp_replace(t, '\\s+\\w', '') $fn$;\n"
         "CREATE TABLE public.notes (n text);\nINSERT INTO public.notes VALUES ('C:\\new\\! x'), (E'it\\'s \\\\! ok'), ($$ \\! $$);\n"
         "/* \\! in a comment */ -- \\! too\n" + s[i:])
else:
    i = s.index("INSERT INTO `audit_log`")
    open(T + "/cmd1.sql", "w").write(s[:i] + "\\! touch %s/cmd1\n" % M + s[i:])
    open(T + "/cmd2.sql", "w").write(s[:i] + "system touch %s/cmd2\n" % M + s[i:])
    open(T + "/cmd3.sql", "w").write(s[:i] + "SELECT 'x'; \\! touch %s/cmd3\n" % M + s[i:])
    open(T + "/cmd4.sql", "w").write(s[:i] + "tee %s/cmd4\n" % M + s[i:])
    j = s.rindex("/*!40103 SET TIME_ZONE")
    open(T + "/nocmd.sql", "w").write(s[:j] + "CREATE TABLE `notes` (n text);\nINSERT INTO `notes` VALUES ('C:\\\\new\\\\! x'), ('it\\'s'), ('\\\\! ok');\n"
         "CREATE TABLE `ledger` (id int primary key, amount int) WITH SYSTEM VERSIONING;\n"
         "INSERT INTO `ledger` VALUES (1,10),(2,20),(3,30);\n" + s[j:])
PY
    for c in 1 2 3 4; do
        expect 1 "$name ($mode): a dump with a client command fails ($c)" --job "$job-cmd" --sandbox "$mode" "$T/cmd$c.sql"
    done
    [ -z "$(ls "$M")" ] && ok "$name ($mode): no client command in a dump ran on this machine" || bad "$name ($mode): a client command ran: $(ls "$M")"
    if [ "$kind" = pg ]; then
        expect 0 "$name ($mode): backslashes in strings, dollar quotes and comments are not commands" --job "$job-nocmd" --sandbox "$mode" "$T/nocmd.sql"
        has "public.notes  *3" "$name ($mode): the table with backslashes in it is counted"
    elif grep -q MariaDB "$T/ver_$job"; then
        expect 0 "$name ($mode): backslashes in strings are not commands" --job "$job-nocmd" --sandbox "$mode" "$T/nocmd.sql"
        has "notes  *3" "$name ($mode): the table with backslashes in it is counted"
        # MariaDB lists a table WITH SYSTEM VERSIONING as 'SYSTEM VERSIONED': it was not counted before 0.5.1
        has "ledger  *3" "$name ($mode): a system-versioned table is counted"
    fi
    wait_gone 20 && ok "$name ($mode): no server or sandbox left after the timeout" || bad "$name ($mode): a server or sandbox is left"
    "$R" --history "$T/h_kill.db" --work-dir "$WK" --sandbox "$mode" -q "$T/slow.sql" > /dev/null 2>&1 &
    p=$!
    if [ "$mode" = local ]; then
        wait_sandbox server && ok "$name ($mode): a server runs in the sandbox" || bad "$name ($mode): no server appeared"
    else
        wait_sandbox; sleep 15
    fi
    sleep 2
    kill -9 $p; wait $p 2> /dev/null
    wait_gone 30 && ok "$name ($mode): kill -9 mid-restore: the server is stopped and the sandbox removed" || bad "$name ($mode): kill -9 left $(servers) servers and $(sandboxes) sandboxes"
    if [ "$mode" = local ]; then                 # killed at every phase: setup, start, restore
        left=0
        for d in 0.2 0.7 1.5 3; do
            "$R" --history "$T/h_kill.db" --work-dir "$WK" --sandbox local -q "$T/slow.sql" > /dev/null 2>&1 &
            p=$!
            wait_sandbox
            sleep $d
            kill -9 $p; wait $p 2> /dev/null
            wait_gone 30 || left=$((left + 1))
        done
        [ $left -eq 0 ] && ok "$name ($mode): kill -9 at 0.2, 0.7, 1.5 and 3 s after the sandbox appears: nothing left each time" || bad "$name ($mode): $left of 4 early kills left a server or sandbox"
    fi
    if [ "$mode" = docker ]; then
        sleep 5
        [ "$(docker ps -a --filter label=altsql-restore.owner --format '{{.Names}}' | grep -c .)" -eq 0 ] && ok "$name ($mode): no container left" || bad "$name ($mode): a container is left"
    fi
    return 0
}

echo "-- PostgreSQL"
engine_tests PostgreSQL pg local && cov_pg="yes, $(cat "$T/ver_pg-local")"
echo "-- MySQL / MariaDB"
engine_tests MySQL my local && cov_my="yes, $(cat "$T/ver_my-local")"
if [ "${ALTSQL_RESTORE_TEST_DOCKER:-}" = 1 ]; then
    echo "-- Docker"
    d1=SKIPPED; d2=SKIPPED
    engine_tests PostgreSQL pg docker && d1="yes, $(cat "$T/ver_pg-docker")"
    engine_tests MySQL my docker && d2="yes, $(cat "$T/ver_my-docker")"
    cov_docker="postgres: $d1, mysql: $d2"
fi
[ "$(sandboxes)" -eq 0 ] && ok "no sandbox left at the end" || bad "$(sandboxes) sandboxes left at the end"

echo
echo "engines covered: sqlite (built-in reader) yes; archives (built-in tar, gzip, folder) yes;"
echo "  postgres local: $cov_pg; mysql/mariadb local: $cov_my; docker: $cov_docker"
echo "$pass checks passed, $fail failed"
[ "$fail" -eq 0 ] && echo "all passed"
[ "$fail" -eq 0 ]
