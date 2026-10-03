# Resume notes

A running log of what each milestone added, the numbers it produced, and how
to reproduce each one. Every number here comes from a command in this repo;
nothing is estimated.

---

## M0: Real SQL engine (parser, planner, executor) + browser demo

**What was built:** The old 429-line command interpreter was rewritten as a SQL
engine with a lexer, a recursive-descent parser, a planner that uses the
primary key for point and range lookups, and pull-based query operators. It's
compiled to WebAssembly for a live browser demo.

**Commits:** `126e4b3` (engine) and the commit that adds this file (demo and notes).

### Numbers

| Number | What it is | Reproduce with |
|---|---|---|
| 7 statement types | CREATE TABLE, DROP TABLE, INSERT, SELECT, UPDATE, DELETE, EXPLAIN | `src/parser.cpp`, `Parser::parseStatement` |
| ~2,200 lines | C++ in `src/` (engine only, excluding tests) | `wc -l src/*.cpp src/*.h` |
| 34 unit tests | lexer, parser, planner, engine | `./build/jerryql_tests` → `34 tests, 0 failures` |
| 4 golden SQL scripts | end-to-end output checked byte for byte | `cd build && ctest` |
| 600,000 random operations, 0 mismatches | 200 seeds × 3,000 steps against a `std::map` oracle, under ASan/UBSan | `for s in $(seq 1 200); do JERRYQL_SEED=$s ./build-asan/jerryql_tests randomized; done` |
| 3,000 malformed inputs, 0 crashes | random token sequences through the shell under ASan/UBSan | throwaway script; not committed (see note) |

Note: the fuzz run was a quick one-off script, so leave it off the resume
unless it gets committed as a reproducible test.

Not measured yet: any performance number. Timing an in-memory `std::map`
isn't meaningful; performance numbers come with the on-disk B+tree (M1) and
the SQLite comparison (M3).

### Candidate resume bullets (M0)

- **Database-focused:** Built a SQL database engine from scratch in C++17 (lexer,
  recursive-descent parser, rule-based query planner, iterator-model executor)
  that turns primary-key filters into index range scans; verified with 34 unit
  tests and 600,000 randomized operations checked against a reference model
  under AddressSanitizer.
- **General backend/infra:** Wrote a C++17 SQL engine supporting 7 statement types
  with a query planner and `EXPLAIN`, compiled it to WebAssembly for a live
  browser demo, and set up CI running sanitizer builds and randomized
  differential tests.
- **Short:** Built a SQL engine from scratch in C++17 with a query planner and
  live WebAssembly demo; 600,000 randomized operations verified against a
  reference model.

### Interview story (STAR)

- **Situation:** The original version parsed `WHERE` by splitting on spaces.
  `DELETE FROM e WHERE id = 2` (with spaces around `=`) didn't match the
  pattern, so the filter was dropped and every row was deleted.
- **Task:** Replace the string matching with a real parser, without
  introducing a new class of silent misbehaviour.
- **Action:**
  - Wrote a tokenizer and a recursive-descent parser with one function per
    precedence level, so `a OR b AND c` groups correctly.
  - Made every unexpected token a reported syntax error rather than something
    silently skipped.
  - Added a regression test for the exact original bug.
  - Added a randomized test that applies thousands of random
    inserts/updates/deletes to both the engine and a `std::map`, comparing
    after every step.
- **Result:**
  - The randomized test caught a real problem during development. In the
    script runner, a *runtime* error, such as a duplicate key, also skipped
    the *next* statement, because the error-recovery path assumed every error
    was a syntax error.
  - Fixed by separating parse-error recovery from execution errors.
  - Lesson: error-handling paths need their own tests, because they're
    exactly the paths a happy-path demo never exercises.
- **Follow-up design question interviewers like:** why does `UPDATE` collect
  matching rows before writing? Writing while scanning can revisit a row whose
  key moved forward (the "Halloween problem"), so `UPDATE t SET id = id + 1`
  would loop or double-update.

---

## M1: On-disk B+tree storage with a buffer pool

**What was built:** Tables now live in a single database file as B+trees of
4 KiB pages, read and written through an LRU buffer pool. The schema is stored
in the file itself, and the planner's primary-key lookups become real B+tree
descents.

**Commits:** `c00d77e` (storage engine), `b1e07e2` (benchmark; the numbers below
were produced at this commit).

### Numbers

