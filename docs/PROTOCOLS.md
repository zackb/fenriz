# fenriz Wayland protocol support

What fenriz speaks today, what it probably should add, and what it deliberately
won't. fenriz targets **wlroots 0.20** and is a personal, single-output tiling
compositor, so the priorities below are weighted for a desktop daily-driver, not a
kiosk or a general-purpose DE.

Every "Supported" row maps to a real `wlr_*_create` (or renderer/seat) call in `src/`.
A few globals are registered implicitly by wlroots: `wlr_renderer_init_wl_display`
(`src/server.cpp`) exposes **wl_shm** and, on the GPU (GLES2) renderer, **linux-dmabuf-v1**
(and legacy **wl_drm**); `wlr_seat` exposes **wl_seat**/pointer/keyboard plumbing.

Protocol **versions matter** — a global can be present but too old for a client to use a
feature (e.g. `wl_compositor` is created at v5, so `wl_surface.preferred_buffer_scale`,
added in v6, is not advertised).

## Supported

| Protocol | wlroots type (version) | Where | Enables |
|---|---|---|---|
| wl_compositor | `wlr_compositor` (v5) | `server.cpp` | Core surfaces |
| wl_subcompositor | `wlr_subcompositor` | `server.cpp` | Subsurfaces (client-side compositing) |
| wl_shm | via `wlr_renderer_init_wl_display` | `server.cpp` | Shared-memory buffers |
| linux-dmabuf-v1 (+ legacy wl_drm) | via renderer init (GLES2) | `server.cpp` | GPU buffer sharing / zero-copy |
| wl_seat (keyboard + pointer) | `wlr_seat` | `server.cpp`, `keyboard.cpp`, `cursor.cpp` | Input routing |
| wl_output | `wlr_output_layout` | `server.cpp`, `output.cpp` | Output advertisement, mode/scale |
| xdg-output-unstable-v1 | `wlr_xdg_output_manager_v1` | `server.cpp` | Logical output geometry for bars/tools |
| xdg-shell (wm_base) | `wlr_xdg_shell` (v3) | `server.cpp`, `view.cpp` | Application windows, popups |
| wlr-layer-shell-v1 | `wlr_layer_shell_v1` (v4) | `layer.cpp` | Bars / wallpapers / launchers (quickshell) |
| xdg-decoration-unstable-v1 | `wlr_xdg_decoration_manager_v1` | `decoration.cpp` | Server-side decorations (forced SSD) |
| org_kde_kwin_server_decoration | `wlr_server_decoration_manager` | `decoration.cpp` | SSD lever GTK/libadwaita honors |
| wl_data_device_manager | `wlr_data_device_manager` | `server.cpp` | Clipboard + drag-and-drop |
| primary-selection-unstable-v1 | `wlr_primary_selection_v1_device_manager` | `server.cpp` | Middle-click paste |
| wlr-data-control-unstable-v1 | `wlr_data_control_manager_v1` | `server.cpp` | Clipboard managers (wl-clipboard, cliphist) |
| ext-data-control-v1 | `wlr_ext_data_control_manager_v1` (v1) | `server.cpp` | Standardized clipboard-manager successor |
| wp-viewporter | `wlr_viewporter` | `server.cpp` | Buffer scaling/cropping (fractional scale) |
| fractional-scale-v1 | `wlr_fractional_scale_manager_v1` (v1) | `server.cpp`, `layer.cpp`, `view.cpp` | Crisp HiDPI at fractional scales |
| cursor-shape-v1 | `wlr_cursor_shape_manager_v1` (v1) | `cursor.cpp` | Named cursors from clients |
| wlr-foreign-toplevel-management-v1 | `wlr_foreign_toplevel_manager_v1` | `server.cpp`, `view.cpp` | Taskbars / window lists |
| wlr-screencopy-v1 | `wlr_screencopy_manager_v1` | `server.cpp` | Screenshots (grim), simple capture |
| wlr-gamma-control-v1 | `wlr_gamma_control_manager_v1` | `server.cpp`, `output.cpp` | Night light (wlsunset/gammastep) |
| wlr-output-power-management-v1 | `wlr_output_power_manager_v1` | `server.cpp`, `output.cpp` | DPMS (wlopm, hypridle) |
| ext-idle-notify-v1 | `wlr_idle_notifier_v1` | `layer.cpp`, `cursor.cpp`, `keyboard.cpp` | Idle detection (swayidle/hypridle) |
| ext-session-lock-v1 | `wlr_session_lock_manager_v1` | `lock.cpp` | Screen lockers |

