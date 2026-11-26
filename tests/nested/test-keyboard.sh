# Nested test (#96), two monitors: the panel and its menus work with the keyboard alone.
# Linux keycodes: Right 106, Left 105, Down 108, Up 103, Enter 28, Escape 1.
log="$home/glimply.log"
seen() { grep -c -- "$1" "$log"; }
glimply_call OpenOverlay si frozen 0 >/dev/null || fail "OpenOverlay failed"
sleep 2
# Right focuses This Monitor, Right moves to Monitors, Down opens it and focuses the first entry.
fake 106 106 108
sleep 1
[[ $(seen 'drawer 0 -> 9') -ge 1 ]] || fail "Down did not open the Monitors menu"
# Down to the second monitor; its outline shows like on hover.
fake 108
sleep 1
shot monitors-entry-focused
# Right switches to the Windows menu even with an entry focused; Up on its top entry returns.
fake 106
sleep 1
[[ $(seen 'drawer 9 -> 1') -ge 1 ]] || fail "Right did not switch to the Windows menu"
fake 103
sleep 1
[[ $(seen 'drawer 1 -> 0') -ge 1 ]] || fail "Up on the top entry did not return to the panel"
# Back to Monitors, into it, second entry, Enter captures that monitor.
fake 105 108 108 28
sleep 2
grep -q 'session end.*Virtual-1' "$log" || fail "Enter on the second monitor did not capture it"

# New overlay: Left focuses Close (no wrap from the start), Enter closes.
glimply_call OpenOverlay si frozen 0 >/dev/null
sleep 2
ends=$(seen 'session end')
fake 105 28
sleep 1
[[ $(seen 'session end') -gt $ends ]] || fail "Left + Enter did not close the overlay"

# Escape levels: Record menu open with an entry focused → Escape → panel → Escape → menu was
# closed already, so the overlay closes.
glimply_call OpenOverlay si frozen 0 >/dev/null
sleep 2
ends=$(seen 'session end')
fake 106 106 106 106 106 108
sleep 1
[[ $(seen 'drawer 0 -> 8') -ge 1 ]] || fail "the Record menu did not open from the keyboard"
fake 1
sleep 1
[[ $(seen 'drawer 8 -> 0') -ge 1 ]] || fail "Escape did not close the Record menu"
[[ $(seen 'session end') -eq $ends ]] || fail "the first Escape closed the overlay"
fake 1
sleep 1
[[ $(seen 'session end') -gt $ends ]] || fail "the second Escape did not close the overlay"
# A menu closed by the mouse while one of its entries has keyboard focus keeps the overlay.
glimply_call OpenOverlay si frozen 0 >/dev/null
sleep 2
fake 106 106 108 # Monitors open, first entry focused
sleep 1
closed=$(seen 'drawer 9 -> 0')
fake m:626,58 d u # click the Monitors button again: the menu closes
sleep 1
[[ $(seen 'drawer 9 -> 0') -gt $closed ]] || fail "the click did not close the Monitors menu"
shot after-mouse-close
pixel=$(magick "$out/after-mouse-close.png" -format '%[fx:int(255*p{800,600}.r)]' info:)
[[ "$pixel" -lt 200 ]] || fail "the overlay vanished when the menu closed under keyboard focus (pixel $pixel)"
fake 1 1
echo "ok"