| Number | What it is | Reproduce with |
|---|---|---|
| 3.5 µs median, 4.8 µs p95 | primary-key point lookup, end to end through SQL, 1M-row file, warm | `bench/run_storage_bench.sh m1-cloud-container --rows 1000000` → `bench/results/m1-cloud-container/summary.md` |
| 185 ms median | the same lookup on an unindexed column (full scan of 1M rows) | same |
| ~50,000× | median full scan ÷ median indexed lookup (52,935× in the committed run) | same |
| 26.1 µs median | 100-row primary-key range scan | same |
| 258k rows/s | bulk load via 1,000-row INSERT statements, fsync after each | same |
| 99.7% vs 69.0% | leaf fill for sequential vs random inserts (append fast path in the split rule) | `./build/jerryql_tests btreeSequential` |
| 2,000,000 random operations, 0 failures | 50 seeds × 40,000 B+tree ops vs `std::map`, 16-page pool, under ASan/UBSan | `for s in $(seq 1 50); do JERRYQL_SEED=$s ./build-asan/jerryql_tests btreeRandomized; done` |
| 47 unit tests | total, after M1 | `./build/jerryql_tests` |

Machine: cloud VM, 4 vCPU Intel Xeon @ 2.10 GHz, Linux 6.18, GCC 13.3, `-O3`.
Saved in `bench/results/m1-cloud-container/environment.txt`.

On the "~50,000×" figure: it's real, but it mostly restates O(log n) vs O(n)
at a million rows. An interviewer may push on it. The absolute numbers (3.5 µs
vs 185 ms) are the stronger claim, and they're what the bullets below use.

### Candidate resume bullets (M1)

- **Database-focused:** Built a SQL database engine from scratch in C++17 with a
  disk-backed B+tree, LRU buffer pool and query planner; primary-key lookups
  take 3.5 µs median vs 185 ms for a full scan on a 1M-row table, verified
  by 2M randomized operations checked against a reference model under
  AddressSanitizer.
- **General backend/infra:** Wrote a C++ storage engine (B+tree on 4 KiB pages,
  buffer pool with LRU eviction, POSIX file I/O with fsync) under a
  hand-written SQL parser and planner; 3.5 µs indexed lookups on a
  1M-row file, with a live WebAssembly demo.
- **Short:** Built a SQL database from scratch in C++ (parser, planner, on-disk
  B+tree, buffer pool); 3.5 µs indexed lookups vs 185 ms full scans at 1M rows.

**Linux on the skills line:** partly earned. The storage layer uses POSIX
`pread`/`pwrite`/`fsync` directly. The crash-injection harness in M2
(`fork` + `SIGKILL`) makes it solid; add Linux back once M2 lands.

### Interview story (STAR)

- **Situation:** The first leaf-split rule split full pages in half. It's
  correct, but it leaves every page about half empty whenever keys arrive in
  increasing order, which is the most common case (auto-increment ids).
- **Task:** Keep sequential loads dense without hurting random-order inserts.
- **Action:**
  - Measured fill factor in a test: 50,000 keys inserted in sorted order
    and again in shuffled order, with a structural check
    (`BTree::check()`) reporting average leaf fill.
  - Added a fast path: when a new key lands past the last key of the
    rightmost leaf, only that cell moves to the new page.
  - Split everything else at the byte midpoint rather than the cell
    midpoint, so pages with mixed row sizes divide their free space
    evenly.
  - Found afterwards that SQLite (`balance_quick`) and PostgreSQL
    (rightmost-page split) do the same thing.
- **Result:**
  - Sequential loads went to 99.7% leaf fill, with 31% fewer leaf pages
    than random order (394 vs 571).
  - Random order stayed at 69%, matching the textbook ln 2 expectation.
  - Lesson: measure the shape of the data structure, not just its
    correctness.
- **Other likely questions:**
  - Why is the root page fixed? The catalog stores it, so a root split
    moves the old root's contents down instead of creating a new root.
  - Why no merge on delete? It's simpler, and many production engines tolerate
    underfull pages; it's a documented limit.
  - What does "cold" mean in the benchmark? An empty buffer pool but a warm
    OS page cache, because dropping that cache needs root.

---

## M2: Write-ahead log, crash recovery, transactions

**What was built:** A write-ahead log makes every committed transaction survive a
crash, recovery replays it on startup, and `BEGIN`/`COMMIT`/`ROLLBACK` group
statements atomically. Two crash-injection harnesses verify it: real `SIGKILL`s,
and a simulated power cut that drops or tears unsynced writes.

**Commits:** `6bb38c6` (WAL, recovery, transactions, harnesses). The crash
numbers below were produced at this commit.

### Numbers

