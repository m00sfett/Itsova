# Nested test (#90): when the recorded window closes, the recording keeps what it has, ends, and
# says why instead of ending silently.
win=$(first_window_id)
[[ -n "$win" ]] || fail "no window listed"
result=$(glimply_call StartRecording si "window:$win" 0)
[[ "$result" == 's ""' ]] || fail "StartRecording: $result"
sleep 3
[[ "$(recording_state)" == recording ]] || fail "not recording: $(glimply_call RecordingState)"
pkill -x kcalc || fail "kcalc not running"
wait_recording_idle 30 || fail "did not finish after the window closed"
grep -q "stream closed by KWin" "$home/glimply.log" || fail "stream close not logged"
grep -q "recording ended early: \"The recorded window was closed.\"" "$home/glimply.log" || fail "no reason given"
file=$(newest_file '*.mp4')
[[ -n "$file" ]] || fail "nothing kept"
duration=$(media_duration "$file")
echo "duration $duration s"
awk -v d="$duration" 'BEGIN { exit !(d >= 2) }' || fail "file is only $duration s"
echo "ok"
