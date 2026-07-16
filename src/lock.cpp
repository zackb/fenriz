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
            wlr_scene_node_set_enabled(&server.scene_overlay->node, !on);
            wlr_scene_node_set_enabled(&server.scene_lock->node, on);
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
            if (had_focus && !g->surfaces.empty())
                focus_surface(*g->server, g->surfaces.front()->handle->surface);
            redraw();
        }

        void on_new_surface(wl_listener* listener, void* data) {
            (void)listener;
            auto* surf = static_cast<wlr_session_lock_surface_v1*>(data);
            Server& server = *g->server;

            // Size the lock surface to its output's logical resolution.
            int w = 0, h = 0;
            wlr_output_effective_resolution(surf->output, &w, &h);
            wlr_session_lock_surface_v1_configure(surf, w, h);

            // Render it in the lock tree at the output origin (auto-destroyed with the surface).
            wlr_box box;
            wlr_output_layout_get_box(server.output_layout, surf->output, &box);
            wlr_scene_tree* tree = wlr_scene_subsurface_tree_create(server.scene_lock, surf->surface);
            wlr_scene_node_set_position(&tree->node, box.x, box.y);

            LockSurface* ls = new LockSurface{};
            ls->handle = surf;
            ls->map.notify = on_surface_map;
            wl_signal_add(&surf->surface->events.map, &ls->map);
            ls->destroy.notify = on_surface_destroy;
            wl_signal_add(&surf->events.destroy, &ls->destroy);
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
                focus_surface(*g->server, g->server->focused_view->toplevel->base->surface);
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

            // Blank the desktop behind a black backdrop before confirming the lock. Sized to
            // the whole layout (null = every output's bounding box), so a second monitor is
            // covered too — and so is any screen whose lock surface hasn't arrived yet.
            {
                wlr_box box;
                wlr_output_layout_get_box(server.output_layout, nullptr, &box);
                const float black[4] = {0, 0, 0, 1};
                g->bg = wlr_scene_rect_create(server.scene_lock, box.width, box.height, black);
                wlr_scene_node_set_position(&g->bg->node, box.x, box.y);
            }
            show_lock_scene(server, true);

            g->new_surface.notify = on_new_surface;
            wl_signal_add(&lock->events.new_surface, &g->new_surface);
            g->unlock.notify = on_unlock;
            wl_signal_add(&lock->events.unlock, &g->unlock);
            g->destroy.notify = on_lock_destroy;
            wl_signal_add(&lock->events.destroy, &g->destroy);

            // The next frame already blanks all normal content (see output.cpp), so it's
            // safe to confirm the lock to the client immediately.
            wlr_session_lock_v1_send_locked(lock);
            redraw();
        }

    } // namespace

    void init(Server& server) {
        g = new LockState{};
        g->server = &server;
        g->manager = wlr_session_lock_manager_v1_create(server.display);
        g->new_lock.notify = on_new_lock;
        wl_signal_add(&g->manager->events.new_lock, &g->new_lock);
    }

    void force_unlock(Server& server) {
        if (!server.locked)
            return;
        server.locked = false;
        end_lock_scene(server);
        // Tear down a still-alive (hung) lock session so it can't re-assert; this fires
        // on_lock_destroy, which drops the listeners and clears g->session. A crashed client
        // already ran that path (g->session == null), so we just restore below.
        if (g && g->session)
            wlr_session_lock_v1_destroy(g->session);
        // Restore keyboard focus like on_unlock does.
        if (server.focused_view)
            focus_surface(server, server.focused_view->toplevel->base->surface);
        else
            wlr_seat_keyboard_notify_clear_focus(server.seat);
        redraw();
        wlr_log(WLR_INFO, "fenriz: session force-unlocked via IPC");
    }

} // namespace fenriz::lock
