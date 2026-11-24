#!/usr/bin/env bash
# Measures Print latency (#67) without touching the running desktop session: starts a nested,
# virtual KWin on a private D-Bus session with a throw-away HOME, runs the given glimply binary
# there, triggers its Print shortcut N times through kglobalaccel and prints `glimply latency`.
#
# Usage: scripts/measure-latency.sh [--runs N] [--monitors N] [--json] <path/to/glimply>
#
# The trigger comes from kglobalaccel's invokeShortcut, which carries no key timestamp, so the
# "key" column stays empty here; real key presses on the desktop fill it (see `glimply latency`).
set -euo pipefail

runs=20
monitors=1
json=()
binary=""
while (($#)); do
    case "$1" in
    --runs) runs="$2"; shift 2 ;;
    --monitors) monitors="$2"; shift 2 ;;
    --json) json=(--json); shift ;;
    -h | --help) sed -n '2,9p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) binary="$(realpath "$1")"; shift ;;
    esac
done
[[ -x "$binary" ]] || { echo "glimply binary not found: ${binary:-<none>}" >&2; exit 2; }

if [[ -z "${GLIMPLY_MEASURE_INNER:-}" ]]; then
    exec env GLIMPLY_MEASURE_INNER=1 dbus-run-session -- "${BASH_SOURCE[0]}" --runs "$runs" \
        --monitors "$monitors" "${json[@]}" "$binary"
fi

home="$(mktemp -d "${TMPDIR:-/tmp}/glimply-latency.XXXXXX")"
socket="glimply-latency-$$"
kwin_pid=""
glimply_pid=""
cleanup() {
    [[ -n "$glimply_pid" ]] && kill "$glimply_pid" 2>/dev/null && wait "$glimply_pid" 2>/dev/null
    [[ -n "$kwin_pid" ]] && kill "$kwin_pid" 2>/dev/null && wait "$kwin_pid" 2>/dev/null
    rm -rf -- "$home"
}
trap cleanup EXIT

export HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
    XDG_CACHE_HOME="$home/.cache" XDG_STATE_HOME="$home/.local/state"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$home/Pictures"

kwin_wayland --virtual --no-lockscreen --socket "$socket" --width 1920 --height 1080 \
    --output-count "$monitors" >"$home/kwin.log" 2>&1 &
kwin_pid=$!
for _ in $(seq 100); do
    busctl --user status org.kde.kglobalaccel >/dev/null 2>&1 && [[ -S "$XDG_RUNTIME_DIR/$socket" ]] && break
    sleep 0.1
done

WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland GLIMPLY_LATENCY_BENCH=1 "$binary" >"$home/glimply.log" 2>&1 &
glimply_pid=$!
for _ in $(seq 100); do
    busctl --user call io.github.m00sfett.glimply /Control io.github.m00sfett.glimply.Control \
        LatencyReport >/dev/null 2>&1 && break
    sleep 0.1
done

samples() {
    "$binary" latency --json | python3 -c '
import json, sys
s = json.load(sys.stdin)["samples"]
done = [x for x in s if x.get("fileUs") is not None and x.get("overlayUs") is not None]
print(len(done))'
}

trigger() {
    local before
    before="$(samples)"
    busctl --user call org.kde.kglobalaccel /component/glimply org.kde.kglobalaccel.Component \
        invokeShortcut s print
    for _ in $(seq 200); do
        [[ "$(samples)" -gt "$before" ]] && break
        sleep 0.05
    done
    sleep 0.3
}

# One warm-up press first: the first capture after start pays for lazy initialisation.
for _ in $(seq 0 "$runs"); do
    trigger
done

"$binary" latency --last "$runs" "${json[@]}"
