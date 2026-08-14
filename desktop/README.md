# fenriz-desktop

An opinionated, optional desktop session for [fenriz](../README.md). This is for people
who want a tiler and don't want to assemble a shell out of five separate tools
with five config formats.

It is not a shell framework and it is not required. If you already run
waybar/rofi/swaybg/swaylock and like them, ignore this entirely. Fenriz works
with them and always will.

## Status

This is early alpha. I use it as my daily driver and it works for me.

## Scope

Wallpaper, launcher, desktop context menu, idle, lock screen, polkit agent, notifications, media keys (brightness, volume, mute, mic mute) with an OSD.

### Non-goals

Bar, system tray, dock, mpris/media controls, clipboard manager, network or bluetooth UI, lock-screen widgets, audio device switching.

Every one of those has a good existing tool that fenriz will always work with: 
waybar, swaync, wlogout, lxqt-policykit. 
This is not meant to be a worse version of [quickshell](https://quickshell.org), which is the right tool if you want to build a shell of your own.

## Portability

Requires only `wlr-layer-shell`, `ext-session-lock-v1`, `ext-idle-notify-v1`, and
`wlr-output-power-management-unstable-v1` (only for `idle_dpms`; without it
everything else still runs). `ext-background-effect-v1` is used for the frosted
backdrop behind the shell's cards; without it they are simply translucent.

WirePlumber has to be running for the volume OSD to work.

fenriz's own IPC (`FENRIZ_SOCKET`) is used as an optional enhancement when
present, never a dependency.

## Build

```sh
make -C desktop          # debug (default)
make -C desktop test
make -C desktop release
```

Or from the fenriz root: `make desktop`, `make run-desktop`,
`make install-desktop`.

## Config

`~/.config/fenriz/fenriz-desktop.conf`, next to `fenriz.conf` and in the same `key = value`. 
With no config file, fenriz-desktop falls back to the defaults it ships at `/usr/share/fenriz-desktop/fenriz-desktop.conf`
[defaults/fenriz-desktop.conf.in](defaults/fenriz-desktop.conf.in).

```ini
wallpaper = ~/Pictures/wall.png
output_wallpaper = DP-1, ~/Pictures/desk.png
wallpaper_dir = ~/Pictures/wallpapers
wallpaper_hook = matugen image "$1"
wallpaper_search = off

terminal = kitty
menu = Files,      nautilus
menu = Screenshot, grim -g "$(slurp)" - | wl-copy
```

## Theming
`fenriz-desktop` will use the system GTK4 theme if one is set (nwg-look), or fall back to Adwaita.
`border_active` and `border_gradient` are used from `fenriz.conf` if it exists, otherwise defaults are used.

The launcher, notifications, OSDs, wallpaper picker, desktop menu and polkit
dialog are translucent, and ask the compositor to blur the wallpaper behind them:

```ini
shell_opacity = 0.80   # 1.0 is opaque, and asks for no blur at all
```

```ini

## Desktop menu

Right-click the desktop. Every installed application, grouped into submenus by
freedesktop category, then Applications / Terminal / your `menu =` entries, then
Power (Lock / Sleep / Log Out / Restart / Shut Down).

Log Out uses `fenrizctl exit` under fenriz and falls back to
`loginctl terminate-session` elsewhere.

## Wallpaper picker

A grid of thumbnails from `wallpaper_dir`, scanned recursively, newest first.
Open it from the desktop menu, or bind a key:

```ini
# fenriz.conf
bind = SUPER, W, exec, fenriz-desktop wallpaper
```

Typing filters by path. `wallpaper_search = off` drops the search bar and uses vim keys instead:
`hjkl` move, `gg` and `G` jump to the first and last, `Enter` or `Space` picks, `q` closes. Arrow keys, `Escape` and the mouse
work in both modes.

## Launcher

Applications are desktop entries, matched on name, ranked by frecency
Open it from the desktop menu, or bind a key:

```ini
# fenriz.conf
bind = SUPER, D, exec, fenriz-desktop launcher
```

Running `fenriz-desktop launcher` does not start a second copy. It is
handed the argument to the instance already running. Escape or the same command
again closes it.

Usage counts live in `$XDG_STATE_HOME/fenriz/launcher.usage`. Delete it
to reset the ranking. `launcher = off` removes the launcher and its menu entry.

## Lock screen

Blurred wallpaper, clock, password field.

Three stages, each independent. The shipped defaults enable all three at the values
below; set any of them to `0` in your own config to disable that stage:

```ini
idle_dim = 300    # dim the backlight after 5 minutes
dim_brightness = 10
idle_lock = 600   # lock after 10 minutes
idle_dpms = 900   # screens off after 15 minutes
lock_blur = 24
```

Any input undoes whichever stages were reached.

Lock now from the desktop menu (Power -> Lock), or bind it:

```ini
bind = SUPER SHIFT, L, exec, fenriz-desktop lock
```

Idle locking uses `ext-idle-notify-v1`'s inhibitor-aware notification, so a
client holding an idle inhibitor (a video player) suppresses it.

## Brightness

```ini
binde = , XF86MonBrightnessUp,   exec, fenriz-desktop brightness +5 || brightnessctl set 5%+
binde = , XF86MonBrightnessDown, exec, fenriz-desktop brightness -5 || brightnessctl set 5%-
```

## Volume

Volume, mute and mic mute require Pipewire

```ini
binde = , XF86AudioRaiseVolume,  exec, fenriz-desktop volume +5 || wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
binde = , XF86AudioLowerVolume,  exec, fenriz-desktop volume -5 || wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
bind  = , XF86AudioMute,         exec, fenriz-desktop volume mute || wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
bind  = , XF86AudioMicMute,      exec, fenriz-desktop volume micmute || wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle
```

### PAM

The lock needs a PAM service at `/etc/pam.d/fenriz-desktop`. Installing fenriz-desktop
puts one there for password only, via `pam_unix`.

Fingerprint and face are **not** installed for you. Each needs its own single-module service and PAM module that may not be present. Install `gaze` and /or `fprintd` and enroll first, then:

```sh
sudo install -m644 /usr/share/fenriz-desktop/pam/fenriz-desktop-fprint /etc/pam.d/
sudo install -m644 /usr/share/fenriz-desktop/pam/fenriz-desktop-gaze   /etc/pam.d/
```

If you do get stuck behind a lock screen, switch to a TTY and force-unlock

```sh
fenrizctl unlock                          # finds the socket on its own
```

### Fingerprint and face

Both are optional and both are off until you install their service file. They only start if the service file is present.

```sh
sudo install -m644 /usr/share/fenriz-desktop/pam/fenriz-desktop-fprint /etc/pam.d/   # pam_fprintd.so
sudo install -m644 /usr/share/fenriz-desktop/pam/fenriz-desktop-gaze   /etc/pam.d/   # pam_gaze.so
```

Enroll first: `fprintd-enroll` for the reader, `gaze` for the camera.

## Notifications

The session's `org.freedesktop.Notifications` daemon, so `notify-send` and every app that uses it works. Nothing to set up.

```ini
notifications = on          # off if you would rather run mako/dunst/swaync
notify_timeout = 5000       # ms, for senders that don't set their own
notify_position = top-right # {top,bottom}-{left,center,right}
```

## Polkit agent

Registers as the session's PolicyKit authentication agent and shows the password
prompt when something needs authorising (`pkexec`, mounting a disk, a system
setting).

It does **not** run its own PAM stack. polkitd only trusts an answer that came
through `PolkitAgentSession`, which drives `polkit-agent-helper-1` itself; the
agent shows the prompt and relays what you type. Authorisation is reported
exactly as polkitd decided it and never on our own authority.

Only one agent may own a session. If another is already registered — GNOME's,
KDE's, `hyprpolkitagent`, or a quickshell config — fenriz-desktop logs a warning
and runs without one rather than fighting over it. Stop the other agent if you
want this one.

## Spawning

Everything fenriz-desktop starts — from the launcher and from the menu — is
launched detached: its own session and process group, stdio on `/dev/null`, and
reparented to init. Restarting or killing fenriz-desktop doesn't takes your
applications with it.

## Run

`fenriz-desktop` with no arguments **is** the desktop: it draws the wallpaper,
owns the menu, and keeps running. Start it once from `fenriz.conf`:

```ini
exec-once = fenriz-desktop
```

`fenriz-desktop <command>` is only a client. It hands the command to the running
desktop over the session bus and exits immediately (tens of milliseconds), so it
is safe to bind to a key:

```ini
bind = SUPER, D, exec, fenriz-desktop launcher
```

| command | effect |
|---------|--------|
| `fenriz-desktop` | run the desktop (wallpaper, menu, launcher) |
| `fenriz-desktop launcher` | toggle the launcher on the running desktop |
| `fenriz-desktop wallpaper` | toggle the wallpaper picker (needs `wallpaper_dir`) |
| `fenriz-desktop lock` | locks the session |
| `fenriz-desktop brightness ±N` | steps the backlight by N percent and shows the level |
| `fenriz-desktop volume ±N` | steps the default sink by N percent and shows the level |
| `fenriz-desktop volume mute` | toggles mute on the default sink |
| `fenriz-desktop volume micmute` | toggles mute on the default source |

A media-key command exits non-zero when it cannot do the job (no pipewire)
