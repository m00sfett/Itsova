#!/usr/bin/env bash
# Mutation testing with Mull (#93), limited to the files a change touches.
#
# Usage: scripts/mutate.sh [--build DIR] [--tests "name1 name2"] <src/file.cpp>...
#   --build DIR    build directory (default: build-mutate next to the sources)
#   --tests LIST   CTest test executables to run the mutants against (default: every
#                  *test executable of the build)
#
# Mull compiles the mutants into the code with a clang plugin and runs each test executable once
# per mutant; a mutant that no test kills "survived" and is reported. Needs clang and Mull built
# for the same LLVM version (mull-runner-<N>, mull-ir-frontend-<N>; Mull's release packages,
# extracted anywhere: set MULL_DIR to the directory that holds bin/ and lib/).
#
# Long runs: start it decoupled (systemd-run --user …), as the host rules require; measure one
# file first.
set -euo pipefail
source_dir="$(realpath "$(dirname "$(realpath "${BASH_SOURCE[0]}")")/..")"
build_dir="$source_dir/build-mutate"
tests=""
files=()
while (($#)); do
    case "$1" in
    --build) build_dir="$(realpath -m "$2")"; shift 2 ;;
    --tests) tests="$2"; shift 2 ;;
    -h | --help) sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) files+=("$(realpath "$1")"); shift ;;
    esac
done
((${#files[@]})) || { echo "Name at least one source file." >&2; exit 2; }

llvm="$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)"
mull_dir="${MULL_DIR:-}"
runner="$(command -v "mull-runner-$llvm" || true)"
[[ -z "$runner" && -n "$mull_dir" ]] && runner="$mull_dir/bin/mull-runner-$llvm"
plugin=""
for candidate in ${mull_dir:+"$mull_dir"/lib} /usr/lib /usr/local/lib "$HOME/.local/lib"; do
    [[ -f "$candidate/mull-ir-frontend-$llvm" ]] && plugin="$candidate/mull-ir-frontend-$llvm" && break
done
if [[ ! -x "$runner" || -z "$plugin" ]]; then
    echo "Mull for LLVM $llvm not found (mull-runner-$llvm, mull-ir-frontend-$llvm). Set MULL_DIR." >&2
    exit 2
fi

# Only the named files get mutants.
mkdir -p "$build_dir"
{
    echo "mutators: [cxx_all]"
    echo "includePaths:"
    for f in "${files[@]}"; do echo "  - '$(printf '%s' "$f" | sed 's/[.[\*^$()+?{|]/\\&/g')'"; done
    echo "timeout: 20000"
} >"$build_dir/mull.yml"
export MULL_CONFIG="$build_dir/mull.yml"
CC=clang CXX=clang++ cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fpass-plugin=$plugin -g -grecord-command-line" \
    -DCMAKE_CXX_FLAGS="-fpass-plugin=$plugin -g -grecord-command-line" >/dev/null
cmake --build "$build_dir" --parallel "$(( $(nproc) > 4 ? 4 : $(nproc) ))"

[[ -n "$tests" ]] || tests="$(cd "$build_dir" && ls -1 ./*test 2>/dev/null | xargs -n1 basename)"
status=0
for t in $tests; do
    echo "=== $t"
    QT_QPA_PLATFORM=offscreen "$runner" --reporters IDE --report-dir "$build_dir/mull-reports" \
        --report-name "$t" "$build_dir/$t" || status=$?
done
exit "$status"
