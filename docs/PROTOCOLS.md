# fenriz Wayland protocol support

What fenriz speaks today, what it probably should add, and what it deliberately
won't. fenriz targets **wlroots 0.20** and is a tiling compositor, so the
priorities below are weighted for a desktop daily-driver, not a kiosk or a
general-purpose DE.

**This list is enforced.** `scripts/protocol-audit.sh` enumerates every protocol wlroots
0.20 and wayland-protocols offer and fails `make test` if one is absent from both the code
and this file. Supported is proven by the code — a `wlr_*_create` call in `src/` is enough.
Everything under "Should implement" or "Won't implement" must name the protocol by its exact
upstream basename (the `` `wlr_…` `` header stem, or the XML stem), which is what the audit
matches on. Without that, the list only ever re-checked what someone had already thought of,
which is how ext-workspace-v1 stayed missing while wlroots shipped a full implementation.

A few globals are registered implicitly: `wlr_renderer_init_wl_shm` (`src/server.cpp`, key
`wlr_shm`) exposes **wl_shm**, and `wlr_seat` exposes **wl_seat**/pointer/keyboard plumbing.
Note fenriz deliberately does *not* call `wlr_renderer_init_wl_display` — that would pin
linux-dmabuf-v1 to v4 and also expose legacy wl_drm; both are created explicitly instead.

Protocol **versions matter** — a global can be present but too old for a client to use a
feature (e.g. `wl_compositor` is created at v6, so `wl_surface.preferred_buffer_scale` is
advertised; at v5 it would not be).

## Supported

