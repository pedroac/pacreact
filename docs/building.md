# Building and installing

## Dependencies

| | Needed for |
|---|---|
| `wayland`, `wayland-protocols` | the connection and the build-time protocol code (`wayland-scanner`) |
| `cairo` | drawing |
| `libsystemd` | sd-bus, for notifications only (`NOTIFY=0` drops it) |
| `freetype2`, `harfbuzz`, `fribidi` | per-language fonts and Arabic/Hebrew shaping (`I18N=0` drops them) |
| `gettext` (`msgfmt`) | building the translation catalogs at build time |
| `pkgconf`, a C compiler, `make` | build |

There is no build or runtime dependency on `libpulse` or `pactl`: volume is read by talking to the sound server's socket directly. Without a PulseAudio-compatible server the volume HUD simply never appears, and pachud keeps retrying quietly.

## Build

```bash
make                 # everything
make NOTIFY=0        # no notification daemon, no libsystemd
make I18N=0          # English only: no catalogs, FreeType, HarfBuzz or FriBidi
make LOCALEDIR=$PWD/build/locale   # development: find catalogs in the build tree
make test            # unit tests (no display needed)
```

The build uses `-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, PIE, full RELRO and, on x86, control-flow protection.

## Install

```bash
sudo make install                       # /usr/local
make install PREFIX=/usr DESTDIR=pkg    # for packaging
```

Installs the binary, `pachud.service` (to `$(PREFIX)/lib/systemd/user`, override with `UNITDIR`), and, unless `NOTIFY=0`, a D-Bus activation file (to `$(PREFIX)/share/dbus-1/services`, override with `DBUSDIR`). `make uninstall` removes them.

## Arch Linux package

`contrib/PKGBUILD` builds a package from the checkout it lives in, uncommitted changes included, so there is no release to download first:

```bash
cd contrib && makepkg -si
```

It runs `make test` as its check step, installs the binary, the man page (`man pachud`), the user unit, the D-Bus activation file and the catalogs, and provides `notification-daemon`. If mako or dunst is installed as well, their D-Bus files claim the same name: stop and disable them (see [Usage](usage.md#replacing-your-notification-daemon)) so D-Bus activates pachud.

## Tests

`make test` builds and runs unit tests that need no display, hardware or network:

| Suite | Covers |
|---|---|
| `power` | battery and charger decisions, uevent parsing |
| `timer` | duration/alarm/recurrence parsing and scheduling, the IPC commands, the state file (including malformed lines), firing, rescheduling and missed alarms |
| `probe` | slowdown alert and recovery thresholds |
| `audio` | the PulseAudio client: handshake packets, framing across partial reads, the sink-info parser (using a frame captured from a real server), truncated and malformed replies, reconnect triggers, server path selection |
| `hud` | the HUD stack (stacking order, refresh, replacement, removal, full-stack and overflow rules, close reasons) with a fake clock, and the drawing helpers in `paint.c` (markup stripping, word wrap, ellipsis, icon files, including hostile ones) |
| `ipc` | the real unix socket: round trips, error replies, single-instance detection, stale sockets, silent clients |
| `notify` | notification timeouts from urgency and requested expiry (skipped with `NOTIFY=0`) |
| `alerts` | the start-up grace period: same, milder and worse alerts, the 30-minute window, restarts reading the saved state, the clock going backwards, damaged state files, `PACHUD_STARTUP_GRACE` parsing |
| `mem` | the memory alert's cooldown: repeats are held back, escalations are not, and an alert left out as already shown does not start the cooldown |
| `sound` | resolving sound names and paths (and refusing traversal), which sounds are on by default, a fake player started with the right file |
| `history` | the notification history ring (replacement in place, close reasons, wrap-around, listing limits and the footer, text flattening, wiping) |
| `dnd` | do not disturb: what it holds back, the commands, the toasts, the timed switch-off |
| `image` | raw notification pixels into the fixed icon buffer: scaling, letterboxing, premultiplied alpha, rejecting malformed data |
| `icon` | icon-theme lookup across theme layouts and precedence, and refusing anything that could leave the icon directories |
| `scale` | fractional scale arithmetic and `PACHUD_SCALE` parsing |
| `display` | how the display reacts to the compositor's preferred scale (no compositor needed) |
| `text` | font choice and the split of mixed-script text into runs, with real-font checks where the fonts exist (skipped with `I18N=0`) |

Several suites `#include` the source file under test so that its static internals are reachable, and stub the display, the HUD and the loop. `tests/manual/mutation_check.sh` checks the tests themselves: it breaks the code in 44 specific ways and requires the matching test to fail each time (`ONLY=text` runs just the mutations whose name contains that).

The rest was checked by running the daemon on a real Wayland session, with isolated environments so nothing else was affected: a private session bus for notifications (`dbus-run-session`) and a private network namespace for network and probe behaviour (`unshare -Urn`).

Checks that need a Wayland session (and `grim`) live in `tests/manual/` and write screenshots to `build/shots/`: `i18n_test.sh` (translations, scripts, right-to-left; build with `make LOCALEDIR=$PWD/build/locale` first), `fade_test.sh` (fade and surface cleanup, Hyprland only), `dnd_test.sh` and `notify_features_test.sh` (run under `dbus-run-session`: do not disturb, actions, icon names, `image-data`), `startup_grace_test.sh` (a real memory-pressure alert across restarts; needs a machine that is actually under pressure), `stack_test.sh` (several HUDs stacked, at the default and a bottom-left anchor), `click_test.sh` (clicking a card with a virtual pointer, with and without `PACHUD_CLICK=0`; run under `dbus-run-session`), `spec_test.sh` (`CloseNotification` and its error, the signal for a notification do-not-disturb suppressed, a critical card that outlives 15 s, and `pachud history`; run under `dbus-run-session`), and `output_test.sh` and `scale_test.sh` (two virtual outputs and the viewport path in a nested headless `labwc`).
