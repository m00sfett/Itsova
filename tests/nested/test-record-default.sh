# Nested test (#124): pin a target in the record overlay with the keyboard, then the recording
# shortcut records exactly that target and stops it on the second press. Pinning Active Window
# makes the shortcut record the focused window.
# Linux keycodes: Right 106, Down 108, Escape 1, P 25.
log="$home/itsova.log"
seen() { grep -c -- "$1" "$log"; }
shortcut() {
    busctl --user call org.kde.kglobalaccel /component/itsova org.kde.kglobalaccel.Component invokeShortcut s \
        record-toggle >/dev/null || fail "could not invoke the recording shortcut"
}
# Without a default the shortcut opens the record overlay.
shortcut
sleep 2
[[ $(seen 'record overlay requested') -ge 1 ]] || fail "no default: the shortcut did not open the overlay"
# Monitors menu, first entry (the only monitor), P pins it.
fake 106 106 108
sleep 1
shot monitors-focused
fake 25
sleep 1
[[ $(seen 'default recording target "screen:') -ge 1 ]] || fail "P did not pin the monitor"
shot monitor-pinned
fake 1 1
sleep 1
grep -q '^DefaultTarget=screen:' "$home/.config/itsovarc" || fail "default not saved"
shortcut
sleep 3
[[ "$(recording_state)" == recording ]] || fail "the shortcut did not record the default: $(itsova_call RecordingState)"
grep -q 'start requested for "screen:' "$log" || fail "not the pinned monitor"
shortcut
wait_recording_idle 30 || fail "the second press did not stop"

# Active Window: Windows menu (record mode), its first entry, P.
itsova_call OpenRecordOverlay >/dev/null
sleep 2
fake 106 106 106 108
sleep 1
fake 25
sleep 1
[[ $(seen 'default recording target "active-window"') -ge 1 ]] || fail "P did not pin Active Window"
shot active-window-pinned
fake 1 1
sleep 1
read -r cx cy < <(window_center kcalc | tr , ' ')
fake m:$cx,$cy d u # focus kcalc
sleep 1
shortcut
sleep 3
[[ "$(recording_state)" == recording ]] || fail "Active Window was not recorded"
grep -q 'start requested for "window:{' "$log" || fail "Active Window did not resolve to a window"
shortcut
wait_recording_idle 30 || fail "did not stop"
echo "ok"
