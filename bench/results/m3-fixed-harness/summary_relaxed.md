SQLite 3.45.1, mode: relaxed (SQLite synchronous=NORMAL, JerryQL no fsync on commit), rows: 1000000

| Workload | Runs | JerryQL median | JerryQL p95 | SQLite median | SQLite p95 | JerryQL ÷ SQLite (median time) |
|---|---:|---:|---:|---:|---:|---:|
| Bulk load: 1,000-row transaction | 1000 | 3003 µs | 4482 µs | 1193 µs | 1858 µs | 2.52× |
| Point lookup, cold cache | 1000 | 4.0 µs | 6.6 µs | 6.5 µs | 10.3 µs | 0.61× |
| Point lookup, warm | 20000 | 3.5 µs | 4.7 µs | 6.0 µs | 9.4 µs | 0.59× |
| Range scan, 100 rows | 2000 | 25.0 µs | 34.1 µs | 33.1 µs | 44.2 µs | 0.75× |
| Full scan, unindexed column | 10 | 51.3 ms | 63.0 ms | 41.3 ms | 41.6 ms | 1.24× |
| Single-row INSERT, own transaction | 500 | 15.9 µs | 25.0 µs | 11.3 µs | 17.0 µs | 1.41× |
| Single-row UPDATE by key, own transaction | 1000 | 16.0 µs | 21.3 µs | 13.4 µs | 18.3 µs | 1.19× |
| Point lookup, warm (SQLite prepared statement, reference) | 20000 | | | 3.5 µs | 3.9 µs | |

Bulk load total: JerryQL 3.56 s (280733 rows/s), SQLite 1.72 s (581462 rows/s)
Size on disk after load: JerryQL 87.7 MB, SQLite 64.1 MB
