# fenriz-bar: findings, decisions, future work

Full spec: `~/.claude/plans/i-d-like-to-spec-steady-twilight.md`.

## Findings (open)

- [ ] native pywal or matugen integration
- [ ] **Island input is unverified on real hardware**: click the pill, hover grow, Escape, Backspace, click-away-to-close, blur. Headless has no pointer or keyboard.
- [ ] **Keybind-opened island focus is unverified**: depends on the `src/layer.cpp` interactivity-change fix; only testable in a real session.
- [ ] **Media and audio controls are unverified by click**: play/pause/next/previous, seek, the player switcher, mute
  buttons, the volume/brightness/mic sliders, device switching and scroll-to-change-volume. Headless has no pointer; the
  display side (fake MPRIS player, real WirePlumber devices, the OSD action) was verified.
- [ ] **http(s) cover art needs GVfs** (`g_file_load_bytes_async`). Without it, Spotify covers are just absent. Fine unless
  someone reports it; the alternative is an HTTP client dependency.
- [ ] **A paused player's track change is not announced**: the pill only shows changes while playing, so resuming a player
  that skipped tracks while paused announces the new one then.
- [ ] **Power page and tiles unverified by click**: Lock, Sleep, Log out, Restart, Shut down (and their second-click
  confirm), the power mode tile and selector, the keep-awake tile. Keep-awake itself was verified through `fenriz-bar awake`
  (inhibitor created on the island's surface, destroyed on toggle off).
- [ ] **Charger and low-battery pill events are unit-tested, not seen live**: UPower is on the real system bus and can't be
  faked without root. Unplug the laptop to check.
- [ ] **Sparklines start almost empty** because sampling only runs while the island is open (history is kept between
  opens). If that looks too bare, sample at a slow rate (10 s) while closed and accept the wakeups.
- [ ] **Keep-awake is not persisted** across a bar restart. Probably right (a forgotten inhibitor drains a battery), but decide.
- [ ] **Bluetooth actions unverified**: connect, disconnect, pair (+ trust + connect), forget, the power switch and tile.
  Not clicked (no pointer headless), and deliberately not driven from a test either: the live session was on Bluetooth
  earbuds. Reading adapter/devices/battery and the page-only scan were verified against the real BlueZ.
- [ ] **PIN/passkey pairing is refused** (keyboards). The agent is NoInputNoOutput and only authorizes the device we
  asked to pair. Supporting it means a passkey display/entry step on the Bluetooth page.
- [ ] **GTK baseline warning on the Bluetooth and Wi-Fi pages** ("GtkImage reported baselines of minimum -2147483648"):
  it is the page's `GtkSwitch` under the Catppuccin GTK theme. A bare switch in an empty window reproduces it; Adwaita
  does not. Not bar code, harmless. Swap the switch for a toggle button if the log noise matters.
- [ ] **Wi-Fi actions unverified**: join (saved, open, new with password), wrong-password re-ask, disconnect, forget,
  the on/off switch and tile. Not driven from a test: the live session was on that wi-fi. Reading the device, networks,
  saved state, security and signal, and the page-only rescans were verified against the real NetworkManager.
- [ ] **Hidden networks** (no SSID) are dropped and cannot be joined from the bar; `nmcli dev wifi connect <ssid> hidden yes`.
- [ ] **Wi-Fi secrets agent**: NetworkManager asking for a password on its own (a saved network whose password changed,
  at auto-connect) is not answered by the bar. It surfaces as "Wrong password", which re-opens the entry in place.
- [ ] **Tray clicks and menus unverified**: left/middle/right click, scroll, and the DBusMenu popover opening from a bar
  layer surface (and whether keyboard navigation works in it without keyboard focus). Verified with fake apps on a
  private bus: watcher mode (register, icon update on NewIcon, removal when the app exits), host mode for a third-party
  watcher, and taking the watcher name when that watcher exits. Menu parsing and building are unit tested.
- [ ] **Tray pixmap icons unverified live**: the fake app used an icon name. `IconPixmap` (ARGB32, network byte order,
  read as GDK_MEMORY_A8R8G8B8) needs an app that only sends pixmaps (Steam, some Electron apps) to eyeball.
- [ ] **Tray menu item icons are not shown** (`icon-name` / `icon-data` in DBusMenu), and radio items render as check marks.
- [ ] **Tray coordinates**: Activate/ContextMenu get 0,0; a layer surface does not know its position on screen. Apps
  that place their own popup at those coordinates (rare) put it in the corner.

## Decisions

- Separate `fenriz-bar` binary, so BlueZ/NM crashes can't take down the lock screen. Desktop sources are compiled in by path; no shared lib until a third consumer exists.
- One config file (`fenriz-desktop.conf`) and one theme sheet (`desktop/src/theme.cpp`).
- Workspaces come from `FENRIZ_SOCKET`. Move to ext-workspace-v1 + foreign-toplevel once fenriz reports one workspace group per output (TODO in `compositor.hpp`).
- The island is one custom widget. Content is laid out at its final size and clipped to a spring-animated rect. The layer surface grows once when a morph starts and shrinks once when it settles, never per frame.
- The island takes EXCLUSIVE keyboard while open (not ON_DEMAND), so a keybind-opened island gets keys. This needed fenriz to hand focus to an already-mapped surface whose interactivity changes (`src/layer.cpp`).
- Blur needs no per-frame region: fenriz masks blur by surface alpha, so the rounded shape blurs correctly.
- OSD hand-off: fenriz-desktop activates the bar's `osd` GAction over D-Bus (`dev.fenriz.Bar`, `org.gtk.Actions.Activate`) and shows its own pill only if that call fails. No custom D-Bus interface, no name watching.
- The bar has its own WirePlumber client (`audio.cpp`) instead of extending desktop's `Volume`: device lists, absolute levels and change signals are bar-only needs.
- Brightness in the bar reuses desktop's `Brightness` (new `percent()` / `set_percent()`). Nothing signals brightness, so
  the slider refreshes when the island opens and when the desktop hands over a key press.
- Volume and mic icons live in `desktop/src/volume.hpp` (`volume_icon`, `mic_icon`), shared by both programs.
- Sliders and seek use GtkRange `change-value`, which only fires for the user, so updates from the backends never write
  back. Our own write wins over mixer readbacks for 500 ms, so a dragged slider does not snap back.
- The island re-measures on every allocation and morphs (from an idle) when page or pill content changes size. Pages
  never call a "resize me" API.
- Pill activities go through `ActivityQueue` (`activity.hpp`, tested): level > event > media for timed ones, plus one
  sticky slot (low battery) underneath that returns whenever nothing timed shows and is dismissed by opening the island.
- Battery events come from a pure `battery_change(before, now, first)` so the plug/unplug/low rules are tested; UPower
  settling after start (Unknown -> Discharging) is not an unplug, and Full -> Charging is not a plug-in.
- Power profiles: `org.freedesktop.UPower.PowerProfiles`, falling back to `net.hadess.PowerProfiles` for older daemons.
- Log out quits fenriz over IPC when connected, else logind `Session.Terminate`. Other actions are logind calls with
  interactive authorization, so polkit prompts if needed.
- Keep-awake is an idle-inhibit-v1 inhibitor on the island's surface (always mapped), re-made when the island remaps to
  another screen. `fenriz-bar awake` toggles it, so it can be bound to a key.
- The Home footer reads left to right in construction order (SystemUi, then PowerUi), not by explicit slots.
- Bluetooth goes straight to BlueZ over D-Bus with a `GDBusObjectManagerClient` (no libbluetooth, no bluez-qt). The
  whole device list is re-read on any signal; connection events and ordering come from pure, tested functions.
- The pairing agent is registered per connection (not as the default agent), so it only answers for pairings the bar
  starts and never fights GNOME's or blueman's agent. It keeps authorizing the device until Trusted is set, because the
  first connect of an untrusted device asks.
- Bar `Surface` uses designated initializers: a positional one silently shifted every widget after a new field and
  cast labels to images.
- Wi-Fi uses `libnm` rather than raw NetworkManager D-Bus: an async, cached, signal-driven client that removes several
  hundred lines of object and property plumbing. It adds one runtime library, which every NetworkManager install has.
- Network list merging, security classification and signal bars are pure and tested. The list is ordered active,
  saved, bars, then name, so a few percent of signal never reshuffles it; rows rebuild only when that order or a
  visible attribute changes, and a typed password survives a rebuild.
- A connection the bar creates for a new network is deleted if its first attempt fails, so a wrong password is not
  saved. A new password for an already-saved network updates its PSK in place, keeping its other settings.
- Tray: one state machine on who owns `org.kde.StatusNotifierWatcher`. Free: we own it and are the watcher. Taken:
  we register as a host of that watcher and read its items. Owner gone: we try to take it, and apps re-register
  with whichever watcher appears. Quickshell holds it in the author's session, so host mode is the everyday path there.
- Tray items are re-read with `GetAll` on any of their signals, because NewIcon/NewStatus/NewTitle carry no value and
  SNI sends no PropertiesChanged. Menus are fetched fresh on every right click and never cached.
- Tray icons are plain boxes with one any-button gesture, not GtkButtons: a button eats the primary click.
- `/NO_DBUSMENU` (what GTK apps without appindicator report) means no menu, so right click falls back to the item's own
  ContextMenu.
- The left cluster uses a custom layout: workspaces at natural width, then title and song split what is left before
  the island's collapsed pill (`flex_share`, a port of quickshell's `flexShare`). The pill width is the same reserve on
  every screen, so a title does not jump when the island moves.
