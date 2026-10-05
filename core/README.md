# AltSql (altsql.com)

The tiny database that runs from sensor to gateway.

Key-value and time-series storage on the microcontroller, SQL on the gateway, one storage
format on both. The device keeps working and deciding when the network is gone, sends less
(a summary instead of every reading, if you like), and its records reach the gateway byte
for byte, with no translation step in between.

**Status: prototype 0.1.** Written to prove the design and measure it. The file format may
still change. Not for production. Open source under the Apache License 2.0.

## What is in the box

| Path | What it is |
|---|---|
| `dist/altsql.h` | The whole engine in one C99 file. Copy it into your project. |
| `src/` | The same code in parts; `make` rebuilds `dist/altsql.h` from it. |
| `tools/cli.c` | `altsql`, a shell for databases kept in a file. |
| `tools/demo.c` | One engine set up for three industries (machine monitoring, cold chain, agriculture), simulated. |
| `tools/*.sh`, `tools/*.py` | Code size, stack depth, fuzzing, planted-bug check, checksum experiments. |
| `tests/` | Functional, power-cut, sync and fuzz tests; libFuzzer targets in `tests/fuzz/`; benchmarks, including one against SQLite. |
| `results/alpha/` | The logs behind the measured numbers. |
| `examples/esp32_sensor/` | ESP-IDF example. **Not yet tested on hardware.** |

## Use it

In exactly one C file:

```c
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"
```

Everywhere else just `#include "altsql.h"`. A sensor build switches off what it does not need:

```c
#define ALTSQL_ENABLE_SQL  0   /* no SQL on the sensor */
#define ALTSQL_ENABLE_TEXT 0   /* no text export */
```

AltSql never calls `malloc`. You give it a block of memory and a flash driver
(read, write, erase; optionally a memory map):

```c
static uint8_t mem[2048];
altsql_config cfg = { .mem = mem, .mem_size = sizeof mem, .create = 1 };
altsql *db;
altsql_open(&db, &my_flash, &cfg);

altsql_ts_create(db, "raw", "time:time,temp:float");
altsql_append(db, "raw", (int64_t)now, 21.53);

/* decide on the device: above 60 C for the last 10 seconds? */
altsql_stats s;
altsql_ts_window(db, "raw", "temp", now - 9, &s);
if (s.count == 10 && s.min > 60.0) { fan_on(); altsql_put(db, "alarm", "overheat", 8); }
```

On the gateway, a copy opened with `cfg.replica = 1` takes the sensor's records as they are
and answers SQL:

```c
/* sensor */   altsql_sync_read(dev, confirmed, buf, sizeof buf, &len, &last, NULL, NULL);
/* gateway */  altsql_sync_apply(gw, buf, len, &confirmed);
               altsql_exec(gw, "SELECT time / 3600 AS hour, AVG(temp), MAX(temp) "
                               "FROM raw GROUP BY hour", print_row, NULL);
```

A filter on `altsql_sync_read` sends only what you choose, for example per-minute summaries.

The shell:

```
$ make
$ ./build/altsql new plant.db
$ ./build/altsql plant.db
altsql> CREATE TABLE readings (time TIME, machine INT, temp FLOAT);
altsql> INSERT INTO readings VALUES (1767225600, 1, 21.53), (1767225660, 2, 64.1);
altsql> SELECT machine, AVG(temp), MAX(temp) FROM readings GROUP BY machine;
altsql> .export
altsql> .sync 0 batch.bin
```

`.sync AFTER FILE` writes the records after position AFTER to a file, as the
device would send them to its gateway; `altsql-db` (in `engines/db`) applies
it on the gateway.

## Measured

All numbers come from the code in this repository; `results/alpha/` holds the logs behind
them. The desktop numbers
were taken on a 2-core Intel Xeon 2.1 GHz cloud machine.

**Code size**, Cortex-M4 (Thumb-2), clang -Os, code plus constants, excluding the C library
and the compiler's floating-point helpers (`make size`; 1 KB = 1,000 bytes here):

| Build | Size |
|---|---|
| Key-value only | 7.2 KB |
| Key-value + time-series | 13.4 KB |
| Key-value + time-series + sync (typical sensor) | 15.0 KB |
| Everything, including SQL and text export (gateway) | 38.1 KB |

**The same code on other chip families** (same rules):

| Chip family | Key-value | Sensor (key-value, time-series, sync) | Gateway (everything) |
|---|---|---|---|
| Cortex-M0+ (ARMv6-M) | 6.9 KB | 14.5 KB | 37.8 KB |
| Cortex-M4 (ARMv7E-M) | 7.2 KB | 15.0 KB | 38.1 KB |
| Cortex-M33 (ARMv8-M) | 7.2 KB | 15.0 KB | 38.1 KB |
| RISC-V RV32IMC | 8.1 KB | 17.1 KB | 44.6 KB |
| x86-64 | 8.8 KB | 18.6 KB | 48.9 KB |

