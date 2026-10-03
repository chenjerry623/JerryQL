#!/usr/bin/env bash
# Builds the browser playground into site/ (index.html + jerryql.js).
# Needs Emscripten on PATH (source emsdk_env.sh first).
#
#   web/build_site.sh && python3 -m http.server -d site 8000
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-web"
site="$root/site"

emcmake cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j

rm -rf "$site"
mkdir -p "$site"
cp "$build/jerryql.js" "$site/"
{
  printf '<!doctype html>\n<html lang="en">\n<head>\n'
  printf '<meta charset="utf-8">\n'
  printf '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
  printf '<meta name="description" content="JerryQL: a SQL engine written from scratch in C++, running in your browser via WebAssembly.">\n'
  printf '</head>\n<body>\n'
  cat "$root/web/playground.html"
  printf '\n</body>\n</html>\n'
} > "$site/index.html"
echo "Site written to $site"
