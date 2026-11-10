#!/bin/bash
# Live do-not-disturb check. Run it as:  dbus-run-session -- bash tests/manual/dnd_test.sh
# A private session bus keeps the real notification daemon out of it; the test daemon gets its
# own IPC directory. Screenshots land in build/shots/ (needs grim).
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pachud-dnd-XXXXXX)
export XDG_RUNTIME_DIR=$R WAYLAND_DISPLAY=/run/user/1000/wayland-1 \
       PULSE_SERVER=unix:/run/user/1000/pulse/native XDG_STATE_HOME=$R/state PACHUD_SOUND=0
shot() { grim -g "900,1230 760x170" build/shots/$1.png; }

./pachud 2>/dev/null &
P=$!
trap 'kill $P 2>/dev/null; rm -rf $R' EXIT
sleep 10   # a machine under memory pressure raises a real alert at start-up: let it pass

notify-send -t 3000 "Before DND" "shown normally"
sleep 0.5; shot dnd_1_before
./pachud dismiss; sleep 0.3

./pachud dnd on 30m
sleep 0.5; shot dnd_2_on
echo "status: $(./pachud dnd status)"
./pachud dismiss; sleep 0.3

notify-send -t 3000 "Held one" "must not appear"
notify-send -u low -t 3000 "Held two" "must not appear either"
sleep 0.6; shot dnd_3_held
notify-send -u critical -t 3000 "Critical" "comes through"
sleep 0.5; shot dnd_4_critical
./pachud dismiss; sleep 0.3
echo "status: $(./pachud dnd status)"

./pachud dnd off
sleep 0.5; shot dnd_5_off
echo "status: $(./pachud dnd status)"
