# Nested test (#95), two monitors: 2 opens the Monitors drawer, hovering "All Monitors" frames
# both monitors, its digit captures all monitors; the record-mode panel opens without a drawer.
glimply_call OpenOverlay si frozen 0 >/dev/null || fail "OpenOverlay failed"
sleep 2
fake 3 # "2"
sleep 1
grep -q 'drawer 0 -> 9' "$home/glimply.log" || fail "2 did not open the Monitors drawer"
shot monitors-drawer
# Monitor 2's left edge before and after hovering "All Monitors" (third row of the drawer).
magick "$out/monitors-drawer.png" -crop 12x900+1600+0 +repage "$out/m2-before.png"
row_y=$(( ${GLIMPLY_ROW3_Y:-168} ))
fake m:600,$row_y w:400
shot all-hover
magick "$out/all-hover.png" -crop 12x900+1600+0 +repage "$out/m2-after.png"
diff_pixels=$(magick compare -metric AE "$out/m2-before.png" "$out/m2-after.png" null: 2>&1 | cut -d' ' -f1)
echo "monitor 2 edge pixels changed by the hover: $diff_pixels"
[[ "${diff_pixels%.*}" -gt 100 ]] || fail "All Monitors hover does not frame the second monitor"
fake 4 # "3": All Monitors
sleep 2
grep -q 'session end.*_all' "$home/glimply.log" || fail "the All Monitors entry did not capture all monitors"
glimply_call OpenRecordOverlay >/dev/null
sleep 2
tail -n 3 "$home/glimply.log" | grep -q 'drawer' && fail "the record-mode panel opened a drawer"
fake 1
echo "ok"
