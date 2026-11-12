# Design

## Staying asleep

There is one `epoll` loop (`src/loop.c`). Every event source registers file descriptors with it and the process blocks in `epoll_wait`. The only timeout it ever passes is the remaining life of the HUD on screen; with nothing showing it waits forever. Measured on a real, busy Hyprland session: zero context switches over 15 s idle, 2.2 MB PSS and 1.1 MB private memory (the rest of the resident size is shared libraries). Start-up is the only busy moment: the audio handshake is four wakeups, and a machine under memory pressure raises its first alert right away. To see what wakes the daemon, build with `make CFLAGS="-O2 -DLOOP_TRACE"`, which logs the fd behind every wakeup to stderr.

The Wayland layer surface and its shared-memory buffer are created when a HUD appears and destroyed when it disappears, so an idle daemon holds no surface and no buffers. The surface is on the overlay layer, takes no keyboard focus and has an empty input region.

The fade uses the `wp_alpha_modifier_v1` protocol: pachud only changes the surface's opacity multiplier, paced by frame callbacks, and the compositor does the blending, so nothing is redrawn and nothing runs while no HUD is on screen. A new HUD arriving during a fade-out ramps back in from the current opacity. Without that protocol HUDs simply appear and vanish. Some compositors (Hyprland among them) also animate layer surfaces themselves; set `PACHUD_FADE_MS=0` if you prefer only theirs.

## Event sources

Alerts that can fire at start-up (memory, battery) go through `src/alerts.c`, which keeps the last alert shown per source in the state directory so a restart does not repeat it (see [Usage](usage.md#alerts-at-start-up)).

| Source | Mechanism | Idle cost |
|---|---|---|
| Power (`src_power.c`) | Kernel uevents on a `NETLINK_KOBJECT_UEVENT` socket, group 1 (kernel only, no udev). If a battery exists, a 60 s sysfs poll runs only while discharging, because some drivers do not emit uevents on capacity changes. | none |
| Memory (`src_mem.c`) | PSI triggers: write a threshold to `/proc/pressure/memory`, then wait for `EPOLLPRI`. Unprivileged triggers need a window that is a multiple of 2 s. Fallback: a 10 s `MemAvailable` poll. | none (fallback: 1 wakeup / 10 s) |
| Network (`src_net.c`) | rtnetlink link and route events, debounced 1 s, then one route dump to see whether a main-table default route exists. | none |
| Probe (`src_probe.c`) | Opt-in unprivileged ICMP echo (`SOCK_DGRAM`), only while online. | 1 wakeup per interval, only if enabled |
| Backlight (`src_backlight.c`) | `EPOLLPRI` on the sysfs `brightness` attribute, which the kernel signals with `sysfs_notify`. | none |
| Audio (`src_audio.c`) | A small client of the PulseAudio native protocol on the server's unix socket: subscribe to sink, source and server events, then read the default output's and default input's volume and mute state (one request at a time) after a 40 ms debounce. Reconnects with backoff (5 s up to 5 min) if the server is absent or restarts. | none |
| Timers (`timer.c`) | One `CLOCK_REALTIME` `timerfd`, absolute, re-armed to the earliest deadline, with `TFD_TIMER_CANCEL_ON_SET` so clock changes are noticed. | none |
| Notifications (`notify.c`) | sd-bus: the bus fd is just another fd in the loop. | none |

Volume is the one source that is not kernel-native: the level lives in the sound server, so pachud talks to it directly instead of running `pactl`. It works with PulseAudio and with PipeWire's pulse layer (`pipewire-pulse`), which is what was tested. It connects to `$PULSE_SERVER` when that is a `unix:` path or an absolute path, otherwise to `$XDG_RUNTIME_DIR/pulse/native`; TCP servers are not supported. Authentication relies on the server accepting the connecting uid on a local socket, so no cookie is sent.

## Platform and limits

- Linux with a compositor that implements `wlr-layer-shell`.
- Text: fonts are loaded from known file paths (Noto, DejaVu, Liberation, Noto CJK), not through fontconfig, and the font is chosen per string from its script. Arabic and Hebrew are shaped and reordered with HarfBuzz and FriBidi; Devanagari, Bengali, Gurmukhi, Gujarati, Tamil, Telugu, Kannada, Malayalam, Sinhala and Thai each have their own Noto font, and text that mixes them with Latin is cut into runs, each drawn with its own font (a script font often lacks Latin letters). Other complex scripts (Khmer, Myanmar, Lao, Tibetan, ...) are routed to the DejaVu font, which does not cover them. Runs, and strings, of 160 bytes or more are not shaped. See [Translations](translations.md).
- Scaling follows the compositor through `wp_fractional_scale_v1` and `wp_viewporter`: buffers are rendered at `ceil(size x scale)` pixels and presented at their logical size, so text stays sharp at 1.25x, 1.5x and so on. If the preferred scale changes while a HUD is up (it moved to another monitor), the HUD is drawn again once. Without those protocols it falls back to the nearest whole scale. `PACHUD_SCALE` pins the scale. Verified: the protocol exchange and the viewport geometry (identical on-screen size at 1x, 1.5x and 2x buffers, in a nested headless compositor, `tests/manual/scale_test.sh`) and the event handling (a unit test). Not verified: a compositor actually running at a fractional scale, because no headless compositor available here could be set to one.
- The output is chosen by the compositor (usually the focused one) unless `PACHUD_OUTPUT` names one. Checked with two virtual outputs in a nested headless compositor (`tests/manual/output_test.sh`).
- Alarm and timer deadlines are wall-clock time. A large clock step (for example an NTP correction right after boot) shifts them by the same amount.
- Paths exercised only in isolation, not on real hardware, because the development machine is a desktop: battery uevents, charger events, and the backlight notification. (PSI memory triggers were seen firing on real pressure.)
- Notification actions are keyboard-driven (`pachud action N`). The surface's input region covers only the notification cards, so everything else (gaps, toasts, sliders) stays click-through; a click on a card is the spec's `default` action followed by a dismissal. `PACHUD_CLICK=0` keeps the whole surface click-through. The seat is bound at version 1 and only the pointer is used, so no focus or keyboard state is involved. Checked with a virtual pointer in a nested compositor (`tests/manual/click_test.sh`).
