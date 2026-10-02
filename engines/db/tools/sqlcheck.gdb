# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
# Breakpoints on the entry points of AltSql Core's SQL parser and executor: a run of AltSql DB must hit none.
set pagination off
set breakpoint pending on
break altsql_exec
break as_lex
break as_parse_expr
break as_select
break as_exec
break as_insert
break as_create
run
info breakpoints
