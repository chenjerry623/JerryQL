# JerryQL

A small SQL database engine written from scratch in C++17: a lexer, a
recursive-descent parser, a rule-based planner and a pull-based ("Volcano")
executor, over a storage interface that an on-disk B+tree will implement next.

**[Try it in your browser →](https://chenjerry623.github.io/JerryQL/)** The engine is
compiled to WebAssembly and runs in the page, with no server.

This is a learning and portfolio project. It covers well-known ground (see
[References](#references)), and doesn't claim anything new.

## Quick start

```sh
cmake -S . -B build && cmake --build build -j
./build/jerryql --echo examples/demo.sql   # run the demo script
./build/jerryql                            # interactive prompt (.help, .quit)
```

Build the browser playground (needs [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)):

```sh
web/build_site.sh && python3 -m http.server -d site 8000   # open http://localhost:8000
```

Run the tests:

```sh
cd build && ctest --output-on-failure
```

## What it supports

```sql
CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, dept TEXT, salary INT);
INSERT INTO employees VALUES (1, 'Ada', 'Engineering', 185000), (2, 'Grace', 'Engineering', 172000);
SELECT name, salary / 12 AS monthly FROM employees
  WHERE dept = 'Engineering' OR salary > 190000 ORDER BY salary DESC LIMIT 5;
UPDATE employees SET salary = salary + 10000 WHERE id = 2;
DELETE FROM employees WHERE id >= 10 AND id < 20;
EXPLAIN SELECT name FROM employees WHERE id = 3;
BEGIN; UPDATE employees SET salary = 0; ROLLBACK;
DROP TABLE employees;
```

- Types: `INT` (64-bit) and `TEXT`. An optional `INT PRIMARY KEY` keys the table.
- Expressions: `+ - * /`, `= <> != < <= > >=`, `AND OR NOT`, parentheses,
  `'it''s'`-style strings. Integer overflow and division by zero are errors,
  not undefined behaviour.
- `EXPLAIN` prints the chosen plan:

```
jerryql> EXPLAIN SELECT name FROM employees WHERE id >= 2 AND id < 5;
+-----------------------------------------------+
| QUERY PLAN                                    |
+-----------------------------------------------+
| PROJECT name                                  |
|   -> FILTER (id >= 2) AND (id < 5)            |
|     -> PK RANGE SCAN employees (2 <= id <= 4) |
+-----------------------------------------------+
```

- Transactions: `BEGIN` / `COMMIT` / `ROLLBACK`. Outside a transaction,
  each write statement commits on its own.

**Not supported (yet):** NULL, joins, aggregates/GROUP BY, subqueries,
secondary indexes, concurrent connections.

Tables live in a single database file (`./build/jerryql --db app.db`), or in
memory when no file is given. Both use the same B+tree and buffer pool code.

## Design

```
SQL text -> Lexer -> Parser -> AST -> Planner -> Operator tree -> Executor
                                                     |
                                       TableStore -> B+tree (one per table)
                                                     |
                                       Pager -> Buffer pool (LRU) -> File
```

| Layer | File | Notes |
|---|---|---|
| Lexer | `src/lexer.cpp` | Bad input becomes an error token, so one bad statement doesn't abort a whole script. |
| Parser | `src/parser.cpp` | Hand-written recursive descent, one function per precedence level (`OR` < `AND` < `NOT` < comparison < `+ -` < `* /` < unary). Recovers at the next `;` after a syntax error. |
| Planner | `src/planner.cpp` | Extracts a primary-key range from top-level `AND`ed comparisons such as `id >= 10 AND id < 20`, giving a point lookup, range scan or full scan. Detects contradictions like `id > 5 AND id < 3`. The full `WHERE` is still applied by a filter, so the range only limits how much is read. |
| Executor | `src/executor.cpp` | Volcano-style operators (`Scan`, `Filter`, `Sort`, `Limit`, `Projection`), each with `next()`. Column names are resolved to indexes once at plan time, not per row. |
| Browser build | `web/` | The same engine compiled with Emscripten behind a three-function C API (`jerryql_run`, `jerryql_reset`, `jerryql_free`). CI checks its output matches the native build byte for byte. |
| Storage interface | `src/table_store.h` | Rows keyed by a 64-bit integer: the primary key, or a hidden row id. Ordered range scans. |
| B+tree | `src/storage/btree.cpp` | One tree per table, rows stored in the leaves (a clustered table, like SQLite's rowid tables). |
| Buffer pool | `src/storage/buffer_pool.cpp` | Fixed number of 4 KiB frames, pin counts, LRU eviction, dirty write-back. |
| Pager | `src/storage/pager.cpp` | Header page, page allocation, freelist; commit, rollback, checkpoint, recovery. |
| Write-ahead log | `src/storage/wal.cpp` | Checksummed full-page frames; finds the last valid commit on open. |

Choices worth explaining:

- **Writes read first, then write.** `UPDATE` and `DELETE` collect every
  matching row before changing anything. Changing rows while scanning can
  visit a row twice when its key moves forward, which is the classic
  "Halloween problem". `UPDATE t SET id = id + 1` shifts every key safely.
- **Statements are all-or-nothing for validation.** A multi-row `INSERT` or
  an `UPDATE` checks types and duplicate keys for every row before applying
  any of them.
- **`AND` / `OR` short-circuit**, so `WHERE id > 0 OR x / 0 = 1` doesn't
  raise division by zero.

## Storage design

**File layout.** The database is a file of 4 KiB pages. 4 KiB matches the
OS page size and SQLite's default page size.
- Page 0 is a header: magic bytes, page size, page count, and the head of
  the free-page list.
- Page 1 is the root of the *schema tree*, which holds one record per table:
  name, `CREATE TABLE` text and root page. It plays the role of SQLite's
  `sqlite_master`. Opening a file re-parses that SQL, so the catalog has no
  separate serialization format.

**Node layout.** Leaves are slotted pages:

```
| header (24 B) | slot array: u16 offsets, sorted by key -> |   free   | <- cells: i64 key, u16 len, row bytes |
```

- Lookups binary-search the slot array directly in the page buffer, with no
  decoding or copying.
- Writes decode the node, modify it and re-encode it. That's simpler, and
  costs a 4 KiB copy.
- Internal nodes hold `child0, (key, child)...`, which allows up to 339 keys
  per node (fanout 340).
- Leaves are linked left to right, so a range scan finds its first key and
  then walks the leaves.

**Split policy.**
- An overfull leaf splits at the byte midpoint, not the cell-count
  midpoint, so pages with uneven row sizes still split into halves with
  similar free space.
- Inserting past the last key of the rightmost leaf (append-order keys) is
  handled specially: only the new cell moves to the new page, leaving the
  old page full.

Measured in `tests/test_btree.cpp`, with 50,000 keys and 20-byte rows:

| Insert order | Leaf fill | Leaf pages |
|---|---:|---:|
| Sequential | 99.7% | 394 |
| Random | 69.0% | 571 |

69% is the textbook expectation for random inserts (about ln 2). The same
append trick appears in SQLite's balance_quick and PostgreSQL's
rightmost-page split heuristic.

**Root never moves.** The catalog stores each table's root page id. When
the root splits, its contents move to a new page and the root becomes the
parent of that page and the new sibling. The root also stores the row count
and the table's last row id.

**Limits, stated plainly:**
- Rows are capped at 1000 bytes, so at least 4 fit per leaf. There are no
  overflow pages, and a larger row is rejected before anything is written.
- Deletes don't merge or rebalance pages, so a delete-heavy table keeps
  underfull pages until `DROP TABLE` returns them to the freelist.
- Single-threaded.
- Byte order is the host's (little-endian on x86 and WebAssembly).

## Write-ahead log and crash recovery

Redo-only, full-page-image logging, close to SQLite's WAL mode.

**Commit.**
1. Every page the transaction dirtied is appended to `<db>-wal` as a
   *frame*: page id, commit marker, salt, checksum, then the 4 KiB page.
2. The header page goes last, flagged as the commit frame.
3. The log is `fsync`ed.

A transaction is committed when that `fsync` returns, and only then does
`COMMIT` (or an autocommit statement) return. The database file isn't
touched on the commit path.

**Reads** check, in order: the buffer pool, this transaction's frames,
committed frames, then the database file, using an in-memory map from page
id to newest frame.

**Big transactions.** A dirty page evicted mid-transaction is written to
the log as an *uncommitted* frame, so a transaction can be larger than the
buffer pool. `ROLLBACK` discards those frames by rewinding the log's append
position, and drops any cached copies.

**Checkpoint.** Once the log holds 1,000 committed frames, or when the
database closes:
1. Copy the newest version of each page into the database file.
2. `fsync` the database file.
3. Reset the log under a new salt.

The order matters: at every moment each committed page is durable in the log
or the file. A crash mid-checkpoint just replays the log again, which is
harmless because frames are full page images.

**Recovery** (on open):
1. Scan frames from the start of the log.
2. Each frame's 64-bit checksum is chained from the previous frame's. The
   scan stops at the first frame with a wrong salt or checksum: a torn
   write, a stale frame from a rolled-back transaction, or a frame from
   before the last reset.
3. Everything up to the last valid commit frame is replayed into the
   database file. Frames after it are discarded.

### Durability guarantee

With the default settings:

- **Atomicity:** after any crash, each transaction is either fully present or
  fully absent.
- **Durability:** a transaction whose `COMMIT` returned survives a process
  crash or power loss, *provided the storage honours `fsync`*. New files also
  `fsync` their directory.
- **Not covered:**
  - Disks or virtual disks that acknowledge `fsync` without persisting.
  - Corruption of data at rest. Database pages have no checksums; only log
    frames do.
  - Concurrent access from several processes.
- **A failed `COMMIT` is in doubt.** If `fsync` itself reports an error,
  `COMMIT` reports failure and rolls back in memory. Some of the frames may
  still have reached the disk, so the transaction may or may not appear
  after a restart.
- **`syncOnCommit = false`** skips the commit `fsync`, like SQLite's
  `synchronous=OFF`. A crash can then lose recent commits, but recovery
  still yields a consistent database.

### Crash testing

`tools/crash_test.cpp` runs a bank-transfer workload: two account updates
plus a ledger insert per transaction, some rolled back, plus churn on a table
of large rows. It uses a 32-page buffer pool and checkpoints every 200
frames, so crashes land mid-eviction and mid-checkpoint too.

After every crash it reopens the database (running recovery) and checks:
- every acknowledged commit is present;
- no acknowledged rollback is present;
- every balance equals 1,000 plus a replay of the ledger, which catches any
  half-applied transaction;
- every table's B+tree passes its structural check.

It has two modes:

- **SIGKILL:** forks a child that runs transactions against a real file and
  reports each acknowledged `COMMIT` through a pipe. The parent kills it at a
  random time between 1 and 300 ms, reopens, and verifies. 25 kills in a row
  share one database, so recovery also runs on already-recovered files.
- **Simulated power loss:** a SIGKILL can't test `fsync` ordering, because
  the kernel still writes out everything the process handed it. This mode
  wraps both files in a fake disk with a volatile write cache, in the spirit
  of [LazyFS](https://github.com/dsrhaslab/lazyfs) and
  [ALICE](https://research.cs.wisc.edu/adsl/Software/alice/). Power is cut at
  a random file operation, and each unsynced write is then kept, lost or
  torn at a 512-byte boundary.

Results, from `tools/run_crash_tests.sh` (raw output in
[`bench/results/m2-cloud-container/`](bench/results/m2-cloud-container/)):

| Mode | Crashes | Corrupted | Acknowledged commits, all present after recovery | Runs where recovery replayed log frames |
|---|---:|---:|---:|---:|
| SIGKILL, real files on ext4 | 1,000 | 0 | 192,400 | 923 |
| Simulated power loss | 10,000 | 0 | 539,715 | 9,668 |

**Checking the harness can fail.** `tools/check_harness.sh` builds three
deliberately broken engines and runs the power-loss mode against each:

| Engine variant | Corrupted runs (of 1,000) |
|---|---:|
| Correct engine | 0 |
| Commit doesn't fsync the log | 938 |
| Checkpoint doesn't fsync the database file before resetting the log | 248 |
| Recovery treats every valid frame as committed | 310 |

The second bug is the kind SIGKILL testing can never find.

## Performance

Indexed lookups versus full scans, end to end through SQL parsing, planning
and execution, on a 1,000,000-row database file (88 MB) with a 4 MiB buffer pool:

| Query | Median | p95 |
|---|---:|---:|
| Point lookup by primary key | 3.5 µs | 4.8 µs |
| Range scan by primary key, 100 rows | 26.1 µs | 32.7 µs |
| Same point lookup on an unindexed column (full scan) | 185 ms | 212 ms |

- **Machine:** a cloud VM with 4 vCPUs (Intel Xeon @ 2.10 GHz), Linux 6.18,
  GCC 13.3 at `-O3`.
- **Load:** 1,000 `INSERT` statements of 1,000 rows each, with an fsync
  after each statement, took 3.9 s (258k rows/s).
- **Cold buffer pool:** 3.9 µs median for a point lookup in a fresh process.
  That run starts with an empty buffer pool but a warm OS page cache.
- **Reproduce:** `bench/run_storage_bench.sh <name> --rows 1000000`. Raw
  per-query latencies and machine details are in
  [`bench/results/m1-cloud-container/`](bench/results/m1-cloud-container/).

A comparison against SQLite under matched durability settings is planned
(see the roadmap).

## Testing

- **Unit tests** (`tests/test_*.cpp`) for the lexer, parser, planner and engine,
  using a small dependency-free harness (`tests/check.h`).
- **Randomized B+tree test:** 40,000 random inserts, replaces, deletes, lookups
  and range scans per seed, checked against `std::map`. It uses a 16-page
  buffer pool so pages are evicted and re-read constantly, and runs a full
  structural check (ordering, separator bounds, equal leaf depth, leaf chain,
  row count) every 2,000 steps.
- **Randomized engine test:** thousands of random inserts, deletes,
  key-moving updates and range queries, checked against a `std::map` after
  every step. It prints its seed; reproduce with
  `JERRYQL_SEED=<n> ./build/jerryql_tests randomized`.
- **Golden-file tests:** each `tests/sql/*.sql` is run through the shell and
  diffed against its `.expected` output.
- **Sanitizers:** `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DJERRYQL_SANITIZE=ON`
  builds with AddressSanitizer and UndefinedBehaviorSanitizer. CI runs both builds.

## Roadmap

1. ~~SQL front end: parser, planner, executor~~ (done)
2. ~~Browser demo (WebAssembly, deployed to GitHub Pages)~~ (done)
3. ~~On-disk B+tree storage with a buffer pool~~ (done)
4. ~~Write-ahead log with crash recovery, verified by crash-injection harnesses~~ (done)
5. Benchmarks against SQLite with documented settings

## References

- [CMU 15-445/645 Database Systems](https://15445.courses.cs.cmu.edu/) (Andy Pavlo): query processing and the iterator model.
- Graefe, "Volcano: An Extensible and Parallel Query Evaluation System" (1994).
- [SQLite architecture](https://www.sqlite.org/arch.html) and [file format](https://www.sqlite.org/fileformat2.html): tables stored as B-trees keyed by row id, `sqlite_master`, freelist.
- Yao, "On Random 2-3 Trees" (1978): ~69% expected node utilization under random inserts.
- [SQLite write-ahead logging](https://www.sqlite.org/wal.html) and its [file format](https://www.sqlite.org/fileformat2.html#walformat): frames, salts, chained checksums, checkpoints.
- Pillai et al., "All File Systems Are Not Created Equal" (OSDI 2014), the ALICE paper: how applications get crash consistency wrong.
