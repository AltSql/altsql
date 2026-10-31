#!/bin/sh
# Case study 3: the Chinook SQLite database copied with cp while the
# application writes to it, then backed up the safe way.
#
#   sh ../fetch.sh && sh run.sh      (needs python3 and the sqlite3 shell)
#
# Chinook (github.com/lerocha/chinook-database) is a public sample database
# of a digital music store. The app keeps it as app/chinook.db.
#   night 1  cp while nothing writes                         -> baseline
#   night 2  cp half-way through the nightly import (20,000 invoices in one
#            transaction), the journal file left behind
#   night 3  sqlite3 .backup, which waits for the import to commit
# Each copy is also checked by SQLite itself (PRAGMA integrity_check), in
# output/integrity-*.txt.
. ../common.sh
mkdir -p "$scratch/app" "$scratch/backups"
cp "$DATA/Chinook_Sqlite.sqlite" "$scratch/app/chinook.db"
check() {  # SQLite's own view of a copy
  { echo "\$ sqlite3 backups/$1 'PRAGMA integrity_check'"
    sqlite3 "$scratch/backups/$1" 'PRAGMA integrity_check' 2>&1 | head -8
    echo "(lines in full: $(sqlite3 "$scratch/backups/$1" 'PRAGMA integrity_check' 2>&1 | wc -l))"
    echo "\$ sqlite3 backups/$1 'SELECT count(*) FROM Invoice; SELECT count(*) FROM InvoiceLine'"
    sqlite3 "$scratch/backups/$1" 'SELECT count(*) FROM Invoice; SELECT count(*) FROM InvoiceLine' 2>&1
  } > "$out/integrity-$1.txt"
}

# night 1
cp "$scratch/app/chinook.db" "$scratch/backups/night1.db"
check night1.db
run night1 --job chinook-app --history history.db --signer "app team" backups/night1.db

# night 2: cp during the import
python3 -I "$here/writer.py" "$scratch/app/chinook.db" "$scratch/pause" "$scratch/go" &
wpid=$!
while [ ! -f "$scratch/pause" ]; do sleep 0.1; done
ls -l "$scratch/app" | awk 'NF>5{print $5, $9}' > "$out/app-folder-during-import.txt"
cp "$scratch/app/chinook.db" "$scratch/backups/night2.db"
touch "$scratch/go"
wait $wpid
check night2.db
run night2 --job chinook-app --history history.db --signer "app team" backups/night2.db

# night 3: the import runs again, and the backup uses SQLite's backup API
rm -f "$scratch/pause" "$scratch/go"
sqlite3 "$scratch/app/chinook.db" "DELETE FROM InvoiceLine WHERE InvoiceLineId >= 100000; DELETE FROM Invoice WHERE InvoiceDate = '2026-10-30 00:00:00';"
python3 -I "$here/writer.py" "$scratch/app/chinook.db" "$scratch/pause" "$scratch/go" &
wpid=$!
while [ ! -f "$scratch/pause" ]; do sleep 0.1; done
( sleep 2; touch "$scratch/go" ) &
start=$(date +%s)
sqlite3 "$scratch/app/chinook.db" ".timeout 120000" ".backup $scratch/backups/night3.db"
note "night 3: sqlite3 .backup returned after $(( $(date +%s) - start )) s, once the import had committed"
wait $wpid
check night3.db
run night3 --job chinook-app --history history.db --signer "app team" backups/night3.db

(cd "$scratch" && "$R" --list --job chinook-app --history history.db) > "$out/list.txt" 2>&1
cp "$scratch/history.db" "$out/history.db"
cat "$out/notes.txt"
