#include "lock.hpp"

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

        // One instance per compositor (fenriz is single-output, so one lock surface).
        // File-local like ipc.cpp's state; callbacks reach it here.
        struct LockState {
            Server* server = nullptr;
            wlr_session_lock_manager_v1* manager = nullptr;
            wlr_session_lock_v1* session = nullptr; // active lock, or null
            LockSurface* surface = nullptr;         // ponytail: single-output -> one surface
            wl_listener new_lock;
            wl_listener new_surface; // on the active session
            wl_listener unlock;
            wl_listener destroy;
        };
        LockState* g = nullptr;

        void redraw() {
            if (g->server->output)
                wlr_output_schedule_frame(g->server->output);
        }

        void on_surface_map(wl_listener* listener, void* data) {
            (void)listener;
            (void)data;
            // Route the keyboard to the lock UI so the password field receives input.
            if (g->surface)
                focus_surface(*g->server, g->surface->handle->surface);
            redraw();
        }

        void on_surface_destroy(wl_listener* listener, void* data) {
            LockSurface* ls = wl_container_of(listener, ls, destroy);
            (void)data;
            wl_list_remove(&ls->map.link);
            wl_list_remove(&ls->destroy.link);
            if (g->surface == ls)
                g->surface = nullptr;
            delete ls;
            redraw();
        }

        void on_new_surface(wl_listener* listener, void* data) {
            (void)listener;
            auto* surf = static_cast<wlr_session_lock_surface_v1*>(data);

            // Size the lock surface to its output's logical resolution.
            int w = 0, h = 0;
            wlr_output_effective_resolution(surf->output, &w, &h);
            wlr_session_lock_surface_v1_configure(surf, w, h);

            LockSurface* ls = new LockSurface{};
            ls->handle = surf;
            ls->map.notify = on_surface_map;
            wl_signal_add(&surf->surface->events.map, &ls->map);
            ls->destroy.notify = on_surface_destroy;
            wl_signal_add(&surf->events.destroy, &ls->destroy);
            g->surface = ls;
        }

        void on_unlock(wl_listener* listener, void* data) {
            (void)listener;
            (void)data;
            g->server->locked = false;
            g->session = nullptr; // session listeners are torn down in on_lock_destroy
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
            g->server->locked = true;

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

    wlr_surface* surface_for(Server& server, wlr_output* output) {
        (void)server;
        (void)output; // single-output
        if (g && g->surface && g->surface->handle->surface->mapped)
            return g->surface->handle->surface;
        return nullptr;
    }

    wlr_surface* surface_at(Server& server, double lx, double ly, double* sx, double* sy) {
        wlr_surface* root = surface_for(server, server.output);
        if (!root)
            return nullptr;
        return wlr_surface_surface_at(root, lx, ly, sx, sy);
    }

} // namespace fenriz::lock
