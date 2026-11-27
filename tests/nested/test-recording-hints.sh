# Nested test (#125): recording hints stay out of the video. A region recording shows its hint
# beside the region at once; a full-monitor recording shows it first and starts once it has gone
# (the first frame has no hint in it); the stop hint names the file; pause and resume say so.
log="$home/itsova.log"
hint_rect() { # the rectangle of the last hint with this title: "x y w h"
    grep "recording hint \"$1" "$log" | tail -1 | grep -oE 'QRect\([0-9]+,[0-9]+ [0-9]+x[0-9]+\)' |
        sed -E 's/QRect\(([0-9]+),([0-9]+) ([0-9]+)x([0-9]+)\)/\1 \2 \3 \4/'
}
pixel() { magick "$1" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info:; }
far() { # two "r,g,b" differ clearly
    awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, ","); split(b, y, ","); d = 0
        for (i = 1; i <= 3; i++) { v = x[i] - y[i]; d += v < 0 ? -v : v }; exit !(d > 60) }'
}

# 1. Region on the only monitor: the hint is beside it, shown right away.
itsova_call StartRecording si "region:100,100,800,600" 0 >/dev/null
sleep 0.5
read -r hx hy hw hh < <(hint_rect "Recording")
[[ -n "$hx" ]] || fail "no start hint for the region"
echo "region hint at $hx,$hy ${hw}x$hh"
((hx > 900 || hy > 700 || hx + hw < 100 || hy + hh < 100)) || fail "the hint overlaps the recorded region"
grep -q "recording waits for its start hint" "$log" && fail "a region with room beside it waited"
shot region-hint
for _ in $(seq 20); do [[ "$(recording_state)" == recording ]] && break; sleep 0.25; done
[[ "$(recording_state)" == recording ]] || fail "region recording did not start"
sleep 1
itsova_call StopRecording >/dev/null
wait_recording_idle 20 || fail "region recording did not finish"
sleep 0.5
file=$(newest_file '*.mp4')
grep "recording hint \"Recording saved\"" "$log" | grep -q . || fail "no stop hint"
grep -q "saved.*$(basename "$file")" "$log" || fail "no file saved"

# 2. The whole monitor: the hint first, the stream after it has gone.
itsova_call StartRecording si "screen:Virtual-0" 0 >/dev/null
sleep 0.4
grep -q "recording waits for its start hint" "$log" || fail "a full-monitor recording did not wait"
read -r hx hy hw hh < <(hint_rect "Recording Virtual-0 starts")
[[ -n "$hx" ]] || fail "no start hint before the full recording"
shot full-hint
cx=$((hx + 20)); cy=$((hy + hh / 2))
with_hint=$(pixel "$out/full-hint.png" $cx $cy)
for _ in $(seq 30); do [[ "$(recording_state)" == recording ]] && break; sleep 0.25; done
[[ "$(recording_state)" == recording ]] || fail "full recording did not start after the hint"
grep -q "start hint gone, starting the recording" "$log" || fail "start not logged after the hint"
if [[ "$(itsova_call RecordingState)" == *'canPause\":true'* ]]; then
    itsova_call PauseRecording b true >/dev/null
    sleep 1
    grep -q 'recording hint "Recording paused' "$log" || fail "no pause hint"
    itsova_call PauseRecording b false >/dev/null
    sleep 0.3
    grep -q 'recording hint "Recording resumes' "$log" || fail "no resume hint before resuming"
    sleep 2.5
fi
sleep 1
itsova_call StopRecording >/dev/null
wait_recording_idle 20 || fail "full recording did not finish"
file=$(newest_file '*.mp4')
ffmpeg -v error -y -i "$file" -frames:v 1 "$out/full-first-frame.png" || fail "no first frame"
in_video=$(pixel "$out/full-first-frame.png" $cx $cy)
echo "hint pixel $with_hint, first frame $in_video"
far "$with_hint" "$in_video" || fail "the first frame shows the hint"
echo "ok"
