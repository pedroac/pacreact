#!/bin/bash
# Live check of the attention deck, in a nested headless compositor with a private session bus:
# ordinary notifications stay at the top right, and critical ones, incoming calls and critical
# system toasts gather at the top centre. Run it as:
#   dbus-run-session -- bash tests/manual/deck_test.sh
# Needs grim, notify-send, wayland-scanner and python3 with PIL (to measure the screenshots,
# 1280 x 720). A virtual pointer (tests/manual/vptr_click.c) clicks. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-deck-XXXXXX)
chmod 700 "$R"
S=tests/manual
for kind in client-header private-code; do
    wayland-scanner $kind $S/vptr_click.xml build/vptr-$kind.${kind%%-*}.tmp
done
mv build/vptr-client-header.client.tmp build/wlr-virtual-pointer-unstable-v1-client-protocol.h
mv build/vptr-private-code.private.tmp build/vptr-protocol.c
cc -O2 -Wall -Ibuild $S/vptr_click.c build/vptr-protocol.c -o build/vptr_click -lwayland-client || exit 1

export XDG_RUNTIME_DIR=$R
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state
mkfifo $R/pointer
build/vptr_click 1280 720 < $R/pointer &
V=$!
exec 9> $R/pointer
tests/manual/pacreact-with.sh 2> $R/stderr &
P=$!
trap 'kill $C $P $V 2>/dev/null; rm -rf $R' EXIT
sleep 1.5

fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
# Height of what is drawn in a band of the screen: the deck's (x 400-880) or the stack's (x 880-1280)
band() {
    grim -o HEADLESS-1 build/shots/deck_$1.png
    python3 - build/shots/deck_$1.png $2 $3 <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB").crop((int(sys.argv[2]), 0, int(sys.argv[3]), 720))
box = im.point(lambda v: 255 if v > 8 else 0).getbbox()
print("0 0 0" if box is None else "%d %d %d" % (box[0] + int(sys.argv[2]), box[1], box[3] - box[1]))
PY
}
deck() { band "$1" 400 880; }
stack() { band "$1" 880 1280; }

# A machine that is really short of memory, disk or battery raises a critical alert of its own at
# start-up, and that belongs in the deck too: clear any before measuring.
for i in 1 2 3 4 5; do
    read dx dy dh <<< "$(deck 0_start)"
    [ "$dh" -eq 0 ] && break
    echo "clearing a start-up alert from the deck (${dh}px)"
    ./pacreact dismiss > /dev/null
    sleep 0.5
done

notify-send -t 30000 "An ordinary notification" "nothing special about it"
sleep 0.6
read dx dy dh <<< "$(deck 1_ordinary)"; read sx sy sh <<< "$(stack 1_ordinary)"
echo "ordinary: deck ${dh}px, stack ${sh}px at x=$sx"
check "an ordinary notification is at the top right" '[ "$sh" -gt 0 ] && [ "$dh" -eq 0 ]'

notify-send -u critical -t 0 "Critical: disk failing" "back up now"
sleep 0.6
read dx dy dh <<< "$(deck 2_critical)"; read sx2 sy2 sh2 <<< "$(stack 2_critical)"
echo "critical: deck ${dh}px from x=$dx y=$dy, stack ${sh2}px"
check "a critical notification is at the top centre" '[ "$dh" -gt 0 ] && [ "$dx" -ge 440 ] && [ "$dx" -le 460 ] && [ "$dy" -le 20 ]'
check "...and the stack did not change" '[ "$sh2" -eq "$sh" ]'
one=$dh

notify-send -t 30000 -h string:category:call.incoming "Incoming call" "Alice"
sleep 0.6
read dx dy dh <<< "$(deck 3_call)"
echo "incoming call: deck ${dh}px"
check "an incoming call joins the deck behind it, as a strip" '[ "$dh" -gt "$one" ] && [ "$dh" -lt $((one + 40)) ]'
two=$dh

./pacreact toast -l crit -t 30 "Battery critical: 4%"
sleep 0.6
read dx dy dh <<< "$(deck 4_system)"
echo "critical system toast: deck ${dh}px"
check "a critical system alert joins it, as another strip" '[ "$dh" -gt "$two" ] && [ "$dh" -lt $((two + 40)) ]'
three=$dh

./pacreact toast -l crit -t 30 "A fourth, which has to wait"
sleep 0.6
read dx dy dh <<< "$(deck 5_fourth)"
# Clicking a strip brings that card to the front. The accent bar at the front card's left edge says
# which card it is: red for the critical ones, blue for the call.
front_accent() {
    grim -o HEADLESS-1 build/shots/deck_$1.png
    python3 - build/shots/deck_$1.png <<'PY'
import sys
from PIL import Image
r, g, b = Image.open(sys.argv[1]).convert("RGB").getpixel((452, 40))
print("red" if r > 200 and b < 150 else "blue" if b > 200 and r < 150 else "other")
PY
}
click_at() {
    echo "move 100 600" >&9; sleep 0.3
    echo "move $1 $2" >&9; sleep 0.3
    echo "move $(( $1 + 1 )) $2" >&9; sleep 0.3
    echo "click ${3:-0x110}" >&9; sleep 0.5
}
check "before: the oldest, the critical notification, is in front" '[ "$(front_accent 4a_before)" = red ]'
for try in 1 2 3; do
    click_at 640 100        # the visible part of the call's strip, just below the front card
    [ "$(front_accent 4b_clicked)" = blue ] && break
done
check "clicking the call's strip brings it to the front" '[ "$(front_accent 4b_clicked)" = blue ]'
read dx dy dh <<< "$(deck 4c_after_click)"
check "...and the deck is as large as before (nothing was closed)" '[ "$dh" -eq "$three" ]'

check "a fourth waits: the deck does not grow" '[ "$dh" -eq "$three" ]'
check "...and no card is lost: the critical one, the call and the ordinary one are all counted" '[ "$(./pacreact count)" = 3 ]'

notify-send -t 30000 "Another ordinary one" "still in the usual place"
sleep 0.6
read dx dy dh <<< "$(deck 6_more_ordinary)"; read sx3 sy3 sh3 <<< "$(stack 6_more_ordinary)"
check "ordinary notifications keep arriving at the top right, the deck unchanged" '[ "$sh3" -gt "$sh2" ] && [ "$dh" -eq "$three" ]'

./pacreact dismiss > /dev/null
sleep 0.6
read dx dy dh <<< "$(deck 7_dismissed)"; read sx4 sy4 sh4 <<< "$(stack 7_dismissed)"
check "dismiss closes the deck's front card first (the stack is untouched)" '[ "$sh4" -eq "$sh3" ]'
echo "after dismiss: deck ${dh}px"

for i in 1 2 3 4 5; do ./pacreact dismiss > /dev/null; done
sleep 0.6
read dx dy dh <<< "$(deck 8_cleared)"
check "with the deck emptied, nothing is left at the top centre" '[ "$dh" -eq 0 ]'
cat $R/stderr
exit $fail
