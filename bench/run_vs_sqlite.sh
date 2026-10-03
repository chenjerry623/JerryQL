#!/usr/bin/env bash
# Builds a Release binary and runs the JerryQL vs SQLite comparison in both
# durability modes, saving summaries, raw latencies and machine details
# under bench/results/<name>/.
#
#   bench/run_vs_sqlite.sh [result-name] [rows]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:-vs-sqlite-$(hostname -s)}"
rows="${2:-1000000}"
out="$root/bench/results/$name"
mkdir -p "$out"

cmake -S "$root" -B "$root/build-bench" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$root/build-bench" -j --target jerryql_vs_sqlite > /dev/null

{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet || echo ' (dirty)')"
  echo "os: $(uname -srm)"
  echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
  echo "cores: $(nproc)"
  echo "memory: $(free -h | awk '/^Mem:/ {print $2}')"
  echo "compiler: $(c++ --version | head -1)"
  echo "build: CMake Release ($(grep CMAKE_CXX_FLAGS_RELEASE: "$root/build-bench/CMakeCache.txt" | cut -d= -f2))"
  echo "sqlite: $(python3 -c 'import sqlite3; print(sqlite3.sqlite_version)') (system library, $(ls /usr/lib/x86_64-linux-gnu/libsqlite3.so* 2>/dev/null | head -1))"
  echo "sqlite settings: page_size=4096, journal_mode=WAL, cache_size=-4096 (4 MiB), wal_autocheckpoint=1000, synchronous=FULL (durable) or NORMAL (relaxed)"
  echo "jerryql settings: 4096-byte pages, 1024-page buffer pool (4 MiB), checkpoint every 1000 frames, fsync per commit (durable) or not (relaxed)"
  echo "filesystem: $(df -T /tmp | awk 'NR==2 {print $2}')"
} > "$out/environment.txt"

for mode in durable relaxed; do
  "$root/build-bench/jerryql_vs_sqlite" --rows "$rows" --mode "$mode" --dir /tmp \
    --csv "$out/latencies_$mode.csv" | tee "$out/summary_$mode.md"
  echo
done
echo "Results written to $out"
