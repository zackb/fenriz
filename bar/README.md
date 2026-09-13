# fenriz-bar

The bar for [fenriz](../README.md): workspaces and the focused window on the left, and an
island in the middle that opens into everything else. A companion to
[fenriz-desktop](../desktop/README.md), not a framework.

## Status

Early. Workspaces, window title, clock, calendar, volume and brightness, audio devices, media
players, battery, power modes, keep-awake, session actions, system stats, bluetooth, wi-fi and the system
tray work.

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
```

| command | effect |
|---------|--------|
| `fenriz-bar` | run the bar |
| `fenriz-bar open [page]` | open the island on the focused screen, or close it if that page is already open. Pages: `home`, `calendar`, `audio`, `media`, `power`, `system`, `bluetooth`, `wifi` |
| `fenriz-bar awake` | toggle keep-awake: the screen does not dim, lock or blank while it is on |
| `fenriz-bar close` | close the island |

Escape or clicking anywhere else closes it. Backspace goes back to `home`.

## Island

Click the clock for home: wi-fi, bluetooth, keep-awake and power mode tiles, volume and brightness sliders, whatever
is playing, and a footer with CPU, memory, temperature and battery. The now-playing chip on the
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

## Config and theme

There is no separate config. The bar reads `fenriz-desktop.conf` (`shell_opacity`) and the
accent colors from `fenriz.conf`, and uses the same GTK theme as fenriz-desktop.

## Requirements

`wlr-layer-shell`, and fenriz's IPC socket (`FENRIZ_SOCKET`) for workspaces. Without the socket
the bar still runs, but shows no workspaces or window title.

WirePlumber for audio, any MPRIS player for media. Covers from the web (Spotify) need GVfs.
UPower for the battery, power-profiles-daemon for power modes, logind for the session actions, and
`idle-inhibit-unstable-v1` for keep-awake, BlueZ for bluetooth, and NetworkManager (`libnm`) for wi-fi and the wired status. Each part hides itself when its service is missing.

System stats are only sampled while the island is open, so a closed bar wakes nothing up.

Logs go to `~/.local/state/fenriz/fenriz-bar.log` (`$FENRIZ_BAR_LOG` overrides).
