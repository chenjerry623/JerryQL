buffer pool: 1024 pages, hits 92914, misses 452427
rows: 1000000, file pages: 21404 (87.7 MB), load: 4.45 s (224870 rows/s, 1000-row INSERT statements, fsync per statement)

| Query | Runs | Median (us) | p95 (us) |
|---|---:|---:|---:|
| PK point lookup (cold buffer pool) | 1000 | 4.0 | 5.8 |
| PK point lookup (warm) | 10000 | 3.5 | 4.7 |
| PK range scan, 100 rows (warm) | 2000 | 23.3 | 38.1 |
| Full scan, same lookup on unindexed k (warm) | 20 | 53175.7 | 67717.3 |

PK lookup vs full scan (median, warm): 15055x faster
