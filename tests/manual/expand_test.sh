#!/bin/bash
# Live check of expanding a long notification: by hovering, and with `pachud expand`. Run it as:
#   dbus-run-session -- bash tests/manual/expand_test.sh
# Needs grim, notify-send, wayland-scanner and python3 with PIL (to measure the card in the
# screenshots). A virtual pointer (tests/manual/vptr_click.c) does the hovering. Screenshots land
# in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pachud-expand-XXXXXX)
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
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; kill $C; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state PACHUD_SOUND=0 PACHUD_FADE_MS=0

mkfifo $R/pointer
build/vptr_click 1280 720 < $R/pointer &
V=$!
exec 9> $R/pointer
./pachud 2> $R/stderr &
P=$!
trap 'kill $C $P $V 2>/dev/null; rm -rf $R' EXIT
sleep 1.5

fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
# The height of what is drawn at the top right of the screen: the card
height() {
    grim -o HEADLESS-1 build/shots/expand_$1.png
    python3 - build/shots/expand_$1.png <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
box = im.point(lambda v: 255 if v > 8 else 0).getbbox()
print(0 if box is None else box[3] - box[1])
PY
}
move() { echo "move $1 $2" >&9; sleep 0.3; }

BODY=$(python3 -c "print(' '.join('word%02d' % i for i in range(1, 91)))")
notify-send -t 30000 "A long message" "$BODY" &
sleep 0.8
a=$(height 1_collapsed); echo "collapsed: ${a}px"

# Hover: travel there like a hand, linger past the delay
move 100 600; move 1000 40; move 1001 40
sleep 0.7
b=$(height 2_hovered); echo "hovered: ${b}px"
check "hovering the card opens it" '[ "$b" -gt "$a" ]'

move 100 600
sleep 0.5
c=$(height 3_left); echo "pointer away: ${c}px"
check "moving away closes it again" '[ "$c" -eq "$a" ]'

# Passing through without lingering opens nothing
move 1000 40; move 100 600
sleep 0.5
d=$(height 4_passed); echo "passed over: ${d}px"
check "passing over without lingering opens nothing" '[ "$d" -eq "$a" ]'

# From the keyboard
./pachud expand > $R/expand1
e=$(height 5_expand); echo "pachud expand: ${e}px ($(cat $R/expand1))"
check "pachud expand opens the newest card" '[ "$e" -gt "$a" ] && grep -q expanded $R/expand1'
./pachud expand > $R/expand2
f=$(height 6_collapse); echo "pachud expand again: ${f}px ($(cat $R/expand2))"
check "...and closes it again" '[ "$f" -eq "$a" ] && grep -q collapsed $R/expand2'

# Nothing to open
./pachud dismiss >/dev/null; sleep 0.3
out=$(./pachud expand 2>&1)
check "with no notification on screen it says so" 'echo "$out" | grep -q "no notification on screen"'

cat $R/stderr
exit $fail
