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

## Layout

```
src/
  main.cpp      entry + event loop
  server.*      backend, renderer, allocator, xdg-shell, seat; owns the window list
  output.*      per-output frame handler
  view.*        xdg_toplevel wrapper (geometry, focus)
  keyboard.*    xkb + keybind dispatch
  cursor.*      pointer focus
  tiling.*      master-stack layout
  layer.*       wlr-layer-shell (bars/panels/wallpapers) + idle-notify
  config.*      Hyprland-style config parser
  renderer.*    GLES2 rounded-rect + border + alpha shader
```

HiDPI: set `scale` in the config (fractional supported, e.g. `scale = 1.5`).