Every one of these builds compiles with `-Wall -Wextra -Wshadow -pedantic -Werror`. The ESP32
and ESP32-S3 use Xtensa cores, which need Espressif's compiler; not measured yet. Nothing has
run on a microcontroller yet: these are compile-only measurements.

**RAM** on a 32-bit MCU: 232 bytes, plus 16 per flash sector, 8 per key slot, 44 per series
and one record buffer. A sensor with 64 KB of flash (16 sectors), 32 keys, 4 series and
128-byte records needs 1,048 bytes. Stack on top (`sh tools/stack.sh`, Cortex-M4): at most
1,376 bytes in a sensor build; about 4.3 KB for ordinary SQL on a gateway.

**Power loss.** `make test` cuts the power 10,000 times at random bytes, including in the
middle of reclaiming a sector and in the middle of erasing one, and checks after every
reboot that every acknowledged write is intact, that the write in flight is either complete
or absent, and that time-series rows form one unbroken run. A longer run of 400,000 cuts
(`./build/test_powercut 400000 0x5eed2026`) found no failures.

**Other tests.** 104,942 functional checks across six flash geometries, write alignments from
1 to 16 bytes, with and without memory-mapped reads; a sync test over lossy links with an
outage and 51-byte messages; 300,000 fuzzed SQL statements, 300,000 fuzzed sync batches and
75,000 fuzzed imports (`./build/test_fuzz 300000`). Clean under AddressSanitizer, UBSan and
valgrind. Coverage-guided fuzzing with libFuzzer (`sh tools/fuzz.sh`) on SQL, sync, import
and whole flash images: 27.5 million inputs so far; it found four defects, all fixed, and
inputs that trigger each of them are kept in `tests/fuzz/regressions/`. To check the checkers,
`python3 tools/mutants.py` plants 16 bugs one at a time; the tests catch all 16.

**Gateway speed**, one million rows (`time, machine, temp`) in a file-backed store:

| Operation | Result |
|---|---|
| Append 1,000,000 rows | 0.18 s; 32.3 bytes per row on flash |
| `SELECT time/3600 AS hour, COUNT(*), AVG(temp), MIN(temp), MAX(temp) ... GROUP BY hour` | 203 ms |
| `SELECT ... ORDER BY temp DESC LIMIT 5` | 155 ms |
| `SELECT ... WHERE time >= (last hour) AND machine = 3` | 0.6 ms (old sectors skipped) |
| Reopen (scan the log, rebuild the index) | 90 ms |

Against SQLite 3.45 on the same machine and the same million rows (`make bench-sqlite`; each
query time is the best of nine: three runs, each timing every query three times):

| Query | AltSql | SQLite |
|---|---|---|
| Hourly averages (`GROUP BY hour`) | 203 ms | 384 ms |
| Average per machine | 166 ms | 259 ms |
| Count with a filter on `temp` | 151 ms | 38 ms |
| Top 5 by `temp` | 155 ms | 60 ms |
| Last hour of one machine | 0.6 ms | 32 ms (0.2 ms with an index) |
| Storage per row | 32.3 bytes | 22.9 bytes |

Most of AltSql's scan time goes to checking every record's checksum on every read
(`sh tools/crc_experiment.sh`). With the check switched off as an experiment, the same
queries ran 2.4 to 3.5 times faster. A checksum computed from a 1 KB table makes them 1.4 to
1.6 times faster and is on the list.

**The demo** runs the same engine set up for three devices, one per industry. Only the
set-up differs: flash layout, memory, data layout, the rule the device applies by itself,
and what it sends over which link.

| | Machine monitoring | Cold chain | Agriculture |
|---|---|---|---|
| Device | machine sensor | truck temperature logger | soil-moisture sensor |
| Flash on the device | 64 KB | 512 KB (64 KB blocks) | 32 KB |
| Engine RAM, 32-bit chip | 1,048 bytes | 920 bytes | 672 bytes |
| Readings | 1 a second for 1 hour | 1 every 30 s for 48 hours | 1 every 15 min for 30 days |
| Decided on the device | overheat: alarm and fan | temperature excursion record | irrigation valve |
| Link | Wi-Fi, 15-minute outage | mobile, at depots only | radio, one 51-byte message a day |
| Sent over the constrained link | 2,533 bytes | 6,810 bytes | 1,140 bytes |
| Every reading as AltSql records | 96,133 bytes | 179,610 bytes | 86,400 bytes (computed) |
| Every reading as JSON | 190,800 bytes | 328,374 bytes | 207,360 bytes |
| Flash written / sector erases | 104,204 bytes / 25 | 191,392 bytes / 2 | 94,820 bytes / 23 |

