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
        // global opacity. Iterated over the xdg surface subtree so every buffer matches.
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
            wlr_scene_buffer_set_opacity(buf, s.config.opacity);
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
            view->workspace = server.active_workspace;
            server.views.push_back(view);

            // Build the scene nodes: a container tree holding the border rect (below) and
            // the xdg surface subtree (inset by the border in place_view_nodes). The View*
            // on the container lets scene hit-testing recover the window; base->data lets
            // popups find their parent scene tree (see on_new_popup in server.cpp).
            view->scene_tree = wlr_scene_tree_create(view->floating ? server.scene_floating : server.scene_tiles);
            view->scene_tree->node.data = view;
            float col[4];
            u32_color(server.config.border_inactive, col);
            view->border = wlr_scene_rect_create(view->scene_tree, 0, 0, col);
            view->surface_tree = wlr_scene_xdg_surface_create(view->scene_tree, view->toplevel->base);
            view->toplevel->base->data = view->surface_tree;

            // New window splits the focused window's tile (focus-aware dwindle).
            tiling::insert(server, view, server.focused_view);

            // HiDPI: put the surface on the output and tell it our (possibly fractional)
            // scale so it renders a native-resolution buffer instead of a 1x one we'd blur.
            wlr_surface* surface = view->toplevel->base->surface;
            if (server.output)
                wlr_surface_send_enter(surface, server.output);
            wlr_fractional_scale_v1_notify_scale(surface, server.config.scale);

            // Publish this window to the foreign-toplevel (taskbar) protocol.
            if (server.foreign_toplevel_manager) {
                view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
                if (view->toplevel->title)
                    wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
                if (view->toplevel->app_id)
                    wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
                if (server.output)
                    wlr_foreign_toplevel_handle_v1_output_enter(view->foreign_handle, server.output);
            }

            tiling::arrange(server);
            focus_view(server, view);
            view->needs_initial_arrange = true; // re-issue size once the client is up (gecko)
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
                view->border = nullptr;
                view->toplevel->base->data = nullptr;
            }
            cursor::forget_view(view); // drop any in-flight mouse grab before the view is gone
            server.views.remove(view);
            tiling::remove(server, view); // sibling reclaims the freed tile
            if (view->foreign_handle) {
                wlr_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
                view->foreign_handle = nullptr;
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
            // One-shot after map: Gecko ignores the size configured at map time and only honors a later one
            if (view->needs_initial_arrange && view->mapped && !view->toplevel->base->initial_commit) {
                view->needs_initial_arrange = false;
                tiling::arrange(*view->server);
                return;
            }
            // A client can change its window geometry after mapping (CSD apps adjust their
            // shadow margin); re-sync the scene nodes so the inset stays correct. No-op
            // until the nodes exist (created on map).
            place_view_nodes(view);
        }

        void view_handle_set_title(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_title);
            (void)data;
            if (view->foreign_handle && view->toplevel->title)
                wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, view->toplevel->title);
            if (view->focused)
                ipc::publish(*view->server); // refresh activeWindow.title in the feed
        }

        void view_handle_set_app_id(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, set_app_id);
            (void)data;
            if (view->foreign_handle && view->toplevel->app_id)
                wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, view->toplevel->app_id);
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

    void focus_view(Server& server, View* view) {
        // While locked, keyboard focus belongs to the lock surface; a window mapping or a
        // click underneath must not steal it. focused_view is left as-is so it's restored
        // on unlock (on_unlock in lock.cpp).
        if (!view || server.locked)
            return;

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
            server.views.remove(view);
            server.views.push_back(view);
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
        view->fullscreen = on;
        wlr_xdg_toplevel_set_fullscreen(view->toplevel, on);
        // Fullscreen views float above the top layer (below the overlay/lock); restore to
        // the normal tile tree when cleared. arrange() re-lays out the box + border.
        if (view->scene_tree)
            wlr_scene_node_reparent(&view->scene_tree->node,
                                    on ? server.scene_fullscreen
                                       : (view->floating ? server.scene_floating : server.scene_tiles));
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
        if (v->floating) {
            // Leave the tree (its slot is reclaimed by the sibling) and keep the current tile
            // box as the initial floating geometry. Reparent into the floating scene tree so it
            // sits above the tiles structurally (v is already focused, so focus_view would
            // no-op — splice the list directly).
            tiling::remove(server, v);
            server.views.remove(v);
            server.views.push_back(v);
            if (v->scene_tree)
                wlr_scene_node_reparent(&v->scene_tree->node, server.scene_floating);
        } else {
            if (v->scene_tree)
                wlr_scene_node_reparent(&v->scene_tree->node, server.scene_tiles);
            tiling::insert(server, v, nullptr); // re-tile at the spiral tail
        }
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
        return view->mapped && view->workspace == server.active_workspace;
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
    }

    void set_workspace(Server& server, int n) {
        n = std::clamp(n, 0, 9);
        if (n == server.active_workspace)
            return;
        server.active_workspace = n;
        tiling::arrange(server);
        // The now-visible workspace's views were arranged from stale (last-seen) boxes;
        // clear any resulting offset so they appear in place instead of sliding in.
        for (View* v : server.views)
            if (view_visible(server, v))
                v->anim_ox = v->anim_oy = 0;
        if (View* v = topmost_visible(server))
            focus_view(server, v);
        else
            clear_focus(server);
        ipc::publish(server); // active workspace changed even if focus didn't
    }

    void move_focused_to_workspace(Server& server, int n) {
        n = std::clamp(n, 0, 9);
        View* v = server.focused_view;
        if (!v || v->workspace == n)
            return;
        tiling::remove(server, v);
        v->workspace = n;                   // now on another workspace -> hidden; we stay on the current one
        tiling::insert(server, v, nullptr); // append to the target workspace's tree
        tiling::arrange(server);
        if (View* next = topmost_visible(server))
            focus_view(server, next);
        else
            clear_focus(server);
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
