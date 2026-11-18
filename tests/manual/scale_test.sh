#!/bin/bash
# Fractional-scale path in a nested headless compositor (labwc, scale 1). With scale the
# buffer is rendered larger and presented at its logical size through wp_viewport, so the HUD
# must keep the same size on screen at every setting. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-scale-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state

run_case() {  # run_case LABEL SCALE-SETTING
    local label=$1 setting=$2
    echo "== $label (scale='$setting')"
    local args=()
    [ -n "$setting" ] && args=(scale=$setting)
    WAYLAND_DEBUG=1 tests/manual/pacreact-with.sh "${args[@]}" 2> $R/debug.log &
    P=$!
    sleep 1.5
    ./pacreact toast -l warn -t 4 "Scale check: sharp text at any scale"
    sleep 0.8
    grim "build/shots/scale_$label.png"
    kill $P; wait $P 2>/dev/null
    sleep 0.3
    sed 's/\x1b\[[0-9;]*m//g' $R/debug.log > $R/plain.log
    echo "  preferred_scale from the compositor: $(grep -o 'preferred_scale([0-9]*)' $R/plain.log | sort -u | tr '\n' ' ')"
    echo "  buffer sizes: $(grep -o 'create_buffer([^)]*)' $R/plain.log | sed 's/.*, \([0-9]*\), \([0-9]*\), [0-9]*, [0-9]*)/\1x\2/' | sort -u | tr '\n' ' ')"
    echo "  viewport destinations: $(grep -o 'set_destination([0-9]*, [0-9]*)' $R/plain.log | sort -u | tr '\n' ' ')"
    echo "  HUD on screen (x0 y0 x1 y1 w h): $(python3 tests/manual/pngbox.py build/shots/scale_$label.png)"
}

run_case default ""
run_case x1_5 1.5
run_case x2 2
