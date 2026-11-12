# Usage

## The daemon

`pachud` with no arguments runs the daemon. It must run inside your Wayland session. A second copy refuses to start ("already running"). The same binary is the client for every other command, talking to the daemon over `$XDG_RUNTIME_DIR/pachud.sock`.

## Commands

| Command | Effect |
|---|---|
| `pachud toast [-l info\|warn\|crit] [-t SECONDS] TEXT...` | One-line message. `warn` and `crit` are alerts and wait for room when the stack is full (see below). |
| `pachud slider PERCENT LABEL...` | A level bar. Useful from your own keybinds. |
| `pachud timer DURATION [LABEL...]` | `90s`, `5m`, `1h30m`, up to 30 days. A unit is required: `5` is rejected. |
| `pachud alarm [-r RECUR] [YYYY-MM-DD] HH:MM [LABEL...]` | The next such wall-clock time, today or tomorrow. With `-r` it repeats: `daily`, `weekdays`, `weekends`, or days such as `mon,wed,fri`. With a date (`pachud alarm 2026-12-25 07:30 …`) it fires once at that moment; a dated alarm cannot repeat. A recurring alarm stays in `pachud list` (shown as `[weekdays]`) and is rescheduled after it fires; occurrences missed while the daemon was down fire once. |
| `pachud list` | Pending timers and alarms with their ids and time left. |
| `pachud snooze [DURATION]` | Re-arm the timer or alarm that last went off as a new one-shot (default 5m, same label). A snoozed recurring alarm still comes back on its schedule. |
| `pachud pause ID` / `resume ID` | Freeze a timer's remaining time, then continue it. Paused timers stay paused across restarts. Alarms cannot be paused. |
| `pachud extend ID DURATION` | Add time to a timer or alarm (to the time left if paused). Up to 30 days ahead. |
| `pachud cancel ID\|all` | Cancel one or all. |
| `pachud dismiss` | Close the newest alert on screen (the newest item if there is none). |
| `pachud action [N]` | Invoke action N (default 1) of the notification card on screen. See [Actions](#actions). |
| `pachud dnd [on [DURATION]\|off\|toggle\|status]` | Do not disturb: see below. |
| `pachud count` | How many notification cards are on screen or waiting for room, for a status bar's bell. Toasts and sliders do not count. |
| `pachud history [N\|clear]` | The last notifications, newest first (10 by default, up to 64), or wipe them: see [History](#history). |

Timers and alarms survive restarts: they are stored in `$XDG_STATE_HOME/pachud/timers` (default `~/.local/state/pachud/timers`). Ones that came due while the daemon was not running fire when it starts.

## How HUDs stack

HUDs stack in one column, newest on top, in the top-right corner unless `PACHUD_ANCHOR` says otherwise. Each leaves on its own timeout, and the stack shows at most four.

- **Alerts** (warn/crit toasts, timers, battery, memory, every notification card) are never lost: when the stack is full they wait in order for room, up to 8 waiting. The same alert text is not duplicated; showing it again refreshes the one on screen.
- **Sliders and info toasts** are ephemeral. When the stack is full the oldest ephemeral one makes room; if everything shown is an alert, a new ephemeral one is dropped.
- **Volume, microphone, audio output and brightness changes** are not part of the stack: they use a separate on-screen display in the middle of the screen, one at a time, gone a second after the last change.
- Left-clicking a notification card dismisses it, and sends the app its `default` action first if it offered one. Right-clicking dismisses it without invoking anything. Everything else is click-through (the gaps between cards, toasts, sliders and the on-screen display), so a click anywhere else reaches the window underneath. `PACHUD_CLICK=0` makes the whole HUD click-through. `pachud dismiss` on a key closes the newest alert without the mouse.

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PACHUD_SCALE` | follows the compositor | Pin the render scale, 1 to 4 and fractions allowed (`1.5`). Normally unnecessary: pachud uses the scale the compositor asks for, per monitor. |
| `PACHUD_CLICK` | on | `0` makes the HUD fully click-through: clicking a notification card then does nothing. |
| `PACHUD_ANCHOR` | `top-right` | Where the HUD stack sits: `top-right`, `top-left`, `top`, `bottom-right`, `bottom-left` or `bottom`. The stack keeps clear of bars and docks. |
| `PACHUD_OUTPUT` | unset | Show HUDs on this monitor (`DP-1`, `HDMI-A-1`, ... as in `hyprctl monitors`). Unset lets the compositor choose, usually the focused one. An unknown name is reported once on stderr with the names that exist, and the compositor's choice is used. |
| `PACHUD_FADE_MS` | 120 | Fade in/out duration in milliseconds; `0` turns the fade off. |
| `PACHUD_PROBE_HOST` | unset | IPv4 address to ping for the slowdown alert. Unset means the probe does not exist. |
| `PACHUD_PROBE_INTERVAL` | 30 | Seconds between probes (5-3600). |
| `XDG_STATE_HOME` | `~/.local/state` | Where timers are saved. |
| `PACHUD_STARTUP_GRACE` | 60 | Seconds after start-up during which an alert that is not worse than one shown in the last 30 minutes is not shown again. `0` turns it off. See [Alerts at start-up](#alerts-at-start-up). |
| `PACHUD_SOUND` | on | `0` turns all sound off. |
| `PACHUD_PLAYER` | `pw-play`, else `paplay` | Program started with the sound file as its only argument. |
| `PACHUD_ALARM_SOUND` | `alarm-clock-elapsed` | Sound for timers and alarms: an absolute file path or a freedesktop sound-theme name. Empty or `0` is silent. |
| `PACHUD_NOTIFY_SOUND` | none | Sound for every notification card that does not ask for its own. |
| `PACHUD_ALERT_SOUND` | none | Sound for battery and memory alerts. |

## Thresholds (compiled in)

- Battery: warning at 15%, critical at 5%, re-armed above 20% or when charging.
- Memory: PSI trigger `some` 500 ms of 2 s (warning), `full` 250 ms of 2 s (critical), 30 s cooldown. Without PSI it polls `MemAvailable` every 10 s and alerts below 10%.
- Network: after the default route changes, it waits 1 s before reporting. The probe alerts after two consecutive replies slower than 400 ms or missing, and reports recovery after two good ones.

## Alerts at start-up

Memory pressure and a low battery describe a condition that is often still true when pachud starts, so without care every restart (a new login, a service restart) would raise the same alert again. pachud therefore remembers, per source (memory, battery), the last alert it showed and how severe it was, in `$XDG_STATE_HOME/pachud/alerts`. During the first 60 seconds after start-up an alert is left out if it is not worse than one shown in the last 30 minutes: the same alert, or a milder one that the earlier one already covered (a warning after a critical alert). A worse one always comes through, and after the grace period everything behaves as usual, so pressure that is still there a minute later can alert again. Alerts left out this way do not start the cooldown between memory alerts, so an escalation right after is not held back, and a warning never holds back the critical alert that follows it. `PACHUD_STARTUP_GRACE=0` disables all of this.

## Do not disturb

`pachud dnd on` (or `on 30m` for a limited time) stops notification cards from appearing and from making sound, except critical ones. They are not queued for later: pachud just counts them and tells you when you switch it off ("Do not disturb off — 3 notifications skipped"). Timers, alarms, battery and memory alerts and the volume and brightness sliders are not affected. The state lives in memory, so it resets when the daemon restarts. `pachud dnd toggle` suits a keybind; `pachud dnd status` prints the state, time left and the skipped count.

## History

`pachud history` lists the last notifications, newest first, one per line: when, what happened to it, then `[app] summary — body`. It shows 10 by default; `pachud history 30` shows more, as many as fit in one reply (a footer says how many older ones are left out). `pachud history clear` wipes it.

```
01:31  on screen  [notify-send] download — done
01:30  dismissed  ! [notify-send] critical — stays until dismissed
01:30  skipped (do not disturb)  [notify-send] suppressed
01:30  closed by app  [notify-send] to close
```

What happened to each is `on screen`, `expired`, `dismissed` (by you, with a click or `pachud dismiss`), `closed by app`, `skipped (do not disturb)` (so what do-not-disturb held back can be read afterwards), or `not shown` (it never fitted on screen). A `!` marks critical ones. A card an app replaces (a download's progress, say) stays one entry with its latest text. Notifications marked `transient` are not kept, and neither are pachud's own alerts (battery, memory, timers) or sliders.

The history lives in memory only, in a fixed ring of 64 entries: when it is full the oldest entry is overwritten, and it is gone when the daemon stops. Nothing is written to disk. Only text is kept, not icons or images. `clear` overwrites the entries with zeros, not just forgets them.

**Who can see it.** The only way to read the history is `pachud history`, over the daemon's socket: there is no D-Bus method, no file and no log line for it. The socket is mode 0600 in `$XDG_RUNTIME_DIR`, and the daemon also checks that each client runs as the same user, so other users on the machine cannot read it or clear it. Without `$XDG_RUNTIME_DIR` the socket goes in `/tmp/pachud-UID/`, a directory pachud creates mode 0700 and refuses to use if it is anyone else's or open to others. The daemon is marked non-dumpable, so a crash leaves no core dump with the history in it and other processes, even of your own user, cannot read its memory through `/proc/PID/mem` or ptrace (the service unit also sets `LimitCORE=0`). The text is cleaned before it is printed: control characters, including escape sequences and the C1 controls, become spaces, so a notification cannot change what your terminal does when you list the history.

What this does not cover: root can read anything, and so can a debugger started as root; the history is ordinary memory, so it can be written to swap unless swap is encrypted; and any process of your own user can already watch notifications as they arrive on the session bus, the same way `dbus-monitor` does, which no daemon can prevent. A notification the sender marks `transient` is never kept.

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

Summary and body (tags stripped, entities decoded), the `urgency` and `resident` hints, replacement by id, `CloseNotification`, sound hints, actions, and icons in three forms: raw pixels (`image-data`, and its older spellings `image_data` and `icon_data`, scaled down to 64x64), a PNG path (`app_icon` or `image-path`, also as `file://`), or an icon-theme name (`dialog-information`, `firefox`, ...). Theme names are looked up as PNG files in the user's and the system's icon directories: the GTK theme named in `~/.config/gtk-3.0/settings.ini` first, then `hicolor`, then `Adwaita`. SVG-only icons are not found and the card is shown without an icon. A PNG is read once, when the notification arrives, and only if it is a regular file of at most 4 MiB whose header declares at most 2048 x 2048 pixels; anything else (a pipe, a device, an oversized or corrupt file) is ignored and the card is shown without an icon. Sound files from the `sound-file` hint must likewise be regular files.

### Actions

A card with actions shows them as numbered hints (a click only dismisses it, or triggers `default`), `[1] Reply  [2] Archive`, and you invoke one from a keybind:

```bash
pachud action 2      # the second action of the card on screen
pachud action        # the first one
```

pachud tells the app (`ActionInvoked`) and closes the card, unless the app set the `resident` hint: a resident card stays on screen after an action is invoked, and so does one that is left-clicked while it has a `default` action (the click invokes it, again each time). A resident card goes away on its timeout, with a right click, with `pachud dismiss`, or when the app closes or replaces it. At most three actions are shown. The special `default` action ("the user clicked the notification") is not shown as a button unless it is the card's only action, in which case it appears as `[1] Open`. Actions apply to the newest card on screen; cards waiting for room are not available until they are shown. A Hyprland example:

**Focus after a click.** When you left-click a card's `default` action, pachud asks the compositor for an activation token (the `xdg-activation-v1` protocol) from that click and sends it to the app as the `ActivationToken` signal, just before `ActionInvoked`, so the app can use it to bring its window to the front instead of the compositor refusing the focus change. This needs a compositor with `xdg-activation-v1` (Hyprland and other wlroots-based ones have it); without it the app gets only `ActionInvoked`, as before. Actions invoked from the keyboard (`pachud action`) carry no token: they are not tied to an input event pachud can see, and compositors ignore a token that is not.

```lua
hl.bind("SUPER + 1", hl.dsp.exec_cmd("~/.local/bin/pachud action 1"))
hl.bind("SUPER + 2", hl.dsp.exec_cmd("~/.local/bin/pachud action 2"))
```

Timeouts follow the specification. `-1` leaves it to pachud: 3 s for low urgency, 6 s for normal, and critical cards never expire. `0` means never expire for any urgency. Any other value is the number of milliseconds the app asked for, though never less than 1.5 s so a card can be read. A card that never expires stays until you click it, close it with `pachud dismiss`, the app closes it, or the app replaces it, and it takes one of the four slots in the stack meanwhile.

`CloseNotification` closes the card and sends `NotificationClosed` (reason 3); for an id that is not on screen (already expired, dismissed, or never shown) it returns a D-Bus error, as the specification says. A notification that do-not-disturb suppressed is answered with an id and then immediately closed with reason 4 (undefined), so apps that wait for it do not hang; if it replaced a card that was on screen, that card is withdrawn.
