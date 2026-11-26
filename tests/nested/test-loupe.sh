# Nested test (#101): the loupe shows while drawing in live mode too, from the Print snapshot.
glimply_call OpenOverlay si live 0 >/dev/null || fail "OpenOverlay failed"
sleep 2
fake_bg m:500,400 d m:620,480 m:700,550 w:2500 u
sleep 1.6
shot live-drag
wait_fake
sleep 1
# The card (light Breeze window colour) appears near the pointer; the live overlay is otherwise
# a thin dim layer over the grey desktop.
bright=$(magick "$out/live-drag.png" -crop 300x300+700+550 +repage -format '%[fx:maxima.r]' info:)
echo "brightest near the pointer: $bright"
awk -v b="$bright" 'BEGIN { exit !(b > 0.85) }' || fail "no loupe card near the pointer in live mode"
fake 1 1
echo "ok"
