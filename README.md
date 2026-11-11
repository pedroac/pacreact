# pachud

A tiny daemon that sleeps until something deserves your attention, then shows a small floating HUD.

`pachud` is the companion to [pacpeek](../pacpeek): pacpeek shows system status when you ask for it, pachud speaks up when the system has something to say. It is built for tiling Wayland compositors such as Hyprland, with the same philosophy:

* **Asleep by default:** one `epoll` loop, no timers running while idle, zero wakeups. Everything it watches is event-driven, preferably straight from the kernel.
* **No configuration overhead:** sensible defaults, a few environment variables, no config language.
* **Minimal dependencies:** `wayland-client` and `cairo`, plus `libsystemd` (sd-bus) for notifications and FreeType, HarfBuzz and FriBidi for translations. `make NOTIFY=0 I18N=0` drops all of those. No toolkit, no GLib, no icon theme.
* **Nothing on screen unless needed:** the Wayland surface exists only while a HUD is visible, and clicks pass straight through it.

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

pachud replaces your notification daemon (mako, dunst, ...); only one can own the bus name.

## Quick start

```bash
make && sudo make install
```

Run it from your compositor, or as a systemd user service:

```bash
systemctl --user enable --now pachud.service
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
pachud                                   run the daemon
pachud toast [-l info|warn|crit] [-t SECONDS] TEXT...
pachud slider PERCENT LABEL...
pachud timer 90s|5m|1h30m [LABEL...]
pachud alarm [-r daily|weekdays|weekends|mon,wed,...] [YYYY-MM-DD] HH:MM [LABEL...]
pachud snooze [DURATION]
pachud pause ID | resume ID | extend ID DURATION
pachud dnd [on [DURATION]|off|toggle|status]
pachud list | cancel ID|all
pachud dismiss
```

## Platform

Linux only, tested on **Arch Linux with Hyprland** (needs the `wlr-layer-shell` protocol, so Sway, river and similar work in principle). It will probably need adapting elsewhere ([details](docs/design.md#platform-and-limits)).

## Documentation

- `man pachud` ([source](docs/pachud.1)): every command and variable in one page
- [Usage](docs/usage.md): commands, environment variables, Hyprland binds, replacing mako
- [Translations](docs/translations.md): languages, fonts, right-to-left, adding a language
- [Building and installing](docs/building.md): dependencies, options, tests
- [Design](docs/design.md): how it stays asleep, each event source, limits

## AI Assistance Disclosure

AI tools were used during the development of pachud for:

- Architecture & Ideation: discussing design trade-offs (kernel-first event sources, a single epoll loop, avoiding heavy dependencies).
- Code Drafting: structuring the Wayland, netlink, PSI and sd-bus code.
- Testing: writing the unit tests and the isolated-session test scripts.
- Documentation & Localization: drafting the documentation and the initial translation catalogs (`po/`), which are machine-written and should be reviewed by native speakers.

All code was reviewed and tested on real hardware by the human author. Some paths (real battery and backlight events) could not be exercised on the development machine and are called out in the documentation.

## License

MIT, see [LICENSE](LICENSE).
