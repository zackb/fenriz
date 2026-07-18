#include "layer.hpp"

#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::layer {

    namespace {

        // The scene tree a layer surface belongs in, by its layer. Reapplied each commit so
        // a client moving between layers (e.g. bottom -> top) restacks correctly.
        wlr_scene_tree* tree_for_layer(Server& server, uint32_t layer) {
            switch (layer) {
            case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
                return server.scene_background;
            case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
                return server.scene_bottom;
            case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
                return server.scene_overlay;
            default:
                return server.scene_top;
            }
        }

        void on_map(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, map);
            (void)data;
            ls->mapped = true;
            // HiDPI: render at this output's (possibly fractional) scale, not 1x — and not
            // some other screen's scale, which is why it's looked up per surface.
            wlr_surface* surface = ls->handle->surface;
            if (ls->handle->output) {
                wlr_surface_send_enter(surface, ls->handle->output);
                if (output::Output* o = output::by_handle(*ls->server, ls->handle->output))
                    wlr_fractional_scale_v1_notify_scale(surface, output::scale_of(*ls->server, o));
            }

            if (ls->handle->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
                focus_surface(*ls->server, surface);
            arrange(*ls->server);
        }

        void on_unmap(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, unmap);
            (void)data;
            Server& server = *ls->server;
            ls->mapped = false;
            // If this surface held the keyboard, hand it back to the focused window.
            if (server.seat->keyboard_state.focused_surface == ls->handle->surface) {
                if (server.focused_view)
                    focus_surface(server, view_surface(server.focused_view));
                else
                    wlr_seat_keyboard_notify_clear_focus(server.seat);
            }
            arrange(server);
        }

        void on_commit(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, commit);
            (void)data;
            // Restack only when the client actually moved between layers.
            if (ls->handle->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER)
                wlr_scene_node_reparent(&ls->scene->tree->node, tree_for_layer(*ls->server, ls->handle->current.layer));
            // arrange() re-sends a configure to every layer surface (the wlroots helper does
            // it unconditionally, with no dedup). Run it ONLY on the initial commit or when
            // layout-affecting state changed. A plain buffer commit (the bar just repainting)
            // has committed == 0; arranging there would send a fresh configure, forcing the
            // client to relayout+repaint and commit again, a full-refresh feedback loop that
            // pins the GPU at idle.
            if (ls->handle->initial_commit || ls->handle->current.committed != 0)
                arrange(*ls->server);
        }

        void on_new_popup(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, new_popup);
            auto* popup = static_cast<wlr_xdg_popup*>(data);
            // Parent the popup into the layer surface's scene tree; base->data lets nested
            // popups find it via the xdg-shell new_popup handler (server.cpp).
            popup_create(*ls->server, popup, ls->scene->tree);
        }

        void on_destroy(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, destroy);
            (void)data;
            Server& server = *ls->server;
            wl_list_remove(&ls->map.link);
            wl_list_remove(&ls->unmap.link);
            wl_list_remove(&ls->commit.link);
            wl_list_remove(&ls->new_popup.link);
            wl_list_remove(&ls->destroy.link);
            server.layer_surfaces.remove(ls);
            delete ls;
            arrange(server);
        }

        void on_new_surface(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            Server& server = *sl->server;
            auto* layer = static_cast<wlr_layer_surface_v1*>(data);

            // Clients may leave output unset, delegating the choice to us: put it on the
            // output the user is on. A surface with no output at all can't be placed, so
            // close it rather than leak a surface we'd never arrange.
            if (!layer->output) {
                output::Output* o = output::focused(server);
                if (!o) {
                    wlr_layer_surface_v1_destroy(layer);
                    return;
                }
                layer->output = o->handle;
            }

            LayerSurface* ls = new LayerSurface{};
            ls->server = &server;
            ls->handle = layer;
            ls->scene = wlr_scene_layer_surface_v1_create(tree_for_layer(server, layer->current.layer), layer);
            ls->map.notify = on_map;
            wl_signal_add(&layer->surface->events.map, &ls->map);
            ls->unmap.notify = on_unmap;
            wl_signal_add(&layer->surface->events.unmap, &ls->unmap);
            ls->commit.notify = on_commit;
            wl_signal_add(&layer->surface->events.commit, &ls->commit);
            ls->new_popup.notify = on_new_popup;
            wl_signal_add(&layer->events.new_popup, &ls->new_popup);
            ls->destroy.notify = on_destroy;
            wl_signal_add(&layer->events.destroy, &ls->destroy);

            server.layer_surfaces.push_back(ls);
        }

    } // namespace

    void arrange(Server& server) {
        // Each output reserves its own space: a bar on one screen must not shrink the tiling
        // area of another. Every output gets its own usable_area, in layout coordinates.
        for (output::Output* out : server.outputs) {
            wlr_box full = {0, 0, 0, 0};
            if (server.output_layout)
                wlr_output_layout_get_box(server.output_layout, out->handle, &full);
            wlr_box usable = full;

            // Exclusive pass first (top -> bottom) so bars reserve space, then the rest. The
            // scene helper does the anchor/margin/exclusive-zone math, sends the configure,
            // positions the node, and shrinks `usable` for us.
            const int order[] = {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                 ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                 ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                                 ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND};
            for (bool exclusive : {true, false})
                for (int lyr : order)
                    for (LayerSurface* ls : server.layer_surfaces) {
                        if (ls->handle->output != out->handle)
                            continue; // another screen's bar
                        if (ls->handle->current.layer != (uint32_t)lyr)
                            continue;
                        if ((ls->handle->current.exclusive_zone > 0) != exclusive)
                            continue;
                        wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
                    }

            out->usable_area = {usable.x, usable.y, usable.width, usable.height};
        }
        tiling::arrange(server);
    }

    void init(Server& server) {
        server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
        server.l_new_layer_surface.server = &server;
        server.l_new_layer_surface.listener.notify = on_new_surface;
        wl_signal_add(&server.layer_shell->events.new_surface, &server.l_new_layer_surface.listener);

        server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    }

} // namespace fenriz::layer
