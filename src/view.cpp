#include "view.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "color.hpp"
#include "cursor.hpp"
#include "ipc.hpp"
#include "server.hpp"
#include "tiling.hpp"
#include "toplevel_drag.hpp"
#include "toplevel_props.hpp"
#include "wlr.hpp"

namespace fenriz {

    void raise_to_tail(Server& server, View* v); // defined below; used in view_handle_map

    namespace {

        // Unpack a 0xRRGGBBAA color into the float[4] the scene setters take.
        // For wlr_render_color which requires R/G/B already multiplied by A
        void u32_color(uint32_t c, float out[4]) {
            const float a = (c & 0xff) / 255.0f;
            out[0] = ((c >> 24) & 0xff) / 255.0f * a;
            out[1] = ((c >> 16) & 0xff) / 255.0f * a;
            out[2] = ((c >> 8) & 0xff) / 255.0f * a;
            out[3] = a;
        }

        // ramp resolution
        constexpr int GRAD_N = 17;

        struct GradBuffer {
            wlr_buffer base;
            uint32_t px[GRAD_N * GRAD_N]; // premultiplied ARGB8888, row-major
        };

        void grad_buffer_destroy(wlr_buffer* buffer) {
            GradBuffer* g = wl_container_of(buffer, g, base);
            delete g;
        }

        bool grad_buffer_begin(wlr_buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
            if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
                return false; // read-only: the colors only change by rebuilding the buffer
            GradBuffer* g = wl_container_of(buffer, g, base);
            *data = g->px;
            *format = DRM_FORMAT_ARGB8888;
            *stride = GRAD_N * sizeof(uint32_t);
            return true;
        }

        void grad_buffer_end(wlr_buffer*) {}

        const wlr_buffer_impl grad_buffer_impl = {
            .destroy = grad_buffer_destroy,
            .get_dmabuf = nullptr,
            .get_shm = nullptr,
            .begin_data_ptr_access = grad_buffer_begin,
            .end_data_ptr_access = grad_buffer_end,
        };

        // 0xRRGGBBAA -> premultiplied ARGB8888, which is what the renderer blends.
        uint32_t premul_argb(uint32_t rgba) {
            const uint32_t a = rgba & 0xff;
            const uint32_t r = (((rgba >> 24) & 0xff) * a + 127) / 255;
            const uint32_t g = (((rgba >> 16) & 0xff) * a + 127) / 255;
            const uint32_t b = (((rgba >> 8) & 0xff) * a + 127) / 255;
            return (a << 24) | (r << 16) | (g << 8) | b;
        }

        wlr_buffer* gradient_texture(const Config& cfg, uint32_t* gen) {
            static wlr_buffer* cached = nullptr;
            static uint32_t cached_from = 0, cached_to = 0, generation = 0;
            static float cached_ease = -1.0f;

            if (cached && cached_from == cfg.border_active && cached_to == cfg.border_gradient &&
                cached_ease == cfg.border_gradient_ease) {
                *gen = generation;
                return cached;
            }

            GradBuffer* g = new GradBuffer{};
            wlr_buffer_init(&g->base, &grad_buffer_impl, GRAD_N, GRAD_N);
            for (int j = 0; j < GRAD_N; j++) {
                for (int i = 0; i < GRAD_N; i++) {
                    const float t = (float)(i + j) / (2 * (GRAD_N - 1));
                    const float e = ramp_ease(t, cfg.border_gradient_ease);
                    g->px[j * GRAD_N + i] = premul_argb(u32_lerp(cfg.border_active, cfg.border_gradient, e));
                }
            }

            if (cached)
                wlr_buffer_drop(cached); // live nodes keep their lock until they re-set

            cached = &g->base;
            cached_from = cfg.border_active;
            cached_to = cfg.border_gradient;
            cached_ease = cfg.border_gradient_ease;
            *gen = ++generation;
            return cached;
        }

        // Map a band's window-space rect into the ramp texture
        wlr_fbox grad_src(int x, int y, int w, int h, int fw, int fh) {
            constexpr double s = GRAD_N - 1;
            return {
                .x = 0.5 + s * x / fw,
                .y = 0.5 + s * y / fh,
                .width = s * w / fw,
                .height = s * h / fh,
            };
        }

        // Per-buffer effects for a mapped window: round the content corners and apply the
        // global opacity. Fullscreen drops all three (border, rounding, opacity) — nothing
        // should show through a fullscreen window, and scenefx only direct-scans-out at
        // opacity 1.0. Iterated over the xdg surface subtree so every buffer matches.
        // ponytail: CSD apps (GTK/Firefox) whose buffer includes a shadow margin round the
        // buffer corner, not the visible window edge — clip each buffer to window geometry
        // (SwayFX-style) if that looks wrong. Rounding radius is inset by the border so the
        // content radius nests inside the (rounded) border frame.
        void apply_fx(wlr_scene_buffer* buf, int /*sx*/, int /*sy*/, void* data) {
            View* v = static_cast<View*>(data);
            Server& s = *v->server;
            const int bw = v->fullscreen ? 0 : s.config.border_width;
            const int r = v->fullscreen ? 0 : std::max(0, s.config.rounding - bw);
            wlr_scene_buffer_set_corner_radius(buf, r);
            // alpha-modifier-v1: a client's own opacity multiplies the compositor's
            float alpha = v->fullscreen ? 1.0f : s.config.opacity;
            if (wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(buf))
                if (const wlr_alpha_modifier_surface_v1_state* am =
                        wlr_alpha_modifier_v1_get_surface_state(ss->surface))
                    alpha *= (float)am->multiplier;
            wlr_scene_buffer_set_opacity(buf, alpha);
        }

        // Tell a toplevel it's tiled on all edges (or none). Advertising the tiled state is
        // what makes GTK/Gecko honor the size we configure and drop their CSD shadow/rounding
        // on those edges.
        void set_tiled(View* view, bool tiled) {
            if (view->kind != View::Kind::Xdg)
                return; // X11 has no tiled/maximized state to advertise; we just size it
            wlr_xdg_toplevel* tl = view->toplevel;
            if (wl_resource_get_version(tl->resource) >= 2) { // TILED_* states since v2
                uint32_t edges =
                    tiled ? (WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM) : WLR_EDGE_NONE;
                wlr_xdg_toplevel_set_tiled(tl, edges);
            } else {
                wlr_xdg_toplevel_set_maximized(tl, tiled);
            }
        }

