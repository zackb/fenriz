#pragma once

#include <wayland-server-core.h>

struct wlr_xdg_toplevel;
struct wlr_xwayland_surface;
struct wlr_surface;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene;
struct wlr_ext_image_capture_source_v1;

namespace fenriz {

    class Server;
    namespace output {
        struct Output;
    }

    // A managed window: wraps an xdg_toplevel and its tiled geometry. Standard-layout
    // (pointers + POD + wl_listener only) so wl_container_of recovers it cleanly.
    class View {
    public:
        // Which shell this window came from. Xdg is a Wayland-native toplevel; Xwl is an
        // XWayland (X11) surface. Exactly one of `toplevel`/`xwl` is set; the free functions
        // below (view_surface/view_app_id/…) branch on this so the tiling/focus/render path
        // stays shell-agnostic.
        enum class Kind { Xdg, Xwl };

        View(Server& server, wlr_xdg_toplevel* toplevel);
        View(Server& server, wlr_xwayland_surface* xwl);

        struct Box {
            int x = 0, y = 0, width = 0, height = 0;
        };

        Server* server = nullptr;
        Kind kind = Kind::Xdg;
        wlr_xdg_toplevel* toplevel = nullptr; // set when kind == Xdg
        wlr_xwayland_surface* xwl = nullptr;  // set when kind == Xwl
        Box box;
        Box saved_box; // geometry to restore on un-fullscreen
        int workspace = 0;
        bool mapped = false;
        bool focused = false;
        bool fullscreen = false;
        bool floating = false;    // escaped the BSP tree; free move/resize, drawn above tiles
        bool pinned = false;      // floating window shown on every workspace of its output
        bool want_center = false; // window-rule center: applied once the float has real size
        bool urgent = false;      // asked to be activated while unfocused; cleared on focus

        // Render offset from box, in logical coords; decays to 0 each frame for the
        // slide-into-place animation (see output.cpp). `dragging` holds the offset
        // (no decay) while the window tracks the cursor, and draws it above the tiles.
        double anim_ox = 0, anim_oy = 0;
        bool dragging = false;

        // wlr-foreign-toplevel handle (taskbar/window-list protocol); live while mapped.
        wlr_foreign_toplevel_handle_v1* foreign_handle = nullptr;
        // ext-foreign-toplevel-list handle: the standardized successor, list-only (it can't
        // activate or close), so it runs alongside foreign_handle rather than replacing it.
        wlr_ext_foreign_toplevel_handle_v1* ext_foreign_handle = nullptr;

        // Scene nodes, created on map (see view_handle_map). scene_tree is the container
        // positioned at the tile origin; surface_tree holds the xdg surface (inset by the
        // border); border is the frame rect.
        //
        // popup_tree is a sibling of surface_tree, not a child, and shares its origin (both
        // sit at the inner border corner, which is the window-geometry top-left the popup
        // protocol positions against). Popups must live outside surface_tree because the
        // toplevel's clip and content effects both sweep that subtree wholesale — a popup
        // parented under it inherits the parent's clip box and corner radius.
        wlr_scene_tree* scene_tree = nullptr;
        wlr_scene_tree* surface_tree = nullptr;
        wlr_scene_tree* popup_tree = nullptr;
        wlr_scene_rect* border = nullptr;
        wlr_scene_shadow* shadow = nullptr; // soft glow, bottom-most; enabled only when focused

        // Per-window screen capture (ext-image-copy-capture): a private scene mirroring just
        // this window's surface, so a portal can capture it isolated from workspace visibility.
        // Built lazily on first capture request; torn down on unmap. See on_new_ext_capture_request.
        wlr_scene* capture_scene = nullptr;
        wlr_ext_image_capture_source_v1* capture_source = nullptr; // freed via capture_scene teardown

        wl_listener map;
        wl_listener unmap;
        wl_listener commit;
        wl_listener destroy;
        wl_listener set_title;
        wl_listener set_app_id; // xwl: reused for the set_class signal
        wl_listener request_fullscreen;

        // XWayland-only. An X surface gets its wlr_surface late, so map/unmap/commit are wired
        // on the surface at `associate` and torn down at `dissociate` (see xwayland.cpp).
        wl_listener associate;
        wl_listener dissociate;
        wl_listener request_configure; // X client asks to move/resize itself
        wl_listener request_activate;  // X client asks for focus
    };

