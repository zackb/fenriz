#include "lock.hpp"

#include <vector>

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::lock {

    namespace {

        struct LockSurface {
            wlr_session_lock_surface_v1* handle;
            wlr_scene_tree* tree; // its node in scene_lock; re-positioned by relayout()
            wl_listener map;
            wl_listener destroy;
        };

        // One instance per compositor. File-local like ipc.cpp's state; callbacks reach it here.
        struct LockState {
            Server* server = nullptr;
            wlr_session_lock_manager_v1* manager = nullptr;
            wlr_session_lock_v1* session = nullptr; // active lock, or null
            std::vector<LockSurface*> surfaces;     // one per output; the client creates them
            wlr_scene_rect* bg = nullptr;           // black backdrop under the lock UI
            wl_listener new_lock;
            wl_listener new_surface; // on the active session
            wl_listener unlock;
            wl_listener destroy;
        };
        LockState* g = nullptr;

        void redraw() {
            // Every screen: a lock that repaints only one output would leave the others
            // showing whatever was last on them.
            for (output::Output* o : g->server->outputs)
                if (o->enabled)
                    wlr_output_schedule_frame(o->handle);
        }

        // Show only the lock tree (a black backdrop + the lock UI) and hide all normal
        // content, or reverse it. A locked compositor must never reveal the desktop.
        void show_lock_scene(Server& server, bool on) {
            wlr_scene_node_set_enabled(&server.scene_background->node, !on);
            wlr_scene_node_set_enabled(&server.scene_bottom->node, !on);
            wlr_scene_node_set_enabled(&server.scene_tiles->node, !on);
            wlr_scene_node_set_enabled(&server.scene_floating->node, !on);
            wlr_scene_node_set_enabled(&server.scene_top->node, !on);
            wlr_scene_node_set_enabled(&server.scene_fullscreen->node, !on);
            wlr_scene_node_set_enabled(&server.scene_unmanaged->node, !on);
            wlr_scene_node_set_enabled(&server.scene_overlay->node, !on);
            wlr_scene_node_set_enabled(&server.scene_lock->node, on);
        }

        // Cover the whole layout with the blanking rect. Re-run whenever the layout changes: an output plugged, etc.
        void size_backdrop(Server& server) {
            if (!g->bg)
                return;
            wlr_box box;
            wlr_output_layout_get_box(server.output_layout, nullptr, &box); // null = whole layout
            wlr_scene_rect_set_size(g->bg, box.width, box.height);
            wlr_scene_node_set_position(&g->bg->node, box.x, box.y);
        }

        // Tear down the lock scene on a real unlock: drop the backdrop and reveal content.
        void end_lock_scene(Server& server) {
            if (g->bg) {
                wlr_scene_node_destroy(&g->bg->node);
                g->bg = nullptr;
            }
            show_lock_scene(server, false);
        }

        void on_surface_map(wl_listener* listener, void* data) {
            LockSurface* ls = wl_container_of(listener, ls, map);
            (void)data;
            // Route the keyboard to the lock UI so the password field receives input. With one
            // surface per output only the first needs the keyboard — they're one client, and
            // handing focus to each new surface would yank it away as monitors appear.
            if (g->server->seat->keyboard_state.focused_surface == nullptr || g->surfaces.size() == 1)
                focus_surface(*g->server, ls->handle->surface);
            redraw();
        }

        void on_surface_destroy(wl_listener* listener, void* data) {
            LockSurface* ls = wl_container_of(listener, ls, destroy);
            (void)data;
            wl_list_remove(&ls->map.link);
            wl_list_remove(&ls->destroy.link);
            std::erase(g->surfaces, ls);
            const bool had_focus = g->server->seat->keyboard_state.focused_surface == ls->handle->surface;
            delete ls;
            // An output went away while locked (lid closed at the lock screen): its surface is
            // gone, so hand the keyboard to a surviving one or the password field goes dead.
            if (had_focus) {
                if (!g->surfaces.empty())
                    focus_surface(*g->server, g->surfaces.front()->handle->surface);
                else
                    // Last surface gone. Drop focus rather than leave the seat pointing at a
                    // surface that no longer exists, so the next one to map focuses cleanly.
                    wlr_seat_keyboard_notify_clear_focus(g->server->seat);
            }
            redraw();
        }

        // Put one lock surface where its output currently is, at its current size.
        void place_surface(Server& server, LockSurface* ls) {
            int w = 0, h = 0;
            wlr_output_effective_resolution(ls->handle->output, &w, &h);
            wlr_session_lock_surface_v1_configure(ls->handle, w, h);

            wlr_box box;
            wlr_output_layout_get_box(server.output_layout, ls->handle->output, &box);
            wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
        }

        void on_new_surface(wl_listener* listener, void* data) {
            (void)listener;
            auto* surf = static_cast<wlr_session_lock_surface_v1*>(data);
            Server& server = *g->server;

            LockSurface* ls = new LockSurface{};
            ls->handle = surf;
            // Render it in the lock tree (auto-destroyed with the surface).
            ls->tree = wlr_scene_subsurface_tree_create(server.scene_lock, surf->surface);
            place_surface(server, ls);

            add_listener(ls->map, surf->surface->events.map, on_surface_map);
            add_listener(ls->destroy, surf->events.destroy, on_surface_destroy);
            g->surfaces.push_back(ls);
        }

        void on_unlock(wl_listener* listener, void* data) {
            (void)listener;
            (void)data;
            g->server->locked = false;
            g->session = nullptr; // session listeners are torn down in on_lock_destroy
            end_lock_scene(*g->server);
            // Restore keyboard focus to the window that had it before the lock.
            if (g->server->focused_view)
                focus_surface(*g->server, view_surface(g->server->focused_view));
            else
                wlr_seat_keyboard_notify_clear_focus(g->server->seat);
            redraw();
        }

        void on_lock_destroy(wl_listener* listener, void* data) {
            (void)listener;
            (void)data;
            wl_list_remove(&g->new_surface.link);
            wl_list_remove(&g->unlock.link);
            wl_list_remove(&g->destroy.link);
            g->session = nullptr;
            // If the lock client vanished without unlocking, stay locked (blank) — a locked
            // session must not fall back to the desktop. Only a new lock client (or a
            // compositor restart) recovers. server.locked is left untouched here.
            redraw();
        }

        void on_new_lock(wl_listener* listener, void* data) {
            (void)listener;
            auto* lock = static_cast<wlr_session_lock_v1*>(data);

            // Only one lock at a time; reject a second client.
            if (g->session) {
                wlr_session_lock_v1_destroy(lock);
                return;
            }

            g->session = lock;
            Server& server = *g->server;
            server.locked = true;

            // Blank the desktop behind a black backdrop before confirming the lock.
            if (g->bg)
                wlr_scene_node_destroy(&g->bg->node);
            const float black[4] = {0, 0, 0, 1};
            g->bg = wlr_scene_rect_create(server.scene_lock, 0, 0, black);
            size_backdrop(server);
            show_lock_scene(server, true);

            wlr_seat_keyboard_notify_clear_focus(server.seat);

            add_listener(g->new_surface, lock->events.new_surface, on_new_surface);
            add_listener(g->unlock, lock->events.unlock, on_unlock);
            add_listener(g->destroy, lock->events.destroy, on_lock_destroy);

            wlr_session_lock_v1_send_locked(lock);
            redraw();
        }

    } // namespace

    void init(Server& server) {
        g = new LockState{};
        g->server = &server;
        g->manager = wlr_session_lock_manager_v1_create(server.display);
        add_listener(g->new_lock, g->manager->events.new_lock, on_new_lock);
    }

    void refocus(Server& server) {
        if (!server.locked || !g || g->surfaces.empty())
            return;
        focus_surface(server, g->surfaces.front()->handle->surface);
    }

    void relayout(Server& server) {
        if (!server.locked || !g)
            return;
        size_backdrop(server);
        for (LockSurface* ls : g->surfaces)
            place_surface(server, ls);
        redraw();
    }

    void force_unlock(Server& server) {
        if (!server.locked)
            return;
        server.locked = false;
        end_lock_scene(server);
        // Tear down a still-alive (hung) lock session so it can't re-assert
        if (g && g->session)
            wlr_session_lock_v1_destroy(g->session);
        // Restore keyboard focus like on_unlock does.
        if (server.focused_view)
            focus_surface(server, view_surface(server.focused_view));
        else
            wlr_seat_keyboard_notify_clear_focus(server.seat);
        redraw();
        wlr_log(WLR_INFO, "fenriz: session force-unlocked via IPC");
    }

} // namespace fenriz::lock
