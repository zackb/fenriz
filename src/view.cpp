#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "cursor.hpp"
#include "ipc.hpp"
#include "server.hpp"
#include "tiling.hpp"
#include "wlr.hpp"

namespace fenriz {

    namespace {

        // Unpack a 0xRRGGBBAA border color into wlr_scene_rect's float[4] (matches the old
        // manual renderer's color_from_u32).
        void u32_color(uint32_t c, float out[4]) {
            out[0] = ((c >> 24) & 0xff) / 255.0f;
            out[1] = ((c >> 16) & 0xff) / 255.0f;
            out[2] = ((c >> 8) & 0xff) / 255.0f;
            out[3] = (c & 0xff) / 255.0f;
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
            wlr_scene_buffer_set_opacity(buf, v->fullscreen ? 1.0f : s.config.opacity);
        }

        // Tell a toplevel it's tiled on all edges (or none). Advertising the tiled state is
        // what makes GTK/Gecko honor the size we configure and drop their CSD shadow/rounding
        // on those edges.
        void set_tiled(View* view, bool tiled) {
            wlr_xdg_toplevel* tl = view->toplevel;
            if (wl_resource_get_version(tl->resource) >= 2) { // TILED_* states since v2
                uint32_t edges =
                    tiled ? (WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM) : WLR_EDGE_NONE;
                wlr_xdg_toplevel_set_tiled(tl, edges);
            } else {
                wlr_xdg_toplevel_set_maximized(tl, tiled);
            }
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

            // Build the scene nodes: a container tree holding the border rect (below), the
            // xdg surface subtree (inset by the border in place_view_nodes), and the popup
            // tree on top. The View* on the container lets scene hit-testing recover the
            // window; base->data lets popups find their parent scene tree (see on_new_popup
            // in server.cpp) — that's popup_tree, not surface_tree, so menus escape the
            // toplevel's clip and effects.
            view->scene_tree = wlr_scene_tree_create(view->floating ? server.scene_floating : server.scene_tiles);
            view->scene_tree->node.data = view;
            // Shadow first so it's the bottom-most child (z-order = insertion order):
            // it must spread out behind the border and surface.
            float scol[4];
            u32_color(server.config.shadow_color, scol);
            view->shadow = wlr_scene_shadow_create(
                view->scene_tree, 0, 0, server.config.rounding, (float)server.config.shadow_blur, scol);
            float col[4];
            u32_color(server.config.border_inactive, col);
            view->border = wlr_scene_rect_create(view->scene_tree, 0, 0, col);
            view->surface_tree = wlr_scene_xdg_surface_create(view->scene_tree, view->toplevel->base);
            view->popup_tree = wlr_scene_tree_create(view->scene_tree); // created last: draws above
            view->toplevel->base->data = view->popup_tree;

            // New window splits the focused window's tile (focus-aware dwindle).
            tiling::insert(server, view, server.focused_view);

            // Publish this window to the foreign-toplevel (taskbar) protocol.
            if (server.foreign_toplevel_manager) {
                view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
                if (view->toplevel->title)
                    wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
                if (view->toplevel->app_id)
                    wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
            }
            // and to its standardized successor
            if (server.ext_foreign_toplevel_list) {
                wlr_ext_foreign_toplevel_handle_v1_state state = {
                    .title = view->toplevel->title,
                    .app_id = view->toplevel->app_id,
                };
                view->ext_foreign_handle =
                    wlr_ext_foreign_toplevel_handle_v1_create(server.ext_foreign_toplevel_list, &state);
            }

            // HiDPI: announce the view's own output + that output's scale, so it renders a
            // native-resolution buffer. Also re-run whenever it migrates between screens.
            view_update_output(server, view);

            set_tiled(view, true); // new windows map tiled; folds into the arrange configure below
            tiling::arrange(server);
            focus_view(server, view);
            ipc::publish(server);
        }

        void view_handle_unmap(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, unmap);
            (void)data;
            Server& server = *view->server;
            view->mapped = false;
            if (view->scene_tree) {
                // Frees the whole subtree (border + surface + any popups).
                wlr_scene_node_destroy(&view->scene_tree->node);
                view->scene_tree = nullptr;
                view->surface_tree = nullptr;
                view->popup_tree = nullptr;
                view->border = nullptr;
                view->shadow = nullptr;
                view->toplevel->base->data = nullptr;
            }
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
            // A client can change its window geometry after mapping (CSD apps adjust their
            // shadow margin); re-sync the scene nodes so the inset stays correct. No-op
            // until the nodes exist (created on map).
            place_view_nodes(view);
        }

