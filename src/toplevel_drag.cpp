#include "toplevel_drag.hpp"

#include <vector>

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

#include "xdg-toplevel-drag-v1-server-protocol.h"

namespace fenriz::toplevel_drag {

    namespace {

        // One per xdg_toplevel_drag_v1 object. `toplevel` is the window the client asked us to move
        struct Drag {
            wl_resource* resource;
            wlr_xdg_toplevel* toplevel;
            wl_listener toplevel_destroy;
            int off_x, off_y;
        };

        struct State {
            Server* server = nullptr;
            std::vector<Drag*> drags; // live xdg_toplevel_drag_v1 objects
            Drag* active = nullptr;
            wlr_drag* drag = nullptr; // the seat drag `active` is on
            bool restore_tiled = false;
            bool origin_tiled = false;
            wl_listener drag_destroy;
        };

        State state;

        Drag* drag_from_resource(wl_resource* resource) {
            return static_cast<Drag*>(wl_resource_get_user_data(resource));
        }

        void unwatch_toplevel(Drag* d) {
            if (d->toplevel) {
                wl_list_remove(&d->toplevel_destroy.link);
                wl_list_init(&d->toplevel_destroy.link);
            }
            d->toplevel = nullptr;
        }

        void on_toplevel_destroy(wl_listener* listener, void*) {
            Drag* d = wl_container_of(listener, d, toplevel_destroy);
            unwatch_toplevel(d);
        }

        // Where the window's frame goes for a cursor at (cx,cy). surface coordinates
        void position(Server& server, View* view, double cx, double cy) {
            const int bw = server.config.border_width;
            view->box.x = (int)cx - state.active->off_x - bw;
            view->box.y = (int)cy - state.active->off_y - bw;
        }

        // Pull the attached window out of the tiling tree
        void engage(Server& server) {
            View* v = attached(server);
            if (v && !v->floating) {
                state.restore_tiled = true;
                set_floating(server, v, true);
            }
        }

        // The window the drag started from, if it is one of ours.
        View* origin_view(Server& server, wlr_surface* origin) {
            if (!origin)
                return nullptr;
            wlr_surface* root = wlr_surface_get_root_surface(origin);
            for (View* v : server.views)
                if (view_surface(v) == root)
                    return v;
            return nullptr;
        }

        void on_drag_destroy(wl_listener* listener, void*) {
            (void)listener;
            Server& server = *state.server;
            View* v = attached(server);
            const bool tile = v && state.restore_tiled && state.origin_tiled;

            wl_list_remove(&state.drag_destroy.link);
            wl_list_init(&state.drag_destroy.link);
            state.active = nullptr;
            state.drag = nullptr;
            state.restore_tiled = state.origin_tiled = false;

            if (tile)
                set_floating(server, v, false);
            // otherwise it keeps the position it was dropped at
        }

        void drag_handle_attach(
            wl_client*, wl_resource* resource, wl_resource* toplevel_resource, int32_t x, int32_t y) {
            Drag* d = drag_from_resource(resource);
            wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
            if (d->toplevel && d->toplevel != toplevel && d->toplevel->base->surface->mapped) {
                wl_resource_post_error(resource,
                                       XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
                                       "a mapped toplevel is already attached to this drag");
                return;
            }
            unwatch_toplevel(d);
            d->toplevel = toplevel;
            d->off_x = x;
            d->off_y = y;
            d->toplevel_destroy.notify = on_toplevel_destroy;
            wl_resource_add_destroy_listener(toplevel_resource, &d->toplevel_destroy);
            if (state.active == d)
                engage(*state.server);
        }

        void drag_handle_destroy(wl_client*, wl_resource* resource) {
            if (state.active == drag_from_resource(resource) && state.drag && !state.drag->dropped) {
                wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG, "the drag has not ended yet");
                return;
            }
            wl_resource_destroy(resource);
        }

        const struct xdg_toplevel_drag_v1_interface drag_impl = {
            .destroy = drag_handle_destroy,
            .attach = drag_handle_attach,
        };

        void drag_resource_destroy(wl_resource* resource) {
            Drag* d = drag_from_resource(resource);
            unwatch_toplevel(d);
            if (state.active == d)
                on_drag_destroy(&state.drag_destroy, nullptr);
            std::erase(state.drags, d);
            delete d;
        }

        void manager_handle_get_drag(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* source) {
            wl_resource* res = wl_resource_create(client, &xdg_toplevel_drag_v1_interface, 1, id);
            if (!res) {
                wl_resource_post_no_memory(resource);
                return;
            }
            Drag* d = new Drag{};
            d->resource = res;
            wl_list_init(&d->toplevel_destroy.link);
            state.drags.push_back(d);
            wl_resource_set_implementation(res, &drag_impl, d, drag_resource_destroy);
        }

        void manager_handle_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

        const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
            .destroy = manager_handle_destroy,
            .get_xdg_toplevel_drag = manager_handle_get_drag,
        };

        void manager_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, (int)version, id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &manager_impl, nullptr, nullptr);
        }

    } // namespace

    void init(Server& server) {
        state.server = &server;
        wl_list_init(&state.drag_destroy.link);
        wl_global_create(server.display, &xdg_toplevel_drag_manager_v1_interface, 1, nullptr, manager_bind);
    }

    void begin(Server& server, wlr_drag* drag, wlr_surface* origin) {
        if (state.active || !drag->seat_client)
            return;
        for (auto it = state.drags.rbegin(); it != state.drags.rend(); ++it) {
            Drag* d = *it;
            if (wl_resource_get_client(d->resource) != drag->seat_client->client)
                continue;
            state.active = d;
            state.drag = drag;
            state.restore_tiled = false;
            View* from = origin_view(server, origin);
            state.origin_tiled = from && !from->floating && !from->fullscreen;
            add_listener(state.drag_destroy, drag->events.destroy, on_drag_destroy);
            engage(server);
            return;
        }
    }

    View* attached(Server& server) {
        if (!state.active || !state.active->toplevel)
            return nullptr;
        // server.views has only mapped windows, so an unmapped toplevel detaches itself.
        for (View* v : server.views)
            if (v->kind == View::Kind::Xdg && v->toplevel == state.active->toplevel)
                return v;
        return nullptr;
    }

    void track(Server& server) {
        View* v = attached(server);
        if (!v || !server.cursor)
            return;
        position(server, v, server.cursor->x, server.cursor->y);
        place_view_nodes(v);
    }

    void adopt(View* view) {
        if (view->kind != View::Kind::Xdg || !state.active || state.active->toplevel != view->toplevel)
            return;
        Server& server = *view->server;
        // Whatever the window rules just decided is what the drop restores it to.
        state.restore_tiled = !view->floating;
        view->floating = true;
        view->want_center = false; // under cursor, not middle screen
        if (server.cursor)
            position(server, view, server.cursor->x, server.cursor->y);
    }

} // namespace fenriz::toplevel_drag
