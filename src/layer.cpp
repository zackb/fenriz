#include "layer.hpp"

#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::layer {

    namespace {

        // Subtract a surface's exclusive zone from the usable area, based on which single
        // edge (or full-width/height edge triplet) it anchors to. Mirrors the reference
        // wlroots layer-shell arrangement.
        void apply_exclusive(wlr_box& usable,
                             uint32_t anchor,
                             int32_t exclusive,
                             int32_t margin_top,
                             int32_t margin_right,
                             int32_t margin_bottom,
                             int32_t margin_left) {
            if (exclusive <= 0)
                return;
            const uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
            const uint32_t B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
            const uint32_t L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
            const uint32_t R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            if (anchor == T || anchor == (L | R | T)) {
                usable.y += exclusive + margin_top;
                usable.height -= exclusive + margin_top;
            } else if (anchor == B || anchor == (L | R | B)) {
                usable.height -= exclusive + margin_bottom;
            } else if (anchor == L || anchor == (T | B | L)) {
                usable.x += exclusive + margin_left;
                usable.width -= exclusive + margin_left;
            } else if (anchor == R || anchor == (T | B | R)) {
                usable.width -= exclusive + margin_right;
            }
        }

        // Place and configure one surface. `exclusive` selects the pass: exclusive-zone
        // surfaces are laid out first (against the full output) so they can reserve space
        // before the rest are fit into the shrinking usable area.
        void arrange_one(LayerSurface* ls, const wlr_box& full, wlr_box& usable, bool exclusive) {
            wlr_layer_surface_v1* layer = ls->handle;
            const wlr_layer_surface_v1_state& state = layer->current;
            if (exclusive != (state.exclusive_zone > 0))
                return;

            const uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
            const uint32_t B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
            const uint32_t L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
            const uint32_t R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

            // exclusive_zone == -1 means "over the top of everything, use the full output".
            const wlr_box bounds = (state.exclusive_zone == -1) ? full : usable;
            wlr_box box = {0, 0, (int)state.desired_width, (int)state.desired_height};

            const uint32_t both_h = L | R;
            if (box.width == 0)
                box.x = bounds.x;
            else if ((state.anchor & both_h) == both_h)
                box.x = bounds.x + (bounds.width - box.width) / 2;
            else if (state.anchor & L)
                box.x = bounds.x;
            else if (state.anchor & R)
                box.x = bounds.x + (bounds.width - box.width);
            else
                box.x = bounds.x + (bounds.width - box.width) / 2;

            const uint32_t both_v = T | B;
            if (box.height == 0)
                box.y = bounds.y;
            else if ((state.anchor & both_v) == both_v)
                box.y = bounds.y + (bounds.height - box.height) / 2;
            else if (state.anchor & T)
                box.y = bounds.y;
            else if (state.anchor & B)
                box.y = bounds.y + (bounds.height - box.height);
            else
                box.y = bounds.y + (bounds.height - box.height) / 2;

            if (box.width == 0) {
                box.x += state.margin.left;
                box.width = bounds.width - (state.margin.left + state.margin.right);
            } else if (state.anchor & L)
                box.x += state.margin.left;
            else if (state.anchor & R)
                box.x -= state.margin.right;

            if (box.height == 0) {
                box.y += state.margin.top;
                box.height = bounds.height - (state.margin.top + state.margin.bottom);
            } else if (state.anchor & T)
                box.y += state.margin.top;
            else if (state.anchor & B)
                box.y -= state.margin.bottom;

            if (box.width < 0 || box.height < 0) {
                wlr_layer_surface_v1_destroy(layer);
                return;
            }

            ls->geo = {box.x, box.y, box.width, box.height};
            apply_exclusive(usable,
                            state.anchor,
                            state.exclusive_zone,
                            state.margin.top,
                            state.margin.right,
                            state.margin.bottom,
                            state.margin.left);
            wlr_layer_surface_v1_configure(layer, box.width, box.height);
        }

        void on_map(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, map);
            (void)data;
            ls->mapped = true;
            // HiDPI: render at the output's (possibly fractional) scale, not 1x.
            wlr_surface* surface = ls->handle->surface;
            if (ls->handle->output)
                wlr_surface_send_enter(surface, ls->handle->output);
            wlr_fractional_scale_v1_notify_scale(surface, ls->server->config.scale);

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
                    focus_surface(server, server.focused_view->toplevel->base->surface);
                else
                    wlr_seat_keyboard_notify_clear_focus(server.seat);
            }
            arrange(server);
        }

        void on_commit(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, commit);
            (void)data;
            // Answers the initial commit with a configure and reacts to anchor/zone changes.
            arrange(*ls->server);
        }

        void on_destroy(wl_listener* listener, void* data) {
            LayerSurface* ls = wl_container_of(listener, ls, destroy);
            (void)data;
            Server& server = *ls->server;
            wl_list_remove(&ls->map.link);
            wl_list_remove(&ls->unmap.link);
            wl_list_remove(&ls->commit.link);
            wl_list_remove(&ls->destroy.link);
            server.layer_surfaces.remove(ls);
            delete ls;
            arrange(server);
        }

        void on_new_surface(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            Server& server = *sl->server;
            auto* layer = static_cast<wlr_layer_surface_v1*>(data);

            // Clients may leave output unset, delegating the choice to us.
            if (!layer->output)
                layer->output = server.output;

            LayerSurface* ls = new LayerSurface{};
            ls->server = &server;
            ls->handle = layer;
            ls->map.notify = on_map;
            wl_signal_add(&layer->surface->events.map, &ls->map);
            ls->unmap.notify = on_unmap;
            wl_signal_add(&layer->surface->events.unmap, &ls->unmap);
            ls->commit.notify = on_commit;
            wl_signal_add(&layer->surface->events.commit, &ls->commit);
            ls->destroy.notify = on_destroy;
            wl_signal_add(&layer->events.destroy, &ls->destroy);

            server.layer_surfaces.push_back(ls);
        }

    } // namespace

    void arrange(Server& server) {
        wlr_box full = {0, 0, 0, 0};
        if (server.output_layout && server.output)
            wlr_output_layout_get_box(server.output_layout, server.output, &full);
        wlr_box usable = full;

        // Exclusive pass first (top -> bottom) so bars reserve space, then the rest.
        const int order[] = {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                             ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                             ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                             ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND};
        for (bool exclusive : {true, false})
            for (int lyr : order)
                for (LayerSurface* ls : server.layer_surfaces)
                    if (ls->handle->current.layer == (uint32_t)lyr)
                        arrange_one(ls, full, usable, exclusive);

        server.usable_area = {usable.x, usable.y, usable.width, usable.height};
        tiling::arrange(server);
    }

    wlr_surface* surface_at(Server& server, double lx, double ly, double* sx, double* sy, bool above) {
        const int hi[] = {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP};
        const int lo[] = {ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND};
        const int* layers = above ? hi : lo;
        for (int i = 0; i < 2; ++i) {
            const int lyr = layers[i];
            for (LayerSurface* ls : server.layer_surfaces) {
                if (!ls->mapped || ls->handle->current.layer != (uint32_t)lyr)
                    continue;
                wlr_surface* s = wlr_layer_surface_v1_surface_at(ls->handle, lx - ls->geo.x, ly - ls->geo.y, sx, sy);
                if (s)
                    return s;
            }
        }
        return nullptr;
    }

    void init(Server& server) {
        server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
        server.l_new_layer_surface.server = &server;
        server.l_new_layer_surface.listener.notify = on_new_surface;
        wl_signal_add(&server.layer_shell->events.new_surface, &server.l_new_layer_surface.listener);

        server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    }

} // namespace fenriz::layer
