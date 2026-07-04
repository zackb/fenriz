#include "view.hpp"

#include <algorithm>

#include "server.hpp"
#include "tiling.hpp"
#include "wlr.hpp"

namespace fenriz {

    namespace {

        // Back-most (topmost) visible view on the active workspace, or null.
        View* topmost_visible(Server& server) {
            for (auto it = server.views.rbegin(); it != server.views.rend(); ++it)
                if (view_visible(server, *it))
                    return *it;
            return nullptr;
        }

        void view_handle_map(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, map);
            (void)data;
            Server& server = *view->server;
            view->mapped = true;
            view->workspace = server.active_workspace;
            server.views.push_back(view);

            // HiDPI: put the surface on the output and tell it our (possibly fractional)
            // scale so it renders a native-resolution buffer instead of a 1x one we'd blur.
            wlr_surface* surface = view->toplevel->base->surface;
            if (server.output)
                wlr_surface_send_enter(surface, server.output);
            wlr_fractional_scale_v1_notify_scale(surface, server.config.scale);

            tiling::arrange(server);
            focus_view(server, view);
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
            // Move focus to another visible window so the keyboard isn't left dangling.
            if (!server.focused_view)
                focus_view(server, topmost_visible(server));
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
            wlr_seat_keyboard_notify_enter(
                server.seat, view->toplevel->base->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }

    void clear_focus(Server& server) {
        if (server.focused_view) {
            wlr_xdg_toplevel_set_activated(server.focused_view->toplevel, false);
            server.focused_view->focused = false;
            server.focused_view = nullptr;
        }
        wlr_seat_keyboard_notify_clear_focus(server.seat);
    }

    bool view_visible(const Server& server, const View* view) {
        return view->mapped && view->workspace == server.active_workspace;
    }

    void set_workspace(Server& server, int n) {
        n = std::clamp(n, 0, 9);
        if (n == server.active_workspace)
            return;
        server.active_workspace = n;
        tiling::arrange(server);
        if (View* v = topmost_visible(server))
            focus_view(server, v);
        else
            clear_focus(server);
    }

    void move_focused_to_workspace(Server& server, int n) {
        n = std::clamp(n, 0, 9);
        View* v = server.focused_view;
        if (!v || v->workspace == n)
            return;
        v->workspace = n; // now on another workspace -> hidden; we stay on the current one
        tiling::arrange(server);
        if (View* next = topmost_visible(server))
            focus_view(server, next);
        else
            clear_focus(server);
    }

    View* view_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy) {
        // Topmost first: views are rendered bottom -> top, so the back of the list is on top.
        for (auto it = server.views.rbegin(); it != server.views.rend(); ++it) {
            View* view = *it;
            if (!view_visible(server, view))
                continue;
            double subx, suby;
            // Match the render offset: content geometry origin sits at the tile origin, so
            // shift the hit-test into surface-local coords by the same geometry offset.
            const wlr_box& geo = view->toplevel->base->geometry;
            wlr_surface* s = wlr_xdg_surface_surface_at(
                view->toplevel->base, lx - view->box.x + geo.x, ly - view->box.y + geo.y, &subx, &suby);
            if (s) {
                *surface = s;
                *sx = subx;
                *sy = suby;
                return view;
            }
        }
        return nullptr;
    }

    View::View(Server& server, wlr_xdg_toplevel* toplevel) : server(&server), toplevel(toplevel) {
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
