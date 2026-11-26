# Nested test (#102): with the mouse button held, arrow keys move the dragged corner by one native
# pixel (Shift: ten); small mouse moves after a key are ignored; the saved file includes the nudge.
log="$home/glimply.log"
glimply_call OpenOverlay si frozen 0 >/dev/null || fail "OpenOverlay failed"
sleep 2
# Draw 200 × 100 from (500,400) to (700,500); keep the button down while the keys come.
fake_bg m:500,400 d m:600,450 m:700,500 w:1200 m:704,503 w:2200 u
sleep 0.9
fake 106 106 106 108 # Right ×3, Down ×1 → 203 × 101
sleep 0.3
fake 42+106          # Shift+Right → 213 × 101
sleep 1.2
shot nudged
wait_fake
sleep 1
fake 28 # Enter saves the selection
sleep 2
file=$(newest_file '*selection*.png')
[[ -n "$file" ]] || fail "no selection file"
size=$(magick identify -format '%wx%h' "$file")
echo "saved $size"
# The jitter to (704,503) stayed inside the dead zone, so only the keys count.
[[ "$size" == "213x101" ]] || fail "expected 213x101 (200x100 + 13/1 native px), got $size"
echo "ok"
