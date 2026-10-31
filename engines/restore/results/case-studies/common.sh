# Sourced by the case-study scripts. Sets paths, starts and stops the
# "production" database servers the backups are taken from, and records each
# altsql-restore run (command, exit status, report) under the case study's
# output folder.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
work="$top/work"
R="$work/bin/altsql-restore"
DB="$work/bin/altsql-db"
DATA="$work/data"
[ -x "$R" ] || { echo "run sh $top/fetch.sh first" >&2; exit 2; }
out="$here/output"
rm -rf "$out"; mkdir -p "$out"
scratch=$(mktemp -d /tmp/altsql-cs.XXXXXX)   # /tmp: the postgres user must enter it
chmod 755 "$scratch"
export TZ=UTC LC_ALL=C.UTF-8

PGBIN=$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -1)
pg_up() {   # a production PostgreSQL server on a unix socket in $scratch/pg
  mkdir -p "$scratch/pg"; chown postgres "$scratch/pg"
  su postgres -c "$PGBIN/initdb -D $scratch/pg/data -U postgres --auth=trust -E UTF8 --locale=C.UTF-8" >/dev/null
  su postgres -c "$PGBIN/pg_ctl -s -D $scratch/pg/data -o \"-k $scratch/pg -c listen_addresses=''\" -w start -l $scratch/pg/log"
  PGHOST="$scratch/pg"; PGUSER=postgres; export PGHOST PGUSER
}
pg_down() { su postgres -c "$PGBIN/pg_ctl -s -D $scratch/pg/data -m fast -w stop" || true; }
psql_q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }

my_up() {   # a production MariaDB server on a unix socket in $scratch/my
  mkdir -p "$scratch/my/data"; chown -R mysql "$scratch/my"
  mariadb-install-db --no-defaults --user=mysql --datadir="$scratch/my/data" \
    --auth-root-authentication-method=normal >/dev/null 2>&1
  mariadbd --no-defaults --user=mysql --datadir="$scratch/my/data" \
    --socket="$scratch/my/sock" --skip-networking --pid-file="$scratch/my/pid" \
    --log-error="$scratch/my/err" &
  i=0; until mariadb --no-defaults -S "$scratch/my/sock" -uroot -e 'select 1' >/dev/null 2>&1; do
    i=$((i+1)); [ $i -lt 60 ] || { cat "$scratch/my/err" >&2; exit 2; }; sleep 0.5; done
}
my_down() { [ -f "$scratch/my/pid" ] && kill "$(cat "$scratch/my/pid")" 2>/dev/null; sleep 2; }
my() { mariadb --no-defaults -S "$scratch/my/sock" -uroot "$@"; }
mydump() { mariadb-dump --no-defaults -S "$scratch/my/sock" -uroot "$@"; }

cleanup() { pg_down >/dev/null 2>&1 || true; my_down >/dev/null 2>&1 || true; rm -rf "$scratch"; }
trap cleanup EXIT

# run NAME [altsql-restore args...]: runs altsql-restore, keeps the command,
# its output and exit status in output/NAME.txt, and the report in
# output/NAME.report.txt. It runs inside the scratch folder, so backups are
# named by relative paths (backups/...) and the sealed reports need no edits.
run() {
  name=$1; shift
  set +e
  (cd "$scratch" && "$R" --report "$out/$name.report.txt" "$@") > "$out/$name.out" 2>&1
  st=$?
  set -e
  {
    printf '$ altsql-restore'
    for a in "$@"; do case "$a" in *[!A-Za-z0-9_./:=@-]*) printf " '%s'" "$a" ;; *) printf ' %s' "$a" ;; esac; done
    echo
    echo
    sed "s#$scratch/##g; s#$work/##g" "$out/$name.out"
    echo
    echo "exit status: $st"
  } > "$out/$name.txt"
  rm -f "$out/$name.out"
  echo "== $name: exit $st"
  return 0
}
note() { echo "$*" >> "$out/notes.txt"; }
