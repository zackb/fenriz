#include "background_blur.hpp"

#include <vector>

#include "layer.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

#include "ext-background-effect-v1-server-protocol.h"
#include "kde-blur-protocol.h"

namespace fenriz::background_blur {

    namespace {

        // One per client blur object. The region is double-buffered like the rest of surface.
        struct Effect {
            enum class Kind { Ext, Kde };
            Kind kind;
            wl_resource* resource;
            wlr_surface* surface; // null once the surface died
            pixman_region32_t pending, current;
            bool pending_whole, current_whole;
            bool dirty;
            wl_listener commit, destroy;
        };

        struct State {
            Server* server = nullptr;
            std::vector<Effect*> effects;
            std::vector<wl_resource*> managers;
        };

        State state;

        uint32_t capabilities(const Server& server) {
            return server.config.blur ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0;
        }

        Effect* effect_for(const wlr_surface* surface, Effect::Kind kind) {
            for (Effect* e : state.effects)
                if (e->surface == surface && e->kind == kind)
                    return e;
            return nullptr;
        }

        // The effect actually drawn for a surface, the first one asking for anything.
        const Effect* drawn_effect(const wlr_surface* surface) {
            for (Effect* e : state.effects)
                if (e->surface == surface && (e->current_whole || pixman_region32_not_empty(&e->current)))
                    return e;
            return nullptr;
        }

        // The window or layer surface owning this surface re-places its blur nodes.
        void replace_nodes(wlr_surface* surface) {
            Server& server = *state.server;
            for (View* v : server.views)
                if (view_surface(v) == surface) {
                    place_view_nodes(v);
                    return;
                }
            for (LayerSurface* ls : server.layer_surfaces)
                if (ls->handle->surface == surface) {
                    layer::place_blur(ls);
                    return;
                }
            popup_place_blur(surface);
        }

        void on_surface_commit(wl_listener* listener, void*) {
            Effect* e = wl_container_of(listener, e, commit);
            if (!e->dirty)
                return;
            e->dirty = false;
            e->current_whole = e->pending_whole;
            pixman_region32_copy(&e->current, &e->pending);
            replace_nodes(e->surface);
        }

        void on_surface_destroy(wl_listener* listener, void*) {
            Effect* e = wl_container_of(listener, e, destroy);
            wl_list_remove(&e->commit.link);
            wl_list_remove(&e->destroy.link);
            wl_list_init(&e->commit.link);
            wl_list_init(&e->destroy.link);
            pixman_region32_clear(&e->current);
            pixman_region32_clear(&e->pending);
            e->current_whole = e->pending_whole = false;
            e->surface = nullptr;
        }

        void effect_handle_set_blur_region(wl_client*, wl_resource* resource, wl_resource* region_resource) {
            auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
            if (!e->surface) {
                wl_resource_post_error(resource,
                                       EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                                       "the surface this effect belongs to is gone");
                return;
            }
            // copy, the client may destroy the wl_region immediately after this
            e->pending_whole = false;
            if (region_resource) {
                const pixman_region32_t* region = wlr_region_from_resource(region_resource);
                pixman_region32_copy(&e->pending, region);
            } else {
                pixman_region32_clear(&e->pending);
            }
            e->dirty = true;
        }

        void effect_handle_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

        const struct ext_background_effect_surface_v1_interface effect_impl = {
            .destroy = effect_handle_destroy,
            .set_blur_region = effect_handle_set_blur_region,
        };

        void effect_resource_destroy(wl_resource* resource) {
            auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
            wlr_surface* surface = e->surface;
            if (surface) {
                wl_list_remove(&e->commit.link);
                wl_list_remove(&e->destroy.link);
            }
            pixman_region32_fini(&e->pending);
            pixman_region32_fini(&e->current);
            std::erase(state.effects, e);
            delete e;
            if (surface)
                replace_nodes(surface);
        }

        // Everything an effect needs regardless of which protocol asked for it.
        Effect* make_effect(wl_resource* res, wlr_surface* surface, Effect::Kind kind, bool whole) {
            Effect* e = new Effect{};
            e->kind = kind;
            e->resource = res;
            e->surface = surface;
            e->pending_whole = e->current_whole = whole;
            pixman_region32_init(&e->pending);
            pixman_region32_init(&e->current);
            add_listener(e->commit, surface->events.commit, on_surface_commit);
            add_listener(e->destroy, surface->events.destroy, on_surface_destroy);
            state.effects.push_back(e);
            return e;
        }

