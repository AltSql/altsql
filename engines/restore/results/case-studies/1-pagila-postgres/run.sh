#!/bin/sh
# Case study 1: Pagila on PostgreSQL 16, five nightly backups.
#
#   sh ../fetch.sh && sh run.sh      (as root; needs local PostgreSQL 16 server programs)
#
# Pagila is the public PostgreSQL sample database of a DVD rental chain
# (github.com/devrimgunduz/pagila). It is loaded into a local "production"
# server, adapted for PostgreSQL 16 by ../adapt_pagila.py, and backed up each
# "night" with pg_dump | gzip. altsql-restore checks each backup with the same
# job name and history. The nights run one after another; each stands for one
# night of a real schedule.
#   night 1  normal backup                            -> baseline
#   night 2  250 new rentals; the upload to the backup host stops at 60%
#   night 3  an archiving job with the wrong cutoff year deletes every rental
#            and payment from before 2025, then the backup runs
#   night 4  nobody has fixed it yet
#   night 5  the deleted rows are put back from the night 1 backup
. ../common.sh
pg_up
python3 -I ../adapt_pagila.py "$DATA/pagila-schema.sql" "$DATA/pagila-data.sql" "$scratch/schema.sql" "$scratch/data.sql"
psql_q -c "create database pagila"
psql_q -d pagila -f "$scratch/schema.sql" >/dev/null
psql_q -d pagila -f "$scratch/data.sql" >/dev/null
mkdir -p "$scratch/backups" "$scratch/upload"
backup() { pg_dump -d pagila | gzip -6 > "$scratch/backups/pagila-night$1.sql.gz"; }
counts() { psql_q -d pagila -Atc "select 'rental', count(*) from rental union all select 'payment', count(*) from payment" | tr '\n' ' '; }

# night 1
backup 1
note "night 1: production $(counts)"
run night1 --job pagila --history history.db --signer "dba on call" backups/pagila-night1.sql.gz

# night 2: new rentals, then the copy to the backup host is cut short
psql_q -d pagila -c "insert into rental (rental_date, inventory_id, customer_id, staff_id)
  select timestamptz '2026-07-31 09:00+00' + n * interval '3 minutes', (n * 7) % 4581 + 1, (n * 13) % 599 + 1, 1
  from generate_series(1, 250) n" >/dev/null
backup 2
size=$(wc -c < "$scratch/backups/pagila-night2.sql.gz")
head -c $((size * 60 / 100)) "$scratch/backups/pagila-night2.sql.gz" > "$scratch/upload/pagila-night2.sql.gz"
note "night 2: production $(counts); dump $size bytes, uploaded copy $(wc -c < "$scratch/upload/pagila-night2.sql.gz") bytes"
run night2 --job pagila --history history.db --signer "dba on call" upload/pagila-night2.sql.gz

# night 3: the archiving job meant 2023-01-01 and ran with 2025-01-01
psql_q -d pagila -c "\\copy (select * from payment where payment_date < '2025-01-01') to '$scratch/payment.saved'" >/dev/null
psql_q -d pagila -c "\\copy (select * from rental where rental_date < '2025-01-01') to '$scratch/rental.saved'" >/dev/null
psql_q -d pagila -c "begin; delete from payment where payment_date < '2025-01-01'; delete from rental where rental_date < '2025-01-01'; commit;" >/dev/null
backup 3
note "night 3: production $(counts)"
run night3 --job pagila --history history.db --signer "dba on call" backups/pagila-night3.sql.gz

# night 4: still not fixed
backup 4
run night4 --job pagila --history history.db --signer "dba on call" backups/pagila-night4.sql.gz

# night 5: rows put back from the night 1 backup
psql_q -d pagila -c "\\copy rental from '$scratch/rental.saved'" >/dev/null
psql_q -d pagila -c "\\copy payment from '$scratch/payment.saved'" >/dev/null
backup 5
note "night 5: production $(counts)"
run night5 --job pagila --history history.db --signer "dba on call" backups/pagila-night5.sql.gz

(cd "$scratch" && "$R" --list --job pagila --history history.db) > "$out/list.txt" 2>&1
ls -l "$scratch/backups" "$scratch/upload" | awk 'NF>5{print $5, $9}' > "$out/backup-sizes.txt"
cp "$scratch/history.db" "$out/history.db"
cat "$out/notes.txt"
