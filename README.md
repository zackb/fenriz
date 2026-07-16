# fenriz

Minimal tiling Wayland compositor built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots),
with transparency, rounded corners, borders, and keybinds from a config file.

## Status

Functional minimal tiling compositor: brings up outputs, maps xdg-shell windows in a
master-stack layout, keyboard/pointer input with config keybinds and click-to-focus, and
renders with per-window opacity, borders, and (GLES2) rounded corners. Rounded corners
require the GLES2 renderer; under the pixman software renderer they degrade to square
corners while opacity and borders still apply.

## Dependencies

wlroots 0.20, wayland-server, xkbcommon, pixman, EGL, GLESv2. On Arch:

```
sudo pacman -S wlroots wayland xkbcommon pixman mesa
```

Also needs `cmake` (>= 3.19) and `ninja`.

## Build

```
make debug      # configure + build into build/debug
make release
make test       # run the config-parser self-check
make run        # build debug and launch
```

## Run

Nested inside an existing Wayland session (opens a window), or from a TTY for the
DRM backend. Config is read from `$XDG_CONFIG_HOME/fenriz/fenriz.conf` (or
`~/.config/fenriz/fenriz.conf`); see `fenriz.conf.example`.

## Multi-monitor and clamshell

Closing the lid is not supposed to be your problem, and **none of this needs configuring**.

Each of the 10 workspaces lives on one output. When a screen goes away — lid shut, cable
pulled, suspend — the workspaces on it move to a surviving screen with their layouts intact,
remembering the screen they were taken off; when it returns, they go back to exactly where
they were. Whatever you were focused on follows you to the surviving screen and stays
focused, so shutting the lid mid-sentence doesn't even interrupt typing. Windows never change
workspace and split trees are never rebuilt, because nothing on the output path can touch
either.

Configure only to override the defaults:

```
output    = eDP-1, preferred, auto, 2.0    # per-screen mode/position/scale
output    = DP-1,  3840x2160@144, auto, 1.0
workspace = 3, DP-1                        # pin ws3 to the big monitor, always
```

fenriz turns the internal panel off when the lid shuts with an external screen connected, and
back on when the lid opens or the external goes away. Suspending with no external screen is
logind's job and its defaults already do it right (`HandleLidSwitch=suspend`,
`HandleLidSwitchDocked=ignore` — it counts external DRM connectors itself), so there is
nothing to configure and no inhibitor to take.

**Your bar does not need reloading.** Disabling a screen removes its `wl_output` global, so a
shell with per-screen surfaces (quickshell `Variants`) tears down and rebuilds through the
normal registry events. If you're carrying a "reload the shell on monitor change" hack from
another compositor, delete it.

## IPC

fenriz exposes a Unix socket (`FENRIZ_SOCKET`) that streams workspace/window/output state as
newline-delimited JSON and accepts one-line commands — for status bars and shells. See
[docs/IPC.md](docs/IPC.md).

## Layout

```
src/
  main.cpp      entry + event loop
  server.*      backend, renderer, allocator, xdg-shell, seat; owns the window list
  output.*      outputs: frame handler, hotplug, enable/disable, clamshell policy
  output_policy.cpp
                pure workspace-assignment rules (evacuate/restore); no wlroots, unit-tested
  view.*        xdg_toplevel wrapper (geometry, focus)
  keyboard.*    xkb + keybind dispatch
  cursor.*      pointer focus
  tiling.*      master-stack layout
  layer.*       wlr-layer-shell (bars/panels/wallpapers) + idle-notify
  ipc.*         FENRIZ_SOCKET control socket (see docs/IPC.md)
  config.*      Hyprland-style config parser
  renderer.*    GLES2 rounded-rect + border + alpha shader
```

HiDPI: set `scale` in the config (fractional supported, e.g. `scale = 1.5`).
