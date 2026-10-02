# Logs behind the Alpha report (27 September 2026)

Machine: cloud VM, 2 cores of an Intel Xeon at 2.1 GHz, Ubuntu 24.04, gcc 13.3, clang 18.1.3,
SQLite 3.45.1. Timings depend on the machine; the other figures repeat exactly.

| Log | Command |
|---|---|
| test_final.log | `make test` (timed) |
| sanitize_final.log | `make sanitize` |
| powercut_400k_final.log | `./build/test_powercut 400000 0x5eed2026` |
| fuzz_300k_final.log | `./build/test_fuzz 300000` |
| powercut_san_50k_final.log, fuzz_san_50k_final.log | the sanitizer builds with 50,000 cuts and 50,000 rounds |
| valgrind_*.log | the four suites under valgrind (quick settings for power cuts and fuzzing) |
| size.log | `make size` |
| stack_full.log | `sh tools/stack.sh` |
| demo.log | `./build/demo` (timed) |
| bench_altsql_[123].log, bench_sqlite_[123].log | `make bench-sqlite`, three runs |
| bench_crc_experiments.log | the checksum experiments (`sh tools/crc_experiment.sh`) |
| mutants_first.log, mutants.log | `python3 tools/mutants.py`, first run (14 of 16) and after two tests were added (16 of 16) |
| fuzz_campaign.log, fuzz_sql2.log, fuzz_sql3.log, fuzz_final.log, fuzz_runs_terminal.txt | coverage-guided fuzzing runs, in order |
| build_gcc.log, build_clang.log, build_time.log | `make -B all` with gcc and with clang, no warnings |
