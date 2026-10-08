# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# AltSql prototype
#   make            single header, tools and tests
#   make test       run the test suite
#   make sanitize   run the tests under AddressSanitizer and UBSan
#   make bench      one million rows on a file-backed store
#   make bench-sqlite the same rows and queries on SQLite (needs libsqlite3-dev)
#   make fuzz       coverage-guided fuzzing (needs clang with libFuzzer)
#   make mutants    plants deliberate bugs and checks that the tests catch them
#   make size       code size for a Cortex-M4 (Thumb-2) sensor build

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -pedantic -Wall -Wextra -Wshadow
DEFS     = -D_POSIX_C_SOURCE=200809L
ifeq ($(shell uname -s),Darwin)
DEFS    += -D_DARWIN_C_SOURCE
endif
INC      = -Idist -Itests
SAN      = -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -fno-sanitize-recover=all

HEADER   = dist/altsql.h
TESTS    = build/test_basic build/test_powercut build/test_sync build/test_fuzz

all: $(HEADER) build/altsql build/demo $(TESTS)

$(HEADER): src/altsql.h src/impl/*.c tools/amalgamate.py
	python3 tools/amalgamate.py

build:
	mkdir -p build

build/%: tests/%.c tests/testutil.h $(HEADER) | build
	$(CC) $(WARN) $(DEFS) $(CFLAGS) $(INC) -o $@ $<

build/altsql: tools/cli.c $(HEADER) | build
	$(CC) $(WARN) $(DEFS) $(CFLAGS) $(INC) -o $@ $<

build/demo: tools/demo.c $(HEADER) | build
	$(CC) $(WARN) $(DEFS) $(CFLAGS) $(INC) -o $@ $<

build/bench: tests/bench.c $(HEADER) | build
	$(CC) $(WARN) $(DEFS) -O2 $(INC) -o $@ $<

test: $(TESTS)
	./build/test_basic
	./build/test_powercut
	./build/test_sync
	./build/test_fuzz

sanitize: $(HEADER) | build
	for t in test_basic test_powercut test_sync test_fuzz; do \
	  $(CC) $(WARN) $(DEFS) $(SAN) $(INC) -o build/$$t.san tests/$$t.c && ./build/$$t.san quick || exit 1; \
	done

bench: build/bench
	./build/bench

build/bench_sqlite: tests/bench_sqlite.c | build
	$(CC) -std=c99 -O2 $(DEFS) -o $@ $< -lsqlite3

bench-sqlite: build/bench build/bench_sqlite
	./build/bench
	./build/bench_sqlite

fuzz: $(HEADER)
	sh tools/fuzz.sh 60

mutants: $(HEADER)
	python3 tools/mutants.py

size: $(HEADER)
	sh tools/size.sh

clean:
	rm -rf build dist/altsql.h

.PHONY: all test sanitize bench bench-sqlite fuzz mutants size clean