        // recommend the biggest window geometry that fits
        void send_bounds(View* view) {
            if (view->kind != View::Kind::Xdg || wl_resource_get_version(view->toplevel->resource) < 4)
                return; // configure_bounds since v4
            Server& server = *view->server;
            const output::Area a = output::usable(server, output::focused(server));
            if (a.width <= 0)
                return;
            const int bw = server.config.border_width;
            wlr_xdg_toplevel_set_bounds(view->toplevel, a.width - 2 * bw, a.height - 2 * bw);
        }

        // Single owner of a view's scene-tree parent
        void restack_view(Server& server, View* view) {
            if (!view->scene_tree)
                return;
            wlr_scene_node_reparent(&view->scene_tree->node,
                                    view->fullscreen ? server.scene_fullscreen
                                    : view->floating ? server.scene_floating
                                                     : server.scene_tiles);
        }

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
            // New windows open on the workspace shown on the output the user is on.
            if (output::Output* o = output::focused(server); o && o->active_ws >= 0)
                view->workspace = o->active_ws;
            server.views.push_back(view);

            // Window rules run before the scene tree is built (it branches on floating) and
            // before tiling/focus below.
            const bool no_focus = apply_window_rules(server, view);
            // A window mapping into an in-flight toplevel drag belongs under the cursor.
            toplevel_drag::adopt(view);

            // Build the scene nodes: a container tree holding the border rect (below), the
            // xdg surface subtree (inset by the border in place_view_nodes), and the popup
            // tree on top. The View* on the container lets scene hit-testing recover the
            // window; base->data lets popups find their parent scene tree (see on_new_popup
            // in server.cpp) — that's popup_tree, not surface_tree, so menus escape the
            // toplevel's clip and effects.
            view->scene_tree = wlr_scene_tree_create(server.scene_tiles);
            view->scene_tree->node.data = view;
            restack_view(server, view);
            // Shadow first so it's the bottom-most child (z-order = insertion order):
            // it must spread out behind the border and surface.
            float scol[4];
            u32_color(server.config.shadow_color, scol);
            view->shadow = wlr_scene_shadow_create(
                view->scene_tree, 0, 0, server.config.rounding, (float)server.config.shadow_blur, scol);
            float col[4];
            u32_color(server.config.border_inactive, col);
            view->border = wlr_scene_rect_create(view->scene_tree, 0, 0, col);

            // gradient ring instead of border rect when enabled
            for (wlr_scene_rect*& corner : view->grad_corner)
                corner = wlr_scene_rect_create(view->scene_tree, 0, 0, col);
            for (wlr_scene_buffer*& edge : view->grad_edge) {
                edge = wlr_scene_buffer_create(view->scene_tree, nullptr);
                wlr_scene_buffer_set_filter_mode(edge, WLR_SCALE_FILTER_BILINEAR); // the ramp itself
            }
            if (view->kind == View::Kind::Xdg) {
                view->surface_tree = wlr_scene_xdg_surface_create(view->scene_tree, view->toplevel->base);
                view->popup_tree = wlr_scene_tree_create(view->scene_tree); // created last: draws above
                view->toplevel->base->data = view->popup_tree;
            } else {
                // X11: a plain surface subtree (no xdg geometry, no popup_tree — X child windows
                // are override-redirect surfaces, out of scope for this managed-only cut).
                view->surface_tree = wlr_scene_subsurface_tree_create(view->scene_tree, view->xwl->surface);
            }

            // New window splits the focused window's tile (focus-aware dwindle) — unless a
            // rule floated it, in which case it stays out of the tree (floating ⟺ not tiled).
            if (!view->floating)
                tiling::insert(server, view, server.focused_view);

