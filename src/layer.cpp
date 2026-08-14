#include "layer.hpp"

#include "background_blur.hpp"

#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::layer {

    bool reconfigure(Server& server);

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
                    wlr_fractional_scale_v1_notify_scale(surface, output::scale_of(o));
            }

            // Never let a layer surface grab the keyboard while locked
            if (ls->handle->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
                !ls->server->locked)
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
            // ...and even then, only relayout the windows if the reserved space actually
            // changed. A bar re-commits its size whenever a module's width changes (clock,
            // workspace widget), roughly once a second, and that leaves usable_area
            // identical, so the whole-compositor tiling::arrange is pure waste.
            if (ls->handle->initial_commit || ls->handle->current.committed != 0)
                if (reconfigure(*ls->server))
                    tiling::arrange(*ls->server);
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
            background_blur::clear(ls->blur);
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
            add_listener(ls->map, layer->surface->events.map, on_map);
            add_listener(ls->unmap, layer->surface->events.unmap, on_unmap);
            add_listener(ls->commit, layer->surface->events.commit, on_commit);
            add_listener(ls->new_popup, layer->events.new_popup, on_new_popup);
            add_listener(ls->destroy, layer->events.destroy, on_destroy);

            server.layer_surfaces.push_back(ls);
        }

    } // namespace

    void place_blur(LayerSurface* ls) {
        if (!ls->scene)
            return;
        if (!ls->mapped) {
            background_blur::clear(ls->blur);
            return;
        }
        const wlr_box content = {0, 0, ls->handle->surface->current.width, ls->handle->surface->current.height};
        background_blur::place(ls->handle->surface,
                               ls->scene->tree->node.parent,
                               &ls->scene->tree->node,
                               ls->scene->tree->node.x,
                               ls->scene->tree->node.y,
                               content,
                               0, // fenriz does not round layer surfaces
                               ls->blur);
    }

    // Configure every layer surface and recompute each output's usable_area. Returns true if
    // any output's usable area actually moved or resized.
    bool reconfigure(Server& server) {
        bool usable_changed = false;
        // Each output reserves its own space: a bar on one screen must not shrink the tiling
        // area of another. Every output gets its own usable_area, in layout coordinates.
        for (output::Output* out : server.outputs) {
            const auto before = out->usable_area;
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
                        place_blur(ls);
                    }

            out->usable_area = {usable.x, usable.y, usable.width, usable.height};
            usable_changed |= before.x != usable.x || before.y != usable.y || before.width != usable.width ||
                              before.height != usable.height;
        }
        return usable_changed;
    }

    void arrange(Server& server) {
        reconfigure(server);
        tiling::arrange(server);
    }

    LayerSurface* interactive_from_node(Server& server, wlr_scene_node* node) {
        if (!node)
            return nullptr;
        for (LayerSurface* ls : server.layer_surfaces) {
            if (!ls->mapped ||
                ls->handle->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
                continue;
            // Popups are parented into ls->scene->tree (on_new_popup), so the ancestor walk catches them too.
            for (wlr_scene_node* n = node; n; n = n->parent ? &n->parent->node : nullptr)
                if (n == &ls->scene->tree->node)
                    return ls;
        }
        return nullptr;
    }

    void init(Server& server) {
        server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
        add_listener(server, server.l_new_layer_surface, server.layer_shell->events.new_surface, on_new_surface);

        server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    }

} // namespace fenriz::layer
