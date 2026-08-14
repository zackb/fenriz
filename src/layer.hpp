#pragma once

#include "background_blur.hpp"

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_blur;
struct wlr_scene_node;

namespace fenriz {

    class Server;

    // Wraps a zwlr_layer_shell_v1 surface (bar, panel, wallpaper, notification daemon).
    // Standard-layout so wl_container_of recovers it from any embedded listener.
    struct LayerSurface {
        Server* server;
        wlr_layer_surface_v1* handle;
        wlr_scene_layer_surface_v1* scene; // renders + positions the surface; owns its subtree
        // ext-background-effect-v1: blur nodes under this surface, one per region rect.
        wlr_scene_blur* blur[background_blur::RECTS_MAX];
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

        // Put this surface's background-effect blur nodes where the surface is. Called after the
        // surface is (re)positioned, and when its blur region changes.
        void place_blur(LayerSurface* ls);

        // Recompute per-output usable area from layer exclusive zones, position/configure
        // every layer surface, then re-tile windows into what's left.
        void arrange(Server& server);

        // The mapped, keyboard-interactive layer surface owning this scene node, or null.
        LayerSurface* interactive_from_node(Server& server, wlr_scene_node* node);

    } // namespace layer

} // namespace fenriz
