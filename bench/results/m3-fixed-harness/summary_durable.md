SQLite 3.45.1, mode: durable (SQLite synchronous=FULL, JerryQL fsync per commit), rows: 1000000

| Workload | Runs | JerryQL median | JerryQL p95 | SQLite median | SQLite p95 | JerryQL ÷ SQLite (median time) |
|---|---:|---:|---:|---:|---:|---:|
| Bulk load: 1,000-row transaction | 1000 | 3803 µs | 5913 µs | 1601 µs | 2417 µs | 2.38× |
| Point lookup, cold cache | 1000 | 3.8 µs | 5.7 µs | 6.2 µs | 8.1 µs | 0.62× |
| Point lookup, warm | 20000 | 3.4 µs | 5.3 µs | 6.1 µs | 9.5 µs | 0.56× |
| Range scan, 100 rows | 2000 | 24.6 µs | 39.3 µs | 34.9 µs | 50.8 µs | 0.71× |
| Full scan, unindexed column | 10 | 50.2 ms | 75.8 ms | 41.9 ms | 43.7 ms | 1.20× |
| Single-row INSERT, own transaction | 500 | 259 µs | 342 µs | 181 µs | 259 µs | 1.43× |
| Single-row UPDATE by key, own transaction | 1000 | 229 µs | 351 µs | 164 µs | 230 µs | 1.39× |
| Point lookup, warm (SQLite prepared statement, reference) | 20000 | | | 4.8 µs | 5.5 µs | |

Bulk load total: JerryQL 4.48 s (222989 rows/s), SQLite 2.02 s (495605 rows/s)
Size on disk after load: JerryQL 87.7 MB, SQLite 64.1 MB
