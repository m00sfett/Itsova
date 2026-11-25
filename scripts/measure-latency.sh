#!/usr/bin/env bash
# Measures Print latency (#67) without touching the running desktop session: starts a nested,
# virtual KWin on a private D-Bus session with a throw-away HOME, runs the given glimply binary
# there, triggers its Print shortcut N times through kglobalaccel and prints `glimply latency`.
#
# Usage: scripts/measure-latency.sh [--runs N] [--monitors N] [--pins] [--json] <path/to/glimply>
#
# --pins pins the warm-up capture to the screen first, so every run waits for a visible pin.
#
# Times start when glimply receives the trigger: KDE sends no key timestamp on Wayland, so the time
# from the key press to the trigger is not measurable, here or on the desktop.
set -euo pipefail

runs=20
monitors=1
json=()
pins=()
binary=""
while (($#)); do
    case "$1" in
    --runs) runs="$2"; shift 2 ;;
    --monitors) monitors="$2"; shift 2 ;;
    --json) json=(--json); shift ;;
    --pins) pins=(--pins); shift ;;
    -h | --help) sed -n '2,11p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) binary="$(realpath "$1")"; shift ;;
    esac
done
[[ -x "$binary" ]] || { echo "glimply binary not found: ${binary:-<none>}" >&2; exit 2; }

if [[ -z "${GLIMPLY_MEASURE_INNER:-}" ]]; then
    # Everything the private session starts must see only the throw-away HOME and no real display,
    # and the private bus must not activate any installed service (it would start the installed
    # glimply with the real HOME instead of the binary under test).
    home="$(mktemp -d "${TMPDIR:-/tmp}/glimply-latency.XXXXXX")"
    inner_pid=""
    outer_cleanup() {
        if [[ -n "$inner_pid" ]]; then
            kill -TERM -- "-$inner_pid" 2>/dev/null || true
            sleep 1
            kill -KILL -- "-$inner_pid" 2>/dev/null || true
            wait "$inner_pid" 2>/dev/null || true
        fi
        rm -rf -- "$home"
    }
    trap outer_cleanup EXIT
    trap 'exit 130' INT TERM
    sed '/<standard_session_servicedirs/d' /usr/share/dbus-1/session.conf >"$home/session.conf"
    mkdir -p "$home/.config" "$home/.local/share" "$home/.cache" "$home/.local/state" "$home/Pictures"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS GLIMPLY_MEASURE_INNER=1 \
        HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
        XDG_CACHE_HOME="$home/.cache" XDG_STATE_HOME="$home/.local/state" \
        dbus-run-session --config-file="$home/session.conf" -- "${BASH_SOURCE[0]}" --runs "$runs" \
        --monitors "$monitors" "${pins[@]}" "${json[@]}" "$binary" &
    inner_pid=$!
    status=0
    wait "$inner_pid" || status=$?
    inner_pid=""
    exit "$status"
fi

home="$HOME"
socket="glimply-latency-$$"
kwin_pid=""
glimply_pid=""
stop() {
    local pid="$1"
    [[ -n "$pid" ]] || return 0
    kill -TERM "$pid" 2>/dev/null || return 0
    for _ in $(seq 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
cleanup() {
    stop "$glimply_pid"
    stop "$kwin_pid"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

kwin_wayland --virtual --no-lockscreen --socket "$socket" --width 1920 --height 1080 \
    --output-count "$monitors" >"$home/kwin.log" 2>&1 &
kwin_pid=$!
for _ in $(seq 100); do
    busctl --user status org.kde.kglobalaccel >/dev/null 2>&1 && [[ -S "$XDG_RUNTIME_DIR/$socket" ]] && break
    sleep 0.1
done

WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland GLIMPLY_LATENCY_BENCH=1 "$binary" >"$home/glimply.log" 2>&1 &
glimply_pid=$!
glimply_ready=""
for _ in $(seq 100); do
    busctl --user --auto-start=no call io.github.m00sfett.glimply /Control \
        io.github.m00sfett.glimply.Control LatencyReport >/dev/null 2>&1 && glimply_ready=1 && break
    kill -0 "$glimply_pid" 2>/dev/null || break
    sleep 0.1
done
if [[ -z "$glimply_ready" ]]; then
    echo "glimply did not start in the nested session:" >&2
    tail -20 "$home/glimply.log" >&2
    exit 1
fi

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
    busctl --user --auto-start=no call org.kde.kglobalaccel /component/glimply \
        org.kde.kglobalaccel.Component invokeShortcut s print
    for _ in $(seq 200); do
        [[ "$(samples)" -gt "$before" ]] && break
        sleep 0.05
    done
    sleep 0.3
}

# One warm-up press first: the first capture after start pays for lazy initialisation.
trigger
if ((${#pins[@]})); then
    busctl --user --auto-start=no call io.github.m00sfett.glimply /Control \
        io.github.m00sfett.glimply.Control PinLastCapture
    sleep 1
fi
for _ in $(seq 1 "$runs"); do
    trigger
done

"$binary" latency --last "$runs" "${json[@]}"
