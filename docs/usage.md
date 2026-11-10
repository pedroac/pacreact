# Usage

## The daemon

`pachud` with no arguments runs the daemon. It must run inside your Wayland session. A second copy refuses to start ("already running"). The same binary is the client for every other command, talking to the daemon over `$XDG_RUNTIME_DIR/pachud.sock`.

## Commands

| Command | Effect |
|---|---|
| `pachud toast [-l info\|warn\|crit] [-t SECONDS] TEXT...` | One-line message. `warn` and `crit` are alerts and queue (see below). |
| `pachud slider PERCENT LABEL...` | A level bar. Useful from your own keybinds. |
| `pachud timer DURATION [LABEL...]` | `90s`, `5m`, `1h30m`, up to 30 days. A unit is required: `5` is rejected. |
| `pachud alarm [-r RECUR] [YYYY-MM-DD] HH:MM [LABEL...]` | The next such wall-clock time, today or tomorrow. With `-r` it repeats: `daily`, `weekdays`, `weekends`, or days such as `mon,wed,fri`. With a date (`pachud alarm 2026-12-25 07:30 …`) it fires once at that moment; a dated alarm cannot repeat. A recurring alarm stays in `pachud list` (shown as `[weekdays]`) and is rescheduled after it fires; occurrences missed while the daemon was down fire once. |
| `pachud list` | Pending timers and alarms with their ids and time left. |
| `pachud snooze [DURATION]` | Re-arm the timer or alarm that last went off as a new one-shot (default 5m, same label). A snoozed recurring alarm still comes back on its schedule. |
| `pachud pause ID` / `resume ID` | Freeze a timer's remaining time, then continue it. Paused timers stay paused across restarts. Alarms cannot be paused. |
| `pachud extend ID DURATION` | Add time to a timer or alarm (to the time left if paused). Up to 30 days ahead. |
| `pachud cancel ID\|all` | Cancel one or all. |
| `pachud dismiss` | Close what is on screen and show the next queued item. |
| `pachud action [N]` | Invoke action N (default 1) of the notification card on screen. See [Actions](#actions). |
| `pachud dnd [on [DURATION]\|off\|toggle\|status]` | Do not disturb: see below. |

Timers and alarms survive restarts: they are stored in `$XDG_STATE_HOME/pachud/timers` (default `~/.local/state/pachud/timers`). Ones that came due while the daemon was not running fire when it starts.

## How HUDs stack

- **Alerts** (warn/crit toasts, timers, battery, memory, every notification card) are queued in order and never lost, up to 8 waiting. The same alert text is not duplicated.
- **Sliders and info toasts** are ephemeral: they take the screen for a moment and the alert they interrupted comes back afterwards.
- Nothing can be clicked: the HUD is click-through, so use `pachud dismiss` on a key to close it early.

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PACHUD_SCALE` | follows the compositor | Pin the render scale, 1 to 4 and fractions allowed (`1.5`). Normally unnecessary: pachud uses the scale the compositor asks for, per monitor. |
| `PACHUD_OUTPUT` | unset | Show HUDs on this monitor (`DP-1`, `HDMI-A-1`, ... as in `hyprctl monitors`). Unset lets the compositor choose, usually the focused one. An unknown name is reported once on stderr with the names that exist, and the compositor's choice is used. |
| `PACHUD_FADE_MS` | 120 | Fade in/out duration in milliseconds; `0` turns the fade off. |
| `PACHUD_PROBE_HOST` | unset | IPv4 address to ping for the slowdown alert. Unset means the probe does not exist. |
| `PACHUD_PROBE_INTERVAL` | 30 | Seconds between probes (5-3600). |
| `XDG_STATE_HOME` | `~/.local/state` | Where timers are saved. |
| `PACHUD_SOUND` | on | `0` turns all sound off. |
| `PACHUD_PLAYER` | `pw-play`, else `paplay` | Program started with the sound file as its only argument. |
| `PACHUD_ALARM_SOUND` | `alarm-clock-elapsed` | Sound for timers and alarms: an absolute file path or a freedesktop sound-theme name. Empty or `0` is silent. |
| `PACHUD_NOTIFY_SOUND` | none | Sound for every notification card that does not ask for its own. |
| `PACHUD_ALERT_SOUND` | none | Sound for battery and memory alerts. |

## Thresholds (compiled in)

- Battery: warning at 15%, critical at 5%, re-armed above 20% or when charging.
- Memory: PSI trigger `some` 500 ms of 2 s (warning), `full` 250 ms of 2 s (critical), 30 s cooldown. Without PSI it polls `MemAvailable` every 10 s and alerts below 10%.
- Network: after the default route changes, it waits 1 s before reporting. The probe alerts after two consecutive replies slower than 400 ms or missing, and reports recovery after two good ones.

## Do not disturb

`pachud dnd on` (or `on 30m` for a limited time) stops notification cards from appearing and from making sound, except critical ones. They are not queued for later: pachud just counts them and tells you when you switch it off ("Do not disturb off — 3 notifications skipped"). Timers, alarms, battery and memory alerts and the volume and brightness sliders are not affected. The state lives in memory, so it resets when the daemon restarts. `pachud dnd toggle` suits a keybind; `pachud dnd status` prints the state, time left and the skipped count.

## Sound

pachud links no audio library: it starts an external player (`pw-play` or `paplay`) with the sound file. By default only timers and alarms make a sound, once when they fire; everything else is silent until you opt in with the variables above. Sound names are looked up as `NAME.oga`, `.ogg` or `.wav` in `sounds/freedesktop/stereo` under `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`, so `message`, `bell` or `dialog-warning` work with the standard sound theme. Several sounds within 400 ms play once.

Notifications can ask for a sound themselves with the `sound-file` or `sound-name` hints, and `suppress-sound` silences them. Those are honoured, as are the player and theme lookups above.

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

Summary and body (tags stripped, entities decoded), the `urgency` hint, replacement by id, `CloseNotification`, sound hints, actions, and icons in three forms: raw pixels (`image-data`, and its older spellings `image_data` and `icon_data`, scaled down to 64x64), a PNG path (`app_icon` or `image-path`, also as `file://`), or an icon-theme name (`dialog-information`, `firefox`, ...). Theme names are looked up as PNG files in the user's and the system's icon directories: the GTK theme named in `~/.config/gtk-3.0/settings.ini` first, then `hicolor`, then `Adwaita`. SVG-only icons are not found and the card is shown without an icon.

### Actions

The HUD cannot be clicked, so a card with actions shows them as numbered hints, `[1] Reply  [2] Archive`, and you invoke one from a keybind:

```bash
pachud action 2      # the second action of the card on screen
pachud action        # the first one
```

pachud tells the app (`ActionInvoked`) and closes the card. At most three actions are shown. The special `default` action ("the user clicked the notification") is not shown as a button unless it is the card's only action, in which case it appears as `[1] Open`. Only the card on screen can be acted on; queued cards are not available until they are shown. A Hyprland example:

```lua
hl.bind("SUPER + 1", hl.dsp.exec_cmd("~/.local/bin/pachud action 1"))
hl.bind("SUPER + 2", hl.dsp.exec_cmd("~/.local/bin/pachud action 2"))
```

Timeouts: low urgency 3 s, normal 6 s, critical 15 s. An explicit timeout is honoured between 1.5 and 30 s, and "never expire" is capped at 30 s because nothing can be clicked to close a card.
