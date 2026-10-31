#!/bin/sh
# Reruns every AltSql Restore case study and the use-example recipes.
#
#   sudo sh run_all.sh
#
# Needs: curl, python3, sqlite3, iconv, and local PostgreSQL 16 and MariaDB
# 10.11 server programs (initdb/postgres/pg_dump, mariadbd/mariadb-dump).
# fetch.sh downloads the released altsql-restore (checksum-checked) and the
# public sample databases into work/ (not committed). Each folder's run.sh
# writes its output/ folder: the command of each run, what it printed, its
# exit status, the sealed report, the history file, and notes. Dates, seals
# and timings change from run to run; counts, verdicts and exit codes don't.
#   1-pagila-postgres    five nights of Pagila backups on PostgreSQL
#   2-restore-scripts    dumps a plain restore script reports as restored
#   3-sqlite-mid-write   a SQLite file copied while the app writes
#   4-use-examples       the recipes in the use examples post
set -eu
cd "$(dirname "$0")"
sh fetch.sh
cp work/versions.txt versions.txt
for d in 1-pagila-postgres 2-restore-scripts 3-sqlite-mid-write 4-use-examples; do
  echo "### $d"
  (cd "$d" && sh run.sh)
done