For the machine sensor, the constrained link is gateway B's, which takes minute summaries
and the fan state; gateway A receives every record. Each device made its decisions while its
link was down, and nothing it was set up to deliver was lost.

## How it works

**Storage.** Flash is a ring of sectors holding a log of records. Each record has a 12-byte
header with a checksum and a sequence number; nothing is ever rewritten in place. When
space runs low, the oldest sector is reclaimed: live key-value records are copied forward,
time-series rows are let go (oldest first), and the sector is retired and erased when next
used. Every sector takes its turn, which spreads wear evenly.

**Power loss.** A record counts only once its checksum is complete. After a cut, the scan
steps over damaged bytes and new writes start after the last byte ever programmed. A
reclaim cut short is simply redone; a sector it had opened is dropped, so repeated cuts
cannot eat the spare space.

**Key-value.** A small hash index in RAM points at each key's newest record. If there are
more keys than slots, lookups fall back to scanning the log: slower, still correct. Listing
all keys, the `kv` table and export need an index that is large enough.

**Time-series.** A series has a schema (`time:time,temp:float,...`). Rows are packed
little-endian. Each sector remembers its newest timestamp, so time-range queries skip
old sectors. Float columns read back as the decimal you stored (21.53, not 21.5300006...).

**SQL** (gateway builds): `CREATE TABLE`, `INSERT`, `SELECT` with `WHERE`, `GROUP BY`,
`HAVING`, `ORDER BY`, `LIMIT/OFFSET`, `COUNT/SUM/AVG/MIN/MAX`, `ABS/ROUND/LENGTH/LOWER/UPPER`,
`LIKE`, `BETWEEN`, `IS NULL`. Each series is a table; the key-value store is the table `kv`.

**Sync.** The sensor hands out its records exactly as stored; the gateway checks each one
and appends it unchanged. Sequence numbers make resending safe. Deletes the gateway has not
confirmed are kept until it has, so a gateway that was offline still learns about them. A
filter picks what travels, down to leaving out series definitions on a link too small to
carry them, once the gateway has been given them at setup.

## Limits of this prototype

- One thread, one writer. No transactions spanning several records.
- SQL has no joins, subqueries, `UPDATE` or `DELETE` statements (keys are deleted through
  the API; rows leave by rollover), and no indexes other than skipping by time.
- Rollover is shared by all series in a database: a long outage drops the oldest rows of
  every series together.
- A replica mirrors one device and cannot forward to another replica yet.
- With sync compiled in, a deleted key keeps a small marker until the gateway confirms it.
  The device remembers one confirmed position, the highest any gateway has reached, so with
  several gateways one that lags behind can miss a delete.
- A multi-row `INSERT` is written row by row: if a later row fails, the earlier ones stay.
- SQL expressions are limited to 200 levels of nesting and 10,000 terms once aliases are
  expanded, so hostile queries get an error instead of exhausting the stack or the CPU.
- Sync data is checked for damage, not authenticated or encrypted.
- The file-backed store (`ALTSQL_PORT_FILE`) is for gateways and tools; its power-loss
  behavior depends on the operating system, unlike real NOR flash.
- The ESP32 example compiles against stub headers only; it has not been built with ESP-IDF
  or run on a board.

## Build and test

```
make                      # dist/altsql.h, the shell, the demo, the tests
make test                 # functional, power-cut, sync and fuzz tests
make sanitize             # the same under AddressSanitizer and UBSan
make bench                # one million rows
make bench-sqlite         # the same rows and queries on SQLite (needs libsqlite3-dev)
make size                 # code size for five chip families (needs clang and llvm-size)
make fuzz                 # coverage-guided fuzzing, 60 s per target (clang with libFuzzer)
make mutants              # plant 16 bugs one at a time; the tests must catch each
sh tools/stack.sh         # stack depth of every call, Cortex-M4
sh tools/crc_experiment.sh  # the two checksum experiments
./build/demo              # or: ./build/demo machine | coldchain | farm
```

Needs a C99 compiler and Python 3 (to assemble the single header). The shell and the
file-backed store need POSIX.

## On-flash format (version 1)

Sector header, 32 bytes: magic `ASQL`, version, write alignment, sequence number, sector size,
sector count, the sequence number of the sector being reclaimed when this one was opened,
CRC-32. Record: marker `0xA5`, type (put, delete, row), payload length, sequence number,
CRC-32 of header and payload, then the payload, padded to the write alignment.

Copyright 2026 AltSql.com. Licensed under the Apache License 2.0; see LICENSE and NOTICE at the top of the repository.
