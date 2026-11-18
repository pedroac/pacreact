#!/bin/bash
# Live check that clicking a notification card dismisses it (and that a resident one stays), in a nested headless compositor
# (labwc) with a private session bus. Run it as:
#   dbus-run-session -- bash tests/manual/click_test.sh
# Needs grim, notify-send and wayland-scanner. A virtual pointer (tests/manual/vptr_click.c)
# does the clicking. Screenshots land in build/shots/.
cd "$(dirname "$0")/../.."
mkdir -p build/shots
R=$(mktemp -d /tmp/pacreact-click-XXXXXX)
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
export WAYLAND_DISPLAY=$R/wayland-0 XDG_STATE_HOME=$R/state

# One virtual pointer for the whole test, driven through a FIFO: making and destroying one per
# click made labwc drop events unpredictably.
mkfifo $R/pointer
build/vptr_click 1280 720 < $R/pointer &
V=$!
exec 9> $R/pointer
trap 'kill $C $P $V $MON 2>/dev/null; rm -rf $R' EXIT
click_at() {   # click_at X Y [BUTTON]: travel there like a hand, then click
    echo "move 100 600" >&9; sleep 0.3
    echo "move $1 $2" >&9; sleep 0.3
    echo "move $(( $1 + 1 )) $2" >&9; sleep 0.3
    echo "click ${3:-0x110}" >&9; sleep 0.5
}

fail=0
# The nested labwc sometimes loses a click (pacreact then sees no pointer event at all), so a click
# that has to have an effect is repeated, up to three times, until that effect is seen. A click
# that never works still fails the test.
click_until() {   # click_until CONDITION X Y [BUTTON]
    local cond=$1
    shift
    for _ in 1 2 3; do
        click_at "$@"
        eval "$cond" && return 0
    done
    return 1
}

run_case() {  # run_case LABEL click-VALUE EXPECT (gone|stays)
    echo "== $1 (click='$2')"
    tests/manual/pacreact-with.sh click=$2 2> $R/stderr &
    P=$!
    sleep 1.5
    notify-send -t 20000 -A default=Open "Click me" "a card with a default action" > $R/chosen &
    N=$!
    sleep 0.7
    grim -o HEADLESS-1 build/shots/click_$1_before.png
    if [ "$3" = gone ]; then click_until '! kill -0 $N 2>/dev/null' 1000 40; else click_at 1000 40; fi
    grim -o HEADLESS-1 build/shots/click_$1_after.png
    if kill -0 $N 2>/dev/null; then
        echo "notify-send still waiting: the card was not closed"; result=stays; kill $N
    else
        echo "notify-send finished, app was told: $(cat $R/chosen)"; result=gone
    fi
    [ "$result" = "$3" ] && echo "ok: card $result" || { echo "FAIL: expected $3, got $result"; fail=1; }
    cat $R/stderr
    kill $P; wait $P 2>/dev/null
    sleep 0.3
}

# Resident cards (the `resident` hint): an invoked action does not close them.
resident_case() {
    echo "== resident"
    dbus-monitor "type='signal',interface='org.freedesktop.Notifications'" > $R/signals 2>&1 &
    MON=$!
    tests/manual/pacreact-with.sh 2> $R/stderr &
    P=$!
    sleep 1.5
    check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
    closed() { grep -a -A2 "member=NotificationClosed" $R/signals | grep -a "uint32 $1$" >/dev/null; }
    invoked() { grep -a -A2 "member=ActionInvoked" $R/signals | grep -a "string \"$1\"" >/dev/null; }

    # busctl, not notify-send: notify-send closes the card itself once it hears an action.
    busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
        Notify susssasa{sv}i resident-test 0 "" "Resident" "stays after an action" \
        4 default Open reply Reply 1 resident b true 20000 > /dev/null
    sleep 0.7
    grim -o HEADLESS-1 build/shots/click_resident_before.png
    ./pacreact action 1 >/dev/null   # `default` is not numbered when real actions exist
    sleep 0.4
    check "pacreact action sends ActionInvoked(reply)" 'invoked reply'
    check "...and the resident card is not closed" '! closed 2'
    click_until 'invoked default' 1000 40
    grim -o HEADLESS-1 build/shots/click_resident_after.png
    check "a left click sends ActionInvoked(default)" 'invoked default'
    # The compositor's activation token (xdg-activation-v1) comes first, so the app can take focus.
    token_first() {
        local t d
        t=$(grep -a -n "member=ActivationToken" $R/signals | head -1 | cut -d: -f1)
        d=$(grep -a -n 'string "default"' $R/signals | head -1 | cut -d: -f1)
        [ -n "$t" ] && [ -n "$d" ] && [ "$t" -lt "$d" ] &&
            grep -a -A2 "member=ActivationToken" $R/signals | grep -aq 'string "[^"][^"]*"'
    }
    check "...preceded by an ActivationToken with a token in it" token_first
    check "...and the resident card is still not closed" '! closed 2'
    click_until 'closed 2' 1000 40 0x111
    check "a right click closes it as dismissed (reason 2)" 'closed 2'
    [ $fail -ne 0 ] && grep -a -E "member=|uint32|string" $R/signals | tail -30
    kill $P $MON 2>/dev/null; wait $P 2>/dev/null
}

# The case with clicks off goes last: its left click lands on the desktop, where labwc opens a
# menu that would swallow the next click.
run_case click "" gone
resident_case
run_case off 0 stays
exit $fail
