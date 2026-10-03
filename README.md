# JerryQL

A small SQL database engine written from scratch in C++17: a lexer, a
recursive-descent parser, a rule-based planner and a pull-based ("Volcano")
executor, over a storage interface that an on-disk B+tree will implement next.

**[Try it in your browser →](https://chenjerry623.github.io/JerryQL/)** The engine is
compiled to WebAssembly and runs in the page, with no server. You can:
- load 100,000 or 1,000,000 rows and watch indexed lookups beat full scans, with live timings
- browse every table page by page
- import your own CSV file
- run any SQL in the console

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
SELECT dept, COUNT(*) AS people, SUM(salary) AS payroll FROM employees
  GROUP BY dept HAVING COUNT(*) > 1 ORDER BY payroll DESC;
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

- Inner joins: `FROM a [AS] x JOIN b y ON ... [JOIN ...]`, with qualified
  columns (`x.id`). The planner chooses an index nested-loop join, a hash
  join or a nested-loop join (see below).
- Secondary indexes: `CREATE INDEX name ON table (column)`, `DROP INDEX`.
  The planner uses them for `=`, `<`, `<=`, `>`, `>=` and ranges, and
  `EXPLAIN` shows which one it picked.
- Aggregates: `COUNT(*)`, `COUNT`, `SUM`, `MIN`, `MAX`, `AVG` (integer
  result), with `GROUP BY` on any expressions, `HAVING`, and `ORDER BY` on
  aggregates or aliases. `LIMIT n OFFSET m`.

**Not supported (yet):** NULL, outer joins, subqueries,
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
| Executor | `src/executor.cpp` | Volcano-style operators (`Scan`, `IndexScan`, `Filter`, three joins, `Aggregate`, `Sort`, `Limit`, `Projection`), each with `next()`. Column names are resolved to indexes once at plan time, not per row. |
| Browser build | `web/` | The same engine compiled with Emscripten behind a three-function C API (`jerryql_run`, `jerryql_reset`, `jerryql_free`). CI checks its output matches the native build byte for byte. |
| Storage interface | `src/table_store.h` | Rows keyed by a 64-bit integer: the primary key, or a hidden row id. Ordered range scans. |
| B+tree | `src/storage/btree.cpp` | Byte-string keys. One tree per table, rows stored in the leaves (a clustered table, like SQLite's rowid tables), plus one per secondary index. |
| Index keys | `src/storage/index_key.cpp` | Order-preserving, prefix-free value encoding for composite index keys. |
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
- **Aggregation rewrites the query.** The planner turns every `GROUP BY`
  key and aggregate in `SELECT`, `HAVING` and `ORDER BY` into a reference
  to a column of the aggregate operator's output row. `SUM(x)` used twice is
  computed once. A bare column that is neither a key nor inside an aggregate
  is rejected, as in standard SQL. Groups are kept in an ordered map, so
  output comes out sorted by key. Without NULL, `MIN`/`MAX`/`AVG` over zero
  rows is an error rather than NULL.
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

**Keys.** B+tree keys are byte strings compared with `memcmp`. Table keys
are 64-bit integers encoded big-endian with the sign bit flipped, so byte
order equals numeric order. Secondary indexes use composite keys (below).

**Node layout.** Both leaves and internal nodes are slotted pages:

```
leaf:     | header (24 B) | slots: u16 offsets -> |  free  | <- cells: u16 key len, u16 row len, key, row |
internal: | header | child0 | slots -> |  free  | <- cells: u32 child, u16 key len, key |
```

- Lookups binary-search the slot array directly in the page buffer, with no
  decoding or copying.
- Writes decode the node, modify it and re-encode it. That's simpler, and
  costs a 4 KiB copy.
- With 8-byte table keys an internal node holds about 250 separators. Keys
  can be up to 256 bytes.
- Leaves are linked left to right, so a range scan finds its first key and
  then walks the leaves.

**Split policy.**
- An overfull leaf, or internal node, splits at the byte midpoint, not the cell-count
  midpoint, so pages with uneven row sizes still split into halves with
  similar free space.
- Inserting past the last key of the rightmost leaf (append-order keys) is
  handled specially: only the new cell moves to the new page, leaving the
  old page full.

Measured in `tests/test_btree.cpp`, with 50,000 keys and 20-byte rows:

| Insert order | Leaf fill | Leaf pages |
|---|---:|---:|
| Sequential | 99.2% | 421 |
| Random | 69.6% | 601 |

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

## Secondary indexes

**Entries.** An index is another B+tree in the same file. Its keys are
`encode(column value) + encode(primary key)`, with no payload. Equal values
sort together, ordered by primary key, and every entry is unique even when
values repeat. The value encoding preserves order under `memcmp` and is
prefix-free:
- `INT`: a type byte, then 8 bytes, big-endian with the sign bit flipped.
- `TEXT`: a type byte, then the bytes with NUL escaped as `00 FF`, then
  `00 00`.

So `'ab' < 'ab\0' < 'abc'`, and no value's encoding is a prefix of another's.

**Range scans become key ranges.** `amount >= 100 AND amount < 300`
becomes the byte range `[enc(100), enc(300))`. An index scan walks that
range and fetches each row from the table by its primary key.

**Plan choice**, in order of preference:
1. A primary-key lookup or range, since the table is clustered on its key.
2. An index with an equality condition.
3. An index with a range condition.
4. A full scan.

The whole `WHERE` clause is still applied as a filter afterwards.
Contradictory ranges (`x > 5 AND x < 3`) produce an empty scan.

**Maintenance.**
- `INSERT`, `UPDATE` and `DELETE` update every index in the same write-ahead
  log transaction as the table, so a crash or `ROLLBACK` can never leave an
  index out of step.
- An `UPDATE` only touches the indexes whose column actually changed.
- `CREATE INDEX` checks every existing value before writing anything, then
  fills the index from a table scan.

**Verification.**
- `.check` (`Database::checkIntegrity`) checks every B+tree's structure,
  and that each index holds exactly one entry per row, carrying that row's
  current value.
- A differential test runs the same random inserts, updates (including
  primary-key moves), deletes and range queries against an indexed and an
  unindexed database, and requires identical results.
- The crash harness's ledger table has two indexes, and recovery has to
  bring them back exactly consistent.

**Limits:**
- One column per index.
- Index keys are capped at 256 bytes, so very long `TEXT` values can't be
  indexed. A row with such a value is rejected before any write.
- No index-only scans; every match fetches its row.
- No `ORDER BY` via index order.

## Joins

Joins are planned left-deep in `FROM` order. For each `JOIN`, the planner
looks in `ON` for an equality between a column of the new (right) table and
an expression over the tables already joined. It then picks the first of
these that applies:

| Algorithm | When | Cost |
|---|---|---|
| Index nested loop, by primary key | the right column is its table's primary key | one B+tree lookup per left row |
| Index nested loop, by secondary index | the right column has an index | one index range scan plus row lookups per left row |
| Hash join | any other equality | builds a hash table of the right table once, then probes it per left row |
| Nested loop | no usable equality (e.g. `a.x < b.y`) | reads the right table once, then compares every pair |

```
jerryql> EXPLAIN SELECT u.name, c.country, o.amount FROM orders o
           JOIN users u ON u.id = o.user_id JOIN cities c ON c.name = u.city
           WHERE o.status = 'paid' AND c.country <> 'USA';
| PROJECT u.name, c.country, o.amount                                    |
|   -> FILTER (o.status = 'paid') AND (c.country <> 'USA')               |
|     -> HASH JOIN c ON (c.name = u.city)                                |
|       -> INDEX NESTED LOOP JOIN u USING PRIMARY KEY (u.id = o.user_id) |
|         -> FILTER o.status = 'paid'                                    |
|           -> SEQ SCAN orders                                           |
|       -> FILTER c.country <> 'USA'                                     |
|         -> SEQ SCAN cities                                             |
```

- **Predicate pushdown:** `WHERE` conditions that read a single table are
  copied into that table's scan, where they can use its primary key or
  indexes and cut rows early. The full `WHERE` is applied again after the
  joins, which is simple and always correct.
- **Hash keys:** values are hashed by their order-preserving encoding, so
  `INT 1` never matches `TEXT '1'`.
- **Not done:** join reordering (no cost-based optimizer; the `FROM` order is
  the join order), outer joins, and spilling large hash tables to disk.
- **Testing:** a randomized test builds random tables, runs join queries
  that hit all four algorithms, and compares against results computed with
  plain C++ loops. An index on the join column is created and dropped
  between queries.

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

Results, from `tools/run_crash_tests.sh crash-with-indexes`:

| Mode | Crashes | Corrupted | Acknowledged commits, all present after recovery | Runs where recovery replayed log frames |
|---|---:|---:|---:|---:|
| SIGKILL, real files on ext4 | 1,000 | 0 | 162,471 | 883 |
| Simulated power loss | 10,000 | 0 | 533,020 | 9,560 |

These runs include two secondary indexes on the ledger. Every check
confirms that each index holds exactly one up-to-date entry per row. Raw
output is in [`bench/results/crash-with-indexes/`](bench/results/crash-with-indexes/).
The earlier run, before indexes existed, is in `m2-cloud-container/`.

**Checking the harness can fail.** `tools/check_harness.sh` builds three
deliberately broken engines and runs the power-loss mode against each:

| Engine variant | Corrupted runs (of 1,000) |
|---|---:|
| Correct engine | 0 |
| Commit doesn't fsync the log | 950 |
| Checkpoint doesn't fsync the database file before resetting the log | 424 |
| Recovery treats every valid frame as committed | 383 |

The second bug is the kind SIGKILL testing can never find.

## Performance

Indexed lookups versus full scans, end to end through SQL parsing, planning
and execution, on a 1,000,000-row database file (88 MB) with a 4 MiB buffer pool:

| Query | Median | p95 |
|---|---:|---:|
| Point lookup by primary key | 3.6 µs | 6.3 µs |
| Range scan by primary key, 100 rows | 23.5 µs | 30.0 µs |
| Point lookup on a column with a secondary index | 5.8 µs | 7.8 µs |
| Same lookup on an unindexed column (full scan) | 56 ms | 72 ms |

`CREATE INDEX` over the 1M rows took 5.0 s.

- **Machine:** a cloud VM with 4 vCPUs (Intel Xeon @ 2.10 GHz), Linux 6.18,
  GCC 13.3 at `-O3`.
- **Load:** 1,000 `INSERT` statements of 1,000 rows each, with an fsync
  after each statement, took 3.9 s (258k rows/s).
- **Cold buffer pool:** 3.9 µs median for a point lookup in a fresh process.
  That run starts with an empty buffer pool but a warm OS page cache.
- **Reproduce:** `bench/run_storage_bench.sh <name> --rows 1000000`. Raw
  per-query latencies and machine details are in
  [`bench/results/storage-1m-with-indexes/`](bench/results/storage-1m-with-indexes/).
  The original M1 run, before the scan speedup (full scan 185 ms), is in
  [`m1-cloud-container/`](bench/results/m1-cloud-container/).

### Compared with SQLite

Both engines run exactly the same SQL text and data on the same machine:
- 1M rows of `(id INTEGER PRIMARY KEY, k INTEGER, payload TEXT)`. In SQLite,
  `INTEGER PRIMARY KEY` makes the table clustered on `id`, as JerryQL's is.
- Settings matched as closely as the two engines allow: 4 KiB pages, a
  4 MiB page cache, write-ahead logging, and a checkpoint every ~1,000 pages.
- Both engines build the same in-memory result rows.
- SQLite 3.45.1 is the Ubuntu system library. Settings are in
  [`environment.txt`](bench/results/m3-fixed-harness/environment.txt).
- JerryQL has no prepared statements, so SQLite also parses every statement
  from text. SQLite's prepared-statement time is shown separately for
  reference.

**Durable mode** (SQLite `synchronous=FULL`, JerryQL default; both fsync the
log on every commit). Median latency:

| Workload | JerryQL | SQLite | JerryQL ÷ SQLite |
|---|---:|---:|---:|
| Point lookup by primary key (warm) | 3.4 µs | 6.1 µs | 0.56× |
| ↳ SQLite with a prepared statement (reference) | | 4.8 µs | |
| Range scan, 100 rows | 24.6 µs | 34.9 µs | 0.71× |
| Full scan of 1M rows (unindexed filter) | 50.2 ms | 41.9 ms | 1.20× |
| Bulk load, 1,000-row transaction | 3.8 ms | 1.6 ms | 2.4× |
| Single-row `INSERT` + commit | 259 µs | 181 µs | 1.4× |
| Single-row `UPDATE` + commit | 229 µs | 164 µs | 1.4× |

**Relaxed mode** (SQLite `synchronous=NORMAL`, JerryQL `syncOnCommit=false`;
neither fsyncs at commit, both fsync at checkpoints):

| Workload | JerryQL | SQLite | JerryQL ÷ SQLite |
|---|---:|---:|---:|
| Point lookup by primary key (warm) | 3.5 µs | 6.0 µs | 0.59× |
| ↳ SQLite with a prepared statement (reference) | | 3.5 µs | |
| Full scan of 1M rows | 51.3 ms | 41.3 ms | 1.24× |
| Single-row `INSERT` + commit | 15.9 µs | 11.3 µs | 1.4× |
| Single-row `UPDATE` + commit | 16.0 µs | 13.4 µs | 1.2× |

What the numbers say:

- **Point lookups:** on par with SQLite. JerryQL looks faster only when both
  engines parse SQL text on every call, because SQLite compiles each
  statement to bytecode first. Against a prepared statement, SQLite took
  3.5–4.8 µs across the two runs, the same range as JerryQL's 3.4–3.5 µs.
- **Full scans:** within 1.2× of SQLite. They were 4.9× slower until
  profiling with `perf` showed about 30% of scan time in `malloc`/`free`.
  The fix was to pin the current leaf, decode rows in place, and compare
  values by reference (commit `123ed31`).
- **Bulk loads (2.4×) and file size:** SQLite's file is 27% smaller (64 MB vs
  88 MB). SQLite stores integers as variable-length integers. JerryQL uses
  fixed 8-byte integers with type tags, and re-encodes the whole page on
  every insert.

The p95 columns, the cold-cache runs and raw per-query latencies are in
[`bench/results/m3-fixed-harness/`](bench/results/m3-fixed-harness/).
Reproduce with `bench/run_vs_sqlite.sh <name> 1000000`.

The first run of this comparison
([`m3-cloud-container`](bench/results/m3-cloud-container/), before the scan
optimization) converted SQLite's integer results to strings but not
JerryQL's, which charged SQLite extra work. It's kept for the record, but
the numbers above replace it.

Caveat: this is a 4-vCPU cloud VM, and fsync latency depends on its virtual
disk. The ratios are more meaningful than the absolute numbers.

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
5. ~~Benchmarks against SQLite with documented settings~~ (done)
6. ~~Profile and speed up full scans~~ (done: 4.9× → 1.2× SQLite's time)
7. ~~Aggregates and `GROUP BY`~~ (done)
8. ~~Secondary indexes~~ (done)
9. ~~Inner joins: index nested loop, hash, nested loop~~ (done)
10. Next ideas: cost-based join ordering, NULL, overflow pages for large rows, page checksums

## References

- [CMU 15-445/645 Database Systems](https://15445.courses.cs.cmu.edu/) (Andy Pavlo): query processing and the iterator model.
- Graefe, "Volcano: An Extensible and Parallel Query Evaluation System" (1994).
- [SQLite architecture](https://www.sqlite.org/arch.html) and [file format](https://www.sqlite.org/fileformat2.html): tables stored as B-trees keyed by row id, `sqlite_master`, freelist.
- Yao, "On Random 2-3 Trees" (1978): ~69% expected node utilization under random inserts.
- [SQLite write-ahead logging](https://www.sqlite.org/wal.html) and its [file format](https://www.sqlite.org/fileformat2.html#walformat): frames, salts, chained checksums, checkpoints.
- Pillai et al., "All File Systems Are Not Created Equal" (OSDI 2014), the ALICE paper: how applications get crash consistency wrong.
