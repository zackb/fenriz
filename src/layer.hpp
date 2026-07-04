#pragma once

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_surface;

namespace fenriz {

    class Server;

    // Wraps a zwlr_layer_shell_v1 surface (bar, panel, wallpaper, notification daemon).
    // Standard-layout so wl_container_of recovers it from any embedded listener.
    struct LayerSurface {
        Server* server;
        wlr_layer_surface_v1* handle;
        struct {
            int x, y, width, height;
        } geo; // output-layout coordinates, computed by layer::arrange
        bool mapped;
        wl_listener map;
        wl_listener unmap;
        wl_listener commit;
        wl_listener destroy;
    };

    namespace layer {

        // Create the layer-shell + idle-notify globals and wire the new-surface handler.
        void init(Server& server);

        // Recompute per-output usable area from layer exclusive zones, position/configure
        // every layer surface, then re-tile windows into what's left.
        void arrange(Server& server);

        // Topmost mapped layer surface under (lx,ly), or nullptr. `above` picks the layers
        // drawn above windows (overlay/top) vs below (bottom/background). Coordinates are
        // output-layout; *sx/*sy return surface-local on hit.
        wlr_surface* surface_at(Server& server, double lx, double ly, double* sx, double* sy, bool above);

    } // namespace layer

} // namespace fenriz
