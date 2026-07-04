#include "decoration.hpp"

#include "server.hpp"
#include "wlr.hpp"

namespace fenriz::decoration {

    namespace {

        // Per-decoration state so we can re-assert server-side mode and clean up.
        struct Decoration {
            wlr_xdg_toplevel_decoration_v1* handle;
            wl_listener request_mode;
            wl_listener surface_commit;
            wl_listener destroy;
        };

        // set_mode asserts the xdg surface is initialized (it schedules a configure), so
        // this is a no-op until the toplevel's first commit. Idempotent: skips if already
        // scheduled server-side to avoid redundant configures.
        void force_server_side(wlr_xdg_toplevel_decoration_v1* dec) {
            if (!dec->toplevel->base->initialized)
                return;
            if (dec->scheduled_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
                return;
            wlr_xdg_toplevel_decoration_v1_set_mode(dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }

        void on_request_mode(wl_listener* listener, void* data) {
            Decoration* d = wl_container_of(listener, d, request_mode);
            (void)data;
            force_server_side(d->handle);
        }

        void on_surface_commit(wl_listener* listener, void* data) {
            Decoration* d = wl_container_of(listener, d, surface_commit);
            (void)data;
            force_server_side(d->handle); // fires once the surface is initialized
        }

        void on_destroy(wl_listener* listener, void* data) {
            Decoration* d = wl_container_of(listener, d, destroy);
            (void)data;
            wl_list_remove(&d->request_mode.link);
            wl_list_remove(&d->surface_commit.link);
            wl_list_remove(&d->destroy.link);
            delete d;
        }

        void on_new_decoration(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            (void)sl;
            auto* dec = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);

            Decoration* d = new Decoration{};
            d->handle = dec;
            d->request_mode.notify = on_request_mode;
            wl_signal_add(&dec->events.request_mode, &d->request_mode);
            d->surface_commit.notify = on_surface_commit;
            wl_signal_add(&dec->toplevel->base->surface->events.commit, &d->surface_commit);
            d->destroy.notify = on_destroy;
            wl_signal_add(&dec->events.destroy, &d->destroy);

            force_server_side(dec); // in case the surface is already initialized
        }

    } // namespace

    void init(Server& server) {
        server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.display);
        server.l_new_decoration.server = &server;
        server.l_new_decoration.listener.notify = on_new_decoration;
        wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
                      &server.l_new_decoration.listener);

        // Legacy KDE protocol: GTK/libadwaita apps (ghostty) ignore xdg-decoration and read
        // this to decide whether to draw their own titlebar. Default to server-side so they
        // don't. ponytail: obsolete-but-required — it's the only decoration lever GTK honors.
        wlr_server_decoration_manager* kde = wlr_server_decoration_manager_create(server.display);
        wlr_server_decoration_manager_set_default_mode(kde, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    }

} // namespace fenriz::decoration
