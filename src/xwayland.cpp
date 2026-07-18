#include "xwayland.hpp"

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::xwayland {

    namespace {

        void on_new_surface(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* xs = static_cast<wlr_xwayland_surface*>(data);
            if (xs->override_redirect)
                return;
            new View(*sl->server, xs);
        }

    } // namespace

    void setup(Server& server) {
        server.xwayland = wlr_xwayland_create(server.display, server.compositor, true);
        if (!server.xwayland) {
            wlr_log(WLR_ERROR, "failed to start XWayland; X11 apps will not run");
            return;
        }
        wlr_xwayland_set_seat(server.xwayland, server.seat);
        setenv("DISPLAY", server.xwayland->display_name, true);
        wlr_log(WLR_INFO, "XWayland on DISPLAY=%s", server.xwayland->display_name);

        server.l_new_xwayland_surface.server = &server;
        server.l_new_xwayland_surface.listener.notify = on_new_surface;
        wl_signal_add(&server.xwayland->events.new_surface, &server.l_new_xwayland_surface.listener);
    }

} // namespace fenriz::xwayland
