# fenriz IPC

fenriz exposes a native control socket for status bars and shells (e.g. quickshell):
it streams the current workspace/window state as newline-delimited JSON and accepts
one-line JSON commands. Implemented in `src/ipc.cpp`.

## Connecting

On startup fenriz binds a Unix stream socket at

```
$XDG_RUNTIME_DIR/fenriz-$WAYLAND_DISPLAY.sock
```

and exports its path as `FENRIZ_SOCKET`. Child processes (including `exec-once`
clients) inherit it, so read `FENRIZ_SOCKET` rather than reconstructing the path.
If `XDG_RUNTIME_DIR` or `WAYLAND_DISPLAY` is unset, no socket is created.

```
socat - UNIX-CONNECT:$FENRIZ_SOCKET
```

## State feed (server → client)

Every line is one JSON object terminated by `\n` (NDJSON). On connect you are
immediately sent the current state. After that, a new line is pushed whenever the
state changes — focus change, window map/unmap/destroy, title/app-id change, or
workspace switch. Identical consecutive snapshots are suppressed, so a line always
means something changed.

```json
{"outputs":[{"name":"eDP-1","active":1,"focused":true,"x":0,"y":0,"width":2560,"height":1600,"scale":2.0,"internal":true}],
 "lid":"open",
 "cursor":{"x":100,"y":200},
 "workspaces":{"active":1,"occupied":[1,2,4],"urgent":[4]},
 "activeWindow":{"appId":"foot","title":"~"}}
```

| Field | Type | Meaning |
|-------|------|---------|
| `outputs` | object[] | Enabled outputs. A disabled screen has no `wl_output` either, so it's absent here too. |
| `outputs[].name` | string | Connector name, e.g. `eDP-1`. |
| `outputs[].active` | int | The workspace this output is showing, 1-indexed. |
| `outputs[].focused` | bool | Whether this is the output receiving new windows. Exactly one is true. |
| `outputs[].x/y/width/height` | int | Layout geometry, in layout coordinates. |
| `outputs[].scale` | float | Effective scale for this output. |
| `outputs[].internal` | bool | True for a built-in laptop panel (by connector name). |
| `lid` | string | `"open"` or `"closed"`. |
| `cursor.x/y` | int | Pointer position in layout coordinates (same space as `outputs[].x/y`). **Only current at connect time** — see below. |
| `workspaces.active` | int | The **focused output's** workspace, 1-indexed. Unchanged meaning on a single screen. |
| `workspaces.occupied` | int[] | Sorted 1-indexed workspaces with mapped windows, plus whatever each output is showing. |
| `workspaces.urgent` | int[] | Sorted 1-indexed workspaces holding a window that asked to be activated (xdg-activation) while unfocused. Cleared when the window is focused. Usually empty. |
| `activeWindow` | object \| null | Focused window, or `null` when nothing is focused. |
| `activeWindow.appId` | string | Focused window's app id. |
| `activeWindow.title` | string | Focused window's title. |

Strings are JSON-escaped; control characters other than `\n` are dropped.

### `cursor` is a connect-time reading, not a live feed

Pointer motion is **not** a publish trigger — pushing a line per motion event would flood the
feed for every client to serve one niche need. The snapshot is built fresh when you connect,
so `cursor` is exactly right at that moment and then goes stale until some *other* trigger
(focus, map/unmap, workspace switch) publishes again.

This suits a one-shot client: connect, read one line, place a menu at the pointer, disconnect.
Do not use it to track the pointer continuously — there is no Wayland-side way for fenriz to
give you that anyway, and a client wanting live pointer position should be reading its own
`wl_pointer` events.

### You do not need this feed to track screens

A bar does not have to watch `outputs` — or be reloaded — to survive a monitor coming and
going. fenriz adds and removes each screen's `wl_output` global as it's enabled and disabled,
so a shell with per-screen surfaces (quickshell's `Variants`, or anything that binds per
`wl_output`) tears down and rebuilds on its own through the ordinary Wayland registry events.
The `outputs` array is for *displaying* state (which workspace is on which screen), not for
driving your shell's lifecycle.

## Commands (client → server)

Send one JSON object per line, terminated by `\n`. Commands implemented:

```json
{"cmd":"workspace","n":3}
```

Shows workspace `n` (1–10); out-of-range values are ignored. If `n` lives on another
output, focus (and the cursor) follow it there rather than dragging it to the current
screen.

```json
{"cmd":"dpms","on":false}
{"cmd":"dpms","on":false,"name":"DP-1"}
```

Powers the display off (`"on":false`) or on (`"on":true`) — DPMS. Without `name` it
applies to every output; with it, just that one. The same effect is available to standard
tools via the `wlr-output-power-management-v1` protocol (e.g. `wlopm`, `hypridle`), which
targets a single output.

```json
{"cmd":"output","name":"eDP-1","enabled":false}
```

Enables or disables an output. Disabling evacuates its workspaces to a surviving screen
(layouts intact) and destroys its `wl_output` global; enabling reverses it and any
workspace homed to it returns. Unknown names are ignored.

```json
{"cmd":"lid","closed":true}
```

Sets the lid state and runs the clamshell policy, exactly as a real lid switch would.
Mainly useful for testing clamshell behavior in a nested session with no hardware:

```
WLR_BACKENDS=wayland WLR_WL_OUTPUTS=2 fenriz    # with `lid_output = WL-1` in the config
printf '{"cmd":"lid","closed":true}\n' | socat - UNIX-CONNECT:$FENRIZ_SOCKET
```

```json
{"cmd":"unlock"}
```

Force-unlocks the session. This is a safety net: if a lock client crashes or hangs 
it would otherwise leave the screen blank forever, recoverable only by killing the compositor. Run it from a TTY to escape.

```json
{"cmd":"exit"}
```

Quits the compositor — the same thing the `exit` keybind action does. Started from a session
manager (greetd), that ends the session, which is what a shell's "Log out" wants.

The command parser is substring-based, not a full JSON parser: a command is
recognized by its `"cmd":"…"` substring (and `"n":` / `"on":true` for arguments),
so whitespace and key order don't matter. Each command must arrive as one complete
line in a single write — a command split across reads is dropped.

## Example

Print the live feed:

```
socat - UNIX-CONNECT:$FENRIZ_SOCKET
```

Switch to workspace 2:

```
printf '{"cmd":"workspace","n":2}\n' | socat - UNIX-CONNECT:$FENRIZ_SOCKET
```

Recover from a broken lock screen, e.g. from a TTY (`Ctrl+Alt+F2`):

```
printf '{"cmd":"dpms","on":true}\n{"cmd":"unlock"}\n' | socat - UNIX-CONNECT:$FENRIZ_SOCKET
```
