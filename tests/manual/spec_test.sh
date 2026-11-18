#!/bin/bash
# Live check of the notification-spec behaviours that need a bus: CloseNotification on a card
# and on an unknown id, NotificationClosed for a notification that do-not-disturb suppressed,
# a critical card that outlives the old 15 s limit, what `pacreact history` lists, and that the
# history is out of reach of other processes. Run it as:
#   dbus-run-session -- bash tests/manual/spec_test.sh
# Needs a nested headless labwc, notify-send and busctl. Takes about 20 seconds.
cd "$(dirname "$0")/.."/..
R=$(mktemp -d /tmp/pacreact-spec-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P $MON 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state
dbus-monitor "type='signal',interface='org.freedesktop.Notifications'" > $R/signals 2>&1 &
MON=$!
tests/manual/pacreact-with.sh 2> $R/stderr &
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
./pacreact dnd on >/dev/null
: > $R/signals
start=$(date +%s.%N)
timeout 5 notify-send -w -t 20000 "suppressed"; rc=$?
took=$(echo "$(date +%s.%N) - $start" | bc)
check "notify-send -w returns (NotificationClosed arrived) instead of waiting" '[ $rc -eq 0 ]'
check "NotificationClosed reason 4" 'grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 4"'
echo "      (took ${took}s)"
./pacreact dnd off >/dev/null

echo "-- critical card is still there after 17 s (the old limit was 15 s)"
notify-send -u critical "critical" "stays until dismissed" &
NS=$!
sleep 17
check "no expiry signal for it" '! grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 1$"'
./pacreact dismiss >/dev/null
sleep 0.3
check "dismissing it reports reason 2" 'grep -a -A2 "member=NotificationClosed" $R/signals | grep -q "uint32 2$"'
kill $NS 2>/dev/null
echo "-- history"
id=$(notify-send -p -t 20000 "download" "10%")
notify-send -t 20000 -r $id "download" "done"
notify-send -t 20000 -h boolean:transient:true "a transient one"
sleep 0.3
./pacreact history > $R/history
cat $R/history | sed 's/^/      /'
check "the notification closed with CloseNotification is listed" 'grep -q "closed by app.*to close" $R/history'
check "the one do-not-disturb skipped is listed" 'grep -q "skipped (do not disturb).*suppressed" $R/history'
check "the critical one dismissed is listed" 'grep -q "dismissed.*! .*critical" $R/history'
check "a replaced card is one entry, with its latest text" '[ $(grep -c "download" $R/history) -eq 1 ] && grep -q "download — done" $R/history'
check "a transient notification is not listed" '! grep -q transient $R/history'
check "newest first" '[ "$(head -1 $R/history | grep -c download)" -eq 1 ]'
./pacreact history clear >/dev/null
check "history clear empties it" './pacreact history | grep -q "no notifications yet"'
echo "-- count (what a status bar's bell shows)"
for i in $(seq 1 20); do [ "$(./pacreact count)" = 0 ] && break; ./pacreact dismiss >/dev/null; done
check "no notifications on screen: 0" '[ "$(./pacreact count)" = 0 ]'
notify-send -t 20000 "one"
notify-send -t 20000 "two"
./pacreact toast -t 20 "a toast is not a notification" >/dev/null
sleep 0.3
check "two cards and a toast: 2" '[ "$(./pacreact count)" = 2 ]'
./pacreact dismiss >/dev/null; ./pacreact dismiss >/dev/null
check "a toast does not count; dismissing the cards brings it to 0" '[ "$(./pacreact count)" = 0 ]'

echo "-- hostile icon files (apps name the path; the daemon must not stall or balloon)"
for i in $(seq 1 20); do [ "$(./pacreact count)" = 0 ] && break; ./pacreact dismiss >/dev/null; done
H=$R/icons; mkdir -p $H
mkfifo $H/pipe.png
ln -s /dev/zero $H/zero.png
# 33 bytes: a PNG header that claims 30000 x 30000 pixels (about 3.6 GB decoded)
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x75\x30\x00\x00\x75\x30\x08\x06\x00\x00\x00' > $H/bomb.png
for f in pipe zero bomb; do notify-send -t 20000 -i $H/$f.png "icon $f" "named a hostile file"; done
sleep 0.5
check "the daemon still answers (not stuck reading a pipe)" 'timeout 3 ./pacreact count >/dev/null'
check "all three cards are shown, without their icons" '[ "$(timeout 3 ./pacreact count)" = 3 ]'
mem=$(awk '/VmHWM/{print $2}' /proc/$P/status)
check "and its memory stayed small (peak ${mem} kB)" '[ "$mem" -lt 200000 ]'
for i in 1 2 3; do ./pacreact dismiss >/dev/null; done

echo "-- the history is private"
./pacreact toast "keep alive" >/dev/null
notify-send -t 20000 "private" "my secret body"
sleep 0.3
check "the socket is mode 600" '[ "$(stat -c %a $R/pacreact.sock)" = 600 ]'
check "another process of this user cannot read the daemon's memory" '! head -c 16 /proc/$P/mem >/dev/null 2>&1'
check "...nor its environment" '! cat /proc/$P/environ >/dev/null 2>&1'
check "the daemon still works (history lists it)" './pacreact history | grep -q "my secret body"'
check "nothing of it reached the daemon's log" '! grep -q "secret" $R/stderr'
cat $R/stderr
exit $fail
