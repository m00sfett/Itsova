#!/usr/bin/env bash
# Line and branch coverage of the offscreen test suite with gcovr (#93). Reported, not a gate.
#
# Usage: scripts/coverage.sh [build-dir]   (default: build-coverage next to the sources)
# Writes <build-dir>/coverage/index.html and prints the summary. The nested-KWin tests are left
# out; run them with `ctest -L nested` in a normal build.
set -euo pipefail
source_dir="$(dirname "$(realpath "${BASH_SOURCE[0]}")")/.."
source_dir="$(realpath "$source_dir")"
build_dir="$(realpath -m "${1:-$source_dir/build-coverage}")"
command -v gcovr >/dev/null || { echo "gcovr is missing (pacman -S gcovr)" >&2; exit 2; }
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=--coverage \
    -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage >/dev/null
cmake --build "$build_dir" --parallel "$(( $(nproc) > 4 ? 4 : $(nproc) ))"
find "$build_dir" -name '*.gcda' -delete
ctest --test-dir "$build_dir" -LE nested --output-on-failure
mkdir -p "$build_dir/coverage"
gcovr --root "$source_dir" --filter "$source_dir/src/" --exclude-unreachable-branches \
    --exclude-throw-branches --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    --html-details "$build_dir/coverage/index.html" --txt-summary --print-summary "$build_dir" \
    | tail -n 5
echo "Report: $build_dir/coverage/index.html"
