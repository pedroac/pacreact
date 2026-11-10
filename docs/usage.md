# Usage

## The daemon

`pachud` with no arguments runs the daemon. It must run inside your Wayland session. A second copy refuses to start ("already running"). The same binary is the client for every other command, talking to the daemon over `$XDG_RUNTIME_DIR/pachud.sock`.

## Commands

| Command | Effect |
|---|---|
| `pachud toast [-l info\|warn\|crit] [-t SECONDS] TEXT...` | One-line message. `warn` and `crit` are alerts and queue (see below). |
| `pachud slider PERCENT LABEL...` | A level bar. Useful from your own keybinds. |
| `pachud timer DURATION [LABEL...]` | `90s`, `5m`, `1h30m`, up to 30 days. A unit is required: `5` is rejected. |
| `pachud alarm [-r RECUR] HH:MM [LABEL...]` | The next such wall-clock time, today or tomorrow. With `-r` it repeats: `daily`, `weekdays`, `weekends`, or days such as `mon,wed,fri`. A recurring alarm stays in `pachud list` (shown as `[weekdays]`) and is rescheduled after it fires; occurrences missed while the daemon was down fire once. |
| `pachud list` | Pending timers and alarms with their ids and time left. |
| `pachud cancel ID\|all` | Cancel one or all. |
| `pachud dismiss` | Close what is on screen and show the next queued item. |

Timers and alarms survive restarts: they are stored in `$XDG_STATE_HOME/pachud/timers` (default `~/.local/state/pachud/timers`). Ones that came due while the daemon was not running fire when it starts.

## How HUDs stack

- **Alerts** (warn/crit toasts, timers, battery, memory, every notification card) are queued in order and never lost, up to 8 waiting. The same alert text is not duplicated.
- **Sliders and info toasts** are ephemeral: they take the screen for a moment and the alert they interrupted comes back afterwards.
- Nothing can be clicked: the HUD is click-through, so use `pachud dismiss` on a key to close it early.

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PACHUD_SCALE` | 1 | Integer buffer scale (1-4) for HiDPI. |
| `PACHUD_FADE_MS` | 120 | Fade in/out duration in milliseconds; `0` turns the fade off. |
| `PACHUD_PROBE_HOST` | unset | IPv4 address to ping for the slowdown alert. Unset means the probe does not exist. |
| `PACHUD_PROBE_INTERVAL` | 30 | Seconds between probes (5-3600). |
| `XDG_STATE_HOME` | `~/.local/state` | Where timers are saved. |

## Thresholds (compiled in)

- Battery: warning at 15%, critical at 5%, re-armed above 20% or when charging.
- Memory: PSI trigger `some` 500 ms of 2 s (warning), `full` 250 ms of 2 s (critical), 30 s cooldown. Without PSI it polls `MemAvailable` every 10 s and alerts below 10%.
- Network: after the default route changes, it waits 1 s before reporting. The probe alerts after two consecutive replies slower than 400 ms or missing, and reports recovery after two good ones.

## Hyprland

Volume and brightness HUDs appear on their own when the level changes, so your keybinds only need to change the level:

```lua
hl.bind("XF86AudioRaiseVolume", hl.dsp.exec_cmd("wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+"))
hl.bind("XF86AudioLowerVolume", hl.dsp.exec_cmd("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"))
hl.bind("XF86AudioMute",        hl.dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"))
hl.bind("SUPER + Escape",       hl.dsp.exec_cmd("pachud dismiss"))
```

(Adapt the syntax to your Hyprland config format.)

## Replacing your notification daemon

Only one program can own `org.freedesktop.Notifications`. If another one holds it, pachud says so on stderr and runs without notifications.

On a system where mako is started by D-Bus activation (as on Arch), stopping it is not enough: the next notification would start it again. A user-level service file takes precedence over the system one:

```bash
systemctl --user stop mako.service
make install PREFIX=$HOME/.local UNITDIR=$HOME/.config/systemd/user
systemctl --user daemon-reload
systemctl --user enable --now pachud.service
dbus-send --session --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig
```

The last line matters when `~/.local/share/dbus-1/services` did not exist before: a session bus that was already running does not watch a directory it never saw, so without the reload D-Bus keeps activating mako whenever pachud is down (a new login picks the directory up on its own).

Check that it took over: `notify-send hello` should show a pachud card, and `busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetServerInformation` should answer `pachud`. To check the activation path too, stop pachud (`systemctl --user stop pachud.service`), send another notification, and confirm that `systemctl --user is-active pachud.service` is `active` again and mako's is not.

For the unit to start, `WAYLAND_DISPLAY` must be in the systemd user environment (most Hyprland setups run `dbus-update-activation-environment --systemd WAYLAND_DISPLAY`).

To go back to mako: `systemctl --user disable --now pachud.service`, remove `~/.local/share/dbus-1/services/org.pachud.Notifications.service`, reload the bus configuration as above, and start mako (`systemctl --user start mako.service`, or just send a notification and let D-Bus start it).

## Notifications: what is supported

Summary and body (tags stripped, entities decoded), the `urgency` hint, PNG icons given as an absolute path (`app_icon` or the `image-path` hint), replacement by id, and `CloseNotification`. Not supported: actions and buttons, `image-data`, icon theme names, sound.

Timeouts: low urgency 3 s, normal 6 s, critical 15 s. An explicit timeout is honoured between 1.5 and 30 s, and "never expire" is capped at 30 s because nothing can be clicked to close a card.
