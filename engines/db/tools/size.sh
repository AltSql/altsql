#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Code size of AltSql DB on this computer (x86-64), beside AltSql Core and SQLite.
#   sh tools/size.sh <dir with sqlite3.c>
# AltSql DB's size is the text of Core + AltSql DB minus the text of Core alone,
# both built for a gateway (every Core switch on), -O2 and -Os.
set -e
cd "$(dirname "$0")/.."
SQ=${1:-../../../third/sqlite}
mkdir -p build/size
cat > build/size/core.c <<'EOF'
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
EOF
cat > build/size/db.c <<'EOF'
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
EOF
cat > build/size/dbports.c <<'EOF'
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
#define ALTSQL_DB_PORT_FILE
#define ALTSQL_DB_PORT_RAM
#define ALTSQL_DB_IMPLEMENTATION
#include "altsql_db.h"
EOF
SQLOPT="-DSQLITE_DQS=0 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_AUTOINIT -DSQLITE_STRICT_SUBTYPE=1"
for opt in -O2 -Os; do
  for f in core db dbports; do
    cc -std=c99 -D_POSIX_C_SOURCE=200809L $opt -I../../core/dist -I. -c build/size/$f.c -o build/size/$f$opt.o
  done
  cc $opt $SQLOPT -c "$SQ/sqlite3.c" -o build/size/sqlite3$opt.o
  core=$(size build/size/core$opt.o | awk 'NR==2 {print $1}')
  db=$(size build/size/db$opt.o | awk 'NR==2 {print $1}')
  dbp=$(size build/size/dbports$opt.o | awk 'NR==2 {print $1}')
  sq=$(size build/size/sqlite3$opt.o | awk 'NR==2 {print $1}')
  echo "$opt  AltSql Core (gateway build): $core bytes of code"
  echo "$opt  AltSql DB, the engine:       $((db - core)) bytes"
  echo "$opt  AltSql DB with both ports:   $((dbp - core)) bytes"
  echo "$opt  SQLite $(grep -m1 'define SQLITE_VERSION ' "$SQ/sqlite3.h" | awk '{print $3}' | tr -d '"'), recommended options: $sq bytes"
done