| Protocol | wlroots type (version) | Where | Enables |
|---|---|---|---|
| wl_compositor | `wlr_compositor` (v6) | `server.cpp` | Core surfaces (v6: `preferred_buffer_scale`/`transform`) |
| wl_subcompositor | `wlr_subcompositor` | `server.cpp` | Subsurfaces (client-side compositing) |
| wl_shm | via `wlr_renderer_init_wl_shm` (`wlr_shm`) | `server.cpp` | Shared-memory buffers |
| linux-dmabuf-v1 | `wlr_linux_dmabuf_v1` (v5, `_create_with_renderer`) | `server.cpp` | GPU buffer sharing / zero-copy. Created explicitly rather than via renderer init, which would pin it to v4 |
| linux-drm-syncobj-v1 (explicit sync) | `wlr_linux_drm_syncobj_manager_v1` (v1) | `server.cpp` | Explicit GPU fencing (wait/signal timelines) — less flicker/latency. The `wlr_scene` surface helpers implement the per-surface protocol; guarded on renderer **and** backend `features.timeline` (real-hw only, nested skips it) |
| wl_seat (keyboard + pointer) | `wlr_seat` | `server.cpp`, `keyboard.cpp`, `cursor.cpp` | Input routing |
| wl_output | `wlr_output_layout` | `server.cpp`, `output.cpp` | Output advertisement, mode/scale |
| xdg-output-unstable-v1 | `wlr_xdg_output_manager_v1` | `server.cpp` | Logical output geometry for bars/tools |
| xdg-shell (wm_base) | `wlr_xdg_shell` (v4) | `server.cpp`, `view.cpp` | Application windows, popups |
| xwayland-shell-v1 | via `wlr_xwayland_create` | `xwayland.cpp` | How Xwayland associates its X11 windows with wl_surfaces. Not created directly — the XWayland global brings it |
| XWayland | `wlr_xwayland` (lazy) | `xwayland.cpp`, `view.cpp` | X11 apps (games, IDEs, legacy tools). Managed toplevels tile/focus as native windows via the shared `View`; override-redirect surfaces (X11 menus/tooltips/dropdowns) render in a dedicated `scene_unmanaged` layer as non-`View` `Unmanaged` objects |
| wlr-layer-shell-v1 | `wlr_layer_shell_v1` (v4) | `layer.cpp` | Bars / wallpapers / launchers (quickshell) |
| xdg-decoration-unstable-v1 | `wlr_xdg_decoration_manager_v1` | `decoration.cpp` | Server-side decorations (forced SSD) |
| org_kde_kwin_server_decoration | `wlr_server_decoration_manager` | `decoration.cpp` | SSD lever GTK/libadwaita honors |
| wl_data_device_manager | `wlr_data_device_manager` | `server.cpp` | Clipboard + drag-and-drop |
| primary-selection-unstable-v1 | `wlr_primary_selection_v1_device_manager` | `server.cpp` | Middle-click paste |
| wlr-data-control-unstable-v1 | `wlr_data_control_manager_v1` | `server.cpp` | Clipboard managers (wl-clipboard, cliphist) |
| ext-data-control-v1 | `wlr_ext_data_control_manager_v1` (v1) | `server.cpp` | Standardized clipboard-manager successor |
| wp-viewporter | `wlr_viewporter` | `server.cpp` | Buffer scaling/cropping (fractional scale) |
| wp-presentation-time | `wlr_presentation` (v2) | `server.cpp` | Accurate vsync/frame timing for video players and games |
| single-pixel-buffer-v1 | `wlr_single_pixel_buffer_manager_v1` | `server.cpp` | Tiny correctness protocol many toolkits probe for |
| content-type-v1 | `wlr_content_type_manager_v1` (v1) | `server.cpp` | Clients hint "this is video/game" for scheduling/tearing policy |
| tearing-control-v1 | `wlr_tearing_control_manager_v1` (v1) | `server.cpp`, `output.cpp` | A fullscreen game trades a visible seam for up to a frame less latency. wlroots implements the per-surface protocol, so there is no handler and nothing is stored per view — `output_handle_frame` looks the committed hint back up through `wlr_tearing_control_manager_v1_surface_hint_from_surface` and sets `wlr_output_state.tearing_page_flip` on the state `wlr_scene_output_build_state` already built. Gated three ways: `tearing = true` in fenriz.conf (default off), a **fullscreen** window on that output whose committed hint is `async` (a seam anywhere else would cut across desktop content nobody opted into), and not locked. The magnifier's `render_zoomed` never tears. A backend that refuses the async flip refuses the *whole commit*, so a rejection latches tearing off for that output and lets the existing failed-commit retry redraw it untorn on the next frame — which is the fallback wlroots' own header prescribes. The DRM cap (`DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP`) is fixed per device, so that costs one dropped frame, once. No scheduler changes: an async flip completes at once, so the existing "commit only when `wlr_scene_output_needs_frame`" loop already free-runs from the client's own commits. Note tearing is illegal in wlroots' device-wide multi-output commits, which fenriz only uses to *test* wlr-output-management configs, never to present |
| fractional-scale-v1 | `wlr_fractional_scale_manager_v1` (v1) | `server.cpp`, `layer.cpp`, `view.cpp` | Crisp HiDPI at fractional scales |
| cursor-shape-v1 | `wlr_cursor_shape_manager_v1` (v1) | `cursor.cpp` | Named cursors from clients |
| wlr-foreign-toplevel-management-v1 | `wlr_foreign_toplevel_manager_v1` | `server.cpp`, `view.cpp` | Taskbars / window lists |
| wlr-screencopy-v1 | `wlr_screencopy_manager_v1` | `server.cpp` | Screenshots (grim), simple capture |
| ext-image-copy-capture-v1 | `wlr_ext_image_copy_capture_manager_v1` (v1) | `server.cpp` | Modern dmabuf/shm capture with damage — the efficient screen-recording path; screencopy stays for grim |
| ext-image-capture-source-v1 | `wlr_ext_output_image_capture_source_manager_v1` + `wlr_ext_foreign_toplevel_image_capture_source_manager_v1` (v1) | `server.cpp`, `view.cpp` | Per-output and **per-window** capture sources for xdg-desktop-portal-wlr. Window capture renders each toplevel into a private mirror scene (`View::capture_scene`) so it works regardless of workspace visibility |
| wlr-gamma-control-v1 | `wlr_gamma_control_manager_v1` | `server.cpp`, `output.cpp` | Night light (wlsunset/gammastep) |
| wlr-output-power-management-v1 | `wlr_output_power_manager_v1` | `server.cpp`, `output.cpp` | DPMS (wlopm, hypridle) |
| ext-idle-notify-v1 | `wlr_idle_notifier_v1` | `layer.cpp`, `cursor.cpp`, `keyboard.cpp` | Idle detection (swayidle/hypridle) |
| ext-session-lock-v1 | `wlr_session_lock_manager_v1` | `lock.cpp` | Screen lockers |
| idle-inhibit-unstable-v1 | `wlr_idle_inhibit_v1` | `server.cpp` | Video/fullscreen apps keep the screen awake (mpv, browsers) |
| xdg-activation-v1 | `wlr_xdg_activation_v1` | `server.cpp`, `view.cpp` | "Raise me" requests (xdg-open, notifications). Marks the window urgent for the bar instead of stealing focus — Hyprland's `focus_on_activate = false` default. Surfaced as `workspaces.urgent` in [IPC.md](IPC.md). |
| ext-foreign-toplevel-list-v1 | `wlr_ext_foreign_toplevel_list_v1` (v1) | `server.cpp`, `view.cpp` | Standardized taskbar protocol. List-only (no activate/close), so it runs alongside the wlr one |
| ext-workspace-v1 | `wlr_ext_workspace_manager_v1` (v1) | `workspace_protocol.cpp` | Workspace list and click-to-switch for any standard bar (waybar's `ext/workspaces`). One group spanning every output, one handle per configured workspace named `1`..`N`; `activate` is the only capability, and it routes into the same `set_workspace` a keybind uses. An empty workspace no screen is showing is reported `hidden`, so a bar lists only the occupied ones (`"ignore-hidden": false` in waybar brings the fixed row back) — same set as `workspaces.occupied` in [IPC.md](IPC.md). State rides on `ipc::publish`, so it never needs its own trigger points |
| xdg-foreign-v1 + xdg-foreign-v2 | `wlr_xdg_foreign_v1` + `wlr_xdg_foreign_v2` over one `wlr_xdg_foreign_registry` | `server.cpp` | Cross-process surface parenting: a portal's file chooser attaches to the app window that opened it instead of floating free. Both versions share a registry — toolkits use v2, v1 is for clients that haven't moved. wlroots keeps `xdg_toplevel.parent` in sync for imported children, and `apply_window_rules` already floats anything with a parent, so no fenriz-side wiring |
| xdg-dialog-v1 | `wlr_xdg_wm_dialog_v1` (v1) | `server.cpp`, `view.cpp` | Clients mark a child toplevel as a dialog, optionally modal. The dialog hint itself needs no handling — the protocol has no effect without a parent toplevel, and `apply_window_rules` already floats anything parented. `modal` is the new information: `focus_view` redirects to a mapped modal child rather than focusing its parent (`modal_front`), walking the chain so a prompt on a file chooser also holds focus. A focus rule, not an input block — the protocol leaves event policy to the compositor and has clients filter their own input |
| xdg-toplevel-icon-v1 | `wlr_xdg_toplevel_icon_manager_v1` (v1) | `toplevel_props.cpp`, `ipc.cpp` | Per-window icons for bars and window lists, better than guessing from app_id. The XDG icon-theme **name** is kept and published as `windows[].icon` in [IPC.md](IPC.md); pixel-buffer icons are dropped, since the feed is text and neither foreign-toplevel protocol carries an icon. No `icon_size` preferences are advertised — that list only guides clients drawing buffers. Stored per-toplevel rather than per-window, so an icon set before the window maps is not lost |
| xdg-toplevel-drag-v1 | hand-rolled (v1) | `toplevel_drag.cpp` | A window goes along with a drag-and-drop, so a browser tab torn out of its strip follows the cursor. |
| xdg-toplevel-tag-v1 | `wlr_xdg_toplevel_tag_manager_v1` (v1) | `toplevel_props.cpp`, `config.cpp` | The client's own stable name for each of its windows ("settings", "composer"), which `windowrule = tag=…` matches on — a title changes while the user works and one app_id covers every window an app opens. wlroots signals the request but stores nothing, and the tag arrives with the initial commit, before the window is in `Server::views`; so it is kept in `toplevel_props.cpp` (keyed by toplevel, alongside the icon name, which has the same timing) and read back at map. `set_toplevel_description` is accepted and ignored — it is a translated a11y label with nothing to display it. Also published as `windows[].tag` in [IPC.md](IPC.md) |
| xdg-system-bell-v1 | `wlr_xdg_system_bell_v1` (v1) | `server.cpp`, `ipc.cpp`, `view.cpp` | The terminal bell reaches the compositor instead of going nowhere. Every ring publishes a `bell` event on the event feed (`FENRIZ_EVENT_SOCKET`, see [IPC.md](IPC.md)); fenriz has no audio, so what that means is the shell's call. A ring also flags the window it names as urgent for the bar (`windows[].urgent`), which focusing it clears. Urgency shares `mark_urgent` with xdg-activation but not its rule: an activation for a window already on screen is noise, while a bell in a visible-but-unfocused window is the case worth showing. The two channels differ at the edges — a bell in the *focused* window and a bell naming no surface both publish an event, but neither has a window to flag |
| alpha-modifier-v1 | `wlr_alpha_modifier_v1` | `server.cpp`, `view.cpp` | A client sets its own surface opacity; `apply_fx` multiplies it into the compositor's `opacity`, including on a fullscreen window (which otherwise forces 1.0 for direct scanout — an explicit client request outranks that). Read back per frame, so no handler. ponytail: windows only. Layer surfaces get no effects pass at all in fenriz today, so a bar asking for alpha is ignored |
| wl_fixes | `wlr_fixes` (v1) | `server.cpp` | Lets a client destroy a `wl_registry`. Without it libwayland leaks one per bind for the life of the connection. No handler — the create call is the whole protocol |
| ext-background-effect-v1 | hand-rolled (v1) | `background_effect.cpp`, `layer.cpp`, `view.cpp` | A client marks a region of its surface and the compositor blurs what is behind it (translucent bar, launcher). No wlroots implementation, so fenriz speaks the protocol itself. The region is double-buffered and latched on the surface's commit, like the rest of surface state. On unless `blur = false`, which makes the manager advertise no capability at all  |
| org_kde_kwin_blur | hand-rolled (v1), `protocols/kde-blur.xml` | `background_blur.cpp` | The predecessor of the above, vendored because plasma-wayland-protocols is not a dependency and it is not in wayland-protocols. Here because it is what clients on this machine actually speak, as do KDE apps that have not migrated. Same store, same nodes, same `blur` config; it just has no capabilities event to answer with when blur is off. Prefer ext-background-effect-v1 in anything new |
| virtual-keyboard-v1 | `wlr_virtual_keyboard_manager_v1` | `keyboard.cpp` | Synthesized keys (wtype, on-screen keyboards, wayvnc). A virtual keyboard keeps its client-supplied keymap — `new_keyboard` skips the system-layout override for it |
| wlr-virtual-pointer-v1 | `wlr_virtual_pointer_manager_v1` | `cursor.cpp` | Synthesized pointer input (ydotool, wayvnc, automated testing) |
| keyboard-shortcuts-inhibit-v1 | `wlr_keyboard_shortcuts_inhibit_manager_v1` | `keyboard.cpp` | A focused VM / remote-desktop client swallows compositor binds. Honored only while the inhibitor's own surface has keyboard focus |
| pointer-gestures-v1 | `wlr_pointer_gestures_v1` | `cursor.cpp` | Touchpad swipe/pinch/hold forwarded to clients (pinch-to-zoom) |
| pointer-constraints-v1 | `wlr_pointer_constraints_v1` (v1) | `cursor.cpp` | Pointer lock/confine for games. A session lock or interactive grab outranks it |
| relative-pointer-v1 | `wlr_relative_pointer_manager_v1` | `cursor.cpp` | Raw pointer deltas — what a pointer-locked client steers with |
| wlr-output-management-v1 | `wlr_output_manager_v1` (v4) | `output.cpp` | Dynamic output config (kanshi, wlr-randr, nwg-displays). An apply is folded into `config.outputs`, so it shares every path with the config file; a config reload re-asserts the file |

Plus a native, non-Wayland control socket (`FENRIZ_SOCKET`) for bars/shells, or from the
command line by `fenrizctl`: see [IPC.md](IPC.md).

## Should implement

Missing but reasonable for a desktop daily-driver, ordered by value. Effort is a rough feel
("S" ≈ create-a-global-and-forward, "M" ≈ real handler logic, "L" ≈ substantial subsystem).
A `wlr_…` key means wlroots ships the implementation and the create call is all that stands
between here and Supported; rows without one need the protocol hand-rolled from its XML.

| Protocol (audit key) | Why it matters | Effort |
|---|---|---|
| text-input-v3 + input-method-v2 (`wlr_text_input_v3`, `wlr_input_method_v2`) | IMEs (fcitx5/ibus), CJK and emoji input — a real gap for non-Latin input | L |
| tablet-v2 (`wlr_tablet_v2`) | Drawing tablets (Wacom) | M |
| security-context-v1 (`wlr_security_context_v1`) | Identify Flatpak-sandboxed clients (restrict privileged protocols). Would also give virtual-keyboard/virtual-pointer a basis to filter on — today any client may synthesize input | S–M |
| color-representation-v1 (`wlr_color_representation_v1`) | YUV coefficients and range for video buffers — correct colors from hardware-decoded video | S–M |
| fifo-v1 + commit-timing-v1 | Presentation pacing (mailbox/FIFO, timed commits) for video and games. Unlike tearing-control (now supported) these do *not* land in the output commit path: both work by holding a client's **surface** commit until a deadline, and `wlr_surface` offers no hook to defer applying committed state. wlroots 0.20 ships no helper for either, so that machinery would be hand-rolled, and a bug in it stalls clients rather than merely looking wrong. Wait for the wlroots helpers | L |
| xwayland-keyboard-grab-v1 | X11 VMs and remote-desktop clients under Xwayland swallowing compositor binds — the Xwayland twin of the keyboard-shortcuts-inhibit already supported. No wlroots helper | M |
| color-management-v1 (`wlr_color_management_v1`) | HDR and wide gamut. wlroots has the header, but the real work is colorimetry in the output path, not the global | L |

## Won't implement

Deprecated, superseded, or out of scope for a personal wlroots tiling WM. "Won't" here
records the current stance, not a vow — the debatable ones are flagged.

| Protocol (audit key) | Why not |
|---|---|
| wl_shell | Deprecated; replaced by xdg-shell |
| wlr-export-dmabuf-v1 (`wlr_export_dmabuf_v1`) | Superseded — ext-image-copy-capture-v1 covers efficient dmabuf capture (output + window) with damage; export-dmabuf is the older output-only wlroots protocol |
| xdg-shell-v6 / xdg-shell-v5 | Obsolete; stable xdg-shell is supported |
| fullscreen-shell-unstable-v1 | Single-fullscreen-surface kiosk model — not a WM |
| ivi-shell | In-vehicle infotainment; irrelevant |
| wl_drm (`wlr_drm`) | Legacy buffer path; linux-dmabuf-v1 is the modern route. Not exposed at all — fenriz skips `wlr_renderer_init_wl_display`, which is the only thing that would have created it |
| drm-lease-v1 (`wlr_drm_lease_v1`) | Direct scanout for VR headsets — niche hardware |
| ext-transient-seat-v1 (`wlr_transient_seat_v1`) | Extra virtual seats for multi-user remote desktop; a personal WM has one seat |
| xdg-session-management-v1 | Session restore across restarts. Very new, no wlroots support, and fenriz has no session-state store to restore into |
| pointer-warp-v1 | Lets a client move the cursor. Niche, and an input-integrity question worth deciding deliberately rather than by default |
| input-timestamps-v1 | High-resolution input timestamps; test tooling more than desktop use |
| text-input-v1, input-method-v1, tablet-v1 | Superseded by the v3/v2 revisions already listed under "Should implement" |
| linux-explicit-synchronization-v1 | Deprecated; linux-drm-syncobj-v1 is the supported successor |
| KDE Plasma / GTK-shell private protocols | Different ecosystems; not this compositor's surface |
| wl_touch / touch protocols | No touchscreen target (revisit if that changes) |

_Cross-check: `./scripts/protocol-audit.sh --report`. It runs as the `protocols` ctest, so a
wlroots or wayland-protocols upgrade that adds something breaks the build until it is
classified here._
