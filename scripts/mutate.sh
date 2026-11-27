#!/usr/bin/env bash
# Mutation testing with Mull (#93), limited to the files a change touches.
#
# Usage: scripts/mutate.sh [--docker] [--build DIR] [--tests "name1 name2"] <src/file.cpp>...
#   --docker       run inside a debian:trixie container with clang-19 and official Mull-19
#                  (useful when the host clang version has no Linux Mull release yet)
#   --build DIR    build directory (default: build-mutate next to the sources)
#   --tests LIST   CTest test executables to build and run the mutants against (default: every
#                  *test executable of the build)
#
# Mull compiles the mutants into the code with a clang plugin and runs each test executable once
# per mutant; a mutant that no test kills "survived" and is reported. Needs clang and Mull built
# for the same LLVM version (mull-runner-<N>, mull-ir-frontend-<N>; Mull's release packages,
# extracted anywhere: set MULL_DIR to the directory that holds bin/ and lib/, or pass --docker).
#
# Long runs: start it decoupled (systemd-run --user …), as the host rules require; measure one
# file first.
set -euo pipefail
source_dir="$(realpath "$(dirname "$(realpath "${BASH_SOURCE[0]}")")/..")"
build_dir="$source_dir/build-mutate"
tests=""
use_docker=0
raw_files=()
while (($#)); do
    case "$1" in
    --docker) use_docker=1; shift ;;
    --build) build_dir="$(realpath -m "$2")"; shift 2 ;;
    --tests) tests="$2"; shift 2 ;;
    -h | --help) sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) raw_files+=("$1"); shift ;;
    esac
done
((${#raw_files[@]})) || { echo "Name at least one source file." >&2; exit 2; }

if ((use_docker)); then
    rel_files=()
    for f in "${raw_files[@]}"; do
        abs="$(realpath "$f")"
        rel="${abs#"$source_dir"/}"
        [[ "$rel" != "$abs" ]] || { echo "File must be inside $source_dir: $f" >&2; exit 2; }
        rel_files+=("/src/$rel")
    done
    mull_base_url="${MULL_BASE_DEB_URL:-https://github.com/mull-project/mull/releases/download/0.39.1/Mull-0.39.1-debian-amd64-13.deb}"
    mull_deb_url="${MULL_DEB_URL:-https://github.com/mull-project/mull/releases/download/0.39.1/Mull-19-0.39.1-LLVM-19.1.7-debian-amd64-13.deb}"
    packages=(
        ca-certificates curl cmake ninja-build clang-19 python3 gettext ffmpeg
        qt6-base-dev qt6-base-dev-tools qt6-image-formats-plugins qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools
        libkf6config-dev libkf6globalaccel-dev libkf6statusnotifieritem-dev libkf6notifications-dev
        libkf6dbusaddons-dev libkf6widgetsaddons-dev libkf6xmlgui-dev libkf6i18n-dev
        qt6-wayland-dev qt6-wayland-dev-tools plasma-wayland-protocols libkpipewire-dev
        liblayershellqtinterface-dev
    )
    exec docker run --rm --cpus 4 --memory 8g \
        -v "${source_dir}:/src:ro" \
        -e DEBIAN_FRONTEND=noninteractive -e QT_QPA_PLATFORM=offscreen \
        -e CLANG=clang-19 -e CLANGXX=clang++-19 \
        debian:trixie bash -c "
            set -euo pipefail
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends ${packages[*]} >/dev/null
            curl -fsSL '${mull_base_url}' -o /tmp/mull-base.deb
            curl -fsSL '${mull_deb_url}' -o /tmp/mull-19.deb
            dpkg -i /tmp/mull-base.deb /tmp/mull-19.deb >/dev/null || apt-get install -f -y -qq >/dev/null
            rm -f /tmp/mull-base.deb /tmp/mull-19.deb
            /src/scripts/mutate.sh --build /build-mutate ${tests:+--tests \"$tests\"} ${rel_files[*]}
        "
fi

files=()
for f in "${raw_files[@]}"; do
    files+=("$(realpath "$f")")
done

clang_bin="${CLANG:-clang}"
clangxx_bin="${CLANGXX:-clang++}"
llvm="$("$clang_bin" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)"
mull_dir="${MULL_DIR:-}"
runner="$(command -v "mull-runner-$llvm" || true)"
[[ -z "$runner" && -n "$mull_dir" ]] && runner="$mull_dir/bin/mull-runner-$llvm"
plugin=""
for candidate in ${mull_dir:+"$mull_dir"/lib} /usr/lib /usr/local/lib "$HOME/.local/lib" "$HOME/.local/opt/mull/lib"; do
    [[ -f "$candidate/mull-ir-frontend-$llvm" ]] && plugin="$candidate/mull-ir-frontend-$llvm" && break
done
if [[ ! -x "$runner" || -z "$plugin" ]]; then
    echo "Mull for LLVM $llvm not found (mull-runner-$llvm, mull-ir-frontend-$llvm). Set MULL_DIR or pass --docker." >&2
    exit 2
fi

# Only the named files get mutants.
mkdir -p "$build_dir/mull-reports"
{
    echo "mutators: [cxx_all]"
    echo "includePaths:"
    for f in "${files[@]}"; do echo "  - '$(printf '%s' "$f" | sed 's/[.[\*^$()+?{|]/\\&/g')'"; done
    echo "timeout: 20000"
} >"$build_dir/mull.yml"
export MULL_CONFIG="$build_dir/mull.yml"
CC="$clang_bin" CXX="$clangxx_bin" cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fpass-plugin=$plugin -g -grecord-command-line" \
    -DCMAKE_CXX_FLAGS="-fpass-plugin=$plugin -g -grecord-command-line" >/dev/null

jobs="$(( $(nproc) > 4 ? 4 : $(nproc) ))"
if [[ -n "$tests" ]]; then
    # shellcheck disable=SC2086
    cmake --build "$build_dir" --parallel "$jobs" --target $tests
else
    cmake --build "$build_dir" --parallel "$jobs"
    tests="$(cd "$build_dir" && ls -1 ./*test 2>/dev/null | xargs -n1 basename)"
fi

status=0
for t in $tests; do
    echo "=== $t"
    QT_QPA_PLATFORM=offscreen "$runner" --reporters IDE --report-dir "$build_dir/mull-reports" \
        --report-name "$t" "$build_dir/$t" || status=$?
    [[ -f "$build_dir/mull-reports/$t.txt" ]] && cat "$build_dir/mull-reports/$t.txt"
done
exit "$status"
