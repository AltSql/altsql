#!/bin/sh
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Builds the AltSql DB demo for the browser.
#   CORE   folder holding altsql.h (AltSql Core, unchanged)
#   DB     folder holding altsql_db.h
#   V      asset version, part of the file names (cache busting)
# Output: build/db-demo.wasm, and app/engine-db.v$V.js with it inside.
# Needs zig, as the Python package: pip install ziglang
set -e
cd "$(dirname "$0")"
CORE=${CORE:-../../core/dist}
DB=${DB:-../../engines/db}
V=${V:-1}
mkdir -p build app
python3 -m ziglang cc -target wasm32-wasi -O2 -mexec-model=reactor -std=c99 -Wall -Wextra \
    -I"$CORE" -I"$DB" src/db_demo.c -o build/db-demo.wasm \
    -Wl,--strip-all -Wl,-z,stack-size=1048576 -Wl,--initial-memory=67108864
python3 - "$V" <<'PY'
import base64, sys
v = sys.argv[1]
wasm = open('build/db-demo.wasm', 'rb').read()
js = open('src/engine-loader.js').read().replace('__WASM_BASE64__', base64.b64encode(wasm).decode())
open('app/engine-db.v%s.js' % v, 'w').write(js)
print('wasm %d bytes, app/engine-db.v%s.js %d bytes' % (len(wasm), v, len(js)))
PY
