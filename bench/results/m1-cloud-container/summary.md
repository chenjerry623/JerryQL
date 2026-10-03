buffer pool: 1024 pages, hits 20313915, misses 452427
rows: 1000000, file pages: 21404 (87.7 MB), load: 3.87 s (258420 rows/s, 1000-row INSERT statements, fsync per statement)

| Query | Runs | Median (us) | p95 (us) |
|---|---:|---:|---:|
| PK point lookup (cold buffer pool) | 1000 | 3.9 | 7.1 |
| PK point lookup (warm) | 10000 | 3.5 | 4.8 |
| PK range scan, 100 rows (warm) | 2000 | 26.1 | 32.7 |
| Full scan, same lookup on unindexed k (warm) | 20 | 185271.3 | 212419.3 |

PK lookup vs full scan (median, warm): 52935x faster