        void manager_handle_get(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface_resource) {
            wlr_surface* surface = wlr_surface_from_resource(surface_resource);
            if (effect_for(surface, Effect::Kind::Ext)) {
                wl_resource_post_error(resource,
                                       EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                                       "this surface already has a background effect");
                return;
            }
            wl_resource* res = wl_resource_create(client, &ext_background_effect_surface_v1_interface, 1, id);
            if (!res) {
                wl_resource_post_no_memory(resource);
                return;
            }
            Effect* e = make_effect(res, surface, Effect::Kind::Ext, false);
            wl_resource_set_implementation(res, &effect_impl, e, effect_resource_destroy);
        }

        void manager_handle_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

        const struct ext_background_effect_manager_v1_interface manager_impl = {
            .destroy = manager_handle_destroy,
            .get_background_effect = manager_handle_get,
        };

        void manager_resource_destroy(wl_resource* resource) { std::erase(state.managers, resource); }

        void manager_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
            wl_resource* res =
                wl_resource_create(client, &ext_background_effect_manager_v1_interface, (int)version, id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &manager_impl, nullptr, manager_resource_destroy);
            state.managers.push_back(res);
            ext_background_effect_manager_v1_send_capabilities(res, capabilities(*state.server));
        }

        // org_kde_kwin_blur: the same thing, asked for the old way
        void kde_handle_set_region(wl_client*, wl_resource* resource, wl_resource* region_resource) {
            auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
            if (!e->surface)
                return; // the surface died under it
            if (region_resource) {
                pixman_region32_copy(&e->pending, wlr_region_from_resource(region_resource));
                e->pending_whole = false;
            } else {
                pixman_region32_clear(&e->pending);
                e->pending_whole = true; // NULL here means the whole surface, not "none"
            }
            e->dirty = true;
        }

        void kde_handle_commit(wl_client*, wl_resource* resource) {
            auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
            e->dirty = true;
        }

