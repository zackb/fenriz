#pragma once

#include <wayland-server-core.h>

struct wlr_xdg_toplevel;
struct wlr_surface;

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
        bool mapped = false;
        bool focused = false;

        wl_listener map;
        wl_listener unmap;
        wl_listener commit;
        wl_listener destroy;
    };

    // Give a view keyboard focus: activate it, deactivate the previous focus, and route
    // keyboard input to its surface. No-op if view is null or already focused.
    void focus_view(Server& server, View* view);

    // Topmost mapped view whose surface tree contains layout point (lx, ly). On a hit,
    // sets *surface to the specific (sub)surface and *sx,*sy to surface-local coords.
    View* view_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);

} // namespace fenriz
