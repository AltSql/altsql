#!/bin/sh
# Case study 2: dumps that a plain restore script reports as restored.
#
#   sh ../fetch.sh && sh run.sh      (as root; local PostgreSQL 16 and MariaDB 10.11)
#
# Each backup goes through two restores: the one-line script many teams use
# (psql -f, gunzip | psql, mariadb < dump), recorded in output/plain-*.txt
# with its exit status, its error lines and what the tables hold afterwards;
# and altsql-restore, recorded in output/*.txt and output/*.report.txt.
#   A  Chinook on PostgreSQL: a good dump, then the same dump re-encoded to
#      Latin-1 (iconv), as an editor or transfer tool set to a Windows code
#      page would leave it.
#   B  Pagila exactly as published upstream (schema file + data file, one
#      plain dump made by a newer PostgreSQL), restored on PostgreSQL 16.
#   C  Chinook on PostgreSQL, the gzipped dump cut short in transfer, through
#      gunzip | psql.
#   D  Chinook on MariaDB: a good dump, then the Latin-1 copy.
. ../common.sh
mkdir -p "$scratch/backups"

# plain NAME DB COMMAND...: runs a plain restore into a fresh database and records it
plain() {
  name=$1; db=$2; shift 2
  psql_q -c "drop database if exists $db" -c "create database $db" >/dev/null
  set +e; sh -c "$*" > "$scratch/plain.out" 2>&1; st=$?; set -e
  {
    echo "\$ $*" | sed "s#$scratch/##g; s#$DATA/##g"
    echo "exit status: $st"
    echo "error lines: $(grep -c 'ERROR' "$scratch/plain.out" || true)"
    echo "first error lines:"
    grep 'ERROR' "$scratch/plain.out" | head -5 | sed "s#$scratch/##g; s#$DATA/##g"
    echo "other messages:"
    grep -v 'ERROR\|^LINE\|^HINT\|^CONTEXT\|^ *^\|^ *$' "$scratch/plain.out" | head -3 | sed "s#$scratch/##g; s#$DATA/##g"
    echo "tables afterwards (rows):"
    psql -X -At -F ' ' -d "$db" -c "select table_schema || '.' || table_name from information_schema.tables where table_schema not in ('pg_catalog','information_schema') and table_type = 'BASE TABLE' order by 1" |
      while read t; do echo "  $t $(psql -X -At -d "$db" -c "select count(*) from $t")"; done
  } > "$out/plain-$name.txt"
  echo "== plain-$name: exit $st"
}

pg_up
psql_q -f "$DATA/Chinook_PostgreSql.sql" >/dev/null 2>&1
pg_dump -d chinook > "$scratch/backups/chinook.sql"
iconv -f UTF-8 -t ISO-8859-1//TRANSLIT "$scratch/backups/chinook.sql" > "$scratch/backups/chinook-latin1.sql"

# A
plain A-good    plaincheck "psql -X -q -d plaincheck -f $scratch/backups/chinook.sql"
plain A-latin1  plaincheck "psql -X -q -d plaincheck -f $scratch/backups/chinook-latin1.sql"
run A-good   --job chinook-pg --history history.db backups/chinook.sql
run A-latin1 --job chinook-pg --history history.db backups/chinook-latin1.sql

# B
cat "$DATA/pagila-schema.sql" "$DATA/pagila-data.sql" > "$scratch/backups/pagila-upstream.sql"
plain B-pagila  plaincheck "psql -X -q -d plaincheck -f $scratch/backups/pagila-upstream.sql"
run B-pagila --job pagila-upstream --history history.db backups/pagila-upstream.sql

# C
gzip -6 -c "$scratch/backups/chinook.sql" > "$scratch/backups/chinook.sql.gz"
size=$(wc -c < "$scratch/backups/chinook.sql.gz")
head -c $((size * 60 / 100)) "$scratch/backups/chinook.sql.gz" > "$scratch/backups/chinook-cut.sql.gz"
note "C: chinook.sql.gz $size bytes, cut copy $(wc -c < "$scratch/backups/chinook-cut.sql.gz") bytes"
plain C-cut plaincheck "gunzip -c $scratch/backups/chinook-cut.sql.gz | psql -X -q -d plaincheck"
run C-cut --job chinook-pg --history history.db backups/chinook-cut.sql.gz

# D
my_up
my < "$DATA/Chinook_MySql.sql"
mydump --databases Chinook > "$scratch/backups/chinook-my.sql"
iconv -f UTF-8 -t ISO-8859-1//TRANSLIT "$scratch/backups/chinook-my.sql" > "$scratch/backups/chinook-my-latin1.sql"
my -e "drop database Chinook"
set +e; my < "$scratch/backups/chinook-my-latin1.sql" > "$scratch/plain.out" 2>&1; st=$?; set -e
{
  echo "\$ mariadb < backups/chinook-my-latin1.sql"
  echo "exit status: $st"
  cat "$scratch/plain.out"
  echo "tables afterwards (rows):"
  for t in $(my -N -e "select table_name from information_schema.tables where table_schema='Chinook' order by 1"); do
    echo "  Chinook.$t $(my -N -e "select count(*) from Chinook.\`$t\`")"; done
} > "$out/plain-D-latin1.txt"
run D-good   --job chinook-my --history history.db backups/chinook-my.sql
run D-latin1 --job chinook-my --history history.db backups/chinook-my-latin1.sql

(cd "$scratch" && "$R" --list --history history.db) > "$out/list.txt" 2>&1
cp "$scratch/history.db" "$out/history.db"
cat "$out/notes.txt"
