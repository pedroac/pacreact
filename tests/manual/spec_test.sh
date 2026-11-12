#!/bin/bash
# Live check of the notification-spec behaviours that need a bus: CloseNotification on a card
# and on an unknown id, NotificationClosed for a notification that do-not-disturb suppressed,
# and a critical card that outlives the old 15 s limit. Run it as:
#   dbus-run-session -- bash tests/manual/spec_test.sh
# Needs a nested headless labwc, notify-send and busctl. Takes about 20 seconds.
cd "$(dirname "$0")/.."/..
R=$(mktemp -d /tmp/pachud-spec-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P $MON 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state PACHUD_SOUND=0 PACHUD_FADE_MS=0
dbus-monitor "type='signal',interface='org.freedesktop.Notifications'" > $R/signals 2>&1 &
MON=$!
./pachud 2> $R/stderr &
P=$!
sleep 1.5
fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
N="busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications"

echo "-- CloseNotification"
id=$(notify-send -p -t 20000 "to close")
check "notify-send returned an id" '[ -n "$id" ]'
check "closing a card that exists succeeds" '$N CloseNotification u $id >/dev/null 2>&1'
sleep 0.3
check "NotificationClosed(id, 3) was emitted" 'grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 3"'
out=$($N CloseNotification u $id 2>&1); rc=$?
check "closing it again is an error" '[ $rc -ne 0 ]'
echo "      ($out)"
check "an id never used is an error" '! $N CloseNotification u 987654 >/dev/null 2>&1'

echo "-- do not disturb"
./pachud dnd on >/dev/null
: > $R/signals
start=$(date +%s.%N)
timeout 5 notify-send -w -t 20000 "suppressed"; rc=$?
took=$(echo "$(date +%s.%N) - $start" | bc)
check "notify-send -w returns (NotificationClosed arrived) instead of waiting" '[ $rc -eq 0 ]'
check "NotificationClosed reason 4" 'grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 4"'
echo "      (took ${took}s)"
./pachud dnd off >/dev/null

echo "-- critical card is still there after 17 s (the old limit was 15 s)"
notify-send -u critical "critical" "stays until dismissed" &
NS=$!
sleep 17
check "no expiry signal for it" '! grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 1$"'
./pachud dismiss >/dev/null
sleep 0.3
check "dismissing it reports reason 2" 'grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 2$"'
kill $NS 2>/dev/null
cat $R/stderr
exit $fail