            // Publish this window to the foreign-toplevel (taskbar) protocol.
            if (server.foreign_toplevel_manager) {
                view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
                if (view_title(view))
                    wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view_title(view));
                if (view_app_id(view))
                    wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view_app_id(view));
            }
            // and to its standardized successor
            if (server.ext_foreign_toplevel_list) {
                wlr_ext_foreign_toplevel_handle_v1_state state = {
                    .title = view_title(view),
                    .app_id = view_app_id(view),
                };
                view->ext_foreign_handle =
                    wlr_ext_foreign_toplevel_handle_v1_create(server.ext_foreign_toplevel_list, &state);
            }

            // HiDPI: announce the view's own output + that output's scale, so it renders a
            // native-resolution buffer. Also re-run whenever it migrates between screens.
            view_update_output(server, view);

            // Tiled windows honor our sizing; a floated one sizes itself and draws above tiles.
            if (view->floating)
                raise_to_tail(server, view);
            set_tiled(view, !view->floating); // folds into the arrange configure below
            tiling::arrange(server);
            if (!no_focus)
                focus_view(server, view);
            ipc::publish(server);
        }

        void view_handle_unmap(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, unmap);
            (void)data;
            Server& server = *view->server;
            view->mapped = false;
            // Tear down the per-window capture mirror first
            if (view->capture_scene) {
                wlr_scene_node_destroy(&view->capture_scene->tree.node);
                view->capture_scene = nullptr;
                view->capture_source = nullptr; // freed transitively above
            }
            if (view->scene_tree) {
                // Frees the whole subtree (border + surface + any popups).
                wlr_scene_node_destroy(&view->scene_tree->node);
                view->scene_tree = nullptr;
                view->surface_tree = nullptr;
                view->popup_tree = nullptr;
                view->border = nullptr;
                view->shadow = nullptr;
                for (wlr_scene_rect*& corner : view->grad_corner)
                    corner = nullptr;
                for (wlr_scene_buffer*& edge : view->grad_edge)
                    edge = nullptr;
                view->grad_gen = 0;
                if (view->kind == View::Kind::Xdg)
                    view->toplevel->base->data = nullptr;
            }
            view->announced_output = nullptr;
            cursor::forget_view(view); // drop any in-flight mouse grab before the view is gone
            server.views.remove(view);
            tiling::remove(server, view); // sibling reclaims the freed tile
            if (view->foreign_handle) {
                wlr_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
                view->foreign_handle = nullptr;
            }
            if (view->ext_foreign_handle) {
                wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_foreign_handle);
                view->ext_foreign_handle = nullptr;
            }
            if (server.focused_view == view)
                server.focused_view = nullptr;
            if (server.nav_return == view) // don't leave a dangling round-trip target
                server.nav_return = nullptr;
            for (Workspace& ws : server.workspaces)
                if (ws.last_focused == view) // don't leave a dangling pointer to freed memory
                    ws.last_focused = nullptr;
            tiling::arrange(server);
            // Move focus to another visible window so the keyboard isn't left dangling.
            if (!server.focused_view)
                focus_view(server, topmost_visible(server));
            ipc::publish(server);
        }

        void view_handle_commit(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, commit);
            (void)data;
            // The initial commit must be answered with a configure.
            if (view->kind == View::Kind::Xdg && view->toplevel->base->initial_commit) {
                wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
                // Advertise tiled in the initial configure, before the client has drawn
                // anything. (Chromium bug)
                set_tiled(view, true);
                send_bounds(view);
            }

            // the client has responded to our last size request, so its committed geometry can be trusted
            view->acked = true;

            if (view->float_self_sized || view->kind == View::Kind::Xwl)
                view_adopt_float_size(view);
            else
                place_view_nodes(view);
        }

        // Push title+app_id to the ext-foreign-toplevel handle. Unlike the wlr protocol's
        // independent setters, update_state takes both at once
        void ext_foreign_update(View* view) {
            if (!view->ext_foreign_handle)
                return;
            wlr_ext_foreign_toplevel_handle_v1_state state = {
                .title = view_title(view),
                .app_id = view_app_id(view),
            };
            wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_foreign_handle, &state);
        }

        void view_handle_set_title(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_title);
            (void)data;
            if (view->foreign_handle && view_title(view))
                wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view_title(view));
            ext_foreign_update(view);
            if (view->focused)
                ipc::publish(*view->server); // refresh activeWindow.title in the feed
        }

        // Wired to xdg set_app_id and (for X11) the set_class signal; both map to view_app_id.
        void view_handle_set_app_id(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_app_id);
            (void)data;
            if (view->foreign_handle && view_app_id(view))
                wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view_app_id(view));
            ext_foreign_update(view);
            if (view->focused)
                ipc::publish(*view->server);
        }

        void view_handle_request_fullscreen(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, request_fullscreen);
            (void)data;
            // requested.fullscreen may arrive before map; set_fullscreen just records the
            // flag + configures, and the map handler's arrange applies the box once visible.
            const bool want =
                view->kind == View::Kind::Xdg ? view->toplevel->requested.fullscreen : view->xwl->fullscreen;
            set_fullscreen(*view->server, view, want);
        }

        // wlroots unmaps a surface before destroying its role object, so `mapped` should always
        // be false here.
        void view_force_unmap(View* view) {
            assert(!view->mapped && "view destroyed while still mapped");
            if (view->mapped)
                view_handle_unmap(&view->unmap, nullptr);
        }

        void view_handle_destroy(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, destroy);
            (void)data;
            view_force_unmap(view);
            wl_list_remove(&view->map.link);
            wl_list_remove(&view->unmap.link);
            wl_list_remove(&view->commit.link);
            wl_list_remove(&view->destroy.link);
            wl_list_remove(&view->set_title.link);
            wl_list_remove(&view->set_app_id.link);
            wl_list_remove(&view->request_fullscreen.link);
            delete view;
        }

        // ---- XWayland-only callbacks ----------------------------------------------------
        // An X surface gets its wlr_surface late and can lose it (dissociate) without being
        // destroyed, so map/unmap/commit are wired here at associate and dropped at dissociate.

        void view_handle_associate(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, associate);
            (void)data;
            add_listener(view->map, view->xwl->surface->events.map, view_handle_map);
            add_listener(view->unmap, view->xwl->surface->events.unmap, view_handle_unmap);
            add_listener(view->commit, view->xwl->surface->events.commit, view_handle_commit);
        }

        void view_handle_dissociate(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, dissociate);
            (void)data;
            wl_list_remove(&view->map.link);
            wl_list_remove(&view->unmap.link);
            wl_list_remove(&view->commit.link);
        }

        void view_handle_request_configure(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, request_configure);
            auto* ev = static_cast<wlr_xwayland_surface_configure_event*>(data);
            // Unmapped, or a free-floating window: let the client place/size itself and just
            // ack it. A tiled or fullscreen window is compositor-authoritative — re-assert our
            // geometry so the X app can't fight the layout.
            if (!view->mapped || (view->floating && !view->fullscreen)) {
                wlr_xwayland_surface_configure(view->xwl, ev->x, ev->y, ev->width, ev->height);
                if (view->mapped) {
                    const int bw = view->server->config.border_width;
                    view->box = {ev->x - bw, ev->y - bw, ev->width + 2 * bw, ev->height + 2 * bw};
                    place_view_nodes(view);
                }
            } else {
                view_configure(view);
            }
        }

        void view_handle_request_activate(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, request_activate);
            (void)data;
            if (view->mapped)
                focus_view(*view->server, view);
        }

        void view_xwl_handle_destroy(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, destroy);
            (void)data;
            view_force_unmap(view);
            // map/unmap/commit were already removed at dissociate (wlroots dissociates before
            // destroy); remove only the surface-independent links wired in the ctor.
            wl_list_remove(&view->destroy.link);
            wl_list_remove(&view->set_title.link);
            wl_list_remove(&view->set_app_id.link);
            wl_list_remove(&view->request_fullscreen.link);
            wl_list_remove(&view->associate.link);
            wl_list_remove(&view->dissociate.link);
            wl_list_remove(&view->request_configure.link);
            wl_list_remove(&view->request_activate.link);
            delete view;
        }

    } // namespace

    wlr_surface* view_surface(View* view) {
        return view->kind == View::Kind::Xdg ? view->toplevel->base->surface : view->xwl->surface;
    }

    const char* view_app_id(View* view) {
        // X11 has no app_id; WM_CLASS is the closest analogue (window rules match on it, and
        // `class` was renamed to `class_` in wlr.hpp for the C++ keyword clash).
        return view->kind == View::Kind::Xdg ? view->toplevel->app_id : view->xwl->class_;
    }

    const char* view_title(View* view) {
        return view->kind == View::Kind::Xdg ? view->toplevel->title : view->xwl->title;
    }

    const char* view_tag(View* view) {
        return view->kind == View::Kind::Xdg ? toplevel_props::tag_of(view->toplevel) : "";
    }

    const char* view_icon(View* view) {
        return view->kind == View::Kind::Xdg ? toplevel_props::icon_of(view->toplevel) : "";
    }

    void view_min_size(const View* view, int& w, int& h) {
        // Client's minimum content size (window-geometry units, CSD excluded); 0 = no minimum.
        // X11 hints are optional (size_hints may be null before the client sets WM_NORMAL_HINTS,
        // and the min/max fields are only meaningful when their flag bit is set).
        if (view->kind == View::Kind::Xdg) {
            w = view->toplevel->current.min_width;
            h = view->toplevel->current.min_height;
        } else if (view->xwl->size_hints && (view->xwl->size_hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)) {
            w = view->xwl->size_hints->min_width;
            h = view->xwl->size_hints->min_height;
        } else {
            w = h = 0;
        }
        w = std::max(0, w);
        h = std::max(0, h);
    }

    void view_max_size(const View* view, int& w, int& h) {
        if (view->kind == View::Kind::Xdg) {
            w = view->toplevel->current.max_width;
            h = view->toplevel->current.max_height;
        } else if (view->xwl->size_hints && (view->xwl->size_hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)) {
            w = view->xwl->size_hints->max_width;
            h = view->xwl->size_hints->max_height;
        } else {
            w = h = 0;
        }
        w = std::max(0, w);
        h = std::max(0, h);
    }

    void view_set_activated(View* view, bool activated) {
        if (view->kind == View::Kind::Xdg)
            wlr_xdg_toplevel_set_activated(view->toplevel, activated);
        else
            wlr_xwayland_surface_activate(view->xwl, activated);
    }

    void view_set_fullscreen(View* view, bool on) {
        if (view->kind == View::Kind::Xdg)
            wlr_xdg_toplevel_set_fullscreen(view->toplevel, on);
        else
            wlr_xwayland_surface_set_fullscreen(view->xwl, on);
    }

    void view_close(View* view) {
        if (view->kind == View::Kind::Xdg)
            wlr_xdg_toplevel_send_close(view->toplevel);
        else
            wlr_xwayland_surface_close(view->xwl);
    }

    void view_configure(View* view) {
        const int bw = view->fullscreen ? 0 : view->server->config.border_width;
        int cw = std::max(1, view->box.width - 2 * bw);
        int ch = std::max(1, view->box.height - 2 * bw);
        if (!view->fullscreen) {
            int min_w, min_h, max_w, max_h;
            view_min_size(view, min_w, min_h);
            view_max_size(view, max_w, max_h);
            cw = tiling::clamp_size(cw, min_w, max_w);
            ch = tiling::clamp_size(ch, min_h, max_h);
        }
        // a genuinely new request
        if (cw != view->req_w || ch != view->req_h) {
            view->req_w = cw;
            view->req_h = ch;
            view->acked = false;
        }
        if (view->kind == View::Kind::Xdg) {
            wlr_xdg_toplevel_set_size(view->toplevel, cw, ch);
        } else {
            // X clients position themselves in absolute layout coords, so a bare size isn't
            // enough — send the on-screen origin (tile corner, inside the border) too.
            // ponytail: fires an X ConfigureNotify per arrange; wlroots dedupes unchanged
            // geometry, so this stays quiet unless the tile actually moved.
            wlr_xwayland_surface_configure(view->xwl, view->box.x + bw, view->box.y + bw, (uint16_t)cw, (uint16_t)ch);
        }
    }

    void focus_surface(Server& server, wlr_surface* surface) {
        if (wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat))
            wlr_seat_keyboard_notify_enter(server.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }

    // Move a view to the tail of the list, which is the top of the stacking/cycle order.
    void raise_to_tail(Server& server, View* v) {
        server.views.remove(v);
        server.views.push_back(v);
    }

    // xdg-dialog-v1: the mapped modal dialog standing in front of `view`, or `view` itself.
    View* modal_front(Server& server, View* view) {
        for (int hop = 0; hop < 8; hop++) {
            View* modal = nullptr;
            for (View* v : server.views) {
                if (!v->mapped || v == view || v->kind != View::Kind::Xdg)
                    continue;
                if (v->toplevel->parent != view->toplevel || !view_visible(server, v))
                    continue;
                const wlr_xdg_dialog_v1* d = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(v->toplevel);
                if (d && d->modal)
                    modal = v; // later views are higher in the stack; take the topmost
            }
            if (!modal)
                return view;
            view = modal;
        }
        return view;
    }

    void focus_view(Server& server, View* view) {
        // While locked, keyboard focus belongs to the lock surface; a window mapping or a
        // click underneath must not steal it. focused_view is left as-is so it's restored
        // on unlock (on_unlock in lock.cpp).
        if (!view || server.locked)
            return;

        if (view->kind == View::Kind::Xdg)
            view = modal_front(server, view);

        // Above the early-return below on purpose
        view->urgent = false;

        if (server.focused_view == view) {
            // Already the focused window, but the seat's keyboard focus can have been grabbed
            // away by a keyboard-interactive layer surface (e.g. a quickshell launcher) while
            // this stayed focused_view. Re-assert it so a click / cycle / workspace-return
            // reclaims the keyboard instead of no-opping and stranding input.
            focus_surface(server, view_surface(view));
            return;
        }

        // Any focus change that isn't a directional jump invalidates the round-trip target
        // (focus_direction re-sets it right after this call). Stops a stale reverse-jump.
        server.nav_return = nullptr;

        View* prev = server.focused_view;
        if (prev) {
            view_set_activated(prev, false);
            prev->focused = false;
            if (prev->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign_handle, false);
        }

        // Floating windows live in their own scene tree (always above tiles). Raise the
        // focused float above the *other* floats so it's on top while in use. Tiled windows
        // keep their list order (raising them would scramble cycle_focus).
        if (view->floating) {
            raise_to_tail(server, view);
            if (view->scene_tree)
                wlr_scene_node_raise_to_top(&view->scene_tree->node);
        }

        server.focused_view = view;
        server.workspaces[view->workspace].last_focused = view; // remembered for workspace return
        view->focused = true;
        view_set_activated(view, true);
        if (view->foreign_handle)
            wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_handle, true);

        // Repaint both borders with their new active/inactive colors.
        place_view_nodes(view);
        if (prev)
            place_view_nodes(prev);

        focus_surface(server, view_surface(view));
        ipc::publish(server);
    }

    void clear_focus(Server& server) {
        if (server.focused_view) {
            View* prev = server.focused_view;
            view_set_activated(prev, false);
            prev->focused = false;
            if (prev->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign_handle, false);
            server.focused_view = nullptr;
            place_view_nodes(prev); // repaint its border inactive
        }
        wlr_seat_keyboard_notify_clear_focus(server.seat);
        ipc::publish(server);
    }

    bool mark_urgent(Server& server, wlr_surface* surface, bool hidden_only) {
        for (View* v : server.views) {
            if (view_surface(v) != surface)
                continue;
            if (v == server.focused_view || (hidden_only && view_visible(server, v)))
                return false;
            v->urgent = true;
            ipc::publish(server);
            return true;
        }
        return false;
    }

    void set_fullscreen(Server& server, View* view, bool on) {
        if (!view || view->fullscreen == on)
            return;
        if (on)
            view->saved_box = view->box;
        else if (view->floating)
            // arrange() re-sizes tiles from their tree slot but deliberately never touches a
            // float's box (that's what preserves free move/resize), so a float's pre-fullscreen
            // geometry has to be put back here, once.
            view->box = view->saved_box;
        view->fullscreen = on; // before view_configure: it insets by the border only when not fullscreen
        view_set_fullscreen(view, on);
        if (!on && view->floating)
            view_configure(view); // tell the restored float its geometry (fullscreen flag now cleared)
        // Fullscreen views sit above the top layer (below the overlay/lock); restore to the
        // tile/float tree when cleared. arrange() re-lays out the box + border.
        restack_view(server, view);
        tiling::arrange(server);
    }

    void toggle_fullscreen(Server& server) {
        if (server.focused_view)
            set_fullscreen(server, server.focused_view, !server.focused_view->fullscreen);
    }

    // The area a free window may use on its output: the usable area (minus bars), falling
    // back to the full output box exactly as tiling::arrange does. Empty if homeless.
    static wlr_box view_area(Server& server, View* view) {
        const output::Area a = output::usable(server, view_output(server, view));
        return {a.x, a.y, a.width, a.height};
    }

    void set_floating(Server& server, View* v, bool on) {
        if (!v || v->floating == on)
            return;
        v->floating = on;
        set_tiled(v, !v->floating); // floating -> normal (own size + shadow); tiled -> honor ours
        if (v->floating) {
            // Leave the tree (its slot is reclaimed by the sibling). Move to the list tail so it
            // draws above the other floats (v is already focused, so focus_view would no-op —
            // splice directly).
            tiling::remove(server, v);
            raise_to_tail(server, v);
            const View::Box old = v->box;
            const wlr_box a = view_area(server, v);
            if (a.width > 0) {
                if (v->float_box.width > 0)
                    v->box = v->float_box;
                else
                    v->box = {0, 0, a.width * 7 / 10, a.height * 7 / 10};
                // Never below the client's minimum, never bigger than the screen.
                int min_w = 0, min_h = 0;
                view_min_size(v, min_w, min_h);
                const int bw = server.config.border_width;
                v->box.width = std::clamp(v->box.width, min_w + 2 * bw, std::max(min_w + 2 * bw, a.width));
                v->box.height = std::clamp(v->box.height, min_h + 2 * bw, std::max(min_h + 2 * bw, a.height));
                if (v->float_box.width > 0) {
                    v->box.x = std::clamp(v->box.x, a.x, std::max(a.x, a.x + a.width - v->box.width));
                    v->box.y = std::clamp(v->box.y, a.y, std::max(a.y, a.y + a.height - v->box.height));
                } else {
                    center_view(server, v);
                }
                v->float_self_sized = false;
                if (server.config.animation_ms > 0 && old.width > 0) {
                    v->anim_ox += old.x - v->box.x;
                    v->anim_oy += old.y - v->box.y;
                }
                view_configure(v);
            }
        } else {
            v->float_box = v->box;              // re-floating returns here
            v->pinned = false;                  // a tiled window can't be pinned
            tiling::insert(server, v, nullptr); // re-tile at the spiral tail
        }
        restack_view(server, v);
        tiling::arrange(server);
    }

    void toggle_floating(Server& server) {
        if (View* v = server.focused_view)
            set_floating(server, v, !v->floating);
    }

    void toggle_pin(Server& server) {
        View* v = server.focused_view;
        if (!v || !v->floating) // pin is floating-only
            return;
        v->pinned = !v->pinned;
    }

    bool apply_window_rules(Server& server, View* view) {
        // Auto-float toplevels the client places itself: transients/dialogs, and anything
        // pinned to one size (it would only refuse its tile anyway).
        bool self_placed = false;
        if (view->kind == View::Kind::Xdg) {
            const wlr_xdg_toplevel_state& st = view->toplevel->current;
            self_placed = auto_float(st.min_width, st.max_width, st.min_height, st.max_height, view->toplevel->parent);
        } else {
            // X11 says the same thing through WM_TRANSIENT_FOR, _NET_WM_STATE_MODAL and the _NET_WM_WINDOW_TYPE atoms
            static constexpr wlr_xwayland_net_wm_window_type kFloatTypes[] = {
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
                WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
            };
            self_placed = view->xwl->modal || view->xwl->parent;
            for (wlr_xwayland_net_wm_window_type t : kFloatTypes)
                self_placed = self_placed || wlr_xwayland_surface_has_window_type(view->xwl, t);
            if (!self_placed) {
                int min_w, min_h, max_w, max_h;
                view_min_size(view, min_w, min_h);
                view_max_size(view, max_w, max_h);
                self_placed = auto_float(min_w, max_w, min_h, max_h, false);
            }
        }
        if (self_placed) {
            view->floating = true;
            view->want_center = true; // center on output like a rule-floated window
        }
        // The matching itself is pure and lives in config.cpp, where it can be unit-tested
        // without a compositor (see test_config.cpp).
        const RuleResult r =
            match_rules(server.config.window_rules, view_app_id(view), view_title(view), view_tag(view));
        if (r.floating)
            view->floating = true;
        if (r.center)
            view->want_center = true;
        return r.no_focus;
    }

    void center_view(Server& server, View* view) {
        const wlr_box a = view_area(server, view);
        if (a.width <= 0)
            return;
        view->box.x =
            std::clamp(a.x + (a.width - view->box.width) / 2, a.x, std::max(a.x, a.x + a.width - view->box.width));
        view->box.y =
            std::clamp(a.y + (a.height - view->box.height) / 2, a.y, std::max(a.y, a.y + a.height - view->box.height));
        place_view_nodes(view);
    }

    void focus_direction(Server& server, int dx, int dy) {
        View* cur = server.focused_view;
        if (!cur)
            return;
        auto cx = [](View* v) { return v->box.x + v->box.width / 2; };
        auto cy = [](View* v) { return v->box.y + v->box.height / 2; };
        View* best = nullptr;
        // Exact reverse of the last directional jump? Return to where we came from (if it's
        // still visible and still in the requested direction) rather than the geometric pick,
        // so an asymmetric split round-trips instead of landing on a different tile.
        if (server.nav_return && dx == -server.nav_dx && dy == -server.nav_dy &&
            view_visible(server, server.nav_return)) {
            const long ddx = cx(server.nav_return) - cx(cur), ddy = cy(server.nav_return) - cy(cur);
            if (ddx * dx + ddy * dy > 0)
                best = server.nav_return;
        }
        if (!best) {
            long best_score = 0;
            for (View* v : server.views) {
                if (v == cur || !view_visible(server, v))
                    continue;
                // Stay on your own axis: a vertical move only considers windows that overlap
                // the current one horizontally (its own column), a horizontal move only those
                // that overlap vertically.
                const bool overlap =
                    dx ? v->box.y < cur->box.y + cur->box.height && cur->box.y < v->box.y + v->box.height
                       : v->box.x < cur->box.x + cur->box.width && cur->box.x < v->box.x + v->box.width;
                if (!overlap)
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
        }
        if (best) {
            focus_view(server, best); // clears nav_return
            server.nav_return = cur;  // remember where we left from, for the reverse move
            server.nav_dx = dx;
            server.nav_dy = dy;
        }
    }

    bool view_visible(const Server& server, const View* view) {
        if (!view->mapped)
            return false;
        const Workspace& ws = server.workspaces[view->workspace];
        // Shown only if its workspace lives on an output AND is the one that output displays.
        return ws.output && ws.output->active_ws == view->workspace;
    }

    output::Output* view_output(const Server& server, const View* view) {
        return server.workspaces[view->workspace].output;
    }

    void view_update_output(Server& server, View* view) {
        if (!view->mapped)
            return;
        output::Output* o = view_output(server, view);
        wlr_surface* surface = view_surface(view);

        // Enter/leave is a paired protocol: announce a change of screen, not the current screen.
        if (o != view->announced_output) {
            if (output::Output* prev = view->announced_output) {
                wlr_surface_send_leave(surface, prev->handle);
                if (view->foreign_handle)
                    wlr_foreign_toplevel_handle_v1_output_leave(view->foreign_handle, prev->handle);
            }
            view->announced_output = o;
            if (o) {
                wlr_surface_send_enter(surface, o->handle);
                if (view->foreign_handle)
                    wlr_foreign_toplevel_handle_v1_output_enter(view->foreign_handle, o->handle);
            }
        }

        if (o)
            wlr_fractional_scale_v1_notify_scale(surface, output::scale_of(o));
    }

    void focus_topmost_visible(Server& server) {
        if (View* v = topmost_visible(server))
            focus_view(server, v);
        else
            clear_focus(server);
    }

    // (Re)apply per-window content effects (opacity + corner radius) to the view's surface
    // buffers. Must run from the frame handler, right before rendering: scenefx re-syncs the
    // surface buffer during its own commit handling (which runs after ours), resetting opacity
    // to 1.0, so a value set at commit time never survives to the render.
    void apply_view_effects(View* view) {
        if (view->surface_tree)
            wlr_scene_node_for_each_buffer(&view->surface_tree->node, apply_fx, view);
    }

    void view_adopt_float_size(View* view) {
        // A floating window sizes itself (we un-tile it, so GTK/Gecko restore their own
        // natural size + CSD margins and never honor a configure). Track the committed
        // size so the border/shadow/clip tighten onto the real content instead of leaving
        // a band of the desktop behind the float showing through. Tiled/fullscreen boxes
        // stay compositor-authoritative; skip while this view is under an interactive grab
        // so a lagging commit can't fight the cursor mid-resize. xdg reports a window
        // geometry (CSD margin excluded); X11 has none, so use the raw surface size.
        const bool xdg = view->kind == View::Kind::Xdg;
        const wlr_box geo = xdg ? view->toplevel->base->geometry
                                : wlr_box{0, 0, view->xwl->surface->current.width, view->xwl->surface->current.height};
        if (view->floating && !view->fullscreen && geo.width > 0 && geo.height > 0 && cursor::grabbed_view() != view) {
            Server& server = *view->server;
            const int bw = server.config.border_width;
            view->box.width = geo.width + 2 * bw;
            view->box.height = geo.height + 2 * bw;
            // client that asks for more than the screen holds would be too big
            const wlr_box a = view_area(server, view);
            if (a.width > 0 && (view->box.width > a.width || view->box.height > a.height)) {
                int min_w, min_h, max_w, max_h;
                view_min_size(view, min_w, min_h);
                view_max_size(view, max_w, max_h);
                const int fit_w = tiling::clamp_size(std::min(geo.width, a.width - 2 * bw), min_w, max_w);
                const int fit_h = tiling::clamp_size(std::min(geo.height, a.height - 2 * bw), min_h, max_h);
                if (fit_w < geo.width || fit_h < geo.height) {
                    view->box.width = fit_w + 2 * bw;
                    view->box.height = fit_h + 2 * bw;
                    view->box.x = std::clamp(view->box.x, a.x, std::max(a.x, a.x + a.width - view->box.width));
                    view->box.y = std::clamp(view->box.y, a.y, std::max(a.y, a.y + a.height - view->box.height));
                    view->float_self_sized = false;
                    view_configure(view);
                }
            }
            // A window-rule center can only run once the float has its real size (now).
            if (view->want_center) {
                center_view(*view->server, view);
                view->want_center = false;
            }
        }
        // Re-sync the scene nodes so the inset stays correct. No-op until the nodes exist.
        place_view_nodes(view);
    }

    // Unreference the gradient texture from this view's edge bands.
    static void release_gradient(View* view) {
        if (view->grad_gen == 0)
            return;
        for (wlr_scene_buffer* edge : view->grad_edge)
            wlr_scene_buffer_set_buffer(edge, nullptr);
        view->grad_gen = 0;
    }

    // has the client answered the last size we asked it for
    static bool view_settled(const View* view) {
        if (view->kind == View::Kind::Xdg)
            return view->toplevel->base->current.configure_serial == view->toplevel->base->scheduled_serial;
        return view->acked;
    }

    void place_view_nodes(View* view) {
        if (!view->scene_tree)
            return; // not mapped yet
        Server& server = *view->server;
        view->frame = view->box; // never leave it zeroed; the real value is computed below
        const bool vis = view_visible(server, view);
        wlr_scene_node_set_enabled(&view->scene_tree->node, vis);
        if (!vis) {
            release_gradient(view);
            return;
        }

        const int bw = view->fullscreen ? 0 : server.config.border_width;

        // The tile, plus the (decaying) slide-animation offset. Only the position animates:
        // size is applied straight from the box (a size animation means configuring or
        // stretching the client, both of which cost more than they're worth).
        const View::Box tile = {view->box.x + (int)std::lround(view->anim_ox),
                                view->box.y + (int)std::lround(view->anim_oy),
                                view->box.width,
                                view->box.height};

        // xdg reports a window geometry whose origin is the CSD content corner (shadow margin
        // excluded). X11 has no geometry — its buffer is the window, so it starts at 0,0.
        const wlr_box geo = view->kind == View::Kind::Xdg
                                ? view->toplevel->base->geometry
                                : wlr_box{0, 0, view->xwl->surface->current.width, view->xwl->surface->current.height};

        View::Box box = tile;
        if (!view->fullscreen && view_settled(view) && cursor::grabbed_view() != view) {
            const tiling::Rect f =
                tiling::fit_content({tile.x, tile.y, tile.width, tile.height}, geo.width, geo.height, bw);
            box = {f.x, f.y, f.w, f.h};
        }
        view->frame = box; // popup unconstraining reads this back (see server.cpp)
        wlr_scene_node_set_position(&view->scene_tree->node, box.x, box.y);

        // Inset the client by the border. wlr_scene_xdg_surface_create already makes the
        // subtree origin the window-geometry top-left (CSD shadow margin handled internally),
        // so we position it at the inner corner directly — no geometry offset here.
        wlr_scene_node_set_position(&view->surface_tree->node, bw, bw);
        // Popups position themselves against the window-geometry origin, which is exactly where surface_tree sits
        if (view->popup_tree)
            wlr_scene_node_set_position(&view->popup_tree->node, bw, bw);

        // Crop the client to its window geometry so CSD shadow margins (Firefox/GTK/
        // Chromium ship a buffer bigger than the geometry) don't draw over the border
        // band, otherwise the border survives only as corner slivers. Anchored at the
        // geometry origin and sized to the frame's inner area — which, since the frame
        // already hugs the geometry, never runs past what the client drew. A client that
        // declares a geometry it isn't actually drawing to slices its own content here.
        // Fullscreen wants the whole buffer (and no clip, to keep direct scanout eligible).
        if (view->fullscreen) {
            wlr_scene_subsurface_tree_set_clip(&view->surface_tree->node, nullptr);
        } else {
            wlr_box clip = {geo.x, geo.y, std::max(1, box.width - 2 * bw), std::max(1, box.height - 2 * bw)};
            wlr_scene_subsurface_tree_set_clip(&view->surface_tree->node, &clip);
        }

        const struct clipped_region hole = {
            .area = {bw, bw, box.width - 2 * bw, box.height - 2 * bw},
            .corners = corner_radii_all(std::max(0, server.config.rounding - bw)),
        };

        // The gradient ring replaces the flat rect rather than tinting it, so border_active and
        // border_gradient read as the two ends of one border instead of stacking alphas.
        const bool show_border = bw > 0;
        const bool grad = show_border && server.config.border_gradient != 0 && view == server.focused_view;
        const int W = box.width, H = box.height;

        wlr_scene_node_set_enabled(&view->border->node, show_border && !grad);
        if (show_border && !grad) {
            wlr_scene_rect_set_size(view->border, W, H);
            // Round the border frame to match the content so it nests instead of poking
            // square corners past the client's rounding.
            wlr_scene_rect_set_corner_radius(view->border, server.config.rounding);
            wlr_scene_rect_set_clipped_region(view->border, hole);
            float col[4];
            u32_color(view == server.focused_view ? server.config.border_active : server.config.border_inactive, col);
            wlr_scene_rect_set_color(view->border, col);
        }

        const int r = server.config.rounding;
        const int c = std::min(std::max(r, bw), std::min(W, H) / 2);
        const int span_w = W - 2 * c, span_h = H - 2 * c;

        for (int i = 0; i < 4; i++) {
            wlr_scene_node_set_enabled(&view->grad_corner[i]->node, grad);
            wlr_scene_node_set_enabled(&view->grad_edge[i]->node, grad && (i % 2 == 0 ? span_w : span_h) > 0);
        }

        if (grad) {
            // corner arcs
            const wlr_box at[4] = {{0, 0, c, c}, {W - c, 0, c, c}, {W - c, H - c, c, c}, {0, H - c, c, c}};
            const fx_corner_radii round[4] = {corner_radii_new(r, 0, 0, 0),
                                              corner_radii_new(0, r, 0, 0),
                                              corner_radii_new(0, 0, r, 0),
                                              corner_radii_new(0, 0, 0, r)};
            const uint32_t mid = u32_mix(server.config.border_active, server.config.border_gradient);
            const uint32_t corner_rgba[4] = {server.config.border_active, mid, server.config.border_gradient, mid};

            for (int i = 0; i < 4; i++) {
                wlr_scene_rect* n = view->grad_corner[i];
                wlr_scene_node_set_position(&n->node, at[i].x, at[i].y);
                wlr_scene_rect_set_size(n, at[i].width, at[i].height);
                wlr_scene_rect_set_corner_radii(n, round[i]);
                wlr_scene_rect_set_clipped_region(
                    n,
                    {.area = {hole.area.x - at[i].x, hole.area.y - at[i].y, hole.area.width, hole.area.height},
                     .corners = hole.corners});
                float col[4];
                u32_color(corner_rgba[i], col);
                wlr_scene_rect_set_color(n, col);
            }

            uint32_t gen = 0;
            wlr_buffer* tex = gradient_texture(server.config, &gen);
            const wlr_box band[4] = {
                {c, 0, span_w, bw}, {W - bw, c, bw, span_h}, {c, H - bw, span_w, bw}, {0, c, bw, span_h}};

            const bool reupload = view->grad_gen != gen;
            for (int i = 0; i < 4; i++) {
                wlr_scene_buffer* n = view->grad_edge[i];
                // Re-upload every band, including currently-empty ones.
                if (reupload) {
                    // HACK: the node caches the built texture and handing it a different buffer does
                    // not invalidate that cache. removing the nullptr makes the bands keep drawing the
                    // old colors after a config reload.
                    wlr_scene_buffer_set_buffer(n, nullptr);
                    wlr_scene_buffer_set_buffer(n, tex);
                }
                if (band[i].width <= 0 || band[i].height <= 0)
                    continue;
                wlr_scene_node_set_position(&n->node, band[i].x, band[i].y);
                wlr_scene_buffer_set_dest_size(n, band[i].width, band[i].height);
                const wlr_fbox src = grad_src(band[i].x, band[i].y, band[i].width, band[i].height, W, H);
                wlr_scene_buffer_set_source_box(n, &src);
            }
            view->grad_gen = gen;
        } else {
            release_gradient(view); // gradient off, or this view lost focus
        }

        // glow
        const bool glow = server.config.shadow && !view->fullscreen && view == server.focused_view;
        wlr_scene_node_set_enabled(&view->shadow->node, glow);
        if (glow) {
            const uint32_t glow_rgba = server.config.border_gradient
                                           ? u32_mix(server.config.border_active, server.config.border_gradient)
                                           : server.config.border_active;
            float scol[4];
            u32_color(glow_rgba, scol);
            scol[3] = (server.config.shadow_color & 0xff) / 255.0f;
            wlr_scene_shadow_set_color(view->shadow, scol);
            wlr_scene_shadow_set_blur_sigma(view->shadow, (float)server.config.shadow_blur);
            wlr_scene_shadow_set_size(view->shadow, box.width, box.height);
            wlr_scene_shadow_set_corner_radius(view->shadow, server.config.rounding);
            wlr_scene_shadow_set_clipped_region(view->shadow, hole);
        }
    }

    void set_workspace(Server& server, int n) {
        n = std::clamp(n, 0, server.config.workspaces - 1);
        Workspace& ws = server.workspaces[n];

        // Homeless (no output has ever shown it, or every screen went away): pull it onto the
        // output we're looking at.
        if (!ws.output)
            ws.output = output::focused(server);
        if (!ws.output)
            return; // no outputs at all; nothing to show it on

        output::Output* o = ws.output;
        if (o->active_ws == n) {
            // Already shown. If it's on another screen, this is still a focus request — fall
            // through to move focus there rather than no-op.
            if (server.focused_view && view_output(server, server.focused_view) == o)
                return;
        }

        // The workspace that output was showing steps aside; this one takes its place.
        o->active_ws = n;

        // Pinned floats follow the output to whatever workspace it now shows.
        for (View* v : server.views)
            if (v->pinned && view_output(server, v) == o)
                v->workspace = n;
        tiling::arrange(server, false); // no slide: a workspace switch appears in place

        // Focus follows the workspace to its output (sway semantics). Warp the cursor when
        // focus crosses screens, or the pointer is left behind on the old one.
        // Prefer the view that was focused when we last left this workspace.
        View* target = ws.last_focused;
        if (!target || !view_visible(server, target) || view_output(server, target) != o) {
            target = nullptr;
            for (auto it = server.views.rbegin(); it != server.views.rend(); ++it)
                if (view_visible(server, *it) && view_output(server, *it) == o) {
                    target = *it;
                    break;
                }
        }
        if (target)
            focus_view(server, target);
        else
            clear_focus(server);
        cursor::warp_to_output(server, o);
        ipc::publish(server); // the shown workspace changed even if focus didn't
    }

    void move_focused_to_workspace(Server& server, int n) {
        n = std::clamp(n, 0, server.config.workspaces - 1);
        View* v = server.focused_view;
        if (!v || v->workspace == n)
            return;
        if (v->pinned) // a pinned float belongs to all workspaces of its output; don't strand it
            return;
        // A floating view isn't in any tree (floating <=> not tiled); only tiled views move
        // between workspace trees, else insert would leave a phantom leaf in the destination.
        if (!v->floating)
            tiling::remove(server, v);
        v->workspace = n; // may be on another output's workspace; we stay put
        if (!v->floating)
            tiling::insert(server, v, nullptr); // append to the target workspace's tree
        // The target workspace may be homeless (no output showing it); give it one so a window
        // sent there isn't stranded invisibly.
        if (!server.workspaces[n].output)
            server.workspaces[n].output = output::focused(server);
        view_update_output(server, v); // it may have just crossed to another screen
        tiling::arrange(server);
        focus_topmost_visible(server);
        ipc::publish(server); // occupancy of workspace n changed
    }

    View::View(Server& server, wlr_xdg_toplevel* toplevel) : server(&server), toplevel(toplevel) {
        wlr_surface* surface = toplevel->base->surface;

        add_listener(map, surface->events.map, view_handle_map);
        add_listener(unmap, surface->events.unmap, view_handle_unmap);
        add_listener(commit, surface->events.commit, view_handle_commit);
        add_listener(destroy, toplevel->events.destroy, view_handle_destroy);
        add_listener(set_title, toplevel->events.set_title, view_handle_set_title);
        add_listener(set_app_id, toplevel->events.set_app_id, view_handle_set_app_id);
        add_listener(request_fullscreen, toplevel->events.request_fullscreen, view_handle_request_fullscreen);
    }

    View::View(Server& server, wlr_xwayland_surface* xwl) : server(&server), kind(Kind::Xwl), xwl(xwl) {
        // map/unmap/commit are wired on the wlr_surface at `associate` (it doesn't exist yet).
        add_listener(associate, xwl->events.associate, view_handle_associate);
        add_listener(dissociate, xwl->events.dissociate, view_handle_dissociate);
        add_listener(destroy, xwl->events.destroy, view_xwl_handle_destroy);
        add_listener(set_title, xwl->events.set_title, view_handle_set_title);
        // Listener and signal are spelled differently here: X11's WM_CLASS is our app_id.
        add_listener(set_app_id, xwl->events.set_class, view_handle_set_app_id);
        add_listener(request_fullscreen, xwl->events.request_fullscreen, view_handle_request_fullscreen);
        add_listener(request_configure, xwl->events.request_configure, view_handle_request_configure);
        add_listener(request_activate, xwl->events.request_activate, view_handle_request_activate);
    }

} // namespace fenriz
