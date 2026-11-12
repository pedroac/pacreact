#!/bin/bash
# Live check of the status icons and of PACHUD_ICONS, PACHUD_ICON_SIZE and PACHUD_MARGIN, in a nested
# headless compositor with a private session bus, using whatever icon theme is installed (Adwaita's
# symbolic SVG icons need librsvg). Run it as:
#   dbus-run-session -- bash tests/manual/icons_test.sh
# Needs grim and python3 with PIL (to measure the screenshots, 1280 x 720). Screenshots land in
# build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pachud-icons-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state PACHUD_SOUND=0 PACHUD_FADE_MS=0

fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }

# run NAME VAR=VALUE... -- COMMAND...: start pachud with those variables, run the command (which
# shows something), and take build/shots/icons_NAME.png
run() {
    local name=$1; shift
    local vars=()
    while [ "$1" != "--" ]; do vars+=("$1"); shift; done
    shift
    env "${vars[@]}" ./pachud 2> $R/stderr_$name &
    P=$!
    sleep 1.2
    for i in 1 2 3 4 5; do ./pachud dismiss >/dev/null 2>&1; done   # a real alert of this machine, if any
    "$@"
    sleep 0.7
    grim -o HEADLESS-1 build/shots/icons_$name.png
    kill $P; wait $P 2>/dev/null
}

# The card in the top right corner: its top edge and its height, along a column through its middle
card() {
    python3 - build/shots/icons_$1.png <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
lit = lambda x, y: max(im.getpixel((x, y))) > 8
x = 1100
y = 200
while y > 0 and not lit(x, y): y -= 1       # find the card from below if it is lower than expected
top = y
while top > 0 and lit(x, top - 1): top -= 1
bottom = y
while bottom < 719 and lit(x, bottom + 1): bottom += 1
print(top, bottom - top + 1)
PY
}

# Light pixels in a column range of the card's first row of text: the icon, or the start of the text
light_in() {
    python3 - build/shots/icons_$1.png $2 $3 $4 $5 <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
print(sum(1 for x in range(x0, x1) for y in range(y0, y1) if min(im.getpixel((x, y))) > 150))
PY
}

TOAST=(./pachud toast -t 30 -i network-offline "Offline")
PLAIN=(./pachud toast -t 30 "Offline")

run withicon A=1 -- "${TOAST[@]}"
run noicon A=1 -- "${PLAIN[@]}"
run off PACHUD_ICONS=0 -- "${TOAST[@]}"
run big PACHUD_ICON_SIZE=48 -- "${TOAST[@]}"
run zoom PACHUD_FONT_SIZE=24 -- "${TOAST[@]}"
run margin PACHUD_MARGIN=40 -- "${PLAIN[@]}"
run nomargin PACHUD_MARGIN=0 -- "${PLAIN[@]}"

read t0 h0 <<< "$(card noicon)"
read t1 h1 <<< "$(card withicon)"
echo "no icon: card at y=$t0, ${h0}px tall; with icon: y=$t1, ${h1}px tall"
check "the icon makes the card as tall as the icon (24 + 32 padding = 56)" '[ "$h1" -eq 56 ] && [ "$h1" -gt "$h0" ]'

# The toast is at x 884..1264; the icon at 21..45 from its left edge, the text after that
gap_icon=$(light_in withicon 930 941 $((t1 + 4)) $((t1 + h1 - 4)))    # between icon and text
gap_plain=$(light_in noicon 930 941 $((t0 + 4)) $((t0 + h0 - 4)))     # where the text runs on, without one
glyph=$(light_in withicon 905 929 $((t1 + 4)) $((t1 + h1 - 4)))       # the icon itself
echo "light pixels: icon area $glyph, gap with icon $gap_icon, same place without icon $gap_plain"
check "there is an icon where the text would have been" '[ "$glyph" -gt 20 ]'
check "the text starts after the icon, not under it" '[ "$gap_icon" -eq 0 ] && [ "$gap_plain" -gt 0 ]'

read t2 h2 <<< "$(card off)"
check "PACHUD_ICONS=0 gives the card it would have without one" 'cmp -s build/shots/icons_off.png build/shots/icons_noicon.png'

read t3 h3 <<< "$(card big)"
echo "PACHUD_ICON_SIZE=48: ${h3}px tall"
check "PACHUD_ICON_SIZE=48 makes the card as tall as that icon (48 + 32)" '[ "$h3" -eq 80 ]'

read t4 h4 <<< "$(card zoom)"
echo "PACHUD_FONT_SIZE=24: ${h4}px tall"
check "the icon follows the font by default (36 px at 24, so 36 + 48 padding)" '[ "$h4" -eq 84 ]'

read t5 h5 <<< "$(card margin)"; read t6 h6 <<< "$(card nomargin)"
echo "margin: default top at $t0, PACHUD_MARGIN=40 at $t5, PACHUD_MARGIN=0 at $t6"
check "the default margin is 16" '[ "$t0" -eq 16 ]'
check "PACHUD_MARGIN=40 puts the card 40 from the edge" '[ "$t5" -eq 40 ]'
check "PACHUD_MARGIN=0 puts it against the edge" '[ "$t6" -eq 0 ]'
python3 - <<'PY'
from PIL import Image
im = Image.open("build/shots/icons_margin.png").convert("RGB")
lit = lambda x, y: max(im.getpixel((x, y))) > 8
x = 1279
while x > 0 and not lit(x - 1, 60): x -= 1
print("right edge of the card at", x)
PY

# Something with all kinds of icons for a look: the stack and the deck, and sliders
many() {
    ./pachud toast -t 30 -i network-offline "Offline"
    ./pachud toast -l warn -t 30 -i battery-low "Battery low — 15%"
    ./pachud toast -t 30 -i audio-headset "Headset connected"
    ./pachud slider -i audio-volume-medium 50 Volume
    ./pachud toast -l crit -t 30 -i battery-caution "Battery critical — 4%"
}
run many A=1 -- many
echo "(a look at all of them: build/shots/icons_many.png)"
cat $R/stderr_withicon
exit $fail
