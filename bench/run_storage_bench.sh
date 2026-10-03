#!/usr/bin/env bash
# Builds a Release binary and runs the storage benchmark, saving the summary,
# raw per-query latencies and machine details under bench/results/<name>/.
#
#   bench/run_storage_bench.sh [result-name] [extra flags for jerryql_bench]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:-storage-$(hostname -s)}"
shift || true
out="$root/bench/results/$name"
mkdir -p "$out"

cmake -S "$root" -B "$root/build-bench" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$root/build-bench" -j --target jerryql_bench > /dev/null

{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet || echo ' (dirty)')"
  echo "os: $(uname -srm)"
  echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
  echo "cores: $(nproc)"
  echo "memory: $(free -h | awk '/^Mem:/ {print $2}')"
  echo "compiler: $(c++ --version | head -1)"
  echo "build: CMake Release ($(grep CMAKE_CXX_FLAGS_RELEASE: "$root/build-bench/CMakeCache.txt" | cut -d= -f2))"
  echo "filesystem: $(df -T /tmp | awk 'NR==2 {print $2}')"
} > "$out/environment.txt"

"$root/build-bench/jerryql_bench" --csv "$out/latencies.csv" "$@" | tee "$out/summary.md"
echo "Results written to $out"
