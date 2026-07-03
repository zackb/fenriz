#include "view.hpp"

#include "server.hpp"
#include "tiling.hpp"
#include "wlr.hpp"

namespace fenriz {

    namespace {

        void view_handle_map(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, map);
            (void)data;
            view->mapped = true;
            view->server->views.push_back(view);
            tiling::arrange(*view->server);
            focus_view(*view->server, view);
        }

        void view_handle_unmap(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, unmap);
            (void)data;
            Server& server = *view->server;
            view->mapped = false;
            server.views.remove(view);
            if (server.focused_view == view)
                server.focused_view = nullptr;
            tiling::arrange(server);
            // Move focus to another window so the keyboard isn't left dangling.
            if (!server.focused_view && !server.views.empty())
                focus_view(server, server.views.back());
        }

        void view_handle_commit(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, commit);
            (void)data;
            // The initial commit must be answered with a configure. Size 0,0 lets the
            // client choose its own dimensions; milestone 3 tiling will impose sizes.
            if (view->toplevel->base->initial_commit)
                wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        }

        void view_handle_destroy(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, destroy);
            (void)data;
            wl_list_remove(&view->map.link);
            wl_list_remove(&view->unmap.link);
            wl_list_remove(&view->commit.link);
            wl_list_remove(&view->destroy.link);
            delete view;
        }

    } // namespace

    void focus_view(Server& server, View* view) {
        if (!view || server.focused_view == view)
            return;

        if (server.focused_view) {
            wlr_xdg_toplevel_set_activated(server.focused_view->toplevel, false);
            server.focused_view->focused = false;
        }

        server.focused_view = view;
        view->focused = true;
        wlr_xdg_toplevel_set_activated(view->toplevel, true);

        if (wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat))
            wlr_seat_keyboard_notify_enter(server.seat, view->toplevel->base->surface, kb->keycodes,
                                           kb->num_keycodes, &kb->modifiers);
    }

    View* view_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy) {
        // Topmost first: views are rendered bottom -> top, so the back of the list is on top.
        for (auto it = server.views.rbegin(); it != server.views.rend(); ++it) {
            View* view = *it;
            if (!view->mapped)
                continue;
            double subx, suby;
            wlr_surface* s = wlr_xdg_surface_surface_at(view->toplevel->base, lx - view->box.x,
                                                        ly - view->box.y, &subx, &suby);
            if (s) {
                *surface = s;
                *sx = subx;
                *sy = suby;
                return view;
            }
        }
        return nullptr;
    }

    View::View(Server& server, wlr_xdg_toplevel* toplevel)
        : server(&server)
        , toplevel(toplevel) {
        wlr_surface* surface = toplevel->base->surface;

        map.notify = view_handle_map;
        wl_signal_add(&surface->events.map, &map);
        unmap.notify = view_handle_unmap;
        wl_signal_add(&surface->events.unmap, &unmap);
        commit.notify = view_handle_commit;
        wl_signal_add(&surface->events.commit, &commit);
        destroy.notify = view_handle_destroy;
        wl_signal_add(&toplevel->base->events.destroy, &destroy);
    }

} // namespace fenriz
