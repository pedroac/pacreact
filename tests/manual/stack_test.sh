#!/bin/bash
# Stacking check in a nested headless compositor (labwc), separate from the real display and
# from any running notification daemon. Shows several HUDs at once at the default anchor and at
# anchor=bottom-left, and captures each with grim into build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-stack-XXXXXX)
chmod 700 "$R"
export XDG_RUNTIME_DIR=$R
unset DBUS_SESSION_BUS_ADDRESS

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    labwc > $R/labwc.log 2>&1 &
C=$!
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state

run_case() {  # run_case LABEL ANCHOR
    echo "== $1 (anchor='$2')"
    tests/manual/pacreact-with.sh anchor=$2 2> $R/stderr &
    P=$!
    sleep 1.5
    ./pacreact toast -l crit -t 6 "Battery critical: 4%"
    ./pacreact toast -l warn -t 6 "Memory pressure is high"
    ./pacreact toast -t 6 "Network back online"
    ./pacreact slider 60 Brightness
    sleep 0.7
    grim -o HEADLESS-1 "build/shots/stack_$1.png" && echo "captured stack_$1.png"
    cat $R/stderr
    kill $P; wait $P 2>/dev/null
    sleep 0.3
}

run_case default ""
run_case bottom-left bottom-left
