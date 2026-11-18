#!/bin/bash
# Multi-monitor check in a nested headless compositor (labwc), separate from the real display.
# It starts labwc with two virtual outputs, runs pacreact with output set to each in turn,
# and captures every output with grim. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-out-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state
echo "outputs: $(grim -l 0 -o NOPE /dev/null 2>&1 | head -1)"

capture() {   # capture OUTPUT FILE
    grim -o "$1" "build/shots/$2.png" 2>/dev/null && echo "captured $1 -> $2"
}

run_case() {  # run_case LABEL OUTPUT-SETTING
    local label=$1 setting=$2
    echo "== $label (output='$setting')"
    local args=()
    [ -n "$setting" ] && args=(output=$setting)
    tests/manual/pacreact-with.sh "${args[@]}" 2> $R/stderr &
    P=$!
    sleep 1.5
    ./pacreact toast -l warn -t 4 "HUD for: $label"
    sleep 0.7
    capture HEADLESS-1 "out_${label}_1"
    capture HEADLESS-2 "out_${label}_2"
    grep -i "output" $R/stderr
    kill $P; wait $P 2>/dev/null
    sleep 0.3
}

run_case second HEADLESS-2
run_case first HEADLESS-1
run_case unset ""
run_case bogus NO-SUCH-OUTPUT