Plus a native, non-Wayland control socket (`FENRIZ_SOCKET`) for bars/shells — see
[IPC.md](IPC.md).

## Should implement

Missing but reasonable for a desktop daily-driver. Effort is a rough feel (wlroots
usually does the heavy lifting; "S" ≈ create-a-global-and-forward, "M" ≈ real handler
logic, "L" ≈ substantial subsystem).

| Protocol | Why it matters | Effort |
|---|---|---|
| idle-inhibit-unstable-v1 | Video/fullscreen apps keep the screen awake — fenriz has idle *notify* but not *inhibit* | S |
| xdg-activation-v1 | Activation tokens: launchers/notifications requesting focus or "raise me" | S–M |
| wlr-output-management-v1 | Dynamic output config (kanshi, wlr-randr, nwg-displays) — currently config-file only | M |
| pointer-constraints-v1 + relative-pointer-v1 | Mouse lock/warp for games and FPS aiming | M |
| keyboard-shortcuts-inhibit-v1 | Let VMs / remote-desktop / terminal multiplexers capture compositor binds | S |
| text-input-v3 + input-method-v2 | IMEs (fcitx5/ibus), CJK and emoji input — a real gap for non-Latin input | L |
| virtual-keyboard-v1 + virtual-pointer-v1 | wtype / ydotool / wayvnc, on-screen keyboards, automated testing | S |
| wp-presentation-time | Accurate vsync/frame timing for video players and games | S |
| tearing-control-v1 | Opt-in tearing for latency-sensitive fullscreen games | S |
| pointer-gestures-unstable-v1 | Forward touchpad swipe/pinch to clients | S |
| single-pixel-buffer-v1 | Tiny correctness protocol many toolkits probe for | S |
| content-type-v1 | Clients hint "this is video/game" for scheduling/tearing policy | S |
| ext-foreign-toplevel-list-v1 | Standardized successor to the wlr taskbar protocol | S |
| security-context-v1 | Identify Flatpak-sandboxed clients (restrict privileged protocols) | S–M |
| ext-image-copy-capture-v1 / wlr-export-dmabuf-v1 | Efficient (dmabuf) screen recording — wf-recorder, better OBS path | M |
| tablet-v2 | Drawing tablets (Wacom) | M |
| linux-drm-syncobj-v1 (explicit sync) | Reduce flicker/latency on some GPU drivers | M |
| **XWayland** | X11-only apps (games, legacy tools). Large, standalone subsystem — a deliberate scope decision, not a drop-in | L |
| bump wl_compositor v5 → v6 | Advertises `preferred_buffer_scale`/`transform`; helps toolkit HiDPI for xdg-toplevels | S |

## Won't implement

Deprecated, superseded, or out of scope for a personal wlroots tiling WM. "Won't" here
records the current stance, not a vow — the debatable ones are flagged.

| Protocol | Why not |
|---|---|
| wl_shell | Deprecated; replaced by xdg-shell |
| zxdg-shell-v6 (and older unstable xdg-shell) | Obsolete; stable xdg-shell is supported |
| fullscreen-shell-unstable-v1 | Single-fullscreen-surface kiosk model — not a WM |
| ivi-shell | In-vehicle infotainment; irrelevant |
| wl_drm as a first-class target | Legacy buffer path; linux-dmabuf-v1 is the modern route (wl_drm still auto-exposed by the renderer for compatibility) |
| drm-lease-v1 | Direct scanout for VR headsets — niche hardware |
| KDE Plasma / GTK-shell private protocols | Different ecosystems; not this compositor's surface |
| wl_touch / touch protocols | No touchscreen target (revisit if that changes) |

_Cross-check: run `grep -rhoE 'wlr_[a-z0-9_]+_create' src/*.cpp | sort -u` to confirm the
Supported table matches the code._
