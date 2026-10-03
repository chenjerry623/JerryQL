SQLite 3.45.1, mode: durable (SQLite synchronous=FULL, JerryQL fsync per commit), rows: 1000000

| Workload | Runs | JerryQL median | JerryQL p95 | SQLite median | SQLite p95 | JerryQL ÷ SQLite (median time) |
|---|---:|---:|---:|---:|---:|---:|
| Bulk load: 1,000-row transaction | 1000 | 3837 µs | 5860 µs | 1526 µs | 2455 µs | 2.51× |
| Point lookup, cold cache | 1000 | 3.7 µs | 5.5 µs | 6.4 µs | 13.3 µs | 0.57× |
| Point lookup, warm | 20000 | 3.4 µs | 4.5 µs | 6.2 µs | 10.6 µs | 0.54× |
| Range scan, 100 rows | 2000 | 30.8 µs | 40.0 µs | 33.2 µs | 47.1 µs | 0.93× |
| Full scan, unindexed column | 10 | 194.0 ms | 224.4 ms | 39.4 ms | 41.6 ms | 4.92× |
| Single-row INSERT, own transaction | 500 | 284 µs | 404 µs | 283 µs | 426 µs | 1.01× |
| Single-row UPDATE by key, own transaction | 1000 | 252 µs | 334 µs | 175 µs | 372 µs | 1.45× |
| Point lookup, warm (SQLite prepared statement, reference) | 20000 | | | 3.5 µs | 4.6 µs | |

Bulk load total: JerryQL 4.52 s (221043 rows/s), SQLite 1.94 s (514390 rows/s)
Size on disk after load: JerryQL 87.7 MB, SQLite 64.1 MB