| Number | What it is | Reproduce with |
|---|---|---|
| 1,000 SIGKILL runs, 0 corrupted | fork + SIGKILL at a random 1–300 ms, reopen, verify; 192,400 acknowledged commits all present | `tools/run_crash_tests.sh m2-cloud-container` → `bench/results/m2-cloud-container/kill.txt` |
| 10,000 simulated power cuts, 0 corrupted | cut at a random file operation; unsynced writes kept, lost or torn; 539,715 acknowledged commits all present | same → `powerloss.txt` |
| 3 of 3 injected bugs caught | no commit fsync: 938/1,000 runs flagged; no fsync before checkpoint: 248/1,000; recovery ignoring the commit marker: 310/1,000 | `tools/check_harness.sh 1000` → `harness_self_check.txt` |
| 58 unit tests | after M2 | `./build/jerryql_tests` |

Machine: same cloud VM as M1 (`bench/results/m2-cloud-container/environment.txt`).

**Linux on the skills line: earned.** The project now does POSIX file I/O,
`fsync` ordering (including directory fsync), and `fork`/`SIGKILL`/`waitpid`
crash testing. That's concrete evidence for "Linux" next to C++.

### Candidate resume bullets (M2)

- **Database-focused:** Built a SQL database engine from scratch in C++17
  (parser, planner, disk-based B+tree, buffer pool, write-ahead log with
  crash recovery); 0 corrupted databases across 1,000 SIGKILL and 10,000
  simulated power-loss crash tests, and the harness caught each of 3
  deliberately injected durability bugs.
- **General backend/infra:** Wrote a crash-safe storage engine in C++ on Linux
  (write-ahead log, fsync ordering, checksummed recovery, transactions) under
  a hand-written SQL layer, verified with fork/SIGKILL and simulated
  power-loss tests: 11,000 crashes, 0 lost commits.
- **Short:** Built a crash-safe SQL database in C++ (B+tree, write-ahead log,
  transactions); 0 lost commits across 11,000 injected crashes.

Combined with M1 for a two-bullet project entry:

1. Built a SQL database engine from scratch in C++17: hand-written parser,
   query planner, disk-based B+tree with an LRU buffer pool, and a live
   WebAssembly demo; indexed lookups take 3.5 µs vs 185 ms for a full scan
   on 1M rows.
2. Added a write-ahead log with crash recovery and transactions; verified
   with fork/SIGKILL and simulated power-loss testing: 0 corrupted
   databases in 11,000 crashes, and the harness caught 3 of 3 deliberately
   injected durability bugs.

### Interview story (STAR)

- **Situation:** The first crash test killed the process with `SIGKILL` and
  checked the data, and it passed every time. That should have raised
  suspicion: after a `SIGKILL` the kernel still writes out everything the
  process handed it, so a missing or misplaced `fsync` is invisible to that
  test.
- **Task:** Build a test that can actually fail when the `fsync` protocol is
  wrong, and prove it can.
- **Action:**
  - Wrote a fault-injecting file layer that models a disk with a volatile
    cache. Writes become durable only at `fsync`. At a random operation the
    power is "cut", and each unsynced write is then kept, lost, or torn at a
    512-byte sector.
  - Then *mutation-tested the harness itself*: a script builds three broken
    engines and confirms the harness flags each one.
  - The three bugs: no `fsync` at commit, no `fsync` of the database file
    before the log is reset in a checkpoint, and recovery ignoring the
    commit marker.
- **Result:**
  - The broken engines were flagged in 938, 248 and 310 of 1,000 runs. The
    real engine: 0 of 10,000.
  - Lesson: a test suite that never fails is a claim, not evidence. The
    checkpoint-ordering bug is the one SIGKILL testing would never find.
- **Likely follow-ups:**
  - Why full page images instead of logical redo records? Recovery
    is idempotent and simple, and torn page writes in the database file
    can't happen silently. The cost is write amplification, the same
    trade-off as PostgreSQL's full-page writes.
  - What happens if `fsync` fails? The commit is reported failed and
    rolled back in memory, but it's *in doubt* on disk (PostgreSQL's
    2018 "fsyncgate" is the classic reference).
  - What isn't covered? Disks that lie about `fsync`, bit rot in the
    database file (no page checksums), and concurrent writers.

---

## M3: Benchmarks against SQLite

**What was built:** A side-by-side benchmark that runs identical SQL on JerryQL
and SQLite 3.45.1 with matched page size, cache size, WAL checkpointing and
durability settings. Median/p95, cold and warm, with raw latencies committed.

**Commit:** `9d629b7` (the numbers below were produced at this commit).

### Numbers (1M rows, durable mode unless noted)

| Number | What it is | Reproduce with |
|---|---|---|
| 3.4 µs vs 3.5 µs | JerryQL point lookup vs SQLite with a prepared statement: on par | `bench/run_vs_sqlite.sh m3-cloud-container` → `summary_durable.md` |
| 0.54× | JerryQL time ÷ SQLite time for point lookups when both parse SQL text | same |
| 1.0× | single-row INSERT + fsynced commit (284 µs vs 283 µs) | same |
| 2.5× slower | bulk load (1,000-row transactions) | same |
| 4.9× slower | full scan of 1M rows | same |
| 27% smaller | SQLite's file vs JerryQL's (64 vs 88 MB) | same |

