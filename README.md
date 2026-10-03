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

**Not supported (yet):** NULL, joins, aggregates/GROUP BY, subqueries,
secondary indexes, transactions. Data is held in memory; on-disk storage
is the next milestone.

## Design

```
SQL text -> Lexer -> Parser -> AST -> Planner -> Operator tree -> Executor
                                                     |
                                              TableStore interface
                                     (std::map today, on-disk B+tree next)
```

| Layer | File | Notes |
|---|---|---|
| Lexer | `src/lexer.cpp` | Bad input becomes an error token, so one bad statement doesn't abort a whole script. |
| Parser | `src/parser.cpp` | Hand-written recursive descent, one function per precedence level (`OR` < `AND` < `NOT` < comparison < `+ -` < `* /` < unary). Recovers at the next `;` after a syntax error. |
| Planner | `src/planner.cpp` | Extracts a primary-key range from top-level `AND`ed comparisons such as `id >= 10 AND id < 20`, giving a point lookup, range scan or full scan. Detects contradictions like `id > 5 AND id < 3`. The full `WHERE` is still applied by a filter, so the range only limits how much is read. |
| Executor | `src/executor.cpp` | Volcano-style operators (`Scan`, `Filter`, `Sort`, `Limit`, `Projection`), each with `next()`. Column names are resolved to indexes once at plan time, not per row. |
| Browser build | `web/` | The same engine compiled with Emscripten behind a three-function C API (`jerryql_run`, `jerryql_reset`, `jerryql_free`). CI checks its output matches the native build byte for byte. |
| Storage | `src/table_store.h` | Rows keyed by a 64-bit integer: the primary key, or a hidden row id. Ordered range scans. |

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

## Testing

- **Unit tests** (`tests/test_*.cpp`) for the lexer, parser, planner and engine,
  using a small dependency-free harness (`tests/check.h`).
- **Randomized differential test:** thousands of random inserts, deletes,
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
3. On-disk B+tree storage with a page cache
4. Write-ahead log with crash recovery, verified by a SIGKILL crash-injection harness
5. Benchmarks against SQLite with documented settings

## References

- [CMU 15-445/645 Database Systems](https://15445.courses.cs.cmu.edu/) (Andy Pavlo): query processing and the iterator model.
- Graefe, "Volcano: An Extensible and Parallel Query Evaluation System" (1994).
- [SQLite architecture](https://www.sqlite.org/arch.html): tables stored as B-trees keyed by row id.
