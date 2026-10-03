#!/usr/bin/env bash
# Checks that the crash harness can fail: builds three deliberately broken
# copies of the engine (in a temporary directory; the working tree is not
# touched) and runs the power-loss harness against each. Each one should
# report corrupted runs. The real engine should report none.
#
#   tools/check_harness.sh [runs]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
runs="${1:-1000}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

variant=0
run_variant() {
  local name="$1" file="$2" from="$3" to="$4"
  variant=$((variant + 1))
  local tree="$work/variant$variant"
  mkdir -p "$tree"
  cp -r "$root/src" "$root/tools" "$root/tests" "$root/web" "$root/bench" "$root/CMakeLists.txt" "$tree/"
  if [[ -n "$file" ]]; then
    python3 - "$tree/$file" "$from" "$to" <<'EOF'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if old not in text:
    sys.exit(f"pattern not found in {path}: {old}")
open(path, "w").write(text.replace(old, new, 1))
EOF
  fi
  cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build "$tree/build" -j --target jerryql_crashtest > /dev/null
  local result
  result="$("$tree/build/jerryql_crashtest" --mode powerloss --runs "$runs" | grep -E '^[0-9]+ runs')" || true
  printf '%-58s %s\n' "$name" "$result"
}

run_variant "correct engine" "" "" ""
run_variant "bug: commit doesn't fsync the log" \
  src/storage/pager.cpp "if (options_.syncOnCommit) wal_.sync();" "/* no fsync */"
run_variant "bug: checkpoint doesn't fsync the database file" \
  src/storage/pager.cpp "    dbFile_->sync();
    wal_.reset();" "    wal_.reset();"
run_variant "bug: recovery treats every frame as committed" \
  src/storage/wal.cpp "if (commitPageCount != 0) {" "if (true) {"
