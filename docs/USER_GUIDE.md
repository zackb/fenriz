# fenriz user guide

A fast, stable tiling Wayland compositor.

- [Requirements](#requirements)
- [Building](#building)
- [Installing](#installing)
- [Running](#running)
- [Configuration](#configuration)
  - [Appearance](#appearance)
  - [Input](#input)
  - [Zoom](#zoom)
  - [Outputs](#outputs)
  - [Workspaces and clamshell](#workspaces-and-clamshell)
  - [Environment and autostart](#environment-and-autostart)
  - [Keybindings](#keybindings)
  - [Mouse](#mouse)
  - [Window rules](#window-rules)
- [Screen sharing](#screen-sharing)
- [Status bars and scripting](#status-bars-and-scripting)
- [Protocol and tool support](#protocol-and-tool-support)
- [A starter config](#a-starter-config)

## Requirements

Build tools: `cmake` (>= 3.19), `ninja`, `wayland-scanner`, `pkg-config`, a C++ compiler.

Libraries:

| Dependency | Notes |
|---|---|
| `wlroots-0.20` | pinned to 0.20; must be built with XWayland |
| `scenefx0.5`  | pinned to 0.5 |
| `wayland-server` | |
| `xkbcommon` | |
| `pixman-1` | |
| `libinput` | |
| `xcb`, `xcb-icccm`, `xcb-ewmh` | X11 window management |
| mesa (EGL / GLESv2) | rendering; usually already present |

On Arch:

```
sudo pacman -S wlroots wayland xkbcommon pixman libinput mesa libxcb xcb-util-wm cmake ninja
```

Then install `scenefx` (package `scenefx-0.5`) from the AUR or build it from source.

```
yay -S scenefx0.5
```

## Building

```
make            # same as `make debug`
make debug      # configure + build into build/debug
make release    # optimized build into build/release
make run        # build debug and launch it
make test       # ctest self-checks
make clean      # remove build/
```

Debug is the default and what you want for hacking. Release is what you install.

## Installing

```
make install
```

Builds release and installs:

| File | Destination (default prefix `/usr/local`) |
|---|---|
| `fenriz` | `bin/fenriz` |
| `fenriz.desktop` | `share/wayland-sessions/` |
| `fenriz-portals.conf` | `share/xdg-desktop-portal/` |

**Packaging:** `make install` does not thread a prefix or `DESTDIR`. Call cmake directly:

```
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset release
DESTDIR="$pkgdir" cmake --install build/release
```

## Running

From a TTY:

```
fenriz
```

Or pick **fenriz** from your login greeter, the installed `fenriz.desktop` shows up as a
Wayland session. You can also run it nested inside an existing Wayland or X11 session for
testing; it opens in a window.

With no config file, fenriz starts on built-in defaults (see the tables below). Drop a
config in place and it picks up changes live with hot-reloading.

## Configuration

Config path, first match wins:

1. `$XDG_CONFIG_HOME/fenriz/fenriz.conf`
2. `~/.config/fenriz/fenriz.conf`
3. built-in defaults (no file needed)

Syntax is one `key = value` per line. `#` starts a comment. Blank lines and lines without
`=` are ignored, as are unknown keys. Booleans accept `true/1/on/yes` and `false/0/off/no`.
Colors are `0xRRGGBBAA` hex.

### Appearance

| Key | Default | Meaning |
|---|---|---|
| `border_width` | `2` | border thickness, px |
| `border_active` | `0x33ccffff` | focused window border |
| `border_inactive` | `0x444444ff` | unfocused window border |
| `shadow` | `on` | soft glow behind the focused window |
| `shadow_color` | `0x33ccff66` | glow color (low alpha = subtle) |
| `shadow_blur` | `18` | glow spread, px |
| `gaps` | `8` | gap between windows, px |
| `rounding` | `10` | corner radius, px |
| `animation` | `150` | slide-into-place duration, ms; `0` = off |
| `opacity` | `1.0` | window opacity, `0.0`–`1.0` |

### Input

| Key | Default | Meaning |
|---|---|---|
| `natural_scroll` | `true` | content follows fingers; `false` = traditional wheel |
| `sensitivity` | `0.0` | pointer speed, `-1.0` (slow) to `1.0` (fast); `0` = default |
| `tap_to_click` | `true` | trackpad tap = click (1/2/3 fingers = left/right/middle) |
| `clickfinger` | `true` | two-finger press = right-click; `false` = bottom-right corner |
| `focus_follows_pointer` | `true` | hovering focuses; `false` = click to focus |
| `repeat_delay` | `250` | ms before a held key repeats |
| `repeat_rate` | `15` | key repeats per second |

### Zoom

Hold `zoom_mod` and scroll to magnify around the cursor.

| Key | Default | Meaning |
|---|---|---|
| `zoom_mod` | `ctrl` | `ctrl` / `alt` / `super` / `shift`; empty = off |
| `zoom_max` | `3.0` | zoom ceiling, `1.0`–`10.0` |
| `zoom_step` | `0.1` | zoom added per scroll notch |

### Outputs

```
output = NAME, mode, position, scale
```

Trailing fields are optional.

| Field | Values |
|---|---|
| `NAME` | connector, e.g. `eDP-1`, `DP-1`, `HDMI-A-1` (`wlr-randr` lists them) |
| `mode` | `preferred` (default), `1920x1080`, `1920x1080@144`, or `disable` |
| `position` | `auto` (default) or explicit `1920x0` |
| `scale` | per-output scale; omit to use the global `scale` |

The global `scale` (default `1.0`, fractional allowed) applies to any output without its
own. With `auto`, screens pack left-to-right in listed order, so just listing them pins the
arrangement.

```
output = eDP-1, preferred, auto, 2.0
output = DP-1,  3840x2160@144, auto, 1.0
```

### Workspaces and clamshell

There are 10 workspaces, each living on one output. Pinning is optional:

```
workspace = 3, DP-1
```

Without it, a workspace evacuated by a lost screen (lid shut, cable pulled) remembers where
it came from and returns there automatically. Set `workspace` only to force one somewhere
permanently.

Clamshell is automatic: fenriz turns off the internal panel when the lid shuts *and* an
external screen is connected, and turns it back on otherwise. Windows never change workspace
and layouts are never rebuilt. Suspend-on-lid is left to logind, which already gets it right.

The panel is detected by connector name (`eDP-*`, `LVDS-*`, `DSI-*`). Override with
`lid_output = eDP-1` if yours differs.

### Environment and autostart

```
env = NAME,VALUE      # exported to all spawned clients, before exec-once
exec-once = foot      # run once at startup; repeatable
```

`env` splits on the first comma only, so the value may contain commas.

### Keybindings

```
bind  = MODS, KEY, action[, arg]
binde = MODS, KEY, action[, arg]    # same, but repeats while held
```

`MODS` are space-separated: `SUPER`, `SHIFT`, `CTRL`, `ALT` (may be empty, e.g. for media
keys). `KEY` is an XKB keysym name (`Return`, `Q`, `left`, `XF86AudioMute`). Use `binde` for
things you hold, like volume.

| Action | Effect |
|---|---|
| `exec` | run the command in `arg` |
| `killactive` | close the focused window |
| `exit` | quit the compositor |
| `focusnext` / `focusprev` | cycle focus |
| `focusleft` / `focusright` / `focusup` / `focusdown` | directional focus |
| `togglelayout` | toggle the layout |
| `fullscreen` | toggle fullscreen |
| `togglefloating` | toggle floating |
| `pin` | pin a floating window to all workspaces |
| `workspace` | switch to workspace `arg` (1–10) |
| `movetoworkspace` | send the focused window to workspace `arg` (1–10) |

### Mouse

Not configurable:

- `SUPER` + drag — move a window
- `SUPER` + `SHIFT` + drag — resize a window
- Tiled: drag swaps with the tile you drop on; `SHIFT`+drag moves the split.
- Floating: drag moves and resizes freely.

### Window rules

```
windowrule = class=REGEX, title=REGEX, float=true, center=true, no_focus=true
```

Fields are `name=value`, comma-separated, any order. All matching rules stack.

| Field | Meaning |
|---|---|
| `class` / `app_id` | regex on xdg app_id (Wayland) or WM_CLASS (XWayland); `^$` matches unset |
| `title` | regex on the window title |
| `float` | open floating |
| `center` | center on screen (pair with `float`) |
| `no_focus` | don't focus when it opens |
| `name` | a label for you; ignored by the compositor |

A rule needs a non-empty `class`/`app_id` or `title` to match. Fields split on commas, so a
comma inside a regex quantifier (`{2,4}`) won't work.

```
windowrule = class=^(org\.pulseaudio\.pavucontrol)$, float=true, center=true
```

## Screen sharing

Install the runtime pieces and make sure the user services are running:

```
sudo pacman -S xdg-desktop-portal xdg-desktop-portal-wlr pipewire wireplumber
```

fenriz handles the rest. It sets `XDG_CURRENT_DESKTOP=fenriz:wlroots` and installs
`fenriz-portals.conf`, which routes `ScreenCast` and `RemoteDesktop` to the wlr backend. Both
full-screen and single-window capture work, so sharing in Zoom, Discord, OBS, and browser
apps (Google Meet, etc.) works with no manual portal config.

If nothing shows up as shareable, check that `pipewire`, `wireplumber`, and
`xdg-desktop-portal` are running as user services (`systemctl --user status ...`).

## Status bars and scripting

fenriz exposes a Unix socket at `$XDG_RUNTIME_DIR/fenriz-$WAYLAND_DISPLAY.sock`, exported to
clients as `FENRIZ_SOCKET`. It streams workspace, window, and output state as newline-delimited
JSON and takes one-line commands. This is enough to drive a status bar or shell (waybar, quickshell,
etc.). There's no CLI wrapper; talk to it with `socat` or `printf`. See [IPC.md](IPC.md).

## Protocol and tool support

fenriz speaks the common wlroots protocols, so tools like grim, wl-clipboard, wlr-randr,
kanshi, wlsunset, and hypridle work out of the box. The full matrix is in [PROTOCOLS.md](PROTOCOLS.md).

## A starter config

Copy the shipped example and edit from there:

```
mkdir -p ~/.config/fenriz
cp fenriz.conf.example ~/.config/fenriz/fenriz.conf
```

A minimal config to get moving:

```
exec-once = kitty

bind = SUPER,       Return, exec, kitty
bind = SUPER,       Q,      killactive
bind = SUPER SHIFT, E,      exit
bind = SUPER,       F,      fullscreen
bind = SUPER,       V,      togglefloating
bind = SUPER,       1,      workspace, 1
bind = SUPER,       2,      workspace, 2
bind = SUPER SHIFT, 1,      movetoworkspace, 1
```

See [`fenriz.conf.example`](../fenriz.conf.example) for the full annotated set.
