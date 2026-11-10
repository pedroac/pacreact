#!/bin/bash
# Live check of notification actions, icon-theme names and image-data. Run it as:
#   dbus-run-session -- bash tests/manual/notify_features_test.sh
# (a private session bus keeps the real notification daemon out of it). Needs grim and
# notify-send >= 0.8. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pachud-feat-XXXXXX)
export XDG_RUNTIME_DIR=$R WAYLAND_DISPLAY=/run/user/1000/wayland-1 \
       PULSE_SERVER=unix:/run/user/1000/pulse/native XDG_STATE_HOME=$R/state PACHUD_SOUND=0
shot() { grim -g "900,1210 760x190" build/shots/$1.png; }

dbus-monitor "type='signal',interface='org.freedesktop.Notifications'" > $R/signals 2>&1 &
MON=$!
./pachud 2>/dev/null &
P=$!
trap 'kill $P $MON 2>/dev/null; rm -rf $R' EXIT
sleep 10   # a machine under memory pressure raises a real alert at start-up: let it pass

echo "-- nothing on screen"
./pachud action 2>&1

echo "-- two actions, invoke the second"
notify-send -t 20000 -A reply=Reply -A archive=Archive "New mail" "From Bob: lunch tomorrow?" > $R/chosen &
sleep 0.7; shot feat_1_actions
./pachud action 2
wait %3 2>/dev/null; sleep 0.3
echo "app was told: $(cat $R/chosen)"
echo "invalid number: $(./pachud action 5 2>&1)"

echo "-- only a default action: plain 'pachud action' picks it"
notify-send -t 20000 -A default=Open "Download finished" "report.pdf" > $R/chosen2 &
sleep 0.7; shot feat_2_default
./pachud action
sleep 0.5
echo "app was told: $(cat $R/chosen2)"

echo "-- icon by theme name (btop is a PNG in hicolor)"
notify-send -t 3000 -i btop "Icon by name" "resolved through the icon theme"
sleep 0.7; shot feat_3_theme_icon
./pachud dismiss; sleep 0.4

echo "-- image-data: a 2x2 picture (red, green / blue, yellow)"
notify-send -t 3000 -h 'variant:image-data:(2, 2, 6, false, 8, 3, [byte 255, byte 0, byte 0, byte 0, byte 255, byte 0, byte 0, byte 0, byte 255, byte 255, byte 255, byte 0])' \
    "Raw pixels" "from the image-data hint"
sleep 0.7; shot feat_4_image_data
./pachud dismiss; sleep 0.3

echo "-- signals:"
grep -E "member=(ActionInvoked|NotificationClosed)" -A2 $R/signals | grep -E "member|uint32|string" | sed 's/.*member=/  /; s/^ *//' | paste -sd' ' | sed 's/  */ /g'
