#!/bin/bash
# Manual check of the fade on a live Hyprland session (needs grim and hyprctl).
# A 1.5 s fade makes the intermediate frames visible; screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
shot() { grim -g "900,1260 760x140" build/shots/$1.png; }
layers() { hyprctl layers 2>/dev/null | grep -c "namespace: pacreact"; }

tests/manual/pacreact-with.sh fade_ms=1500 2>/dev/null &
P=$!
# A machine under memory pressure raises a (real) alert right after start-up, then stays quiet
# for the 30 s cooldown: let that pass so it does not occupy the screen during the test.
sleep 10
echo "layer surfaces before: $(layers)"

./pacreact toast -t 4 "Fade test"
sleep 0.45; shot fade_in_early
sleep 0.5;  shot fade_in_mid
sleep 1.5;  shot fade_in_done
echo "layer surfaces while shown: $(layers)"

sleep 2.2   # the 4 s timeout expires; the fade-out starts
shot fade_out_early
sleep 0.5; shot fade_out_mid

# A new HUD arriving during the fade-out must bring it back, not leave a stale surface.
./pacreact toast -t 1 "Back again"
sleep 0.4;  shot fade_back
sleep 5
echo "layer surfaces after everything expired: $(layers)"

kill $P; wait $P 2>/dev/null
echo "layer surfaces after daemon exit: $(layers)"
