#!/bin/bash
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
#
# Runs a command in CI and, if it fails, turns the end of its output and every
# line that reports a failure into one error annotation, so the failure can be
# read from the check run without the full log.
#
#   bash .github/ci-run.sh make -C engines/db test
set -uo pipefail
log=$(mktemp)
"$@" 2>&1 | tee "$log"
code=${PIPESTATUS[0]}
if [ "$code" -ne 0 ]; then
  {
    echo "$* exited with $code"
    grep -iE -- 'fail|error|assert|mismatch|differ|abort|signal|core dumped|\.c:[0-9]+' "$log" | head -n 60
    echo "... last lines:"
    tail -n 40 "$log"
  } > "$log.msg"
  # An annotation is one line; %0A is a line break inside it.
  msg=$(sed -e 's/%/%25/g' -e 's/\r//g' "$log.msg" | awk 'BEGIN{ORS="%0A"} {print}')
  echo "::error title=$1 failed::$msg"
fi
exit "$code"
