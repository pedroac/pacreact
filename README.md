# pacreact

A tiny daemon that sleeps until something deserves your attention, then shows a small floating HUD.

`pacreact` is the companion to [pacpeek](../pacpeek): pacpeek shows system status when you ask for it, pacreact speaks up when the system has something to say. It is built for tiling Wayland compositors such as Hyprland, with the same philosophy:

* **Asleep by default:** one `epoll` loop, and while idle it wakes only for the timers it documents (a 5-minute disk check, a 2-minute temperature check, and the memory fallback). Everything else it watches is event-driven, preferably straight from the kernel.
* **No configuration overhead:** sensible defaults, a plain config file, no config language.
* **Minimal dependencies:** `wayland-client` and `cairo`, plus `libsystemd` (sd-bus) for notifications, FreeType, HarfBuzz and FriBidi for translations, and `libspeechd` for [speech](docs/usage.md#speech). `make NOTIFY=0 I18N=0 SPEECH=none` drops all of those. No toolkit and no GLib; the status icons are Nerd Font glyphs where that font is installed and the icon theme's otherwise, and SVG ones are drawn by the optional `rsvg-convert` tool.
* **Nothing on screen unless needed:** the Wayland surface exists only while a HUD is visible, and clicks pass straight through it, except on notification cards (click one to dismiss it; `click = 0` turns that off).

## What it reacts to

| Event | Source | Shown as |
|---|---|---|
| Volume, mute; microphone level, mute | PulseAudio native protocol (PulseAudio / PipeWire) | slider |
| Brightness | sysfs notification on `/sys/class/backlight` | slider |
| Battery low / critical, charger plugged | kernel uevents (`NETLINK_KOBJECT_UEVENT`) | alert |
| Memory pressure | kernel PSI triggers (`/proc/pressure/memory`) | alert |
| Online / offline | rtnetlink | toast |
| Network slowdown (opt-in) | ICMP probe | alert |
| Timers and alarms | one `timerfd` | alert |
| Desktop notifications | `org.freedesktop.Notifications` on D-Bus | card |

pacreact replaces your notification daemon (mako, dunst, ...); only one can own the bus name.

## Quick start

```bash
make && sudo make install
```

The shared code is the `pacommon` submodule: clone with `--recurse-submodules` (or run `git submodule update --init`), or set `PACOMMON=` to another checkout.

Run it from your compositor, or as a systemd user service:

```bash
systemctl --user enable --now pacreact.service
```

To take over notifications from mako, see [Usage](docs/usage.md#replacing-your-notification-daemon).

## Hyprland

If you're using Hyprland, I recommend binding the FN key:

```lua
local media_binds = {
    { "XF86AudioRaiseVolume",  "pactl set-sink-volume @DEFAULT_SINK@ +5%" },
    { "XF86AudioLowerVolume",  "pactl set-sink-volume @DEFAULT_SINK@ -5%" },
    { "XF86AudioMute",         "pactl set-sink-mute @DEFAULT_SINK@ toggle" },
    { "XF86AudioMicMute",      "pactl set-source-mute @DEFAULT_SOURCE@ toggle" },
    { "XF86AudioPlay",         "playerctl play-pause" },
    { "XF86AudioNext",         "playerctl next" },
    { "XF86AudioPrev",         "playerctl previous" },
    { "XF86AudioStop",         "playerctl stop" },
    { "XF86MonBrightnessUp",   "brightnessctl set +10%" },
    { "XF86MonBrightnessDown", "brightnessctl set 10%-" },
}
-- Drop standalone F23 so it doesn't type characters in terminals
hl.bind("F23", hl.dsp.exec_cmd("true"))
for _, bind in ipairs(media_binds) do
    hl.bind(bind[1], hl.dsp.exec_cmd(bind[2]), { locked = true })
end
```

## Usage

```
pacreact                                   run the daemon
pacreact toast [-l info|warn|crit] [-t SECONDS] TEXT...
pacreact slider PERCENT LABEL...
pacreact timer 90s|5m|1h30m [LABEL...]
pacreact alarm [-r daily|weekdays|weekends|mon,wed,...] [YYYY-MM-DD] HH:MM [LABEL...]
pacreact snooze [DURATION]
pacreact pause ID | resume ID | extend ID DURATION
pacreact dnd [on [DURATION]|off|toggle|status]
pacreact list | cancel ID|all
pacreact dismiss
```

## Platform

Linux only, tested on **Arch Linux with Hyprland** (needs the `wlr-layer-shell` protocol, so Sway, river and similar work in principle). It will probably need adapting elsewhere ([details](docs/design.md#platform-and-limits)).

## Documentation

- `man pacreact` ([source](docs/pacreact.1)): every command and variable in one page
- [Usage](docs/usage.md): commands, settings, Hyprland binds, replacing mako
- [Translations](docs/translations.md): languages, fonts, right-to-left, adding a language
- [Building and installing](docs/building.md): dependencies, options, tests
- [Design](docs/design.md): how it stays asleep, each event source, limits
- [Platform support and known limits](docs/platform.md)
- [Decisions](docs/decisions.md): decisions after the design, and why
- [Platform](docs/platform.md): what assumes Linux, and what was run
- [Contributing](CONTRIBUTING.md)

## What it stores

- `$XDG_STATE_HOME/pacreact/` (default `~/.local/state/pacreact/`, mode 0700): `timers`: the timers, alarms and peeks that are waiting, so that they survive a restart; and `alerts`: when each kind of alert was last shown, so that a restart does not repeat it.
- `$XDG_RUNTIME_DIR/pacreact.sock`: the socket the `pacreact` commands talk to the daemon through. Removed when it ends.
- The notification history is kept in memory only (at most 64 entries) and is gone when the daemon stops.

Deleting the state folder forgets the timers; nothing else depends on it. The one thing it can send over the network is the opt-in `probe_host` ping, and it asks the CUPS server (normally the local one) about print jobs when printing is watched ([decisions](docs/decisions.md)).

## Size

The binary is 196 KiB stripped (`make`, GCC `-Os`, measured 2026-11-15). Its idle memory and wakeups are measured in [design](docs/design.md).

## AI Assistance Disclosure

The tools were Claude (code generation), Dropstone (review and improvement), and Gemini and ChatGPT (investigation), as [the principles](pacommon/PRINCIPLES.md#ai-assisted-human-owned) describe. They were used during the development of pacreact for:

- Architecture & Ideation: discussing design trade-offs (kernel-first event sources, a single epoll loop, avoiding heavy dependencies).
- Code Drafting: structuring the Wayland, netlink, PSI and sd-bus code.
- Testing: writing the unit tests and the isolated-session test scripts.
- Documentation & Localization: drafting the documentation and the initial translation catalogs (`po/`), which are machine-written and should be reviewed by native speakers.

All code was reviewed and tested on real hardware by the human author. Some paths (real battery and backlight events) could not be exercised on the development machine and are called out in the documentation.

## License

MIT, see [LICENSE](LICENSE).
