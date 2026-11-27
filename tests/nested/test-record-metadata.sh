# Nested test (#103): a monitor recording and a window recording carry monitor, position and
# scale as the "itsova" tag, read back with ffprobe.
record() {
    itsova_call StartRecording si "$1" 0 >/dev/null
    for _ in $(seq 20); do [[ "$(recording_state)" == recording ]] && break; sleep 0.25; done
    sleep 1.5
    itsova_call StopRecording >/dev/null
    wait_recording_idle 20 || fail "recording $1 did not finish"
}
tag() { ffprobe -v error -show_entries format_tags=itsova -of default=nw=1:nk=1 "$1"; }
record "screen:Virtual-0"
file=$(newest_file '*Virtual-0.mp4')
meta=$(tag "$file")
echo "monitor: $meta"
[[ "$meta" == *'"kind":"monitor"'* ]] || fail "monitor recording has no metadata"
[[ "$meta" == *'"connector":"Virtual-0"'* ]] || fail "no monitor identity"
[[ "$meta" == *'"video":[0,0,1920,1080]'* ]] || fail "monitor rectangle in video pixels missing"
win=$(first_window_id)
record "window:$win"
file=$(newest_file '*window-kcalc.mp4')
meta=$(tag "$file")
echo "window: $meta"
[[ "$meta" == *'"kind":"window"'* ]] || fail "window recording has no metadata"
[[ "$meta" == *'"windowRect":['* ]] || fail "window rectangle missing"
echo "ok"
