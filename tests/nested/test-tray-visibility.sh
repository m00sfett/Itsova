# Nested test (#123): with "Tray icon: Only while recording" no status notifier item is registered
# until a recording starts, and it goes away when the recording ends. A plain second start of
# itsova opens the settings.
start_fake_sni_watcher
# Registered items whose bus connection is still there (KStatusNotifierItem registers with a
# connection of its own and drops it when the item goes).
sni_count() {
    local n=0 name
    for name in $(awk '{ print $2 }' "$out/sni.log" | sort -u); do
        busctl --user status "$name" >/dev/null 2>&1 && n=$((n + 1))
    done
    echo "$n"
}
itsova_pid=$(busctl --user status io.github.m00sfett.itsova 2>/dev/null | grep -oE '^PID=[0-9]+' | cut -d= -f2)
[[ -n "$itsova_pid" ]] || fail "no itsova pid"
sleep 1
(( $(sni_count) == 0 )) || fail "tray item registered while idle"
win=$(first_window_id)
[[ -n "$win" ]] || fail "no window listed"
result=$(itsova_call StartRecording si "window:$win" 0)
[[ "$result" == 's ""' ]] || fail "StartRecording: $result"
sleep 2
[[ "$(recording_state)" == recording ]] || fail "not recording"
(( $(sni_count) == 1 )) || fail "no tray item while recording ($(sni_count))"
itsova_call StopRecording >/dev/null
wait_recording_idle 30 || fail "did not finish"
sleep 1
(( $(sni_count) == 0 )) || fail "tray item still registered after the recording"
grep -q "tray icon shown" "$home/itsova.log" || fail "show not logged"
grep -q "tray icon hidden" "$home/itsova.log" || fail "hide not logged"
# Second start without arguments.
WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland "$binary" >/dev/null 2>&1
sleep 2
grep -q "second start without arguments: opening the settings" "$home/itsova.log" || fail "second start did not open the settings"
shot settings
echo "ok"
