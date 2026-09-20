# fenriz-bar

The bar for [fenriz](../README.md): workspaces and the focused window on the left, and an
island in the middle that opens into everything else. A companion to
[fenriz-desktop](../desktop/README.md), not a framework.

## Status

Early. Workspaces, window title, clock, calendar, volume and brightness, audio devices, media
players, battery, power modes, keep-awake, session actions, system stats, bluetooth, wi-fi, the system
tray, screen recording and plugins work.

## Build

```sh
make -C bar          # debug (default)
make -C bar test
make -C bar release
```

Or from the fenriz root: `make bar`, `make run-bar`, `make install-bar`.

## Run

```ini
# fenriz.conf
exec-once = fenriz-bar
bind = SUPER, C, exec, fenriz-bar open calendar
bind = , XF86PowerOff, exec, fenriz-bar open power
bind = SUPER SHIFT, S, exec, fenriz-bar shot region --copy --edit
```

| command | effect |
|---------|--------|
| `fenriz-bar` | run the bar |
| `fenriz-bar open [page]` | open the island on the focused screen, or close it if that page is already open. Pages: `home`, `calendar`, `audio`, `media`, `power`, `system`, `bluetooth`, `wifi`, `record`, `shot` |
| `fenriz-bar record` | start recording the focused screen, or stop the recording that is running |
| `fenriz-bar shot screen\|window\|region [--focused] [--copy] [--save[=PATH]] [--edit]` | take a screenshot, see [Screenshots](#screenshots) |
| `fenriz-bar awake` | toggle keep-awake: the screen does not dim, lock or blank while it is on |
| `fenriz-bar close` | close the island |

Escape or clicking anywhere else closes it. Backspace goes back to `home`.

## Island

Click the clock for home: wi-fi, bluetooth and power mode tiles, volume and brightness sliders, whatever
is playing, and a footer with a record dot, CPU, memory, temperature, a keep-awake eye and battery. The now-playing chip on the
left opens `media`. On the right, the speaker opens `audio` (scroll it to change the volume), the
monitor opens `system`, the network icon opens `wifi`, the bluetooth icon opens `bluetooth`, and the battery and power button open `power`: power mode, Lock, Sleep,
Log out, Restart and Shut down. The last three ask for a second click.

## Wi-Fi

The tile turns wi-fi on and off and names the network you are on; its arrow opens the page, which
lists networks by name, one entry per network however many access points it has. Click the connected
network to disconnect, a saved or open one to join it, or a secured one to type its password in
place. A wrong password asks again in the same spot, and a network joined with one is not kept.
The bin forgets a saved network. The list is rescanned only while the page is open.

WPA/WPA2/WPA3 personal and open networks. Enterprise (802.1X) and WEP networks are listed but set
up with `nm-connection-editor`; once they are saved, the bar joins them like any other.

## Recording

The dot at the bottom left of Home records the screen the focused window is on; click it again, or
the red timer on the bar, to stop. It turns red and counts up while recording. The file lands in
`~/Videos` as `fenriz-recording-<date>-<time>.mp4`, and a notification names it when it is done.

Right-click the dot (a two-finger tap on a touchpad) for the page, which picks what to record
sound from: nothing, a microphone, or a `(system)` entry, which records what you hear. The choice
lasts until the bar exits.

Encoding is `wf-recorder` over wlr-screencopy; without it installed there is no dot and no page.
Whole screens only — no region, no single window.

## Screenshots

`fenriz-bar shot` captures one of:

- `screen`: the focused screen.
- `region`: freezes every screen so you can drag a box. A click without a drag takes the whole screen.
- `window`: freezes every screen and highlights the window under the pointer; click one to take it as it looks on
  screen, border and rounded corners included. `window --focused` takes the focused window straight away instead.

Esc cancels the picker. `--copy` puts the result on the clipboard as `image/png`. `--save` writes it to
`~/Pictures/Screenshots/fenriz-shot-<date>-<time>.png`, and `--save=PATH` writes it to PATH instead. Give one
or both. The saved path is printed. The command waits until the shot is done and exits 1 on cancel or failure.

The camera next to the record dot on Home takes the same shot with one click. It closes the island first.
Right-click it for the page, which chooses what to capture (region, window or screen) and what happens next
(annotate, copy, save). The default is region, annotate, copy.

`--edit` opens the result for annotation first. The tools are brush (B), line (L), arrow (A), box (R),
ellipse (E) and text (T). There are seven colours and three widths, and Ctrl+Z undoes. Enter finishes and Esc
cancels. While you type text, Enter or Esc ends the text.

It needs ext-image-copy-capture-v1 and ext-data-control-v1 from the compositor. Rotated screens are captured
unrotated.

## Tray

Apps that put an icon in a system tray (StatusNotifierItem: Discord, Steam, Nextcloud, nm-applet,
anything using libappindicator) show on the right of the bar. Left click is the app's main action,
middle click its secondary one, right click its menu, and scrolling goes to the app. Icons the app
marks passive are hidden.

If nothing else runs a tray, the bar is the session's StatusNotifierWatcher. If something does
(quickshell, waybar, KDE), the bar shows the same icons alongside it, and takes over if it exits.
Old XEmbed tray icons are not supported.

## Bluetooth

The tile turns the adapter on and off and names the connected device; its arrow opens the page.
Click a paired device to connect or disconnect it, or a nearby one to pair, trust and connect it.
The bin forgets a device. Nearby devices are scanned for only while the page is open, since a scan
takes radio time from a connected headset.

Pairing works for devices that need no code (headphones, speakers, controllers, most mice). A
device that asks for a PIN or passkey, which most keyboards do, is refused: pair it once with
`bluetoothctl` and it connects from the bar after that.

The pill shows a track change for a few seconds, and the level while you press a volume or
brightness key, plugging in or unplugging the charger, a bluetooth device connecting or
disconnecting, joining a wi-fi network, and toggling keep-awake. A low battery (10%)
stays in the pill until you open the island or plug in. Volume and brightness keys are still fenriz-desktop's (`fenriz-desktop volume +5`); when the bar is
running, the desktop hands the level to the island instead of showing its own OSD.

## Plugins

A plugin is any program that prints what to show as JSON lines. The bar draws it in its own style: a notice in the
pill, a tile and a footer chip on Home, a page named after the plugin (`fenriz-bar open NAME`), and a status
capsule on the bar, left of the tray.

```ini
# fenriz-desktop.conf
plugin = mlb,      fenriz-plugin-mlb -team SEA
plugin = weather,  fenriz-plugin-weather -lat 45.43 -lon -122.77 -fahrenheit
plugin = calendar, fenriz-plugin-calendar
plugin = updates,  fenriz-plugin-updates -interval 1800 -run ~/bin/installupdates.sh
```

These live in `plugins/`: `make -C bar plugins`, `make -C bar install-plugins`. MLB scores and standings,
Open-Meteo weather, upcoming events from a vdirsyncer store in `~/.local/share/calendars`, and a count of pending
repo, AUR and Flatpak updates whose capsule runs `-run` (default `yay -Syu`) in a terminal.

Each line replaces the slots it names; `null` clears one. Unchanged slots cost nothing, so a plugin may resend
everything each poll.

```json
{"pill": {"text": "SEA 3 – 2 LAD", "image": "/path/logo.svg"}}
{"status": {"text": "37", "icon": "software-update-available-symbolic", "tooltip": "Repo 30 · AUR 7", "action": "install"}}
{"chip": {"text": "61°", "icon": "weather-overcast-symbolic"}, "tile": {"title": "Mariners", "image": "…"}}
{"page": {"title": "AL West", "blocks": [
  {"type": "row", "icon": "…", "text": "Seattle", "subtitle": "…", "trailing": "7", "action": "open"},
  {"type": "text", "text": "Final", "style": "dim | section | title"},
  {"type": "table", "columns": ["", "W", "L"], "rows": [["SEA", 70, 80]], "highlight": 0},
  {"type": "list", "items": [{"text": "Standup", "subtitle": "9:30am"}]},
  {"type": "level", "value": 0.4},
  {"type": "button", "text": "Refresh", "action": "refresh"}
]}}
```

`image` is a file path and wins over `icon`. A plugin's environment has `TERMINAL` set from `terminal =`. The chip and tile open the page once there is one.

The bar writes events to the plugin's stdin: `{"event": "open"}` and `"close"` as the island opens and closes,
`"resume"` after suspend, and `{"event": "action", "id": "refresh"}` for a clicked row or button. A plugin must exit
when stdin closes. Its stderr goes to the bar's log; if it exits, its slots clear and it is restarted, backing off to
a minute. A plugin named after a built-in page (`calendar`) adds its page to the bottom of that one.

## Config and theme

There is no separate config. The bar reads `fenriz-desktop.conf` (`shell_opacity`, `theme`).
`theme` defaults to `wallpaper` which will use the matugen colors from the wallpaper. The other 
option is `gtk` which will use the colors from the GTK theme.

## Requirements

`wlr-layer-shell`, and fenriz's IPC socket (`FENRIZ_SOCKET`) for workspaces. Without the socket
the bar still runs, but shows no workspaces or window title.

`wf-recorder` for screen recording. WirePlumber for audio, any MPRIS player for media. Covers from the web (Spotify) need GVfs.
UPower for the battery, power-profiles-daemon for power modes, logind for the session actions, and
`idle-inhibit-unstable-v1` for keep-awake, BlueZ for bluetooth, and NetworkManager (`libnm`) for wi-fi and the wired status. Each part hides itself when its service is missing.

System stats are only sampled while the island is open, so a closed bar wakes nothing up.

Logs go to `~/.local/state/fenriz/fenriz-bar.log` (`$FENRIZ_BAR_LOG` overrides).
