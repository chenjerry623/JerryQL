buffer pool: 1024 pages, hits 4133282, misses 509980
rows: 1000000, file pages: 21912 (89.8 MB), load: 4.87 s (205146 rows/s, 1000-row INSERT statements, fsync per statement)

| Query | Runs | Median (us) | p95 (us) |
|---|---:|---:|---:|
| PK point lookup (cold buffer pool) | 1000 | 4.0 | 5.9 |
| PK point lookup (warm) | 10000 | 3.6 | 6.3 |
| PK range scan, 100 rows (warm) | 2000 | 23.5 | 30.0 |
| Full scan, same lookup on unindexed k (warm) | 20 | 56390.6 | 71507.5 |
| Secondary index lookup on k (warm) | 10000 | 5.8 | 7.8 |

PK lookup vs full scan (median, warm): 15526x faster
Secondary index lookup vs full scan (median, warm): 9721x faster
CREATE INDEX over 1000000 rows: 4.98 s
