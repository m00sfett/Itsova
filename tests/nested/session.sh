#!/usr/bin/env bash
# Nested-KWin acceptance tests (#93). Runs one test script against a glimply binary inside a
# nested, virtual KWin on a private D-Bus session with a throw-away HOME, so the desktop session
# is never touched.
#
# Usage: tests/nested/session.sh [options] <glimply> <fakekey> <test.sh>
#   --monitors N        virtual outputs (default 1)
#   --size WxH          output size (default 1920x1080)
#   --scale F           output scale of the nested KWin
#   --apps "a b"        Wayland apps started before glimply (windows for window tests)
#   --rc "[G]\nK=V"     lines appended to glimplyrc before glimply starts
#   --scheme NAME       colour scheme, e.g. BreezeLight or BreezeDark
#   --out DIR           keep screenshots and files there (default: a temporary directory)
#
# The test script runs with bash, sources tests/nested/lib.sh and gets $socket, $fakekey,
# $kwinshot, $out and $home. Its exit code is the test result (77 = skipped).
set -euo pipefail

monitors=1
size=1920x1080
scale=""
apps=""
rc=""
scheme=""
out=""
positional=()
while (($#)); do
    case "$1" in
    --monitors) monitors="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --scale) scale="$2"; shift 2 ;;
    --apps) apps="$2"; shift 2 ;;
    --rc) rc="$2"; shift 2 ;;
    --scheme) scheme="$2"; shift 2 ;;
    --out) out="$(realpath -m "$2")"; shift 2 ;;
    -h | --help) sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) positional+=("$1"); shift ;;
    esac
done
((${#positional[@]} == 3)) || { sed -n '6,6p' "${BASH_SOURCE[0]}" >&2; exit 2; }
binary="$(realpath "${positional[0]}")"
fakekey="$(realpath "${positional[1]}")"
test_script="$(realpath "${positional[2]}")"
[[ -x "$binary" ]] || { echo "glimply binary not found: $binary" >&2; exit 2; }
for tool in kwin_wayland dbus-run-session busctl python3 magick ffprobe; do
    command -v "$tool" >/dev/null || { echo "skipped: $tool is missing" >&2; exit 77; }
done
python3 -c 'import dbus' 2>/dev/null || { echo "skipped: python-dbus is missing" >&2; exit 77; }

if [[ -z "${GLIMPLY_NESTED_INNER:-}" ]]; then
    # Everything the private session starts must see only the throw-away HOME and no real display,
    # and the private bus must not activate any installed service (it would start the installed
    # glimply with the real HOME instead of the binary under test).
    home="$(mktemp -d "${TMPDIR:-/tmp}/glimply-nested.XXXXXX")"
    [[ -n "$out" ]] || out="$home/out"
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
    setsid env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS GLIMPLY_NESTED_INNER=1 \
        HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
        XDG_CACHE_HOME="$home/.cache" XDG_STATE_HOME="$home/.local/state" QT_QPA_PLATFORM= \
        dbus-run-session --config-file="$home/session.conf" -- "${BASH_SOURCE[0]}" --monitors "$monitors" \
        --size "$size" ${scale:+--scale "$scale"} --apps "$apps" --rc "$rc" ${scheme:+--scheme "$scheme"} --out "$out" \
        "$binary" "$fakekey" "$test_script" &
    inner_pid=$!
    status=0
    wait "$inner_pid" || status=$?
    inner_pid=""
    exit "$status"
fi

home="$HOME"
here="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
socket="glimply-nested-$$"
kwin_pid=""
glimply_pid=""
app_pids=()
stop_pid() {
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
    stop_pid "$glimply_pid"
    for pid in "${app_pids[@]}"; do stop_pid "$pid"; done
    stop_pid "$kwin_pid"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir -p "$out" "$home/.local/share/applications"
# KWin grants restricted interfaces to executables named in desktop files.
printf '[Desktop Entry]\nType=Application\nName=kwinshot\nNoDisplay=true\nExec=%s\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n' \
    "$(realpath "$(command -v python3)")" >"$home/.local/share/applications/kwinshot.desktop"
printf '[Desktop Entry]\nType=Application\nName=fakekey\nNoDisplay=true\nExec=%s\nX-KDE-Wayland-Interfaces=org_kde_kwin_fake_input\n' \
    "$fakekey" >"$home/.local/share/applications/fakekey.desktop"
printf '[Desktop Entry]\nType=Application\nName=glimply-under-test\nNoDisplay=true\nExec=%s\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n' \
    "$binary" >"$home/.local/share/applications/glimply-under-test.desktop"
[[ -n "$rc" ]] && printf '%b\n' "$rc" >>"$home/.config/glimplyrc"
if [[ -n "$scheme" ]]; then
    cp "/usr/share/color-schemes/${scheme}.colors" "$home/.config/kdeglobals"
    printf '\n[General]\nColorScheme=%s\n' "$scheme" >>"$home/.config/kdeglobals"
fi
# Tests never open the first-start hint.
printf '[General]\nOverlayFirstHintShown=true\n' >>"$home/.config/glimplyrc"

kwin_wayland --virtual --no-lockscreen ${scale:+--scale "$scale"} --socket "$socket" --width "${size%x*}" \
    --height "${size#*x}" --output-count "$monitors" >"$home/kwin.log" 2>&1 &
kwin_pid=$!
for _ in $(seq 100); do
    busctl --user status org.kde.kglobalaccel >/dev/null 2>&1 && [[ -S "$XDG_RUNTIME_DIR/$socket" ]] && break
    sleep 0.1
done
for app in $apps; do
    WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland "$app" >"$home/app-$(basename "$app").log" 2>&1 &
    app_pids+=("$!")
done
[[ -n "$apps" ]] && sleep 3
QT_FORCE_STDERR_LOGGING=1 WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland LANGUAGE=en LANG=en_US.UTF-8 \
    "$binary" >"$home/glimply.log" 2>&1 &
glimply_pid=$!
ready=0
for _ in $(seq 100); do
    if busctl --user --auto-start=no call io.github.m00sfett.glimply /Control \
        io.github.m00sfett.glimply.Control LatencyReport >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done
if ((!ready)); then
    echo "glimply did not come up"; cat "$home/glimply.log"; exit 1
fi

status=0
socket="$socket" fakekey="$fakekey" kwinshot="$here/kwinshot.py" out="$out" home="$home" \
    GLIMPLY_NESTED_LIB="$here/lib.sh" bash -c 'source "$GLIMPLY_NESTED_LIB"; source "$0"' "$test_script" || status=$?
mkdir -p "$out/files" && cp -r "$home/Pictures/." "$out/files/" 2>/dev/null || true
if ((status != 0)); then
    echo "--- glimply.log (test exited with $status)"; tail -n 80 "$home/glimply.log"
fi
cp "$home/glimply.log" "$out/glimply.log" 2>/dev/null || true
exit "$status"
