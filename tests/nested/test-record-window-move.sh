# Nested test (#122): moving a recorded window does not end the recording. Content changes after
# the move are in the video, and the file is as long as the recording (last frame held).
geom() {
    glimply_call ListWindows | sed 's/^s "//; s/"$//; s/\\"/"/g' | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["appName"] == "kcalc":
        print(w["x"], w["y"], w["width"], w["height"])
        break'
}
type_digits() { for _ in $(seq "$1"); do fake 2 3 4 w:300; done; }
win=$(first_window_id)
[[ -n "$win" ]] || fail "no window listed"
read -r x y w h < <(geom)
cx=$((x + w / 2))
fake m:$cx,$((y + h / 2)) d u # focus kcalc
result=$(glimply_call StartRecording si "window:$win" 0)
[[ "$result" == 's ""' ]] || fail "StartRecording: $result"
started=$(date +%s.%N)
sleep 2
[[ "$(recording_state)" == recording ]] || fail "not recording: $(glimply_call RecordingState)"
type_digits 2
# Drag the title bar.
ty=$((y + 12))
fake m:$cx,$ty d m:$((cx + 60)),$((ty + 40)) m:$((cx + 200)),$((ty + 120)) w:500 u
read -r x2 y2 _ _ < <(geom)
[[ "$x2,$y2" != "$x,$y" ]] || fail "window did not move ($x,$y)"
moved=$(date +%s.%N)
sleep 2
type_digits 4
sleep 2
state=$(recording_state)
stopped=$(date +%s.%N)
glimply_call StopRecording >/dev/null
wait_recording_idle 30 || fail "did not finish"
[[ "$state" == recording ]] || fail "recording ended on its own after the move (state $state)"
file=$(newest_file '*.mp4')
[[ -n "$file" ]] || fail "no file"
after=$(awk -v m="$moved" -v s="$started" 'BEGIN { printf "%.2f", m - s + 1.5 }')
frames_after=$(ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$file" |
    awk -v a="$after" '$1 > a { n++ } END { print n + 0 }')
echo "frames after ${after}s: $frames_after"
((frames_after >= 5)) || fail "no content changes after the move in the video"
duration=$(media_duration "$file")
length=$(awk -v a="$stopped" -v s="$started" 'BEGIN { printf "%.2f", a - s }')
echo "duration $duration s, recorded ~$length s"
awk -v d="$duration" -v l="$length" 'BEGIN { exit !(d >= l - 1.5) }' || fail "file is $duration s, expected ~$length s"
echo "ok"
