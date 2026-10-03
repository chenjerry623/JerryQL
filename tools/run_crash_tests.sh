#!/usr/bin/env bash
# Builds a Release binary and runs both crash harnesses, saving their output
# and machine details under bench/results/<name>/.
#
#   tools/run_crash_tests.sh [result-name] [kill-runs] [powerloss-runs]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:-crash-$(hostname -s)}"
kill_runs="${2:-1000}"
powerloss_runs="${3:-10000}"
out="$root/bench/results/$name"
mkdir -p "$out"

cmake -S "$root" -B "$root/build-bench" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$root/build-bench" -j --target jerryql_crashtest > /dev/null

{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet || echo ' (dirty)')"
  echo "os: $(uname -srm)"
  echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
  echo "cores: $(nproc)"
  echo "compiler: $(c++ --version | head -1)"
  echo "filesystem for kill mode: $(df -T /tmp | awk 'NR==2 {print $2}')"
} > "$out/environment.txt"

"$root/build-bench/jerryql_crashtest" --mode powerloss --runs "$powerloss_runs" --transactions 150 \
  | tee "$out/powerloss.txt"
"$root/build-bench/jerryql_crashtest" --mode kill --runs "$kill_runs" --dir /tmp/jerryql_crash \
  | tee "$out/kill.txt"
"$root/tools/check_harness.sh" 1000 | tee "$out/harness_self_check.txt"
echo "Results written to $out"
