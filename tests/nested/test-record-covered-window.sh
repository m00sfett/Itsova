# Nested test (#90): a recording of a static window that another window covers completely keeps
# running until it is stopped, and the file is as long as the recording.
seconds=${GLIMPLY_TEST_SECONDS:-60}
win=$(first_window_id)
[[ -n "$win" ]] || fail "no window listed"
result=$(glimply_call StartRecording si "window:$win" 0)
[[ "$result" == 's ""' ]] || fail "StartRecording: $result"
sleep 2
[[ "$(recording_state)" == recording ]] || fail "not recording: $(glimply_call RecordingState)"
# Cover the recorded window completely: a second window, maximized.
WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland kwrite >/dev/null 2>&1 &
cover=$!
sleep 3
busctl --user call org.kde.kglobalaccel /component/kwin org.kde.kglobalaccel.Component invokeShortcut s \
    "Window Maximize" >/dev/null || fail "could not maximize the covering window"
sleep 1
shot covered
sleep $((seconds - 6))
state=$(recording_state)
glimply_call StopRecording >/dev/null
wait_recording_idle 30 || fail "did not finish"
kill "$cover" 2>/dev/null
[[ "$state" == recording ]] || fail "recording ended on its own before the stop (state $state)"
file=$(newest_file '*.mp4')
[[ -n "$file" ]] || fail "no file"
duration=$(media_duration "$file")
echo "duration $duration s"
awk -v d="$duration" -v s="$seconds" 'BEGIN { exit !(d >= s - 1.5) }' || fail "file is $duration s, expected ~$seconds s"
grep -q "stop requested by \"D-Bus StopRecording\"" "$home/glimply.log" || fail "stop origin not logged"
echo "ok"
