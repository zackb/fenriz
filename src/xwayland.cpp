#include "xwayland.hpp"

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::xwayland {

    namespace {

        // An X11 override-redirect surface: a menu, tooltip, dropdown, combo-box, DND icon or
        // splash. The X client positions and manages it itself, so unlike a managed toplevel
        // it is NOT a View
        struct Unmanaged {
            Server* server = nullptr;
            wlr_xwayland_surface* xwl = nullptr;
            wlr_scene_tree* surface_tree = nullptr; // live only while mapped

            // Surface arrives late (associate) and can be lost without destroy (dissociate),
            // so map/unmap are wired on the wlr_surface at associate, like managed views.
            wl_listener associate;
            wl_listener dissociate;
            wl_listener map;
            wl_listener unmap;
            wl_listener set_geometry;
            wl_listener request_configure;
            wl_listener request_activate;
            wl_listener destroy;
        };

        void unmanaged_map(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, map);
            (void)data;
            u->surface_tree = wlr_scene_subsurface_tree_create(u->server->scene_unmanaged, u->xwl->surface);
            // node.data intentionally left null: view_from_node must not mistake this for a
            // View (cursor.cpp), while surface hit-testing still routes pointer input to it.
            wlr_scene_node_set_position(&u->surface_tree->node, u->xwl->x, u->xwl->y);
            wlr_scene_node_raise_to_top(&u->surface_tree->node);
            // Not while locked: an override-redirect window must not steal the keyboard from
            // the lock surface (focus_view guards this for managed windows).
            if (wlr_xwayland_surface_override_redirect_wants_focus(u->xwl) && !u->server->locked)
                focus_surface(*u->server, u->xwl->surface);
        }

        void unmanaged_unmap(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, unmap);
            (void)data;
            // If this menu held the keyboard, hand focus back so input isn't stranded on a
            // surface that's about to be freed.
            const bool had_focus = u->server->seat->keyboard_state.focused_surface == u->xwl->surface;
            if (u->surface_tree) {
                wlr_scene_node_destroy(&u->surface_tree->node);
                u->surface_tree = nullptr;
            }
            if (had_focus)
                focus_topmost_visible(*u->server);
        }

        void unmanaged_set_geometry(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, set_geometry);
            (void)data;
            // Submenus/reflowing menus move themselves; follow along.
            if (u->surface_tree)
                wlr_scene_node_set_position(&u->surface_tree->node, u->xwl->x, u->xwl->y);
        }

        void unmanaged_request_configure(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, request_configure);
            auto* ev = static_cast<wlr_xwayland_surface_configure_event*>(data);
            // Override-redirect clients own their geometry — just honor the request.
            wlr_xwayland_surface_configure(u->xwl, ev->x, ev->y, ev->width, ev->height);
        }

        void unmanaged_request_activate(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, request_activate);
            (void)data;
            if (u->xwl->surface && u->xwl->surface->mapped && !u->server->locked &&
                wlr_xwayland_surface_override_redirect_wants_focus(u->xwl))
                focus_surface(*u->server, u->xwl->surface);
        }

        void unmanaged_associate(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, associate);
            (void)data;
            add_listener(u->map, u->xwl->surface->events.map, unmanaged_map);
            add_listener(u->unmap, u->xwl->surface->events.unmap, unmanaged_unmap);
        }

        void unmanaged_dissociate(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, dissociate);
            (void)data;
            wl_list_remove(&u->map.link);
            wl_list_remove(&u->unmap.link);
        }

        void unmanaged_destroy(wl_listener* listener, void* data) {
            Unmanaged* u = wl_container_of(listener, u, destroy);
            (void)data;
            // map/unmap already removed at dissociate; drop the surface-independent links.
            wl_list_remove(&u->associate.link);
            wl_list_remove(&u->dissociate.link);
            wl_list_remove(&u->set_geometry.link);
            wl_list_remove(&u->request_configure.link);
            wl_list_remove(&u->request_activate.link);
            wl_list_remove(&u->destroy.link);
            delete u;
        }

        void new_unmanaged(Server& server, wlr_xwayland_surface* xs) {
            Unmanaged* u = new Unmanaged{&server, xs, nullptr, {}, {}, {}, {}, {}, {}, {}, {}};
            add_listener(u->associate, xs->events.associate, unmanaged_associate);
            add_listener(u->dissociate, xs->events.dissociate, unmanaged_dissociate);
            add_listener(u->set_geometry, xs->events.set_geometry, unmanaged_set_geometry);
            add_listener(u->request_configure, xs->events.request_configure, unmanaged_request_configure);
            add_listener(u->request_activate, xs->events.request_activate, unmanaged_request_activate);
            add_listener(u->destroy, xs->events.destroy, unmanaged_destroy);
        }

        void on_new_surface(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* xs = static_cast<wlr_xwayland_surface*>(data);
            // no set_override_redirect handler
            if (xs->override_redirect)
                new_unmanaged(*sl->server, xs);
            else
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

        add_listener(server, server.l_new_xwayland_surface, server.xwayland->events.new_surface, on_new_surface);
    }

} // namespace fenriz::xwayland
