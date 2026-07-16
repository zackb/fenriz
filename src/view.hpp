#pragma once

#include <wayland-server-core.h>

struct wlr_xdg_toplevel;
struct wlr_surface;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_scene_tree;
struct wlr_scene_rect;

namespace fenriz {

    class Server;

    // A managed window: wraps an xdg_toplevel and its tiled geometry. Standard-layout
    // (pointers + POD + wl_listener only) so wl_container_of recovers it cleanly.
    class View {
    public:
        View(Server& server, wlr_xdg_toplevel* toplevel);

        struct Box {
            int x = 0, y = 0, width = 0, height = 0;
        };

        Server* server = nullptr;
        wlr_xdg_toplevel* toplevel = nullptr;
        Box box;
        int workspace = 0;
        bool mapped = false;
        bool focused = false;
        bool fullscreen = false;
        // Set at map, triggers one re-arrange on the first post-map commit.
        bool needs_initial_arrange = false;
        bool floating = false; // escaped the BSP tree; free move/resize, drawn above tiles

        // Render offset from box, in logical coords; decays to 0 each frame for the
        // slide-into-place animation (see output.cpp). `dragging` holds the offset
        // (no decay) while the window tracks the cursor, and draws it above the tiles.
        double anim_ox = 0, anim_oy = 0;
        bool dragging = false;

        // wlr-foreign-toplevel handle (taskbar/window-list protocol); live while mapped.
        wlr_foreign_toplevel_handle_v1* foreign_handle = nullptr;

        // Scene nodes, created on map (see view_handle_map). scene_tree is the container
        // positioned at the tile origin; surface_tree holds the xdg surface (inset by the
        // border) and is also the parent for this window's popups. border is the frame rect.
        wlr_scene_tree* scene_tree = nullptr;
        wlr_scene_tree* surface_tree = nullptr;
        wlr_scene_rect* border = nullptr;

        wl_listener map;
        wl_listener unmap;
        wl_listener commit;
        wl_listener destroy;
        wl_listener set_title;
        wl_listener set_app_id;
        wl_listener request_fullscreen;
    };

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

    // Focus the nearest visible view whose center lies in direction (dx,dy), each in
    // {-1,0,1}: left (-1,0), right (1,0), up (0,-1), down (0,1). No-op if none.
    void focus_direction(Server& server, int dx, int dy);

    // A view is shown only when mapped and on the active workspace.
    bool view_visible(const Server& server, const View* view);

    // Position/size a view's scene nodes from its box + animation offset, set the border
    // color from focus state, and toggle visibility for the active workspace. Called from
    // everywhere that mutates box/anim (tiling::arrange, cursor grabs, the animation tick,
    // focus/workspace changes). No-op before the view's nodes exist (pre-map).
    void place_view_nodes(View* view);

    // (Re)apply SceneFX content effects (opacity + corner radius) to a view's surface buffers.
    // Call from the output frame handler before rendering — see the definition for why it can't
    // live in the commit path.
    void apply_view_effects(View* view);

    // Switch the active workspace / send the focused window to a workspace (0-indexed).
    void set_workspace(Server& server, int n);
    void move_focused_to_workspace(Server& server, int n);

} // namespace fenriz
