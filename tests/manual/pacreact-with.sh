#!/bin/bash
# pacreact-with.sh [key=value ...] [-- ARGS...]: runs pacreact with those settings. pacreact reads its
# settings from pacreact.conf files only, so they are written to one in a private folder and pacreact
# is started from there (the project folder's own pacreact.conf, with the defaults, is not in the way).
# sound=0 and fade_ms=0 come first, so that a test is silent and does not wait for a fade; a setting
# given here replaces them. PAC_TEST_BIN names another pacreact binary.
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=$(mktemp -d /tmp/pacreact-with-XXXXXX)
{
    echo "sound = 0"
    echo "fade_ms = 0"
    while [ $# -gt 0 ] && [ "$1" != -- ]; do
        echo "${1%%=*} = ${1#*=}"
        shift
    done
} > "$dir/pacreact.conf"
[ "$1" = -- ] && shift
bin=${PAC_TEST_BIN:-$root/pacreact}
( sleep 5; rm -rf "$dir" ) > /dev/null 2>&1 &   # pacreact reads its settings at start-up
cd "$dir" && exec "$bin" "$@"
