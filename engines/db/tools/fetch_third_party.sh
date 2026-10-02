#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Fetches what Gate 1 compares against, into ../../../third (beside the altsql folder):
#   SQLite 3.53.4 amalgamation, as shipped in the npm package better-sqlite3 13.0.3
#   LMDB 0.9.35, from the PyPI source package lmdb 2.3.0
# Then builds them with the options Gate 1 uses.
set -e
cd "$(dirname "$0")/.."
T=../../../third
mkdir -p "$T/sqlite" "$T/lmdb" build
( cd "$T" && npm pack better-sqlite3@13.0.3 >/dev/null && tar xzf better-sqlite3-13.0.3.tgz package/deps/sqlite3 \
  && cp package/deps/sqlite3/sqlite3.c package/deps/sqlite3/sqlite3.h sqlite/ )
( cd "$T" && pip download lmdb==2.3.0 --no-binary :all: --no-deps -d lmdbsrc >/dev/null && tar xzf lmdbsrc/lmdb-2.3.0.tar.gz \
  && cp lmdb-2.3.0/lib/mdb.c lmdb-2.3.0/lib/midl.c lmdb-2.3.0/lib/lmdb.h lmdb-2.3.0/lib/midl.h lmdb/ )
SQLOPT="-DSQLITE_DQS=0 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_AUTOINIT -DSQLITE_STRICT_SUBTYPE=1"
cc -O2 $SQLOPT -c "$T/sqlite/sqlite3.c" -o build/sqlite3.o
cc -O2 -w -c "$T/lmdb/mdb.c" -o build/mdb.o -I"$T/lmdb"
cc -O2 -w -c "$T/lmdb/midl.c" -o build/midl.o -I"$T/lmdb"
cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -O2 -g -I../../core/dist -I. -I"$T/sqlite" -I"$T/lmdb" \
   -o build/gate1 tools/gate1.c build/sqlite3.o build/mdb.o build/midl.o -lpthread -lm
echo "built build/gate1; run: python3 tools/gate1_report.py <scratch dir> 1000000 (idle machine)"
