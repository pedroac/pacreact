#!/bin/bash
# Sanity check of the unit tests themselves: break the code on purpose and confirm that the
# matching test fails. Every mutation must be caught. Restores the sources afterwards.
cd "$(dirname "$0")/../.."
BK=$(mktemp -d)
cp src/*.c src/*.h "$BK"/
restore() { cp "$BK"/*.c "$BK"/*.h src/; rm -rf "$BK"; }
trap restore EXIT

# name | test | file | python expression: old -> new
mutate() {
    local name=$1 test=$2 file=$3 old=$4 new=$5
    [ -n "$ONLY" ] && [[ "$name" != *$ONLY* ]] && return   # ONLY=text re-runs the matching ones
    cp "$BK"/*.c "$BK"/*.h src/
    OLD="$old" NEW="$new" FILE="src/$file" python3 - <<'EOF' || { echo "SKIP  $name (pattern not found: fix the mutation)"; MISSED=1; return; }
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

# --- features added later
mutate "sound: theme names may contain slashes"  sound  sound.c     "if (strchr(spec, '/') || spec[0] == '-' || spec[0] == '.') return false;" "if (0) return false;"
mutate "sound: a failed lookup keeps its partial path" sound sound.c "if (!theme_lookup(spec, found, sizeof found)) return false;" "theme_lookup(spec, found, sizeof found);"
mutate "dnd: critical notifications are held"    dnd    dnd.c       "if (!active || urgency >= CRITICAL) return false;" "if (!active) return false;"
mutate "dnd: the off message ignores the count"  dnd    dnd.c       "    if (missed > 0)" "    if (0)"
mutate "timer: snooze uses the wrong delay"      timer  timer.c     "time(NULL) + secs, last_fired.label" "time(NULL) + 1, last_fired.label"
mutate "timer: pause forgets the time left"      timer  timer.c     "e->left = e->when - now > 1 ? e->when - now : 1;" "e->left = 1;"
mutate "timer: a paused timer is saved as running" timer timer.c    "e->paused ? 'p' : e->alarm ? 'a' : 't'" "e->alarm ? 'a' : 't'"
mutate "timer: February 30th is accepted"        timer  timer.c     "tm.tm_mon != mo - 1 || tm.tm_mday != d) return -1;" "0) return -1;"
mutate "icon: theme names may contain slashes"   icon   icon.c      "if (strchr(spec, '/') || spec[0] == '.' || strlen(spec) > 120) return false;" "if (0) return false;"
mutate "icon: misses are remembered"             icon   icon.c      "    if (found) {
        int slot = next++ % CACHE;" "    {
        int slot = next++ % CACHE;"
mutate "image: a stride smaller than a row"      image  image.c     "if (rowstride < width * channels) return false;" "if (0) return false;"
mutate "image: alpha is not premultiplied"       image  image.c     "(uint32_t)(r * a / 255) << 16" "(uint32_t)r << 16"
mutate "hud: the default action is always shown" hud    hud.c       "if (strcmp(a[i], \"default\") != 0) add_action" "if (1) add_action"
mutate "hud: copies always leave out the image"  hud    hud.c       "if (src->has_image) n = sizeof *src;" ""
mutate "hud: toasts are not shortened"           hud    hud.c       "ellipsize(line, MAX_TEXT_W, FONT_SIZE, false, out, n);" "snprintf(out, n, \"%s\", line);"
mutate "audio: the microphone is labelled as volume" audio src_audio.c "if (t == T_SOURCE) return muted" "if (0) return muted"
mutate "audio: input events are ignored"         audio  src_audio.c "if (facility == FACILITY_SOURCE || facility == FACILITY_SERVER) tracks[T_SOURCE].dirty = true;" "if (facility == FACILITY_SERVER) tracks[T_SOURCE].dirty = true;"
mutate "scale: buffer sizes are rounded down"    scale  scale.h     "(long long)logical * scale120 + SCALE_UNIT - 1" "(long long)logical * scale120"
mutate "display: a pinned scale is overridden"   display display.c  "if (d->scale_fixed || scale < SCALE_MIN" "if (scale < SCALE_MIN"
mutate "text: Thai is not split from Latin"      text   text.c      "if (cls != -2 && c != cls && n < max - 1) {" "if (0) {"

[ $MISSED -eq 0 ] && echo "all mutations caught" || { echo "SOME MUTATIONS WERE MISSED"; exit 1; }