        // Push title+app_id to the ext-foreign-toplevel handle. Unlike the wlr protocol's
        // independent setters, update_state takes both at once
        void ext_foreign_update(View* view) {
            if (!view->ext_foreign_handle)
                return;
            wlr_ext_foreign_toplevel_handle_v1_state state = {
                .title = view->toplevel->title,
                .app_id = view->toplevel->app_id,
            };
            wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_foreign_handle, &state);
        }

        void view_handle_set_title(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_title);
            (void)data;
            if (view->foreign_handle && view->toplevel->title)
                wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
            ext_foreign_update(view);
            if (view->focused)
                ipc::publish(*view->server); // refresh activeWindow.title in the feed
        }

        void view_handle_set_app_id(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_app_id);
            (void)data;
            if (view->foreign_handle && view->toplevel->app_id)
                wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
            ext_foreign_update(view);
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

    // Move a view to the tail of the list, which is the top of the stacking/cycle order.
    void raise_to_tail(Server& server, View* v) {
        server.views.remove(v);
        server.views.push_back(v);
    }

    void focus_view(Server& server, View* view) {
        // While locked, keyboard focus belongs to the lock surface; a window mapping or a
        // click underneath must not steal it. focused_view is left as-is so it's restored
        // on unlock (on_unlock in lock.cpp).
        if (!view || server.locked)
            return;

        // Above the early-return below on purpose
        view->urgent = false;

        if (server.focused_view == view) {
            // Already the focused window, but the seat's keyboard focus can have been grabbed
            // away by a keyboard-interactive layer surface (e.g. a quickshell launcher) while
            // this stayed focused_view. Re-assert it so a click / cycle / workspace-return
            // reclaims the keyboard instead of no-opping and stranding input.
            focus_surface(server, view->toplevel->base->surface);
            return;
        }

        View* prev = server.focused_view;
        if (prev) {
            wlr_xdg_toplevel_set_activated(prev->toplevel, false);
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
        view->focused = true;
        wlr_xdg_toplevel_set_activated(view->toplevel, true);
        if (view->foreign_handle)
            wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_handle, true);

        // Repaint both borders with their new active/inactive colors.
        place_view_nodes(view);
        if (prev)
            place_view_nodes(prev);

        focus_surface(server, view->toplevel->base->surface);
        ipc::publish(server);
    }

    void clear_focus(Server& server) {
        if (server.focused_view) {
            View* prev = server.focused_view;
            wlr_xdg_toplevel_set_activated(prev->toplevel, false);
            prev->focused = false;
            if (prev->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign_handle, false);
            server.focused_view = nullptr;
            place_view_nodes(prev); // repaint its border inactive
        }
        wlr_seat_keyboard_notify_clear_focus(server.seat);
        ipc::publish(server);
    }

    void set_fullscreen(Server& server, View* view, bool on) {
        if (!view || view->fullscreen == on)
            return;
        if (on) {
            view->saved_box = view->box;
        } else if (view->floating) {
            // arrange() re-sizes tiles from their tree slot but deliberately never touches a
            // float's box (that's what preserves free move/resize), so a float's pre-fullscreen
            // geometry has to be put back and the client told about it here, once.
            view->box = view->saved_box;
            const int bw = server.config.border_width;
            wlr_xdg_toplevel_set_size(
                view->toplevel, std::max(1, view->box.width - 2 * bw), std::max(1, view->box.height - 2 * bw));
        }
        view->fullscreen = on;
        wlr_xdg_toplevel_set_fullscreen(view->toplevel, on);
        // Fullscreen views sit above the top layer (below the overlay/lock); restore to the
        // tile/float tree when cleared. arrange() re-lays out the box + border.
        restack_view(server, view);
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
        set_tiled(v, !v->floating); // floating -> normal (own size + shadow); tiled -> honor ours
        if (v->floating) {
            // Leave the tree (its slot is reclaimed by the sibling) and keep the current tile
            // box as the initial floating geometry. Move to the list tail so it draws above the
            // other floats (v is already focused, so focus_view would no-op — splice directly).
            tiling::remove(server, v);
            raise_to_tail(server, v);
        } else {
            tiling::insert(server, v, nullptr); // re-tile at the spiral tail
        }
        restack_view(server, v);
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
        output::Output* o = view_output(server, view);
        if (!o || !view->mapped)
            return;
        wlr_surface* surface = view->toplevel->base->surface;
        wlr_surface_send_enter(surface, o->handle);
        // Scale is per-output now, so a window dragged/evacuated to another screen must be
        // told the new one or it renders at the old scale (blurry or oversharp).
        wlr_fractional_scale_v1_notify_scale(surface, output::scale_of(server, o));
        if (view->foreign_handle)
            wlr_foreign_toplevel_handle_v1_output_enter(view->foreign_handle, o->handle);
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

    void place_view_nodes(View* view) {
        if (!view->scene_tree)
            return; // not mapped yet
        Server& server = *view->server;
        const bool vis = view_visible(server, view);
        wlr_scene_node_set_enabled(&view->scene_tree->node, vis);
        if (!vis)
            return;

        // Container sits at the tile origin plus the (decaying) slide-animation offset.
        const int ox = (int)std::lround(view->anim_ox);
        const int oy = (int)std::lround(view->anim_oy);
        wlr_scene_node_set_position(&view->scene_tree->node, view->box.x + ox, view->box.y + oy);

        // Inset the client by the border. wlr_scene_xdg_surface_create already makes the
        // subtree origin the window-geometry top-left (CSD shadow margin handled internally),
        // so we position it at the inner corner directly — no geometry offset here.
        const int bw = view->fullscreen ? 0 : server.config.border_width;
        wlr_scene_node_set_position(&view->surface_tree->node, bw, bw);
        // Popups position themselves against the window-geometry origin, which is exactly
        // where surface_tree sits — so popup_tree has to track it.
        wlr_scene_node_set_position(&view->popup_tree->node, bw, bw);

        // Crop the client to its window geometry so CSD shadow margins (Firefox/GTK/
        // Chromium ship a buffer bigger than the geometry) don't draw over the border
        // band, otherwise the border survives only as corner slivers. Anchor at the
        // geometry origin and cap to the inner border area so the frame always shows on
        // all sides even if the client hasn't honored our size yet. Fullscreen wants the
        // whole buffer (and no clip, to keep direct scanout eligible).
        if (view->fullscreen) {
            wlr_scene_subsurface_tree_set_clip(&view->surface_tree->node, nullptr);
        } else {
            const wlr_box& geo = view->toplevel->base->geometry;
            wlr_box clip = {
                geo.x, geo.y, std::max(1, view->box.width - 2 * bw), std::max(1, view->box.height - 2 * bw)};
            wlr_scene_subsurface_tree_set_clip(&view->surface_tree->node, &clip);
        }

        // Note: content opacity + corner radius are applied per-frame in the output frame
        // handler (apply_view_effects), not here — scenefx re-syncs the surface buffer after
        // our commit handler runs, clobbering opacity set at commit time back to 1.0.

        const bool show_border = bw > 0;
        wlr_scene_node_set_enabled(&view->border->node, show_border);
        if (show_border) {
            wlr_scene_rect_set_size(view->border, view->box.width, view->box.height);
            // Round the border frame to match the content so it nests instead of poking
            // square corners past the client's rounding.
            wlr_scene_rect_set_corner_radius(view->border, server.config.rounding);
            // Punch out the interior (where the surface sits) so the rect is only the frame:
            // a filled rect behind a translucent surface would bleed its color through the
            // window. Inner radius matches the surface's own rounding (rounding - bw) so the
            // frame is a uniform bw-wide rounded band.
            struct clipped_region hole = {
                .area = {bw, bw, view->box.width - 2 * bw, view->box.height - 2 * bw},
                .corners = corner_radii_all(std::max(0, server.config.rounding - bw)),
            };
            wlr_scene_rect_set_clipped_region(view->border, hole);
            float col[4];
            u32_color(view == server.focused_view ? server.config.border_active : server.config.border_inactive, col);
            wlr_scene_rect_set_color(view->border, col);
        }

        // Soft glow: only the focused, non-fullscreen window. Sized to the box; the blur
        // spills outside as a halo while the opaque window occludes the solid center.
        const bool glow = server.config.shadow && !view->fullscreen && view == server.focused_view;
        wlr_scene_node_set_enabled(&view->shadow->node, glow);
        if (glow) {
            wlr_scene_shadow_set_size(view->shadow, view->box.width, view->box.height);
            wlr_scene_shadow_set_corner_radius(view->shadow, server.config.rounding);
        }
    }

    void set_workspace(Server& server, int n) {
        n = std::clamp(n, 0, WS_COUNT - 1);
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
        tiling::arrange(server, false); // no slide: a workspace switch appears in place

        // Focus follows the workspace to its output (sway semantics). Warp the cursor when
        // focus crosses screens, or the pointer is left behind on the old one.
        View* target = nullptr;
        for (auto it = server.views.rbegin(); it != server.views.rend(); ++it)
            if (view_visible(server, *it) && view_output(server, *it) == o) {
                target = *it;
                break;
            }
        if (target)
            focus_view(server, target);
        else
            clear_focus(server);
        cursor::warp_to_output(server, o);
        ipc::publish(server); // the shown workspace changed even if focus didn't
    }

    void move_focused_to_workspace(Server& server, int n) {
        n = std::clamp(n, 0, WS_COUNT - 1);
        View* v = server.focused_view;
        if (!v || v->workspace == n)
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
