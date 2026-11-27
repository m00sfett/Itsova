# Nested test (#93): the overlay opens on D-Bus, shows on screen and closes with Escape.
itsova_call OpenOverlay si frozen 0 >/dev/null || fail "OpenOverlay failed"
sleep 2
shot overlay-open
grep -q "session start" "$home/itsova.log" || fail "no session start in the journal trail"
fake 1 # Escape
sleep 1
grep -q "session end" "$home/itsova.log" || fail "Escape did not close the overlay"
echo "ok"
