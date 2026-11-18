# Platform support

pacreact (a notification and alert daemon) is Linux only. The project started on **Arch Linux** and the author's setup is **Hyprland**, **Sway** and **pacland**; anything not listed here as run is *untested*, not *supported* (principle 14).

## What was run

| Distribution | Compositor | Run by | Date |
|---|---|---|---|
| Arch Linux | the author's setup | the author, daily, as their only notification daemon and OSD | not recorded |
| Debian, Ubuntu or Fedora | | | **not tried** |
| Alpine (musl) | | | **not tried** |

`make test` runs without a display. The release check of principle 15 (a glibc distribution that is not Arch, and Alpine) has not been done for pacreact; `../pacommon/tests/distros.sh` does it for pacommon. Whoever runs pacreact somewhere else should add a row.

## What assumes Linux or a setup

The system interfaces pacreact uses, with where each is read, are in [design.md](design.md#platform-and-limits). In short: `epoll`, `timerfd`, netlink (`NETLINK_ROUTE`, `NETLINK_KOBJECT_UEVENT`), `/proc/pressure`, `/sys/class/backlight` and `/sys/class/power_supply`, evdev, the PulseAudio native protocol, `org.freedesktop.Notifications` on D-Bus through sd-bus (libsystemd), the `wlr-layer-shell` protocol, and CUPS for print jobs. Font paths for Arch, Debian and Noto layouts are in `text.c`.

## Size

Built on Arch Linux with the default options: **208,712 bytes** (`-Os`, stripped,
`--gc-sections`), or **204,616 bytes** with `SPEECH=none`. The resident memory of an idle daemon is in [design.md](design.md): about 2.2 MB
PSS and 1.1 MB private.

## Known limits

- Speech was checked only to build and link (and for what would be said, in `make test`); it has **not been heard** with a running speech-dispatcher, and not with a screen reader's user (principle 36's check, with the eyes closed, is still to do).
- Sizes in mm, in and pt, and `text_size_min` and `text_size_max`, are tested with made-up output sizes; they have **not been checked on a real compositor** or on a screen that reports a wrong size.
