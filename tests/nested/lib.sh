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
