#!/bin/bash
# Manual visual check of translations and script handling on a live Wayland session.
# Build first with a catalog directory the binary can find:
#   make LOCALEDIR=$PWD/build/locale
# Screenshots land in build/shots/ (needs grim).
cd "$(dirname "$0")/../.."
mkdir -p build/shots
shot() { grim -g "900,1240 760x160" build/shots/$1.png; }
export LANG=en_US.UTF-8

for l in de ja ar; do
    LANGUAGE=$l ./pacreact 2>/dev/null &
    P=$!
    sleep 0.5
    ./pacreact timer 2s tea > /dev/null
    sleep 2.7
    shot i18n_timer_$l
    case $l in
        ar) ./pacreact slider 40 "مستوى الصوت" ;;
        ja) ./pacreact slider 40 "音量" ;;
        de) ./pacreact slider 40 "Lautstärke" ;;
    esac
    sleep 0.3
    shot i18n_slider_$l
    kill $P; wait $P 2>/dev/null
    sleep 0.3
done

# Content-based fonts: an English UI showing text in other scripts
./pacreact 2>/dev/null &
P=$!
sleep 0.5
./pacreact toast -t 4 "日本語のテスト — 한국어 — 中文"
sleep 0.4; shot i18n_content_cjk
./pacreact dismiss
./pacreact toast -t 4 "مرحبا بالعالم — wlan0"
sleep 0.4; shot i18n_content_arabic
kill $P; wait $P 2>/dev/null
echo done
