# Helpers for nested-KWin tests (#93), sourced by session.sh before each test script.
# shellcheck shell=bash

# glimply_call <Method> [signature args...]: one D-Bus call to the glimply under test.
glimply_call() {
    busctl --user --auto-start=no call io.github.m00sfett.glimply /Control io.github.m00sfett.glimply.Control "$@"
}

# fake <step>...: keys and pointer through KWin's fake input (see fakekey.c for the step syntax).
fake() {
    WAYLAND_DISPLAY="$socket" "$fakekey" "$@" >/dev/null 2>&1
}

# shot <name>: screenshot of the whole nested workspace into $out/<name>.png.
shot() {
    python3 "$kwinshot" "$out/$1.png" >/dev/null
}

# recording_state: the "state" field of RecordingState (idle, starting, recording, paused, finishing).
recording_state() {
    glimply_call RecordingState | grep -oE '\\"state\\":\\"[a-z]+' | grep -oE '[a-z]+$'
}

# wait_recording_idle [seconds]: waits until the recorder is idle again; fails on timeout.
wait_recording_idle() {
    local limit=$((${1:-30} * 2))
    for _ in $(seq "$limit"); do
        [[ "$(recording_state)" == idle ]] && return 0
        sleep 0.5
    done
    echo "recorder did not become idle: $(glimply_call RecordingState)"
    return 1
}

# first_window_id: the id of the first window glimply lists.
first_window_id() {
    glimply_call ListWindows | grep -oE '\{[0-9a-f-]+\}' | head -1
}

# media_duration <file>: duration in seconds (ffprobe).
media_duration() {
    ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$1"
}

# newest_file <glob>: the newest file in the capture folder matching the glob.
newest_file() {
    ls -t "$home"/Pictures/glimply/$1 2>/dev/null | head -1
}

fail() {
    echo "FAIL: $*"
    exit 1
}

# window_center <appName>: "X,Y" of the centre of the first listed window of that app.
window_center() {
    glimply_call ListWindows | sed 's/^s "//; s/"$//; s/\\"/"/g' | python3 -c '
import json, sys
app = sys.argv[1]
for w in json.load(sys.stdin):
    if w["appName"] == app:
        print("%d,%d" % (w["x"] + w["width"] // 2, w["y"] + w["height"] // 2))
        break' "$1"
}

# fake_bg <step>...: one fake-input run in the background, so the pointer stays where the steps put
# it while the test takes screenshots; wait_fake waits for it. (Every fakekey run adds and removes
# its own device, and clients may drop the pointer when it goes away.)
fake_bg() {
    WAYLAND_DISPLAY="$socket" "$fakekey" "$@" >/dev/null 2>&1 &
    fake_pid=$!
}
wait_fake() {
    wait "$fake_pid" 2>/dev/null
}

# start_fake_notifications [prefix]: a notification server that logs to $out/notifications.log and
# presses the first action of notifications whose summary starts with prefix.
start_fake_notifications() {
    : >"$out/notifications.log" # --out may be reused between runs
    python3 "$(dirname "$kwinshot")/fakenotify.py" "$out/notifications.log" "${1:-}" >/dev/null 2>&1 &
    for _ in $(seq 50); do
        busctl --user status org.freedesktop.Notifications >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    fail "fake notification server did not start"
}

# start_fake_sni_watcher: a StatusNotifierWatcher with a host (#123), logging to $out/sni.log, so
# the tray item registers on the private bus.
start_fake_sni_watcher() {
    : >"$out/sni.log"
    python3 "$(dirname "$kwinshot")/fakesniwatcher.py" "$out/sni.log" >/dev/null 2>&1 &
    for _ in $(seq 50); do
        busctl --user status org.kde.StatusNotifierWatcher >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    fail "fake status notifier watcher did not start"
}