- With no GTK theme, `window_bg_color`/`popover_bg_color`/`view_bg_color` fall back to the built-in theme's
  `theme_bg_color`/`theme_base_color` through a FALLBACK-priority provider; any real theme outranks it.
- Covers are `GtkImage` with a pixel size: a `GtkPicture`'s natural size is the image's own size.

## Done

- [x] Phase 1: skeleton, workspaces + title, clock, island morph, Calendar, `open`/`close`
- [x] Phase 2: volume/brightness sliders, Sound page, OSD hand-off from fenriz-desktop, media chip/card/page, track-change pill
- [x] Phase 3: battery glyph/footer/Power page, power modes, session actions, keep-awake, system stats page, charger and
  low-battery pill events, `fenriz-bar awake`
- [x] Phase 4: Bluetooth tile/page/glyph, connect/disconnect/pair/forget, page-only scanning, connection events
- [x] Phase 5: Wi-Fi tile/page/glyph (wired too), join/disconnect/forget, inline password, page-only rescans,
  connected and failure events
- [x] Phase 6: system tray (StatusNotifierWatcher or host, DBusMenu popovers, pixmap and themed icons)
- [x] Left cluster shrinks to clear the island; no-theme fallback colors; discovery starts when the adapter powers on;
  "Searching…" on an empty nearby list; one keep-awake icon set

## Future work

- [ ] Per-app volume (stream list on the Sound page)
- [ ] Config keys when needed: `bar_clock` format (12h users), `bar_position`
- [ ] Reconnect to `FENRIZ_SOCKET` uses a fixed 2 s retry; add backoff if it ever matters
- [ ] Multi-monitor: the island lives on one screen and moves on open; decide whether each screen should get its own pill
- [ ] `make preview` for island states (pill, pages) to PNG, like desktop's `desktop_preview`
