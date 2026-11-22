#!/usr/bin/env bash
# Builds glimply in Release mode, runs the tests and installs it into a user prefix
# (default ~/.local). Usage: scripts/install.sh [--prefix DIR] [--uninstall]
set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${source_dir}/build-release"
prefix="${HOME}/.local"
uninstall=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --uninstall) uninstall=true; shift ;;
        -h|--help) sed -n '2,3p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ "${uninstall}" == true ]]; then
    manifest="${build_dir}/install_manifest.txt"
    if [[ ! -f "${manifest}" ]]; then
        echo "No install manifest at ${manifest}; nothing to remove." >&2
        exit 1
    fi
    while IFS= read -r file; do
        [[ -n "${file}" && -e "${file}" ]] && rm -v -- "${file}"
    done < "${manifest}"
    rm -fv -- "${HOME}/.config/autostart/io.github.m00sfett.glimply.desktop"
    echo "glimply was removed. Settings in ~/.config/glimplyrc and your captures were kept."
    exit 0
fi

cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}"
cmake --build "${build_dir}" --parallel "$(( $(nproc) > 4 ? 4 : $(nproc) ))"
ctest --test-dir "${build_dir}" --output-on-failure
cmake --install "${build_dir}"

if command -v update-desktop-database >/dev/null; then
    update-desktop-database "${prefix}/share/applications" 2>/dev/null || true
fi
if command -v kbuildsycoca6 >/dev/null; then
    kbuildsycoca6 >/dev/null 2>&1 || true
fi
echo "Installed glimply to ${prefix}. Start it from the application menu or run ${prefix}/bin/glimply."
