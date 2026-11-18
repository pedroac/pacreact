# Usage

## The daemon

`pacreact` with no arguments runs the daemon. It must run inside your Wayland session. A second copy refuses to start ("already running"). The same binary is the client for every other command, talking to the daemon over `$XDG_RUNTIME_DIR/pacreact.sock`.

## Commands

| Command | Effect |
|---|---|
| `pacreact toast [-l info\|warn\|crit] [-t SECONDS] [-i ICON] TEXT...` | A message; long text wraps onto up to three lines. `warn` and `crit` are alerts and wait for room when the stack is full (see below). |
| `pacreact slider PERCENT LABEL...` | A level bar. Useful from your own keybinds. |
| `pacreact timer DURATION [LABEL...]` | `90s`, `5m`, `1h30m`, up to 30 days. A unit is required: `5` is rejected. |
| `pacreact alarm [-r RECUR] [YYYY-MM-DD] HH:MM [LABEL...]` | The next such wall-clock time, today or tomorrow. With `-r` it repeats: `daily`, `weekdays`, `weekends`, or days such as `mon,wed,fri`. With a date (`pacreact alarm 2026-12-25 07:30 …`) it fires once at that moment; a dated alarm cannot repeat. A recurring alarm stays in `pacreact list` (shown as `[weekdays]`) and is rescheduled after it fires; occurrences missed while the daemon was down fire once. |
| `pacreact peek [-r RECUR] HH:MM [LABEL...]` | Show the time quietly at that time of day, like glancing at a clock: a small toast, no sound. Repeats with `-r` like an alarm. See [Peeking at the time](#peeking-at-the-time). |
| `pacreact peek every DURATION [LABEL...]` | Show the time after that much time at the computer, again and again: `30m`, `1h`. At least `1m`. |
| `pacreact print` | The print jobs that have not finished, one per line: id, state (`waiting`, `held`, `printing`, `stopped`), queue and name. Asks CUPS when run. |
| `pacreact time` | Show the time and the date in the middle of the screen for a moment. For a key. |
| `pacreact list` | Pending timers, alarms and peeks with their ids and time left. |
| `pacreact snooze [DURATION]` | Re-arm the timer or alarm that last went off as a new one-shot (default 5m, same label). A snoozed recurring alarm still comes back on its schedule. |
| `pacreact pause ID` / `resume ID` | Freeze a timer's remaining time, then continue it. Paused timers stay paused across restarts. Alarms cannot be paused. |
| `pacreact extend ID DURATION` | Add time to a timer or alarm (to the time left if paused). Up to 30 days ahead. |
| `pacreact cancel ID\|all` | Cancel one or all (peeks too). |
| `pacreact dismiss` | Close the front card of the [attention deck](#the-attention-deck), else the newest alert on screen (the newest item if there is none). |
| `pacreact action [N]` | Invoke action N (default 1) of the notification card on screen. See [Actions](#actions). |
| `pacreact dnd [on [DURATION]\|off\|toggle\|status]` | Do not disturb: see below. |
| `pacreact expand` | Show more of the front card of the attention deck (else the newest card), or less again: see [Reading long notifications](#reading-long-notifications). |
| `pacreact count` | How many notification cards are on screen or waiting for room, for a status bar's bell. Toasts and sliders do not count. |
| `pacreact history [N\|clear]` | The last notifications, newest first (10 by default, up to 64), or wipe them: see [History](#history). |

Timers and alarms survive restarts: they are stored in `$XDG_STATE_HOME/pacreact/timers` (default `~/.local/state/pacreact/timers`). Ones that came due while the daemon was not running fire when it starts.

## How HUDs stack

HUDs stack in one column, all the same width (a long toast wraps onto up to three lines rather than growing wider), newest on top, in the top-right corner unless `anchor` says otherwise. Each leaves on its own timeout, and the stack shows at most four.

- **Alerts** (warn/crit toasts, timers, battery, memory, every notification card) are never lost: when the stack is full they wait in order for room, up to 8 waiting. The same alert text is not duplicated; showing it again refreshes the one on screen.
- **Sliders and info toasts** are ephemeral. When the stack is full the oldest ephemeral one makes room; if everything shown is an alert, a new ephemeral one is dropped.
- **Volume, microphone and brightness changes** are not part of the stack: they use a separate on-screen display in the middle of the screen, one at a time, gone a second after the last change.
- Left-clicking a notification card dismisses it, and sends the app its `default` action first if it offered one. Right-clicking dismisses it without invoking anything. Everything else is click-through (the gaps between cards, toasts, sliders and the on-screen display), so a click anywhere else reaches the window underneath. `click = 0` makes the whole HUD click-through. `pacreact dismiss` on a key closes the newest alert without the mouse.

## Config file

pacreact is configured in a file named `pacreact.conf`, one `key = value` per line. `#` starts a comment:

```ini
# ~/.config/pac/pacreact.conf
font_size = 22
font = /usr/share/fonts/TTF/MyFont.ttf
anchor = bottom-right
peek_idle = 10
print = done
```

pacreact reads these, each overriding the earlier: `/etc/pac/pacreact.conf`, `~/.config/pac/pacreact.conf` (or under `$XDG_CONFIG_HOME`), and `pacreact.conf` in the current folder (the one in the project folder lists every setting with its default, so run from a checkout and it is read as it is). The other pac tools look in the same three places. A copy of the one with the defaults is installed in `share/doc/pacreact/` to start from.

The config file is the only place settings come from: environment variables are not read for them, so a systemd unit sets them in the file too. A line that is not `key = value`, or a key that is not a setting, is reported on standard error and skipped. The file is read when the daemon starts; restart it to apply changes.

## Settings

Settings that are numbers are read strictly: the whole value must be a whole number. Something else (`abc`, `1.5`, `10kg`) is reported on standard error and the default is used; a number outside the setting's range is reported and brought to the nearest end of it. An empty value is the same as unset.

| Key | Default | Meaning |
|---|---|---|
| `osd` | `center` | `stack` shows the volume and brightness bars and the `pacreact time` clock in the stack with everything else, instead of in the middle of the screen. |
| `contrast` | normal | `high` is a pure black background, white text and brighter colours, with nothing dimmed. The layout does not change. |
| `language` | environment | The language of the texts, e.g. `pt` or `pt_BR`; unset uses `LANGUAGE`/`LC_MESSAGES`/`LANG`. |
| `icons` | on | `0` turns the status icons off; `theme` uses only the icon theme's, not Nerd Font glyphs. See [Status icons](#status-icons). |
| `icon_font` | found | The font file (`.ttf`, `.otf`) the glyph status icons come from. Unset looks for `SymbolsNerdFont-Regular.ttf` (or the Mono one) under `~/.local/share/fonts`, `~/.fonts`, `/usr/local/share/fonts` and `/usr/share/fonts`. |
| `icon_size` | `24` | The size of the status icons in pixels, 12 to 128 (or a [length](#lengths-with-a-unit)); by default 24 at the default font size, larger or smaller with it. |
| `margin` | `16` | The distance from the screen's edge to the stack (and to the top of the attention deck), in pixels, 0 to 200 (or a [length](#lengths-with-a-unit)). |
| `font_size` | `16` | The size of the text in pixels, 10 to 48 (or a [length](#lengths-with-a-unit)). Everything is scaled with it: all the fonts, the padding, the icons and bars, and the width of the cards and toasts, so a 20 makes the HUD 1.25 times as large. See [Size and fonts](#size-and-fonts). |
| `text_scale` | `1` | The shared name for the text size, 0.5 to 2.5; `1` is 16 px, and it sets `font_size`. |
| `width` | follows the font size (380) | The width of the cards, toasts and sliders in the stack, in pixels, 200 to 1200 (or a [length](#lengths-with-a-unit)), on its own. |
| `text_size_min`, `text_size_max` | unset | The smallest and the largest the smallest text on the HUD may be, as a [length](#lengths-with-a-unit) such as `3mm`: the same size on every screen, whatever `font_size` or `text_scale` say. See [Size and fonts](#size-and-fonts). |
| `speech` | `yes` | `no` does not speak what the HUD shows, and does not connect to speech-dispatcher. Only in a build with speech. See [Speech](#speech). |
| `font` | built in | A font file (`.ttf`, `.otf`) for Latin text instead of the default. |
| `font_bold` | | The bold file of that font; without it, bold is made from the regular one. |
| `scale` | follows the compositor | Pin the render scale, 1 to 4 and fractions allowed (`1.5`). Normally unnecessary: pacreact uses the scale the compositor asks for, per monitor. |
| `click` | on | `0` makes the HUD fully click-through: clicking a notification card then does nothing. |
| `anchor` | `top-right` | Where the HUD stack sits (the [attention deck](#the-attention-deck) is always at the top centre, except that with `top` it is the stack): `top-right`, `top-left`, `top`, `bottom-right`, `bottom-left` or `bottom`. The stack keeps clear of bars and docks. |
| `output` | unset | Show HUDs on this monitor (`DP-1`, `HDMI-A-1`, ... as in `hyprctl monitors`). Unset lets the compositor choose, usually the focused one. An unknown name is reported once on stderr with the names that exist, and the compositor's choice is used. |
| `fade_ms` | 120 | Fade in/out duration in milliseconds; `0` turns the fade off. |
| `probe_host` | unset | IPv4 address to ping for the slowdown alert. Unset means the probe does not exist. |
| `probe_interval` | 30 | Seconds between probes (5-3600). |
| `startup_grace` | 60 | Seconds after start-up during which an alert that is not worse than one shown in the last 30 minutes is not shown again. `0` turns it off. See [Alerts at start-up](#alerts-at-start-up). |
| `clock_every` | `30` | Minutes between the times the clock shows itself in the middle of the screen, counted from the hour: `30` is at :00 and :30, `15` at :00, :15, :30 and :45. 1 to 60; `0` turns it off. Not shown while do-not-disturb is on, and gone after a moment. With `osd = stack` it is a toast. |
| `peek_idle` | `5` | Minutes without touching the keyboard or the pointer after which you count as away from the computer, 1 to 120. Only for [peeks after use](#peeking-at-the-time). |
| `peek_break` | `15` | Minutes away that count as a break: the use counted so far starts over, 1 to 720. |
| `print` | on | `0` turns the printing toasts off; `done` leaves out the one for a job starting to print. See [Printing](#printing). |
| `print_debug` | off | `1` prints what CUPS and the bus said, on standard error. For bug reports. |
| `usb` | all | `0` turns the USB device toasts off. A comma separated list of kinds (`camera`, `printer`, `tablet`, `storage`, `audio`, `input`, `other`) announces only those. See [USB devices](#usb-devices). |
| `bluetooth` | on | `0` turns the Bluetooth device toasts off. |
| `lock_keys` | off | `1` watches the keyboards for Caps Lock and Num Lock, with a kernel filter that lets only LED events through. |
| `jacks` | auto | `0` turns the audio-jack toasts off. `generic` makes every headphone or microphone jack plug say "Audio device connected" or "Audio device disconnected". See [Audio jacks](#audio-jacks). |
| `jacks_debug` | off | `1` prints each jack switch event, port change, volume reading (with the device and port it came from) and decision, with timestamps, on standard error. For bug reports. |
| `disk` | on | `0` turns the disk-space alerts off. |
| `disk_warn` | `10` | Free space, in percent, that warns (2 to 95). |
| `disk_crit` | `5` | Free space, in percent, that is critical (1 to 94). |
| `disk_interval` | `300` | Seconds between disk checks; `0` checks only when the mount table changes. |
| `temp` | on | `0` turns the temperature alerts off. |
| `temp_warn` | `85` | Degrees Celsius that warns (20 to 150). |
| `temp_crit` | `95` | Degrees Celsius that is critical (20 to 150). |
| `temp_interval` | `120` | Slowest seconds between temperature checks; `0` reacts only to kernel events. |
| `sound` | on | `0` turns all sound off. |
| `player` | `pw-play`, else `paplay` | Program started with the sound file as its only argument. |
| `alarm_sound` | `alarm-clock-elapsed` | Sound for timers and alarms: an absolute file path or a freedesktop sound-theme name. Empty or `0` is silent. |
| `notify_sound` | none | Sound for every notification card that does not ask for its own. |
| `alert_sound` | none | Sound for battery and memory alerts. |
| `sound_volume` | `1` with speech, else none | A short tick on every output volume or mute change (even in quick succession), `0` for none: `1` for the theme's `audio-volume-change`, or a sound name or path. |
| `sound_power` | none | Sound when the charger is plugged in or unplugged. See [Sound](#sound) for the values. |
| `sound_devices` | none | Sound when a headset, headphones, microphone, USB device or Bluetooth device connects or disconnects. |
| `sound_network` | none | Sound when the network comes online or goes offline. |

Timers are saved under `$XDG_STATE_HOME` (`~/.local/state`).

## Thresholds (compiled in)

- Battery: warning at 15%, critical at 5%, re-armed above 20% or when charging.
- Memory: PSI trigger `some` 500 ms of 2 s (warning), `full` 250 ms of 2 s (critical), 30 s cooldown. Without PSI it polls `MemAvailable` every 10 s and alerts below 10%.
- Network: after the default route changes, it waits 1 s before reporting. The probe alerts after two consecutive replies slower than 400 ms or missing, and reports recovery after two good ones.

## Alerts at start-up

Memory pressure and a low battery describe a condition that is often still true when pacreact starts, so without care every restart (a new login, a service restart) would raise the same alert again. pacreact therefore remembers, per source (memory, battery), the last alert it showed and how severe it was, in `$XDG_STATE_HOME/pacreact/alerts`. During the first 60 seconds after start-up an alert is left out if it is not worse than one shown in the last 30 minutes: the same alert, or a milder one that the earlier one already covered (a warning after a critical alert). A worse one always comes through, and after the grace period everything behaves as usual, so pressure that is still there a minute later can alert again. Alerts left out this way do not start the cooldown between memory alerts, so an escalation right after is not held back, and a warning never holds back the critical alert that follows it. `startup_grace = 0` disables all of this.

## The attention deck

Most notifications do not need you, and it is easy to learn to ignore them. A few do, so those do not appear in the stack at the top right but in a place of their own, at the top centre:

- notifications of **critical urgency**,
- notifications with the category **`call.incoming`** (a call that is ringing),
- pacreact's own **critical alerts**: the red ones for battery, memory, disk space and temperature.

Nothing else goes there, and it cannot be configured to: warnings (the amber ones, including a battery that is merely low), timers, alarms and everything ordinary stay in the stack. A notification that is replaced by its app and becomes critical moves to the deck, and back.

The deck holds **three** at most, as a pile of cards. The **front** one is the oldest that is still unhandled, shown in full, and it is never swapped for a newer one, so it does not change under your pointer when something else arrives. The others sit behind it as one-line **strips** of their summaries, tucked under the front card and each a little narrower. When the front one is dismissed, handled or expires, the next comes forward. A fourth waits out of sight until there is room.

The front card is the one that `pacreact action`, `pacreact expand`, `pacreact dismiss` and the hover and click behaviour work on, before anything in the stack. Clicking a strip brings that card to the front (that is your choice, never done on its own), and right-clicking a strip dismisses that card without touching the front one. Each keeps its own timeout wherever it is in the pile. Critical notifications with no timeout asked for do not expire; they wait for you.

Do-not-disturb still lets only critical urgency through, so a call of normal urgency is held back while it is on. The deck needs room beside the stack. With the stack in a top corner, the screen has to be at least 1204 logical pixels wide (three items of 380, two margins and two gaps of 16); on a narrower screen the two would overlap, so pacreact does not use the deck there and attention items go in the stack at the top right like everything else, still colour-coded by level. The width is the *logical* one, after scaling (a 1920-pixel screen at scale 2 is 960 wide), read from the compositor with the `xdg-output` protocol and followed as it changes. Which screen: the one named by `output`, else the one the HUD last appeared on, else (before it has ever appeared) the narrowest. If the width is not known it is taken to be enough. With a stack at the bottom corners the deck needs no width, being at the top, and with `anchor = top` the stack is itself at the top centre, so there is no separate place and attention items simply go in the stack.

## Reading long notifications

A card shows the first four lines of its body. To read more, rest the pointer on the card: after a quarter of a second it opens to show the body up to 20 lines, and it closes when the pointer leaves. While the pointer is on a card, or the card is open, its timeout waits, so it does not vanish while you read; when you let go it stays for at least two seconds more. Passing over a card without stopping opens nothing.

Without the mouse, `pacreact expand` opens the newest card (`ok expanded`) and the same command closes it again (`ok collapsed`). An opened card does not expire until it is closed, so bind it to a key and close it when done. If an app updates a card you opened, it stays open.

Only cards open this way; toasts and sliders are short and stay click-through. Opening by pointer needs the HUD to take the pointer over cards, so `click = 0` turns it off (`pacreact expand` still works). Opening moves the cards below it down, and closing moves them back, so after moving from an open card to the one below, the lower card slides up and one more movement is needed to rest on it. A body is kept up to 2048 bytes; anything longer is cut, and beyond 20 lines an opened card ends in an ellipsis.

## Printing

pacreact tells you what a printer does when you are not looking at it:

- `Printing — Quarterly report.pdf` when a job goes to the printer (`print = done` leaves this one out), then `Printed — Quarterly report.pdf` when it finishes, `Printed with errors — …` when it finishes with warnings, and `Print failed — …` when it aborts (a warning, in amber).
- When a printer needs attention: `HP ENVY 4520 series: out of paper`, `paper jam`, `out of ink or toner`, `ink or toner low`, `cover open`, `offline` or `stopped`, each once when it starts, and `ready` when everything is well again. Ink running low is information; the others are warnings that wait for room like any other alert.
- Not a job you cancelled yourself. A printer you paused on purpose is not a problem.

`pacreact print` lists the jobs that have not finished, for a key or a status bar. It asks CUPS when you run it, so it costs nothing in between.

CUPS announces these on the system bus, but only to a program that has asked. pacreact asks over CUPS's socket (`/run/cups/cups.sock`, or the path in `CUPS_SERVER`) the way GNOME's print notifications do, marks its request with its process id, renews it before its 24 hour lease runs out, and gives it back when it stops. If it was killed without that, the next start cancels the leftover, and only one whose process no longer runs, so CUPS does not send everything twice (a pacreact that is running keeps its own). While nothing prints, nothing wakes the daemon. If CUPS is not installed, pacreact does nothing; if it starts later, pacreact keeps trying with longer and longer pauses. Where CUPS starts on demand, pacreact's first question starts it; `print = 0` avoids that. A job's name is whatever the program called it, so it is shown on one line, cut if long. `print_debug = 1` prints what CUPS and the bus said, on standard error.

Scanning has no such announcements: nothing outside the scanning program is told when a scan starts or ends, so there is nothing to show. A scanner being plugged in is reported with the other [USB devices](#usb-devices).

## USB devices

Plugging a USB device in or out gives a toast with its name and an icon for what it is: a webcam, a printer, a drawing tablet, a USB drive, a headset, a keyboard or mouse, a phone. For example `HP ENVY 4520 series connected`, `Wacom Intuos BT M disconnected`. The name is the one the device reports about itself (its maker, if the product name does not already say it), shown with any control characters replaced, since a device chooses its own strings.

What it announces:

- Not what is already plugged in when pacreact starts, though unplugging one of those is announced.
- Not hubs, Bluetooth adapters (the devices on them are reported as [Bluetooth](#sound) devices), or devices the kernel says are built in, such as a laptop's own webcam: they are not things you connect.
- A device that goes out and comes straight back within a second (waking from sleep, a loose cable) is not announced; one that is swapped for another in the same port is.

Each device is one of seven kinds, from what it says it is: `camera` (a video interface, even when it has a microphone too), `printer`, `storage` (a USB drive or a card reader), `audio`, `tablet`, `input` (a keyboard, mouse or game controller, with its own icon) and `other`. A drawing tablet is an ordinary HID device to the kernel; pacreact finds it by the pen it reports, whoever makes it, and by the maker (Wacom, Huion, XP-Pen, UC-Logic, Waltop) when that is not there yet. `usb = camera,printer,tablet` announces only those kinds, and `usb = 0` turns all of it off. A name that is not a kind is reported on standard error and left out. `sound_devices` makes a sound for these too, like the jacks and Bluetooth.

It reports the connection, not that a drive is mounted and ready. It does not see SD cards in a built-in reader, Thunderbolt or FireWire, or a printer on the network, which does not connect.

## Peeking at the time

Without a task bar there is no clock to glance at. A peek is that glance: the time, shown quietly, when you would want to know it. It is not a timer or an alarm: a small toast with no sound, never in the history, not in the way, and not shown at all while [do not disturb](#do-not-disturb) is on.

- `pacreact time` shows the time and the date, large, in the middle of the screen for a couple of seconds. Bind it to a key.
- At a time of day: `pacreact peek 11:45 Lunch soon` once, or `pacreact peek -r weekdays 23:00 Too late` every weekday, with the same recurrences as an alarm (`daily`, `weekdays`, `weekends`, `mon,wed,fri`). The toast reads `11:45 — Lunch soon`. A peek that is late, because the daemon was not running or the machine was asleep, is skipped rather than shown with the wrong time; a recurring one carries on.
- After a stretch of use: `pacreact peek every 1h` shows `14:32 — 1h of use` after every hour at the computer, `pacreact peek every 30m Stretch` every half hour, with your label instead.

`pacreact list` shows them with the alarms and timers (`peek`), `pacreact cancel ID` removes one, and they are kept across restarts in the same file. Peeks are not snoozed, paused or extended.

**What counts as use.** Use is the time you are at the computer. You are away after `peek_idle` minutes (5 by default) with no keyboard or pointer input, and the counting stops, ignoring those minutes. When you come back it continues, unless you were away for `peek_break` minutes (15) or more, which starts the count over. A sleep as long as a break does too, and time asleep is never counted. This needs the compositor's idle notifications (`ext-idle-notify-v1`, which Hyprland, sway and most others have). Without them pacreact cannot tell when you are away, and counts the time since it started instead. The count is kept in memory only: a restart starts it over, though the peeks themselves are saved.

## Do not disturb

`pacreact dnd on` (or `on 30m` for a limited time) stops notification cards from appearing and from making sound, except critical ones. They are not queued for later: pacreact just counts them and tells you when you switch it off ("Do not disturb off — 3 notifications skipped"). Timers, alarms, battery and memory alerts and the volume and brightness sliders are not affected, except that the opt-in sounds for power, devices and network (see [Sound](#sound)) are held back while it is on. The state lives in memory, so it resets when the daemon restarts. `pacreact dnd toggle` suits a keybind; `pacreact dnd status` prints the state, time left and the skipped count.

## History

`pacreact history` lists the last notifications, newest first, one per line: when, what happened to it, then `[app] summary — body`. It shows 10 by default; `pacreact history 30` shows more, as many as fit in one reply (a footer says how many older ones are left out). `pacreact history clear` wipes it.

```
01:31  on screen  [notify-send] download — done
01:30  dismissed  ! [notify-send] critical — stays until dismissed
01:30  skipped (do not disturb)  [notify-send] suppressed
01:30  closed by app  [notify-send] to close
```

What happened to each is `on screen`, `expired`, `dismissed` (by you, with a click or `pacreact dismiss`), `closed by app`, `skipped (do not disturb)` (so what do-not-disturb held back can be read afterwards), or `not shown` (it never fitted on screen). A `!` marks critical ones. A card an app replaces (a download's progress, say) stays one entry with its latest text. Notifications marked `transient` are not kept, and neither are pacreact's own alerts (battery, memory, timers) or sliders.

The history lives in memory only, in a fixed ring of 64 entries: when it is full the oldest entry is overwritten, and it is gone when the daemon stops. Nothing is written to disk. Only text is kept, not icons or images. `clear` overwrites the entries with zeros, not just forgets them.

**Who can see it.** The only way to read the history is `pacreact history`, over the daemon's socket: there is no D-Bus method, no file and no log line for it. The socket is mode 0600 in `$XDG_RUNTIME_DIR`, and the daemon also checks that each client runs as the same user, so other users on the machine cannot read it or clear it. Without `$XDG_RUNTIME_DIR` the socket goes in `/tmp/pacreact-UID/`, a directory pacreact creates mode 0700 and refuses to use if it is anyone else's or open to others. The daemon is marked non-dumpable, so a crash leaves no core dump with the history in it and other processes, even of your own user, cannot read its memory through `/proc/PID/mem` or ptrace (the service unit also sets `LimitCORE=0`). The text is cleaned before it is printed: control characters, including escape sequences and the C1 controls, become spaces, so a notification cannot change what your terminal does when you list the history.

What this does not cover: root can read anything, and so can a debugger started as root; the history is ordinary memory, so it can be written to swap unless swap is encrypted; and any process of your own user can already watch notifications as they arrive on the session bus, the same way `dbus-monitor` does, which no daemon can prevent. A notification the sender marks `transient` is never kept.

## Status icons

pacreact's own messages come with the icon theme's status icons where the theme has them: a battery on the battery alerts, a plug or a cable for the charger, the network (offline, wired or wireless), a headset, headphones or microphone when one is connected, Bluetooth, a bell struck through for do-not-disturb, an alarm for timers, a drive for a full disk, and a speaker with more or fewer waves on the volume bar (and a microphone on the microphone bar), a sun on the brightness bar. They are the symbolic icons (`battery-low-symbolic`), which have no colour of their own, drawn in the text's colour, or in the level's amber or red for warnings and critical alerts. A theme with only coloured icons has them drawn as they are.

The icons come from the GTK icon theme named in `~/.config/gtk-3.0/settings.ini`, then `hicolor`, `Adwaita` and `AdwaitaLegacy`, in the user's and the system's icon directories. Most themes ship SVG now (Adwaita and Breeze do, with no PNG), so pacreact runs `rsvg-convert` (in the `librsvg` package; pacreact does not link it) to draw them; without that tool it finds only PNG icons, which on a modern theme is next to none. An icon the theme does not have is simply left out, and the text keeps the room. They are looked for once, the first time they are needed.

- `icons = 0` turns them all off. `icons = theme` takes them from the icon theme only (see below).
- `icon_size` sets their size in pixels (12 to 128). By default they are 24 at the default font size and follow `font_size` like everything else; when set, the size is what you asked for.
- `pacreact toast -i battery-low "text"` and `pacreact slider -i audio-volume-high 50 Label` give your own messages an icon, by name: any icon of the theme, or several separated by commas, of which the first that exists is used (`-i battery-low,dialog-warning`). Names are theme names, not paths.

The notification cards from apps keep the icon the app sent; strips of the attention deck have none. Icons are the theme's, so which ones exist and how they look is up to it. The names pacreact uses are the standard freedesktop ones, for example `battery-caution`, `network-offline`, `audio-volume-medium`, `display-brightness`.

**Nerd Font glyphs.** Where a [Nerd Font](https://www.nerdfonts.com/) (version 3) is installed, the volume and microphone bars and the messages for a webcam, a printer, headphones and a headset use its Material Design glyphs instead of the theme's icons: a speaker with more or fewer waves, a microphone, headphones and a headset (struck through when they are unplugged), a webcam and a printer (with an alert for a problem), each with a struck-through twin for when it is unplugged or offline. Glyphs have no theme to search, need no `rsvg-convert` and are drawn in the HUD's colour. The font is the symbols-only `Symbols Nerd Font` that Nerd Fonts ships (the `ttf-nerd-fonts-symbols` package on Arch), found by its file name in the font directories; `icon_font` takes the path of another file, such as a patched font, or of one in a directory that is not searched. pacreact reads the file itself and does not ask fontconfig, which would map its cache of every installed font (several MB on a machine with many) for a few icons; the file is opened once, when the first icon is drawn, and kept. A build with `I18N=0` has no FreeType and so no glyphs. Everything else (battery, network, Bluetooth, the clock...) and anything the font lacks keeps the theme's icon, and so does everything when the font is not installed, so nothing breaks without it. `icons = theme` skips the glyphs. A message names its glyph by the same freedesktop name as before; the unplugged ones add an `-off` name that only the font has (`audio-headset-off`, `audio-headphones-off`, `audio-input-microphone-off`, `camera-web-off`, `printer-off`).

## Size and fonts

For larger text, set `font_size`, the size in pixels of the main text (16 by default; the summary, body, actions and the like are in proportion). It scales the whole HUD together, because text that grows inside a card that does not looks broken: the fonts, the padding, the icon, the accent bar, the sliders and the on-screen display, and the width of the cards and toasts. `font_size = 20` is 1.25 times everything, `24` is 1.5 times, and `12` is three quarters. Values outside 10 to 48 are brought to the nearest, and anything that is not a whole number of pixels is reported on stderr and ignored.

`width` sets the width of the stack's items by itself, from 200 to 1200 pixels, without touching the sizes of anything else, for wider cards at the default size or a set width at any size. With both, the width is the one asked for and the rest follows the font. The on-screen display (the volume bar) follows the font only. The distance from the screen's edge is its own setting, `margin` (16 pixels by default, 0 to 200), and is not scaled with the font.

`font` is the path to a font file to use for Latin text, in place of the default (Noto Sans, or DejaVu or Liberation Sans where it is missing). `font_bold` is its bold; without one, bold text is the same file with the bold made by the renderer. Other scripts (Arabic, Chinese, Thai and so on) keep their own fonts, so only the Latin letters change, and the font has to have those letters: a missing one shows as a box. A path that is not a readable regular file is reported and the default is used. These settings are ignored in a build without translation support (`I18N=0`), which draws with the system's default font. Readable fonts made for dyslexia, such as Atkinson Hyperlegible, are the kind of use this is for.

For example, in `~/.config/pac/pacreact.conf`:

```ini
font_size = 22
font = /usr/share/fonts/atkinson-hyperlegible/AtkinsonHyperlegible-Regular.ttf
font_bold = /usr/share/fonts/atkinson-hyperlegible/AtkinsonHyperlegible-Bold.ttf
```

`scale` is a different thing: it pins how sharply the HUD is rendered on a high-density screen, and does not change its size.

The [attention deck](#the-attention-deck) needs three item widths of room (see there), so a large font or a large width makes it fall back to the stack on a screen that would have had room at the default size.

`text_size_min` and `text_size_max` (the same two keys, with the same meaning, in every pac tool) hold the smallest text on the HUD, which is the actions on a card (12 pixels at the default font), between two lengths: `text_size_min = 3mm` means no text smaller than 3 mm on any screen, and the whole HUD (the other texts, the padding, the icons, the width of the items) grows in proportion with it. They hold whatever `font_size` or `text_scale` says, and win over them. A maximum under 2 mm or a minimum over 40 mm is taken to be a mistake and is pulled to it; a minimum above the maximum wins. The one thing that wins over them is the screen: the items are never made wider than it, since text that does not fit is worse than small text. The sizes follow the screen the HUD is on, as in [lengths with a unit](#lengths-with-a-unit).

### Lengths with a unit

`margin`, `font_size`, `width` and `icon_size` take a whole number of pixels, as above, or a number with a unit: `mm`, `in` or `pt` (a size that is the same on every screen, `4mm`, `0.2in`, `10pt`), `px` (the same as no unit), or `%` of the screen's shorter side (`5%`). The number must be above 0 (write `0` for no margin), may have a decimal point, and the unit is not case sensitive. Anything else is reported and the setting is ignored.

The pixels in a millimetre come from the panel's size and mode that the compositor reports (`wl_output`), divided by the scale, on the screen the HUD is on: the one named by `output`, else the one it last appeared on, else (before it has ever appeared) the narrowest one that has told its size. A screen that says nothing, or something that is not a panel (0 mm, a size that makes under 50 or over 600 dots per inch), is taken to be 96 dpi. The size is worked out again when the HUD moves to another screen or the scale changes. The limits of each setting (a font of 10 to 48 pixels, for one) still apply to the result, without a message.

```ini
margin = 5mm
font_size = 4mm
width = 90mm
```

## Speech

For people who cannot see the screen, pacreact says what it shows through speech-dispatcher (libspeechd, shared with the other pac tools): every toast and notification (the summary, then the body), the volume and brightness bars with their value (`Volume 40%`), said once the bar has stopped moving for half a second rather than at every step (each volume key press gives a short tick instead, see `sound_volume`), and the clock. A warning or a critical message starts with that word, since colour alone would not tell. A message that is shown again unchanged is not said again, and a new one waits for the one being said to end, so several at once are said one after the other. Nothing is spoken that is not on the screen, and a notification waiting for room in the queue is said when it is shown.

It is on when built with `SPEECH=speechd` (the default) and `speech-dispatcher` is running; `speech = no` leaves it unloaded and unconnected; `make SPEECH=none` has no speech code and no library. Without a server, one line on standard error says so, once, and pacreact goes on. The language is `language`, else the environment's. pacreact speaks every notification of every program, so someone who uses it for that should know it reads their notifications aloud.

## Audio jacks

Plugging something into a 3.5 mm jack gives a toast: headphones, a headset, a microphone or line out, connected or disconnected. The kernel only says that a jack changed, not what is in it, so pacreact also asks the sound server which card ports changed (headphones, headset or microphone) and names the toast from that: headphones and a microphone together are a headset. If the jack changed and no port did, the hardware is not saying what it is, and the toast says "Audio device connected" or "Audio device disconnected". Without a sound server it uses the jack's own name. HDMI and DisplayPort audio are not announced.

Plugging a jack or a device in or out usually moves the default output, and each output and port has a volume of its own, so the level the sound server reports afterwards differs from the one before. That is not you changing the volume, so no volume bar appears for it, and nothing appears for about half a second after, while the server finishes setting the level. A different default output, other than through a jack, gets a message instead (`Audio output: WH-1000XM4`); the jack's own toast already says the rest. Pressing a volume key in that half second still changes the volume but may show no bar.

Some codecs report one connector as two jacks, or let one contact lag behind the other: a headset's removal may release only the microphone contact, and a speaker plugged in right after then changes nothing the kernel can report, so it gets no toast until it is pulled out. That is the hardware, and pacreact cannot see past it. pacreact treats a headphone-out and a microphone-in pin that share a panel position as one socket, which it reads from the codec's pin configuration in `/sys/class/sound/hwC*D*` when it starts (or, failing that, once a plug has been seen to change both contacts together), and calls a microphone change that happens while the headphone contact is engaged the headset. `jacks = generic` skips all naming for machines where it cannot be trusted. With `jacks_debug = 1` pacreact prints what each switch and port did, which is what a bug report needs.

If the headphone contact of a combo jack lags like that, the fix is in the kernel's audio driver, not in pacreact. Check `/proc/asound/card*/codec#*` for the codec, and the kernel's HD-Audio documentation for a headset-mode model for it. On one Realtek ALC671 board, `options snd-hda-intel model=alc662-headset` in a file under `/etc/modprobe.d/` made the headphone contact release with the plug and the second plug get its own event. That is a single report from one machine, not a recommendation: a model replaces the driver's own settings for the board, so check that the microphone and the output switching still work. To try one without rebooting, stop the PipeWire services, write the model name to `/sys/class/sound/hwC0D0/modelname` (the number is your card and codec), write `1` to `reconfig` in the same directory, and start the services again; a reboot undoes it.

## Sound

pacreact links no audio library: it starts an external player (`pw-play` or `paplay`) with the sound file. By default only timers and alarms make a sound, once when they fire; everything else is silent until you opt in with the variables above. Sound names are looked up as `NAME.oga`, `.ogg` or `.wav` in `sounds/freedesktop/stereo` under `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`, so `message`, `bell` or `dialog-warning` work with the standard sound theme. Several sounds within 400 ms play once.

Things that merely happen are silent unless you ask: `sound_power` (charger plugged or unplugged), `sound_devices` (a headset, headphones, microphone, USB device or Bluetooth device connects or disconnects) and `sound_network` (online or offline). Each takes one of:

| Value | Plays |
|---|---|
| unset, empty, `0` | nothing (the default) |
| `1` or `on` | the sound theme's own pair: `power-plug` and `power-unplug`, `device-added` and `device-removed`, `network-connectivity-established` and `network-connectivity-lost` |
| `NAME` | that sound, a theme name or an absolute path, for both directions |
| `UP,DOWN` | one sound for connecting and another for disconnecting; an empty half is silent (`bell,` plays only on connect). Paths cannot contain a comma. |

The volume has a sound of its own, `sound_volume`: off unless set, `1` (or `on`) for the theme's `audio-volume-change`, or any sound name or path. It plays when you change the output volume or mute it, the same moments the volume bar appears, but not for the microphone or for switching to another output. A key held down plays it at most about twice a second. Unlike the three above, do-not-disturb does not silence it, since it answers something you just did.

For example `sound_power = 1` gives the pair for the charger, and `sound_devices = device-added,` sounds only when something connects. Nothing plays for the state found when pacreact starts, only for changes after it, and these sounds are held back while do-not-disturb is on.

Notifications can ask for a sound themselves with the `sound-file` or `sound-name` hints, and `suppress-sound` silences them. Those are honoured, as are the player and theme lookups above.

## Hyprland

Volume and brightness HUDs appear on their own when the level changes, so your keybinds only need to change the level:

```lua
hl.bind("XF86AudioRaiseVolume", hl.dsp.exec_cmd("wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+"))
hl.bind("XF86AudioLowerVolume", hl.dsp.exec_cmd("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"))
hl.bind("XF86AudioMute",        hl.dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"))
hl.bind("SUPER + Escape",       hl.dsp.exec_cmd("pacreact dismiss"))
```

(Adapt the syntax to your Hyprland config format.)

## Replacing your notification daemon

Only one program can own `org.freedesktop.Notifications`. If another one holds it, pacreact says so on stderr and runs without notifications.

On a system where mako is started by D-Bus activation (as on Arch), stopping it is not enough: the next notification would start it again. A user-level service file takes precedence over the system one:

```bash
systemctl --user stop mako.service
make install PREFIX=$HOME/.local UNITDIR=$HOME/.config/systemd/user
systemctl --user daemon-reload
systemctl --user enable --now pacreact.service
dbus-send --session --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig
```

The last line matters when `~/.local/share/dbus-1/services` did not exist before: a session bus that was already running does not watch a directory it never saw, so without the reload D-Bus keeps activating mako whenever pacreact is down (a new login picks the directory up on its own).

Check that it took over: `notify-send hello` should show a pacreact card, and `busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetServerInformation` should answer `pacreact`. To check the activation path too, stop pacreact (`systemctl --user stop pacreact.service`), send another notification, and confirm that `systemctl --user is-active pacreact.service` is `active` again and mako's is not.

For the unit to start, `WAYLAND_DISPLAY` must be in the systemd user environment (most Hyprland setups run `dbus-update-activation-environment --systemd WAYLAND_DISPLAY`).

To go back to mako: `systemctl --user disable --now pacreact.service`, remove `~/.local/share/dbus-1/services/org.pacreact.Notifications.service`, reload the bus configuration as above, and start mako (`systemctl --user start mako.service`, or just send a notification and let D-Bus start it).

## Notifications: what is supported

Summary and body (tags stripped, entities decoded), the `urgency` and `resident` hints, replacement by id, `CloseNotification`, sound hints, actions, and icons in three forms: raw pixels (`image-data`, and its older spellings `image_data` and `icon_data`, scaled down to 64x64), a PNG path (`app_icon` or `image-path`, also as `file://`), or an icon-theme name (`dialog-information`, `firefox`, ...). Theme names are looked up as PNG files in the user's and the system's icon directories: the GTK theme named in `~/.config/gtk-3.0/settings.ini` first, then `hicolor`, then `Adwaita`. SVG-only icons are not found and the card is shown without an icon. A PNG is read once, when the notification arrives, and only if it is a regular file of at most 4 MiB whose header declares at most 2048 x 2048 pixels; anything else (a pipe, a device, an oversized or corrupt file) is ignored and the card is shown without an icon. Sound files from the `sound-file` hint must likewise be regular files.

### Actions

A card with actions shows them as numbered hints (a click only dismisses it, or triggers `default`), `[1] Reply  [2] Archive`, and you invoke one from a keybind:

```bash
pacreact action 2      # the second action of the card on screen
pacreact action        # the first one
```

pacreact tells the app (`ActionInvoked`) and closes the card, unless the app set the `resident` hint: a resident card stays on screen after an action is invoked, and so does one that is left-clicked while it has a `default` action (the click invokes it, again each time). A resident card goes away on its timeout, with a right click, with `pacreact dismiss`, or when the app closes or replaces it. At most three actions are shown. The special `default` action ("the user clicked the notification") is not shown as a button unless it is the card's only action, in which case it appears as `[1] Open`. Actions apply to the newest card on screen; cards waiting for room are not available until they are shown. A Hyprland example:

**Focus after a click.** When you left-click a card's `default` action, pacreact asks the compositor for an activation token (the `xdg-activation-v1` protocol) from that click and sends it to the app as the `ActivationToken` signal, just before `ActionInvoked`, so the app can use it to bring its window to the front instead of the compositor refusing the focus change. This needs a compositor with `xdg-activation-v1` (Hyprland and other wlroots-based ones have it); without it the app gets only `ActionInvoked`, as before. Actions invoked from the keyboard (`pacreact action`) carry no token: they are not tied to an input event pacreact can see, and compositors ignore a token that is not.

```lua
hl.bind("SUPER + 1", hl.dsp.exec_cmd("~/.local/bin/pacreact action 1"))
hl.bind("SUPER + 2", hl.dsp.exec_cmd("~/.local/bin/pacreact action 2"))
```

Timeouts follow the specification. `-1` leaves it to pacreact: 3 s for low urgency, 6 s for normal, and critical cards never expire. `0` means never expire for any urgency. Any other value is the number of milliseconds the app asked for, though never less than 1.5 s so a card can be read. A card that never expires stays until you click it, close it with `pacreact dismiss`, the app closes it, or the app replaces it, and it takes one of the four slots in the stack meanwhile.

`CloseNotification` closes the card and sends `NotificationClosed` (reason 3); for an id that is not on screen (already expired, dismissed, or never shown) it returns a D-Bus error, as the specification says. A notification that do-not-disturb suppressed is answered with an id and then immediately closed with reason 4 (undefined), so apps that wait for it do not hang; if it replaced a card that was on screen, that card is withdrawn.
