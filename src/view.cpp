#include "view.hpp"

#include <algorithm>
#include <cstdlib>

#include "cursor.hpp"
#include "ipc.hpp"
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
            // New window splits the focused window's tile (focus-aware dwindle).
            tiling::insert(server, view, server.focused_view);

            // HiDPI: put the surface on the output and tell it our (possibly fractional)
            // scale so it renders a native-resolution buffer instead of a 1x one we'd blur.
            wlr_surface* surface = view->toplevel->base->surface;
            if (server.output)
                wlr_surface_send_enter(surface, server.output);
            wlr_fractional_scale_v1_notify_scale(surface, server.config.scale);

            // Publish this window to the foreign-toplevel (taskbar) protocol.
            if (server.foreign_toplevel_manager) {
                view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
                if (view->toplevel->title)
                    wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
                if (view->toplevel->app_id)
                    wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
                if (server.output)
                    wlr_foreign_toplevel_handle_v1_output_enter(view->foreign_handle, server.output);
            }

            tiling::arrange(server);
            focus_view(server, view);
            ipc::publish(server);
        }

        void view_handle_unmap(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, unmap);
            (void)data;
            Server& server = *view->server;
            view->mapped = false;
            cursor::forget_view(view); // drop any in-flight mouse grab before the view is gone
            server.views.remove(view);
            tiling::remove(server, view); // sibling reclaims the freed tile
            if (view->foreign_handle) {
                wlr_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
                view->foreign_handle = nullptr;
            }
            if (server.focused_view == view)
                server.focused_view = nullptr;
            tiling::arrange(server);
            // Move focus to another visible window so the keyboard isn't left dangling.
            if (!server.focused_view)
                focus_view(server, topmost_visible(server));
            ipc::publish(server);
        }

        void view_handle_commit(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, commit);
            (void)data;
            // The initial commit must be answered with a configure. Size 0,0 lets the
            // client choose its own dimensions; milestone 3 tiling will impose sizes.
            if (view->toplevel->base->initial_commit)
                wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        }

        void view_handle_set_title(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_title);
            (void)data;
            if (view->foreign_handle && view->toplevel->title)
                wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
            if (view->focused)
                ipc::publish(*view->server); // refresh activeWindow.title in the feed
        }

        void view_handle_set_app_id(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_app_id);
            (void)data;
            if (view->foreign_handle && view->toplevel->app_id)
                wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
            if (view->focused)
                ipc::publish(*view->server);
        }

        void view_handle_request_fullscreen(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, request_fullscreen);
            (void)data;
            // requested.fullscreen may arrive before map; set_fullscreen just records the
            // flag + configures, and the map handler's arrange applies the box once visible.
            set_fullscreen(*view->server, view, view->toplevel->requested.fullscreen);
        }

        void view_handle_destroy(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, destroy);
            (void)data;
            wl_list_remove(&view->map.link);
            wl_list_remove(&view->unmap.link);
            wl_list_remove(&view->commit.link);
            wl_list_remove(&view->destroy.link);
            wl_list_remove(&view->set_title.link);
            wl_list_remove(&view->set_app_id.link);
            wl_list_remove(&view->request_fullscreen.link);
            delete view;
        }

    } // namespace

    void focus_surface(Server& server, wlr_surface* surface) {
        if (wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat))
            wlr_seat_keyboard_notify_enter(server.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }

    void focus_view(Server& server, View* view) {
        if (!view || server.focused_view == view)
            return;
        // While locked, keyboard focus belongs to the lock surface; a window mapping or a
        // click underneath must not steal it. focused_view is left as-is so it's restored
        // on unlock (on_unlock in lock.cpp).
        if (server.locked)
            return;

        if (server.focused_view) {
            wlr_xdg_toplevel_set_activated(server.focused_view->toplevel, false);
            server.focused_view->focused = false;
            if (server.focused_view->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(server.focused_view->foreign_handle, false);
        }

        // Raise floating windows to the top of the stack so they stay above tiles while in
        // use. Tiled windows keep their list order (raising them would scramble cycle_focus).
        if (view->floating) {
            server.views.remove(view);
            server.views.push_back(view);
        }

        server.focused_view = view;
        view->focused = true;
        wlr_xdg_toplevel_set_activated(view->toplevel, true);
        if (view->foreign_handle)
            wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_handle, true);

        focus_surface(server, view->toplevel->base->surface);
        ipc::publish(server);
    }

    void clear_focus(Server& server) {
        if (server.focused_view) {
            wlr_xdg_toplevel_set_activated(server.focused_view->toplevel, false);
            server.focused_view->focused = false;
            if (server.focused_view->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(server.focused_view->foreign_handle, false);
            server.focused_view = nullptr;
        }
        wlr_seat_keyboard_notify_clear_focus(server.seat);
        ipc::publish(server);
    }

    void set_fullscreen(Server& server, View* view, bool on) {
        if (!view || view->fullscreen == on)
            return;
        view->fullscreen = on;
        wlr_xdg_toplevel_set_fullscreen(view->toplevel, on);
        tiling::arrange(server);
    }

    void toggle_fullscreen(Server& server) {
        if (server.focused_view)
            set_fullscreen(server, server.focused_view, !server.focused_view->fullscreen);
    }

    void toggle_floating(Server& server) {
        View* v = server.focused_view;
        if (!v)
            return;
        v->floating = !v->floating;
        if (v->floating) {
            // Leave the tree (its slot is reclaimed by the sibling) and keep the current tile
            // box as the initial floating geometry; raise it above the tiles (v is already
            // focused, so focus_view would no-op — splice directly).
            tiling::remove(server, v);
            server.views.remove(v);
            server.views.push_back(v);
        } else {
            tiling::insert(server, v, nullptr); // re-tile at the spiral tail
        }
        tiling::arrange(server);
    }

    void focus_direction(Server& server, int dx, int dy) {
        View* cur = server.focused_view;
        if (!cur)
            return;
        auto cx = [](View* v) { return v->box.x + v->box.width / 2; };
        auto cy = [](View* v) { return v->box.y + v->box.height / 2; };
        View* best = nullptr;
        long best_score = 0;
        for (View* v : server.views) {
            if (v == cur || !view_visible(server, v))
                continue;
            const long ddx = cx(v) - cx(cur), ddy = cy(v) - cy(cur);
            const long proj = ddx * dx + ddy * dy; // must move in the requested direction
            if (proj <= 0)
                continue;
            const long perp = dx ? std::labs(ddy) : std::labs(ddx);
            const long score = proj + 2 * perp; // prefer aligned, then closest
            if (!best || score < best_score) {
                best = v;
                best_score = score;
            }
        }
        if (best)
            focus_view(server, best);
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
        // The now-visible workspace's views were arranged from stale (last-seen) boxes;
        // clear any resulting offset so they appear in place instead of sliding in.
        for (View* v : server.views)
            if (view_visible(server, v))
                v->anim_ox = v->anim_oy = 0;
        if (View* v = topmost_visible(server))
            focus_view(server, v);
        else
            clear_focus(server);
        ipc::publish(server); // active workspace changed even if focus didn't
    }

    void move_focused_to_workspace(Server& server, int n) {
        n = std::clamp(n, 0, 9);
        View* v = server.focused_view;
        if (!v || v->workspace == n)
            return;
        tiling::remove(server, v);
        v->workspace = n;                   // now on another workspace -> hidden; we stay on the current one
        tiling::insert(server, v, nullptr); // append to the target workspace's tree
        tiling::arrange(server);
        if (View* next = topmost_visible(server))
            focus_view(server, next);
        else
            clear_focus(server);
        ipc::publish(server); // occupancy of workspace n changed
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
        wl_signal_add(&toplevel->events.destroy, &destroy);
        set_title.notify = view_handle_set_title;
        wl_signal_add(&toplevel->events.set_title, &set_title);
        set_app_id.notify = view_handle_set_app_id;
        wl_signal_add(&toplevel->events.set_app_id, &set_app_id);
        request_fullscreen.notify = view_handle_request_fullscreen;
        wl_signal_add(&toplevel->events.request_fullscreen, &request_fullscreen);
    }

} // namespace fenriz
