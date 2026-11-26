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
    # Stop glimply and give the Print key back to its previous owner (usually Spectacle) first.
    pkill -u "$(id -u)" -x glimply || true
    for _ in $(seq 50); do pgrep -u "$(id -u)" -x glimply >/dev/null || break; sleep 0.1; done
    if [[ -x "${prefix}/bin/glimply" ]]; then
        "${prefix}/bin/glimply" --release-print-key || echo "Note: could not hand the Print key back." >&2
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
# Nested-KWin tests (label "nested") take minutes and are run by developers, not by the installer.
ctest --test-dir "${build_dir}" --output-on-failure -LE nested
widget_metadata="${prefix}/share/plasma/plasmoids/io.github.m00sfett.glimply.widget/metadata.json"
widget_before="$(sha256sum "${widget_metadata}" 2>/dev/null || true)"

cmake --install "${build_dir}"
# Before 0.11.0 the notification events were installed under a name KNotification never looked up.
rm -f -- "${prefix}/share/knotifications6/glimply.notifyrc"

widget_after="$(sha256sum "${widget_metadata}" 2>/dev/null || true)"

if command -v update-desktop-database >/dev/null; then
    update-desktop-database "${prefix}/share/applications" 2>/dev/null || true
fi
# Let the session bus see the new D-Bus activation file without a new login.
if command -v busctl >/dev/null; then
    busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null; then
    kbuildsycoca6 >/dev/null 2>&1 || true
fi
# Replace a running older glimply and start the new one (through D-Bus activation), so the
# installation works right away without logging out.
if pgrep -u "$(id -u)" -x glimply >/dev/null; then
    pkill -u "$(id -u)" -x glimply || true
    for _ in $(seq 50); do pgrep -u "$(id -u)" -x glimply >/dev/null || break; sleep 0.1; done
fi
if command -v busctl >/dev/null; then
    busctl --user call io.github.m00sfett.glimply /Control io.github.m00sfett.glimply.Control HasLastCapture >/dev/null 2>&1 \
        || echo "Note: glimply could not be started through D-Bus; start it from the application menu."
fi

# Plasma's system tray only picks up new or changed widgets when the shell starts. Reload it once
# in that case, so the glimply widget appears without any manual configuration.
if [[ "${widget_before}" != "${widget_after}" ]] && systemctl --user is-active --quiet plasma-plasmashell 2>/dev/null; then
    echo "Reloading the Plasma shell once so the glimply widget appears in the system tray..."
    systemctl --user restart plasma-plasmashell || true
fi

echo "Installed glimply to ${prefix}. It is running and starts automatically at login."
