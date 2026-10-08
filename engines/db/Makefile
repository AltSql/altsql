# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# AltSql DB prototype
#   make            the tests
#   make test       unit and model tests, tables, sync from Core devices, SQL, the interface test, statement savepoints, secondary indexes, crash matrix, fault injection
#   make sanitize   the same under AddressSanitizer and UBSan (quick settings)
#   make valgrind   unit, model, table, sync and SQL tests under Valgrind (quick settings)
#   make strict     every warning an error, gcc and clang
#   make vs-sqlite  random SQL on AltSql DB and SQLite, answers compared (fetches SQLite first)
#   make fuzz       coverage-guided fuzzing for 45 minutes (clang with libFuzzer)
#   make mutants    plants deliberate bugs and checks that the tests catch them
#   make gate1      Gate 1 against SQLite and LMDB (fetches them first; idle machine)
#   make size       code size beside AltSql Core and SQLite

CC      ?= cc
CORE     = ../../core
WARN     = -std=c99 -pedantic -Wall -Wextra -Wshadow
DEFS     = -D_POSIX_C_SOURCE=200809L
ifeq ($(shell uname -s),Darwin)
DEFS    += -D_DARWIN_C_SOURCE
endif
INC      = -I$(CORE)/dist -I. -Itests
SAN      = -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -fno-sanitize-recover=all
TESTS    = build/test_db build/test_rows build/test_sync build/test_sql build/test_same build/test_savepoint build/test_index build/test_crash build/test_fault
DEPS     = altsql_db.h tests/dbtest.h $(CORE)/dist/altsql.h

all: $(TESTS)

build:
	mkdir -p build

$(CORE)/dist/altsql.h:
	$(MAKE) -C $(CORE) dist/altsql.h

build/%: tests/%.c $(DEPS) | build
	$(CC) $(WARN) $(DEFS) -O2 -g $(INC) -o $@ $< -lm

test: $(TESTS)
	./build/test_db
	./build/test_rows
	./build/test_sync
	./build/test_sql
	./build/test_same
	./build/test_savepoint
	./build/test_index
	./build/test_crash
	./build/test_fault

sanitize: | build
	for t in test_db test_rows test_sync test_sql test_same test_savepoint test_index test_crash test_fault; do \
	  $(CC) $(WARN) $(DEFS) $(SAN) $(INC) -o build/$$t.san tests/$$t.c -lm && ./build/$$t.san quick || exit 1; \
	done

valgrind: | build
	for t in test_db test_rows test_sync test_sql test_same test_savepoint test_index; do \
	  $(CC) $(WARN) $(DEFS) -O1 -g $(INC) -o build/$$t.vg tests/$$t.c -lm && \
	  valgrind --error-exitcode=9 --leak-check=full ./build/$$t.vg quick || exit 1; \
	done

strict: | build
	for c in gcc clang; do \
	  for t in test_db test_rows test_sync test_sql test_same test_savepoint test_index; do \
	    $$c $(WARN) -Werror $(DEFS) -O2 $(INC) -o build/strict_$${c}_$$t tests/$$t.c -lm || exit 1; \
	  done; \
	  $$c $(WARN) -Werror $(DEFS) -O2 $(INC) -o build/strict_$${c}_shell tools/shell.c -lm || exit 1; \
	done

clean:
	rm -rf build

THIRD    = ../../../third

vs-sqlite: | build
	test -f build/sqlite3.o || sh tools/fetch_third_party.sh
	$(CC) $(WARN) $(DEFS) -O2 -g $(INC) -I$(THIRD)/sqlite -o build/test_vs_sqlite tests/test_vs_sqlite.c build/sqlite3.o -lm -lpthread
	./build/test_vs_sqlite

fuzz: | build
	python3 tools/fuzz_seeds.py build/fuzzseeds build/db.dict $(CORE)/tests/fuzz/sql.dict
	clang $(DEFS) -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(INC) -o build/fuzz_db tests/fuzz_db.c -lm
	mkdir -p build/corpus
	./build/fuzz_db tests/fuzz_regressions/*
	./build/fuzz_db -max_total_time=2700 -dict=build/db.dict -rss_limit_mb=2048 -print_final_stats=1 build/corpus build/fuzzseeds

mutants:
	python3 tools/mutants.py

gate1:
	sh tools/fetch_third_party.sh
	taskset -c 1 python3 tools/gate1_report.py build/gate1-files 1000000

size:
	sh tools/size.sh ../../../third/sqlite

.PHONY: all test sanitize valgrind strict vs-sqlite fuzz mutants gate1 size clean

# The shell, altsql-db, and its end-to-end test
#   make shell       build/altsql-db
#   make test-shell  build both shells, then run tests/test_shell.sh against tests/test_shell.expected
build/altsql-db: tools/shell.c altsql_db.h $(CORE)/dist/altsql.h | build
	$(CC) $(WARN) $(DEFS) -O2 $(INC) -o $@ $< -lm

shell: build/altsql-db

test-shell: build/altsql-db
	$(MAKE) -C $(CORE) build/altsql
	sh tests/test_shell.sh

.PHONY: shell test-shell
