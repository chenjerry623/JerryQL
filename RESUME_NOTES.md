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
