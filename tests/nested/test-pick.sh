# Nested test (#97): hovering frames the window or monitor under the pointer; a click (or a drag
# below the threshold) takes exactly that, as a picture or, in record mode, as a recording.
log="$home/itsova.log"
seen() { grep -c -- "$1" "$log"; }
center=$(window_center kcalc)
[[ -n "$center" ]] || fail "kcalc not listed"
cx=${center%,*}; cy=${center#*,}
echo "kcalc centre $cx,$cy"

# Plain Print overlay: hover kcalc, the frame shows; a click captures the window.
itsova_call OpenOverlay si frozen 0 >/dev/null; sleep 2
fake_bg m:$cx,$cy w:2500 d u
sleep 1.5
shot hover-window
wait_fake; sleep 2
grep -q 'click takes window "KCalc"' "$log" || fail "a click on kcalc did not take the window"
ls "$home"/Pictures/itsova/*window-kcalc* >/dev/null 2>&1 || fail "no window capture written"

# Over the desktop, a small drag (below the threshold) still counts as a click: the monitor.
itsova_call OpenOverlay si frozen 0 >/dev/null; sleep 2
fake m:1700,950 w:300 d m:1702,951 u; sleep 2
grep -q 'click takes monitor "Virtual-0"' "$log" || fail "a click on the desktop did not take the monitor"

# A drag above the threshold draws a selection instead.
itsova_call OpenOverlay si frozen 0 >/dev/null; sleep 2
takes=$(seen 'click takes')
fake m:1500,700 d m:1700,850 u; sleep 1
[[ $(seen 'click takes') -eq $takes ]] || fail "a real drag was taken as a click"
fake 1 1; sleep 1

# Open menu: the Windows drawer is open, a click into the image still takes the window.
itsova_call OpenOverlay si frozen 0 >/dev/null; sleep 2
fake 17; sleep 1 # W
fake m:$cx,$cy w:300 d u; sleep 2
[[ $(seen 'click takes window "KCalc"') -ge 2 ]] || fail "with a menu open the click did not take the window"

# Record mode: a click on kcalc records the window.
itsova_call OpenRecordOverlay >/dev/null; sleep 2
fake m:$cx,$cy w:300 d u; sleep 3
[[ "$(recording_state)" == recording ]] || fail "record-mode click did not start a recording"
itsova_call StopRecording >/dev/null
wait_recording_idle 30 || fail "recording did not finish"
ls "$home"/Pictures/itsova/*window-kcalc*.mp4 >/dev/null 2>&1 || fail "no window recording written"

# Full-screen window: the monitor is taken.
busctl --user call org.kde.kglobalaccel /component/kwin org.kde.kglobalaccel.Component invokeShortcut s \
    "Window Fullscreen" >/dev/null 2>&1
sleep 2
itsova_call OpenOverlay si frozen 0 >/dev/null; sleep 2
monitors=$(seen 'click takes monitor')
fake m:400,300 w:300 d u; sleep 2
[[ $(seen 'click takes monitor') -gt $monitors ]] || fail "a click on a full-screen window did not take the monitor"
echo "ok"