        void kde_handle_release(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

        // Stop an effect drawing without touching its wl_resource.
        void neutralize(Effect* e) {
            e->pending_whole = e->current_whole = false;
            pixman_region32_clear(&e->pending);
            pixman_region32_clear(&e->current);
            e->dirty = false;
        }

        const struct org_kde_kwin_blur_interface kde_blur_impl = {
            .commit = kde_handle_commit,
            .set_region = kde_handle_set_region,
            .release = kde_handle_release,
        };

        void kde_manager_handle_create(wl_client* client,
                                       wl_resource* resource,
                                       uint32_t id,
                                       wl_resource* surface_resource) {
            wlr_surface* surface = wlr_surface_from_resource(surface_resource);
            for (Effect* old : state.effects)
                if (old->surface == surface && old->kind == Effect::Kind::Kde)
                    neutralize(old);

            wl_resource* res = wl_resource_create(client, &org_kde_kwin_blur_interface, 1, id);
            if (!res) {
                wl_resource_post_no_memory(resource);
                return;
            }
            // Blurring everything is this protocol's starting state, and for many clients its
            // only one.
            Effect* e = make_effect(res, surface, Effect::Kind::Kde, true);
            wl_resource_set_implementation(res, &kde_blur_impl, e, effect_resource_destroy);
        }

        void kde_manager_handle_unset(wl_client*, wl_resource*, wl_resource* surface_resource) {
            wlr_surface* surface = wlr_surface_from_resource(surface_resource);
            bool any = false;
            for (Effect* e : state.effects)
                if (e->surface == surface && e->kind == Effect::Kind::Kde) {
                    neutralize(e);
                    any = true;
                }
            if (any)
                replace_nodes(surface);
        }

        const struct org_kde_kwin_blur_manager_interface kde_manager_impl = {
            .create = kde_manager_handle_create,
            .unset = kde_manager_handle_unset,
        };

        void kde_manager_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &org_kde_kwin_blur_manager_interface, (int)version, id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &kde_manager_impl, nullptr, nullptr);
        }

        // SceneFX's blur parameters are global to the scene, not per-node.
        void apply_blur_data(Server& server) {
            wlr_scene_set_blur_data(server.scene,
                                    server.config.blur_passes,
                                    server.config.blur_radius,
                                    0.02f, // noise: breaks up banding in the blurred result
                                    0.9f,  // brightness
                                    0.9f,  // contrast
                                    1.1f); // saturation
        }

    } // namespace

    void init(Server& server) {
        state.server = &server;
        apply_blur_data(server);
        wl_global_create(server.display, &ext_background_effect_manager_v1_interface, 1, nullptr, manager_bind);
        wl_global_create(server.display, &org_kde_kwin_blur_manager_interface, 1, nullptr, kde_manager_bind);
    }

    void reload(Server& server) {
        apply_blur_data(server);
        // The protocol says capabilities are re-sent whenever they change
        for (wl_resource* manager : state.managers)
            ext_background_effect_manager_v1_send_capabilities(manager, capabilities(server));
        for (Effect* e : state.effects)
            if (e->surface)
                replace_nodes(e->surface);
    }

    void place(wlr_surface* surface,
               wlr_scene_tree* parent,
               wlr_scene_node* below,
               int ox,
               int oy,
               const wlr_box& content,
               int radius,
               wlr_scene_blur* nodes[RECTS_MAX]) {
        const Effect* e = state.server && state.server->config.blur ? drawn_effect(surface) : nullptr;

        // surface-local coordinates
        pixman_region32_t clipped;
        pixman_region32_init(&clipped);
        int count = 0;
        const pixman_box32_t* rects = nullptr;
        pixman_box32_t bounds;
        if (e && e->current_whole) {
            bounds = {content.x, content.y, content.x + content.width, content.y + content.height};
            rects = &bounds;
            count = content.width > 0 && content.height > 0 ? 1 : 0;
        } else if (e) {
            pixman_region32_intersect_rect(&clipped, &e->current, content.x, content.y, content.width, content.height);
            int n = 0;
            rects = pixman_region32_rectangles(&clipped, &n);
            if (n > RECTS_MAX) {
                // Too finely chopped to be worth a node each
                bounds = *pixman_region32_extents(&clipped);
                rects = &bounds;
                n = 1;
            }
            count = n;
        }

        for (int i = 0; i < RECTS_MAX; i++) {
            if (i >= count) {
                if (nodes[i]) {
                    wlr_scene_node_destroy(&nodes[i]->node);
                    nodes[i] = nullptr;
                }
                continue;
            }
            const int w = rects[i].x2 - rects[i].x1, h = rects[i].y2 - rects[i].y1;
            if (!nodes[i]) {
                nodes[i] = wlr_scene_blur_create(parent, w, h);
                if (!nodes[i])
                    continue;
            }
            wlr_scene_blur_set_size(nodes[i], w, h);
            // Round only the corners this rectangle shares with the window itself.
            fx_corner_radii corners = corner_radii_none();
            if (radius > 0) {
                const bool left = rects[i].x1 <= content.x, top = rects[i].y1 <= content.y;
                const bool right = rects[i].x2 >= content.x + content.width;
                const bool bottom = rects[i].y2 >= content.y + content.height;
                corners = corner_radii_new(left && top ? radius : 0,
                                           right && top ? radius : 0,
                                           right && bottom ? radius : 0,
                                           left && bottom ? radius : 0);
            }
            wlr_scene_blur_set_corner_radii(nodes[i], corners);
            wlr_scene_node_set_position(&nodes[i]->node, ox + rects[i].x1, oy + rects[i].y1);
            if (nodes[i]->node.parent != parent)
                wlr_scene_node_reparent(&nodes[i]->node, parent);
            wlr_scene_node_place_below(&nodes[i]->node, below);
        }
        pixman_region32_fini(&clipped);
    }

    void forget(wlr_scene_blur* nodes[RECTS_MAX]) {
        for (int i = 0; i < RECTS_MAX; i++)
            nodes[i] = nullptr;
    }

    void clear(wlr_scene_blur* nodes[RECTS_MAX]) {
        for (int i = 0; i < RECTS_MAX; i++) {
            if (nodes[i])
                wlr_scene_node_destroy(&nodes[i]->node);
            nodes[i] = nullptr;
        }
    }

} // namespace fenriz::background_blur
