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

## Tests

`make test` covers the logic that does not need hardware: battery and charger decisions with uevent parsing, duration and alarm-time parsing, and the slowdown decision logic.

The rest was checked by running the daemon on a real Wayland session, with isolated environments so nothing else was affected: a private session bus for notifications (`dbus-run-session`) and a private network namespace for network and probe behaviour (`unshare -Urn`).

Visual checks that need a live Wayland session (and `grim`) live in `tests/manual/`: `i18n_test.sh` (translations, scripts, right-to-left; build with `make LOCALEDIR=$PWD/build/locale` first) and `fade_test.sh` (fade in/out and surface cleanup, Hyprland only). They write screenshots to `build/shots/`.