    // Shell-agnostic accessors. Each branches on View::kind so the tiling/focus/render code
    // never touches wlr_xdg_toplevel / wlr_xwayland_surface directly.
    wlr_surface* view_surface(View* view);
    const char* view_app_id(View* view); // xdg app_id, or X11 WM_CLASS
    const char* view_title(View* view);
    void view_set_activated(View* view, bool activated);
    void view_set_fullscreen(View* view, bool on);
    void view_close(View* view); // ask the client to close (kill-active keybind)
    // Tell the client its on-screen geometry: xdg gets a size, X gets an absolute-coords
    // configure (position + size). Computed from view->box minus the border.
    void view_configure(View* view);

    // Route seat keyboard input to a surface (notify_enter with the current keyboard
    // state). Used by focus_view and by layer surfaces that grab the keyboard.
    void focus_surface(Server& server, wlr_surface* surface);

    // Give a view keyboard focus: activate it, deactivate the previous focus, and route
    // keyboard input to its surface. No-op if view is null or already focused.
    void focus_view(Server& server, View* view);

    // Drop keyboard focus entirely (e.g. switching to an empty workspace).
    void clear_focus(Server& server);

    // Make a view cover the whole output (no border/gap, above the bar) or restore it
    // to tiling. Driven by client set_fullscreen requests and the fullscreen keybind.
    void set_fullscreen(Server& server, View* view, bool on);

    // Toggle fullscreen on the currently focused view.
    void toggle_fullscreen(Server& server);

    // Toggle floating on the currently focused view: pull it out of the tiling tree (or
    // return it), so it can be freely moved/resized with the mouse.
    void toggle_floating(Server& server);

    // Toggle "pin" on the focused view (floating-only): a pinned float follows the active
    // workspace on its output, so it stays visible across workspace switches.
    void toggle_pin(Server& server);

    // Apply matching `windowrule` config to a just-mapped view (before it's tiled/focused):
    // sets view->floating / view->want_center from every rule whose app_id/title regex
    // matches. Returns true if a rule asked for no_focus (caller skips focusing it).
    bool apply_window_rules(Server& server, View* view);

    // Center a floating view on its output's usable area (from its current box size).
    void center_view(Server& server, View* view);

    // Focus the nearest visible view whose center lies in direction (dx,dy), each in
    // {-1,0,1}: left (-1,0), right (1,0), up (0,-1), down (0,1). No-op if none.
    void focus_direction(Server& server, int dx, int dy);

    // A view is shown only when mapped and its workspace is the one currently shown on the
    // output that workspace lives on. A workspace on no output (all screens gone) shows
    // nothing — its windows and tree are still there, just not rendered.
    bool view_visible(const Server& server, const View* view);

    // The output the view's workspace currently lives on, or null if homeless.
    output::Output* view_output(const Server& server, const View* view);

    // Re-announce a view's output and scale to the client (wl_surface.enter + fractional
    // scale + foreign-toplevel). Call after a view's workspace migrates to another output, so
    // it re-renders at the new screen's scale instead of staying blurry.
    void view_update_output(Server& server, View* view);

    // Focus the topmost visible view anywhere, or clear focus if there is none.
    void focus_topmost_visible(Server& server);

    // Position/size a view's scene nodes from its box + animation offset, set the border
    // color from focus state, and toggle visibility for the active workspace. Called from
    // everywhere that mutates box/anim (tiling::arrange, cursor grabs, the animation tick,
    // focus/workspace changes). No-op before the view's nodes exist (pre-map).
    void place_view_nodes(View* view);

    // For a floating (non-fullscreen, non-grabbed) view, adopt the client's committed
    // window geometry into `box` and re-place the scene nodes so border/shadow/clip tighten
    // onto the real content. No-op for tiled/fullscreen/grabbed views. Called from the commit
    // handler and once more when a float resize grab ends (a lagging commit is otherwise only
    // reconciled on the next focus change).
    void view_reconcile_float_size(View* view);

    // (Re)apply SceneFX content effects (opacity + corner radius) to a view's surface buffers.
    // Call from the output frame handler before rendering — see the definition for why it can't
    // live in the commit path.
    void apply_view_effects(View* view);

    // Show workspace `n` (0-indexed). If it lives on another output, focus follows it there
    // (sway semantics); if it's homeless, it's pulled onto the focused output.
    void set_workspace(Server& server, int n);
    // Send the focused window to workspace `n`, leaving focus where it is.
    void move_focused_to_workspace(Server& server, int n);

} // namespace fenriz
