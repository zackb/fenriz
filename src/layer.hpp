#pragma once

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_scene_layer_surface_v1;

namespace fenriz {

    class Server;

    // Wraps a zwlr_layer_shell_v1 surface (bar, panel, wallpaper, notification daemon).
    // Standard-layout so wl_container_of recovers it from any embedded listener.
    struct LayerSurface {
        Server* server;
        wlr_layer_surface_v1* handle;
        wlr_scene_layer_surface_v1* scene; // renders + positions the surface; owns its subtree
        bool mapped;
        wl_listener map;
        wl_listener unmap;
        wl_listener commit;
        wl_listener new_popup;
        wl_listener destroy;
    };

    namespace layer {

        // Create the layer-shell + idle-notify globals and wire the new-surface handler.
        void init(Server& server);

        // Recompute per-output usable area from layer exclusive zones, position/configure
        // every layer surface, then re-tile windows into what's left.
        void arrange(Server& server);

    } // namespace layer

} // namespace fenriz
