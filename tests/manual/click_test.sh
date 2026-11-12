#!/bin/bash
# Live check that clicking a notification card dismisses it, in a nested headless compositor
# (labwc) with a private session bus. Run it as:
#   dbus-run-session -- bash tests/manual/click_test.sh
# Needs grim, notify-send and wayland-scanner. A virtual pointer (tests/manual/vptr_click.c)
# does the clicking. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pachud-click-XXXXXX)
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
trap 'kill $C $P 2>/dev/null; rm -rf $R' EXIT
for i in $(seq 1 50); do [ -S $R/wayland-0 ] && break; sleep 0.1; done
[ -S $R/wayland-0 ] || { echo "labwc did not start:"; cat $R/labwc.log; exit 1; }
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state PACHUD_SOUND=0 PACHUD_FADE_MS=0

run_case() {  # run_case LABEL PACHUD_CLICK-VALUE EXPECT (gone|stays)
    echo "== $1 (PACHUD_CLICK='$2')"
    PACHUD_CLICK=$2 ./pachud 2> $R/stderr &
    P=$!
    sleep 1.5
    notify-send -t 20000 -A default=Open "Click me" "a card with a default action" > $R/chosen &
    N=$!
    sleep 0.7
    grim -o HEADLESS-1 build/shots/click_$1_before.png
    build/vptr_click 1000 40 1280 720
    sleep 0.5
    grim -o HEADLESS-1 build/shots/click_$1_after.png
    if kill -0 $N 2>/dev/null; then
        echo "notify-send still waiting: the card was not closed"; result=stays; kill $N
    else
        echo "notify-send finished, app was told: $(cat $R/chosen)"; result=gone
    fi
    [ "$result" = "$3" ] && echo "ok: card $result" || echo "FAIL: expected $3, got $result"
    cat $R/stderr
    kill $P; wait $P 2>/dev/null
    sleep 0.3
}

run_case click "" gone
run_case off 0 stays
