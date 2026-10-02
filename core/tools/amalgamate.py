#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Builds dist/altsql.h: the public header followed by the implementation.

The implementation sits outside the header guard, behind
ALTSQL_IMPLEMENTATION, so the one file that defines it gets the code and
every other file gets only the declarations.
"""
import glob
import os
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "dist", "altsql.h")

with open(os.path.join(root, "src", "altsql.h")) as f:
    header = f.read().rstrip() + "\n"

parts = [header, "",
         "/* ======================================================================",
         " * Implementation. Compiled only where ALTSQL_IMPLEMENTATION is defined.",
         " * ====================================================================== */",
         "#if defined(ALTSQL_IMPLEMENTATION) && !defined(ALTSQL_IMPL_DONE)",
         "#define ALTSQL_IMPL_DONE", ""]
for path in sorted(glob.glob(os.path.join(root, "src", "impl", "*.c"))):
    with open(path) as f:
        parts.append("/* ---- " + os.path.basename(path) + " " + "-" * (60 - len(os.path.basename(path))) + " */")
        parts.append(f.read().rstrip() + "\n")
parts.append("#endif /* ALTSQL_IMPLEMENTATION */")

os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "w") as f:
    f.write("\n".join(parts) + "\n")
print("wrote", os.path.relpath(out_path, root))
