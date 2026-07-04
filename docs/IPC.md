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
{"workspaces":{"active":1,"occupied":[1,2,4]},"activeWindow":{"appId":"foot","title":"~"}}
```

| Field | Type | Meaning |
|-------|------|---------|
| `workspaces.active` | int | Active workspace, 1-indexed (1–10). |
| `workspaces.occupied` | int[] | Sorted 1-indexed workspaces that have mapped windows. Always includes `active`. |
| `activeWindow` | object \| null | Focused window, or `null` when nothing is focused. |
| `activeWindow.appId` | string | Focused window's app id. |
| `activeWindow.title` | string | Focused window's title. |

Strings are JSON-escaped; control characters other than `\n` are dropped.

## Commands (client → server)

Send one JSON object per line, terminated by `\n`. Commands implemented:

```json
{"cmd":"workspace","n":3}
```

Switches to workspace `n` (1–10); out-of-range values are ignored.

```json
{"cmd":"dpms","on":false}
```

Powers the display off (`"on":false`) or on (`"on":true`) — DPMS. The same effect
is available to standard tools via the `wlr-output-power-management-v1` protocol
(e.g. `wlopm`, `hypridle`).

```json
{"cmd":"unlock"}
```

Force-unlocks the session. This is a safety net: if a lock client crashes or hangs 
it would otherwise leave the screen blank forever, recoverable only by killing the compositor. Run it from a TTY to escape.

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
