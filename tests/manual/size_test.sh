#!/bin/bash
# Live check of font_size, width and font in a nested headless compositor with
# a private session bus. The same notification is shown at the default size, with a larger font, with
# a set width and with another font, and the card is measured in the screenshots (1280 x 720).
# Run it as:  dbus-run-session -- bash tests/manual/size_test.sh
# Needs grim, notify-send and python3 with PIL. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-size-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state

fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }

# shot NAME [VAR=VALUE...]: start pacreact with those variables, show one card, screenshot it, and
# print the card's width and height; stderr of the daemon is kept in $R/stderr_NAME
shot() {
    local name=$1; shift
    tests/manual/pacreact-with.sh "$@" 2> $R/stderr_$name &
    P=$!
    sleep 1.2
    for i in 1 2 3 4 5; do ./pacreact dismiss >/dev/null 2>&1; done   # a real alert of this machine, if any
    notify-send -t 30000 "A notification" "with a line of body text that is long enough to need wrapping when the card is narrow"
    sleep 0.7
    grim -o HEADLESS-1 build/shots/size_$name.png
    kill $P; wait $P 2>/dev/null
    # The card is the run of lit pixels along the top and the right edge of the stack's corner, so
    # whatever else is on the screen (an alert of this machine's own, in the middle) does not count
    python3 - build/shots/size_$name.png <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
lit = lambda x, y: max(im.getpixel((x, y))) > 8
# Probe from inside the card, away from its rounded corners: the middle of its first 60 rows
x = 1263
while x > 0 and lit(x - 1, 60): x -= 1
w = 1264 - x if lit(1263, 60) else 0
top = bottom = 60
while top > 0 and lit(1240, top - 1): top -= 1
while bottom < 719 and lit(1240, bottom + 1): bottom += 1
h = bottom - top + 1 if lit(1240, 60) else 0
print(w, h)
PY
}

read w0 h0 <<< "$(shot default A=1)"
echo "default:            ${w0} x ${h0}"
check "the default card is 380 wide" '[ "$w0" -eq 380 ]'

read w1 h1 <<< "$(shot big font_size=24)"
echo "font_size=24: ${w1} x ${h1}"
check "a 24 px font makes the card 1.5 times as wide (570)" '[ "$w1" -eq 570 ]'
check "...and taller (larger text, padding and icon area)" '[ "$h1" -gt "$h0" ]'

read w2 h2 <<< "$(shot wide width=500)"
echo "width=500:    ${w2} x ${h2}"
check "width sets the width alone" '[ "$w2" -eq 500 ]'
check "...without changing the sizes: no taller than the default" '[ "$h2" -le "$h0" ]'

read w3 h3 <<< "$(shot small font_size=12)"
echo "font_size=12: ${w3} x ${h3}"
check "a smaller font makes it smaller (285 wide)" '[ "$w3" -eq 285 ]'

read w4 h4 <<< "$(shot both font_size=24 width=450)"
echo "both:                ${w4} x ${h4}"
check "with both, the width is the one asked for" '[ "$w4" -eq 450 ]'

# Nonsense is said so and ignored; out of range values are brought in
read w5 h5 <<< "$(shot bad font_size=huge width=wide)"
check "nonsense is ignored: the default size" '[ "$w5" -eq 380 ]'
check "...and the daemon said so" 'grep -q "font_size must be a whole number" $R/stderr_bad'
read w6 h6 <<< "$(shot huge font_size=500)"
check "a font size beyond the largest is brought to it (48: 1140 wide)" '[ "$w6" -eq 1140 ]'

# A font file: the text is drawn with it, and a bad one is said so and the default is used
FONT=""
for f in /usr/share/fonts/TTF/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
         /usr/share/fonts/liberation/LiberationSans-Regular.ttf /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf; do
    [ -r $f ] && { FONT=$f; break; }
done
if [ -n "$FONT" ]; then
    shot font font=$FONT > /dev/null
    shot badfont font=/nonexistent/font.ttf > /dev/null
    shot nofont A=1 > /dev/null
    same() {   # whether the stack's corner of two screenshots is identical
        python3 - build/shots/size_$1.png build/shots/size_$2.png <<'PY'
import sys
from PIL import Image, ImageChops
a, b = (Image.open(f).convert("RGB").crop((880, 0, 1280, 300)) for f in sys.argv[1:3])
sys.exit(0 if ImageChops.difference(a, b).getbbox() is None else 1)
PY
    }
    check "another font changes how it looks" '! same font nofont'
    check "a font that does not exist is said so..." 'grep -q "cannot use the font" $R/stderr_badfont'
    check "...and the default is used" 'same badfont nofont'
else
    echo "skip: no second font installed to try font with"
fi
exit $fail
