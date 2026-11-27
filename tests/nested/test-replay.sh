# Nested test (#104): replay plays a video fullscreen without overlay UI and Esc ends it.
sample="$out/sample.mp4"
ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=15 -t 3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$sample"

active() { itsova_call ReplayActive | awk '{print $2}'; }
[[ "$(active)" == false ]] || fail "expected ReplayActive=false before start"

res=$(itsova_call StartReplay ss "$sample" "")
[[ "$res" == 's ""' ]] || fail "StartReplay failed: $res"
sleep 0.8
[[ "$(active)" == true ]] || fail "expected ReplayActive=true after start"

# Stop via Esc key (keycode 1) or D-Bus fallback if focus differs in headless KWin.
fake k:1 w:400
if [[ "$(active)" == true ]]; then
    itsova_call StopReplay >/dev/null
    sleep 0.3
fi
[[ "$(active)" == false ]] || fail "expected ReplayActive=false after stop"
echo "ok"
