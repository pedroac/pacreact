#!/bin/bash
# Live check of the start-up grace period on a machine that really is under memory pressure.
# The alert only fires when the pressure triggers do, which comes and goes, so this seeds the
# state with an alert of the level that is about to fire, runs instances until a trigger fires
# during the grace period, and reports whether the HUD stayed quiet. It uses a LOOP_TRACE build
# to see which trigger fired. Needs Hyprland (hyprctl layers). Takes up to a few minutes.
cd "$(dirname "$0")/../.."
S=$(mktemp -d /tmp/pachud-grace-XXXXXX)
trap 'rm -rf $S' EXIT

echo "building a trace binary..."
make clean >/dev/null 2>&1; mkdir -p build
make CFLAGS="-O2 -DLOOP_TRACE" >/dev/null 2>&1 || { echo "build failed"; exit 1; }
cp pachud $S/pachud-trace
make clean >/dev/null 2>&1; mkdir -p build; make >/dev/null 2>&1   # leave a normal binary behind

export WAYLAND_DISPLAY=/run/user/1000/wayland-1 PULSE_SERVER=unix:/run/user/1000/pulse/native \
       PACHUD_SOUND=0 PACHUD_FADE_MS=0
layers() { hyprctl layers 2>/dev/null | grep -c "namespace: pachud"; }

attempt() {  # attempt N SEEDED-LEVEL [ENV=VALUE]; prints a summary line, returns 0 if a trigger fired
    local n=$1 level=$2; shift 2
    local R=$S/rt$n st=$S/state$n
    mkdir -p $R $st/pachud; chmod 700 $R
    echo "memory $level $(date +%s)" > $st/pachud/alerts
    env XDG_RUNTIME_DIR=$R XDG_STATE_HOME=$st "$@" $S/pachud-trace 2> $S/log$n &
    local P=$! seen=0
    sleep 1.5
    # the two PSI trigger files are registered in order: "some" (warn), then "full" (crit)
    local fds=$(ls -l /proc/$P/fd 2>/dev/null | awk '/pressure\/memory/{print $9}' | tr '\n' ' ')
    for t in $(seq 1 24); do sleep 0.5; [ "$(layers)" -gt 0 ] && seen=1; done
    kill $P 2>/dev/null; wait $P 2>/dev/null
    local some=${fds%% *}; local full=$(echo $fds | awk '{print $2}')
    local fired_some=$(grep -c "^wake fd=$some " $S/log$n) fired_full=$(grep -c "^wake fd=$full " $S/log$n)
    echo "attempt $n (seeded level $level $*): trigger some=$fired_some full=$fired_full, HUD shown: $([ $seen = 1 ] && echo YES || echo no), state: $(cat $st/pachud/alerts | tr '\n' ' ')"
    [ $((fired_some + fired_full)) -gt 0 ]
}

echo "--- with the grace period (default 60 s)"
for n in 1 2 3 4 5 6 7 8 9 10; do
    level=$((n % 2 + 1))   # alternate seeding warn (1) and crit (2)
    attempt $n $level && break
done
echo "--- same, grace period off (PACHUD_STARTUP_GRACE=0): an alert should show"
for n in 11 12 13 14 15 16; do
    level=$((n % 2 + 1))
    attempt $n $level PACHUD_STARTUP_GRACE=0 && break
done
