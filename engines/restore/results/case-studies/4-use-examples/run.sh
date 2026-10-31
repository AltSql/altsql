#!/bin/sh
# The recipes in the "use examples" post, each run as written.
#
#   sh ../fetch.sh && sh run.sh      (as root; needs local PostgreSQL 16, python3, sqlite3, curl)
#
#   1  the nightly cron line: the command from the line, run by hand (no cron
#      daemon here) on a gzipped PostgreSQL dump of Chinook
#   2  --max-shrink: the same 4% loss under the default 10% and under 2%;
#      then --accept after a planned clean-up
#   3  the history queried with the altsql-db shell
#   4  a keyed seal: --sign-key, --verify-report, a report with one number
#      edited, and the wrong key
#   5  a webhook: the JSON a local listener received
#   6  NOT VERIFIED: a PostgreSQL dump on a host with no usable server
. ../common.sh
mkdir -p "$scratch/backups" "$scratch/reports" "$scratch/etc"
cp "$DATA/Chinook_Sqlite.sqlite" "$scratch/chinook.db"

# 1. the cron line
pg_up
psql_q -f "$DATA/Chinook_PostgreSql.sql" >/dev/null 2>&1
pg_dump -d chinook | gzip > "$scratch/backups/shop-latest.sql.gz"
cat > "$out/1-crontab.txt" <<'TAB'
# m  h  dom mon dow  command   (backup host; the dump lands by 02:30)
15 3  *   *   *    /usr/local/bin/altsql-restore -q --job shop-db --history /var/lib/altsql/restore.db --report-dir /var/log/altsql-restore --signer "backup host" /backups/shop-latest.sql.gz || logger -t altsql-restore "shop-db backup check exit $?"
TAB
run 1-cron -q --job shop-db --history restore.db --report-dir reports --signer "backup host" backups/shop-latest.sql.gz
ls "$scratch/reports" | sed 's/[0-9]\{8\}-[0-9]\{6\}/YYYYMMDD-HHMMSS/' > "$out/1-report-dir.txt"
pg_down

# 2. --max-shrink, then --accept
cp "$scratch/chinook.db" "$scratch/backups/night1.db"
run 2a-baseline -q --job music --history restore.db backups/night1.db
sqlite3 "$scratch/chinook.db" "DELETE FROM InvoiceLine WHERE InvoiceLineId % 25 = 0"
cp "$scratch/chinook.db" "$scratch/backups/night2.db"
note "2: InvoiceLine after the delete: $(sqlite3 "$scratch/chinook.db" 'SELECT count(*) FROM InvoiceLine') rows"
run 2b-default-10 --job music --history restore.db backups/night2.db
run 2c-max-shrink-2 --job music-strict --history restore-strict.db --max-shrink 2 backups/night1.db
run 2d-max-shrink-2 --job music-strict --history restore-strict.db --max-shrink 2 backups/night2.db
run 2e-accept --job music-strict --history restore-strict.db --max-shrink 2 --accept --signer "dba: planned clean-up, ticket 4127" backups/night2.db
run 2f-next-night --job music-strict --history restore-strict.db --max-shrink 2 backups/night2.db

# 3. the history in the altsql-db shell
{
  for q in "SELECT job, run, verdict, backup FROM runs WHERE verdict <> 'PASSED';" \
           "SELECT run, rows FROM counts WHERE job = 'music-strict' AND item = 'InvoiceLine';" \
           "SELECT verdict, count(*) FROM runs GROUP BY verdict;"; do
    echo "\$ altsql-db restore-strict.db \"$q\""
    (cd "$scratch" && "$DB" restore-strict.db "$q") 2>&1
    echo
  done
  echo "\$ altsql-restore --list --history restore-strict.db"
  (cd "$scratch" && "$R" --list --history restore-strict.db) 2>&1
} > "$out/3-history.txt"

# 4. keyed seal
head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$scratch/etc/audit.key"
chmod 600 "$scratch/etc/audit.key"
run 4a-signed -q --job music-audit --history restore-audit.db --signer "ops on call" --sign-key etc/audit.key --report reports/audit-night1.txt backups/night1.db
cp "$scratch/reports/audit-night1.txt" "$out/4-audit-night1.txt"
sed 's/^InvoiceLine  *2,240/InvoiceLine              2,420/' "$scratch/reports/audit-night1.txt" > "$scratch/reports/audit-edited.txt"
head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$scratch/etc/other.key"
vr() { name=$1; shift; set +e; (cd "$scratch" && "$R" "$@") > "$out/$name.txt.tmp" 2>&1; st=$?; set -e
       { echo "\$ altsql-restore $*"; cat "$out/$name.txt.tmp"; echo "exit status: $st"; } > "$out/$name.txt"; rm "$out/$name.txt.tmp"; echo "== $name: exit $st"; }
vr 4b-verify       --verify-report reports/audit-night1.txt --sign-key etc/audit.key
vr 4c-verify-edit  --verify-report reports/audit-edited.txt --sign-key etc/audit.key
vr 4d-verify-wrong --verify-report reports/audit-night1.txt --sign-key etc/other.key
vr 4e-verify-nokey --verify-report reports/audit-night1.txt
vr 4f-list         --list --history restore-audit.db
diff "$scratch/reports/audit-night1.txt" "$scratch/reports/audit-edited.txt" > "$out/4-edit.diff" || true

# 5. webhook
port=18765   # fixed, so the URL in the sealed report needs no editing
python3 -I "$here/hook.py" "$port" "$scratch/hook.log" & hpid=$!
sleep 1
run 5a-baseline -q --job music-hook --history restore.db --max-shrink 2 backups/night1.db
run 5b-webhook -q --job music-hook --history restore.db --max-shrink 2 --webhook "http://127.0.0.1:$port/backups" backups/night2.db
kill $hpid
cp "$scratch/hook.log" "$out/5-webhook-received.txt"

# 6. NOT VERIFIED: no PostgreSQL server programs in the folders it may use, no Docker
mkdir -p "$scratch/empty"
run 6-not-verified --job shop-db-elsewhere --history restore.db --engine-path empty --docker /nonexistent/docker backups/shop-latest.sql.gz
(cd "$scratch" && "$R" --list --job shop-db-elsewhere --history restore.db) > "$out/6-list.txt" 2>&1
cat "$out/notes.txt"
