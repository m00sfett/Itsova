# Nested test (#98): the first three recordings show a "how to stop" notification with a Stop
# action, the fourth does not; the Stop action stops the recording.
log="$home/glimply.log"
seen() { grep -c -- "$1" "$log"; }
record_once() {
    glimply_call StartRecording si "screen:Virtual-0" 0 >/dev/null
    for _ in $(seq 20); do [[ "$(recording_state)" == recording ]] && break; sleep 0.25; done
    [[ "$(recording_state)" == recording ]] || fail "recording $1 did not start"
    sleep 1
}
start_fake_notifications "Recording Virtual"
record_once 1
sleep 2 # the fake server presses Stop after a second
wait_recording_idle 20 || fail "the Stop action did not stop the recording"
grep -q 'stop requested by "notification Stop action"' "$log" || fail "Stop action not logged as the origin"
for n in 2 3 4; do
    record_once $n
    glimply_call StopRecording >/dev/null
    wait_recording_idle 20 || fail "recording $n did not finish"
done
hints=$(grep -c '^Recording Virtual' "$out/notifications.log")
echo "stop hints: $hints"
[[ "$hints" -eq 3 ]] || fail "expected 3 stop hints, got $hints"
grep -q 'stop hint shown 3 of 3' "$log" || fail "third hint not logged"
grep '^Recording Virtual' "$out/notifications.log" | head -1 | grep -q 'to stop' || fail "the hint does not say how to stop"
echo "ok"
