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
