# Nested test (#127): resizing a recorded window preserves aspect ratio via letterboxing on the
# initial canvas size instead of stretching the maximized window into the initial dimensions.
geom() {
    itsova_call ListWindows | sed 's/^s "//; s/"$//; s/\\"/"/g' | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["appName"] == "kcalc":
        print(w["x"], w["y"], w["width"], w["height"])
        break'
}
type_digits() { for _ in $(seq "$1"); do fake 2 3 4 w:250; done; }
win=$(first_window_id)
[[ -n "$win" ]] || fail "no window listed"
read -r x y w h < <(geom)
cx=$((x + w / 2))
fake m:$cx,$((y + h / 2)) d u
result=$(itsova_call StartRecording si "window:$win" 0)
[[ "$result" == 's ""' ]] || fail "StartRecording: $result"
sleep 1.5
[[ "$(recording_state)" == recording ]] || fail "not recording"
type_digits 2
# Maximize via double-click on title bar.
fake m:$cx,$((y + 12)) d u w:80 d u w:400
read -r x2 y2 w2 h2 < <(geom)
[[ "$w2" -gt "$w" ]] || fail "window did not maximize ($w,$h -> $w2,$h2)"
type_digits 3
sleep 1.5
# Restore via double-click on title bar.
fake m:$((x2 + w2 / 2)),$((y2 + 12)) d u w:80 d u w:400
type_digits 2
sleep 1
itsova_call StopRecording >/dev/null
wait_recording_idle 30 || fail "did not finish"
file=$(newest_file '*.mp4')
[[ -n "$file" ]] || fail "no file"

# Extract a frame from the maximized segment (~3.2 s) and verify black letterbox bars at top and
# bottom while the center contains non-black window pixels.
frame="$out/maximized-frame.png"
ffmpeg -v error -y -ss 3.2 -i "$file" -frames:v 1 "$frame"
if ! python3 -c 'import PIL' 2>/dev/null; then
    top_mean=$(magick "$frame" -crop "100%x30+0+0" +repage -format "%[fx:mean]" info:)
    bot_mean=$(magick "$frame" -gravity South -crop "100%x30+0+0" +repage -format "%[fx:mean]" info:)
    mid_mean=$(magick "$frame" -gravity Center -crop "60%x40%+0+0" +repage -format "%[fx:mean]" info:)
    echo "letterbox means: top=$top_mean mid=$mid_mean bot=$bot_mean"
    awk -v t="$top_mean" -v b="$bot_mean" -v m="$mid_mean" 'BEGIN { exit !(t < 0.02 && b < 0.02 && m > 0.05) }' \
        || fail "expected black letterbox bars (top=$top_mean, bot=$bot_mean) around content (mid=$mid_mean)"
else
    python3 - "$frame" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
w, h = im.size
def mean(box):
    crop = im.crop(box)
    data = list(crop.getdata())
    return sum(data) / (len(data) * 255.0)
t = mean((0, 0, w, 30))
b = mean((0, h - 30, w, h))
m = mean((int(w * 0.2), int(h * 0.3), int(w * 0.8), int(h * 0.7)))
print(f"letterbox means: top={t:.4f} mid={m:.4f} bot={b:.4f}")
assert t < 0.02 and b < 0.02 and m > 0.05, f"letterbox check failed: top={t} mid={m} bot={b}"
PY
fi
echo "ok"
