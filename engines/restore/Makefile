# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# AltSql Restore, the backup restore verifier
#   make             build/altsql-restore and the unit tests
#   make test        unit tests, then the end-to-end tests (tests/test_restore.sh)
#   make sanitize    the same under AddressSanitizer and UBSan
#   make strict      every warning an error, gcc and clang

CC      ?= cc
CORE     = ../../core
DB       = ../db
WARN     = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wno-format-truncation
DEFS     = -D_POSIX_C_SOURCE=200809L
ifeq ($(shell uname -s),Darwin)
DEFS    += -D_DARWIN_C_SOURCE
endif
INC      = -I$(CORE)/dist -I$(DB) -I.
SRC      = altsql_restore.c src/*.c $(DB)/altsql_db.h $(CORE)/dist/altsql.h
SAN      = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: build/altsql-restore build/test_units

build:
	mkdir -p build

$(CORE)/dist/altsql.h:
	$(MAKE) -C $(CORE) dist/altsql.h

build/altsql-restore: $(SRC) | build
	$(CC) $(WARN) $(DEFS) -O2 $(INC) -o $@ altsql_restore.c -lm

build/test_units: tests/test_units.c $(SRC) | build
	$(CC) $(WARN) $(DEFS) -O2 -g $(INC) -o $@ tests/test_units.c -lm

test: build/altsql-restore build/test_units
	./build/test_units
	sh tests/test_restore.sh

sanitize: | build
	$(CC) $(WARN) $(DEFS) $(SAN) $(INC) -o build/altsql-restore.san altsql_restore.c -lm
	$(CC) $(WARN) $(DEFS) $(SAN) $(INC) -o build/test_units.san tests/test_units.c -lm
	./build/test_units.san
	ALTSQL_RESTORE=$$PWD/build/altsql-restore.san sh tests/test_restore.sh

strict: | build
	for c in gcc clang; do \
	  $$c $(WARN) -Werror $(DEFS) -O2 $(INC) -o build/strict_$$c altsql_restore.c -lm || exit 1; \
	done

clean:
	rm -rf build

.PHONY: all test sanitize strict clean