How to talk about it: "on par with SQLite for indexed lookups and fsync-bound
commits, 2.5–5× slower for scans and bulk loads." Don't say "faster than
SQLite": the 0.54× only holds without prepared statements, and the README
says so.

### Candidate resume bullets (M3)

- **Database-focused:** Built a SQL database engine from scratch in C++17 (parser,
  planner, B+tree, buffer pool, write-ahead log); matches SQLite's indexed
  lookup latency (3.4 µs vs 3.5 µs at 1M rows) under documented, matched
  settings; 0 corrupted databases across 11,000 injected crashes.
- **General backend/infra:** Wrote a crash-safe SQL storage engine in C++ on Linux;
  benchmarked against SQLite on identical workloads (on par for lookups and
  fsync-bound commits, within 5× on scans), and verified durability with
  fork/SIGKILL and simulated power-loss testing.
- **Short:** Built a crash-safe SQL database in C++ that matches SQLite's indexed
  lookup latency; 0 lost commits in 11,000 injected crashes.

### Interview story (STAR)

- **Situation:** The first SQLite comparison showed JerryQL nearly 2× *faster* on
  point lookups. A from-scratch engine beating SQLite at its core operation
  is a red flag, not a win.
- **Task:** Find out whether the comparison was fair before reporting it.
- **Action:** Broke the latency down. Both engines were being handed SQL text
  for every query. SQLite compiles each statement to bytecode (its VDBE), so
  that compile step was most of its time. I added a reference run with a
  prepared statement, which is how SQLite is used in practice.
- **Result:**
  - With the compile cost removed, SQLite took 3.5 µs vs JerryQL's 3.4 µs:
    on par, not faster.
  - The README reports both numbers and says which one is the fair
    comparison.
  - The same breakdown showed where JerryQL really loses: full scans,
    because it decodes every column of every row, which SQLite doesn't.
    That became the next optimization target.

---

## Update: numbers after the full-scan speedup (current `main`)

Re-ran the storage benchmark on `main` at `2a08458`, which includes the
allocation fixes from `123ed31` (found by profiling with `perf`):

| Number | What it is | Reproduce with |
|---|---|---|
| 3.5 µs median | primary-key lookup, 1M-row file, warm | `bench/run_storage_bench.sh storage-1m-after-scan-speedup --rows 1000000` |
| 53 ms median | full scan of the same 1M rows (was 185 ms before the speedup: 3.5× faster) | same |
| 23.3 µs median | 100-row range scan (was 26.1 µs) | same |

The SQLite comparison (M3) was run before this speedup and its harness has a
known flaw on SQLite's side (it converts integer results to strings), so
the SQLite numbers are left off the resume until the harness is fixed and re-run.

### Resume bullets as submitted (Oct 2026)

- Built a SQL database from scratch in C++ (B+tree, buffer pool, planner);
  3.5 µs indexed lookups vs. 53 ms full scans on 1M rows.
- Added a write-ahead log with crash recovery; 0 corrupted databases across
  11,000 injected crashes (1,000 real SIGKILLs + 10,000 simulated power cuts).

---

## Update: SQLite comparison re-run with the harness fixed (`d59b829`)

The harness now builds identical result rows for both engines. Results at 1M
rows, durable mode (`bench/run_vs_sqlite.sh m3-fixed-harness 1000000` →
`bench/results/m3-fixed-harness/`):

| Number | What it is |
|---|---|
| 1.2× | JerryQL full-scan time ÷ SQLite's (50 ms vs 42 ms); 4.9× before the `perf`-guided fix |
| 3.4 µs vs 3.5–4.8 µs | JerryQL point lookup vs SQLite with a prepared statement (two runs): on par |
| 2.4× | bulk load time ÷ SQLite's |
| 1.4× | single-row durable commit time ÷ SQLite's |

### Bullet options using these

- Built a SQL database from scratch in C++ (B+tree, buffer pool, write-ahead
  log); profiled with `perf` to cut full-scan time **3.5×**, bringing scans
  within **1.2×** of SQLite on **1M** rows.
- One-liner with a baseline a reader understands: "...serving key lookups in
  **3.5 µs** on a **1M**-row table, on par with SQLite."

Interview framing: on par for lookups (prepared-statement comparison),
1.2× on scans, 2.4× on bulk loads, and 27% larger files. Know the reasons:
varints vs fixed 8-byte integers, and whole-page re-encoding on insert.
