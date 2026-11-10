#!/bin/bash
# Sanity check of the unit tests themselves: break the code on purpose and confirm that the
# matching test fails. Every mutation must be caught. Restores the sources afterwards.
cd "$(dirname "$0")/../.."
BK=$(mktemp -d)
cp src/*.c "$BK"/
restore() { cp "$BK"/*.c src/; rm -rf "$BK"; }
trap restore EXIT

# name | test | file | python expression: old -> new
mutate() {
    local name=$1 test=$2 file=$3 old=$4 new=$5
    cp "$BK"/*.c src/
    OLD="$old" NEW="$new" FILE="src/$file" python3 - <<'EOF' || { echo "SKIP  $name (pattern not found)"; return; }
import os, sys
p, old, new = os.environ["FILE"], os.environ["OLD"], os.environ["NEW"]
s = open(p).read()
if old not in s:
    sys.exit(1)
open(p, "w").write(s.replace(old, new, 1))
EOF
    if ! make "build/test_$test" >/dev/null 2>&1; then
        echo "INVALID $name (the mutated code does not compile: fix the mutation)"
        MISSED=1
    elif timeout -s KILL 30 "build/test_$test" >/dev/null 2>&1; then
        echo "MISSED  $name"
        MISSED=1
    else
        echo "caught  $name"
    fi
    pkill -9 -x "test_$test" 2>/dev/null
}

MISSED=0
mutate "audio: forget the sink index field"      audio src_audio.c  "    get_u32(in);      /* index */" "    /* index */"
mutate "audio: wrong rounding"                   audio src_audio.c  "VOLUME_NORM / 2) / VOLUME_NORM" "0) / VOLUME_NORM"
mutate "audio: show the first channel only"      audio src_audio.c  "if (v > max) max = v;" "if (!max) max = v;"
mutate "hud: duplicate alerts are queued"        hud   hud.c        "else if (it->card || !queued_text(it->text)) enqueue(it);" "else enqueue(it);"
mutate "hud: interrupted alert loses its time"   hud   hud.c        "resumed.ms = left > MIN_RESUME_MS ? (int)left : MIN_RESUME_MS;" "resumed.ms = 100;"
mutate "hud: remove reports a close"             hud   hud.c        "if (visible && cur.id == id) { advance(0); return true; }" "if (visible && cur.id == id) { advance(HUD_CLOSED_OTHER); return true; }"
mutate "hud: wrap ignores the ellipsis room"     hud   hud.c        "double w = n == maxlines - 1 ? maxw - ell : maxw;" "double w = maxw;"
mutate "timer: recurring alarm is not rescheduled" timer timer.c    "if (entries[i].days) {" "if (0) {"
mutate "timer: label may contain a newline"      timer timer.c      "if (*s == '\\n' || *s == '\\t') *s = ' ';" "(void)0;"
mutate "timer: weekday mask off by one"          timer timer.c      "#define WEEKDAYS 0x3E" "#define WEEKDAYS 0x7C"
mutate "ipc: any reply starting with ok is ok"   ipc   ipc.c        "(reply[2] == '\\0' || reply[2] == ' ')" "1"
mutate "notify: short timeouts not raised"       notify notify.c    "ms = expire < MIN_MS ? MIN_MS : expire;" "ms = expire;"
mutate "power: warn threshold"                   power src_power.c  "#define WARN_PCT 15" "#define WARN_PCT 10"
mutate "probe: alert after one bad probe"        probe src_probe.c  "#define BAD_TO_ALERT 2" "#define BAD_TO_ALERT 1"

[ $MISSED -eq 0 ] && echo "all mutations caught" || { echo "SOME MUTATIONS WERE MISSED"; exit 1; }
