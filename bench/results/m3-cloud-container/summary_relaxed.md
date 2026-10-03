SQLite 3.45.1, mode: relaxed (SQLite synchronous=NORMAL, JerryQL no fsync on commit), rows: 1000000

| Workload | Runs | JerryQL median | JerryQL p95 | SQLite median | SQLite p95 | JerryQL ÷ SQLite (median time) |
|---|---:|---:|---:|---:|---:|---:|
| Bulk load: 1,000-row transaction | 1000 | 3045 µs | 5748 µs | 1165 µs | 1580 µs | 2.61× |
| Point lookup, cold cache | 1000 | 3.8 µs | 5.6 µs | 6.3 µs | 8.1 µs | 0.59× |
| Point lookup, warm | 20000 | 3.4 µs | 4.7 µs | 5.8 µs | 6.6 µs | 0.59× |
| Range scan, 100 rows | 2000 | 30.1 µs | 41.3 µs | 33.5 µs | 39.4 µs | 0.90× |
| Full scan, unindexed column | 10 | 178.0 ms | 209.1 ms | 41.7 ms | 42.9 ms | 4.26× |
| Single-row INSERT, own transaction | 500 | 20.0 µs | 41.5 µs | 8.5 µs | 11.7 µs | 2.36× |
| Single-row UPDATE by key, own transaction | 1000 | 16.8 µs | 27.3 µs | 9.9 µs | 12.9 µs | 1.71× |
| Point lookup, warm (SQLite prepared statement, reference) | 20000 | | | 3.5 µs | 4.0 µs | |

Bulk load total: JerryQL 3.80 s (263438 rows/s), SQLite 1.66 s (601566 rows/s)
Size on disk after load: JerryQL 87.7 MB, SQLite 64.1 MB
