#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <regex>

#include "cursor.hpp"
#include "ipc.hpp"
#include "server.hpp"
#include "tiling.hpp"
#include "wlr.hpp"

namespace fenriz {

    void raise_to_tail(Server& server, View* v); // defined below; used in view_handle_map

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
                if (view->kind == View::Kind::Xdg)
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
            // The initial commit must be answered with a configure. Size 0,0 lets the
            // client choose its own dimensions; milestone 3 tiling will impose sizes.
            // (X11 has no initial-commit handshake — its configure is driven separately.)
            if (view->kind == View::Kind::Xdg && view->toplevel->base->initial_commit)
                wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
            // Adopt the client's committed float size (if any) and re-sync scene nodes. A
            // client can also change its window geometry after mapping (CSD apps adjust their
            // shadow margin); the re-place keeps the inset correct.
            view_reconcile_float_size(view);
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

        // ---- XWayland-only callbacks ----------------------------------------------------
        // An X surface gets its wlr_surface late and can lose it (dissociate) without being
        // destroyed, so map/unmap/commit are wired here at associate and dropped at dissociate.

        void view_handle_associate(wl_listener* listener, void* data) {
            View* view = wl_container_of(listener, view, associate);
            (void)data;
            view->map.notify = view_handle_map;
            wl_signal_add(&view->xwl->surface->events.map, &view->map);
            view->unmap.notify = view_handle_unmap;
            wl_signal_add(&view->xwl->surface->events.unmap, &view->unmap);
            view->commit.notify = view_handle_commit;
            wl_signal_add(&view->xwl->surface->events.commit, &view->commit);
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

    void view_min_size(const View* view, int& w, int& h) {
        // Client's minimum content size (window-geometry units, CSD excluded); 0 = no minimum.
        // X11 hints are optional (size_hints may be null before the client sets WM_NORMAL_HINTS).
        if (view->kind == View::Kind::Xdg) {
            w = view->toplevel->current.min_width;
            h = view->toplevel->current.min_height;
        } else if (view->xwl->size_hints) {
            w = view->xwl->size_hints->min_width;
            h = view->xwl->size_hints->min_height;
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
        const int cw = std::max(1, view->box.width - 2 * bw);
        const int ch = std::max(1, view->box.height - 2 * bw);
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
            focus_surface(server, view_surface(view));
            return;
        }

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
            v->pinned = false;                  // a tiled window can't be pinned
            tiling::insert(server, v, nullptr); // re-tile at the spiral tail
        }
        restack_view(server, v);
        tiling::arrange(server);
    }

    void toggle_pin(Server& server) {
        View* v = server.focused_view;
        if (!v || !v->floating) // pin is floating-only
            return;
        v->pinned = !v->pinned;
    }

    bool apply_window_rules(Server& server, View* view) {
        // Auto-float toplevels the client places itself: a parent link (dialogs,
        // modals, Chromium's "sharing your screen" bubble) or a fixed non-resizable size.
        if (view->kind == View::Kind::Xdg) {
            const wlr_xdg_toplevel_state& st = view->toplevel->current;
            const bool fixed =
                st.max_width > 0 && st.max_width == st.min_width && st.max_height > 0 && st.max_height == st.min_height;
            if (view->toplevel->parent || fixed) {
                view->floating = true;
                view->want_center = true; // center on output like a rule-floated window
            }
        }
        // A pattern matches when empty (any) or its regex matches the value; a null
        // app_id/title is treated as "" so `^$` rules can target unset identity. A bad
        // regex matches nothing (parser-style: swallow, don't crash).
        auto matches = [](const std::string& pat, const char* value) {
            if (pat.empty())
                return true;
            try {
                return std::regex_search(value ? value : "", std::regex(pat));
            } catch (...) {
                return false;
            }
        };
        bool no_focus = false;
        for (const WindowRule& r : server.config.window_rules) {
            if (!matches(r.app_id, view_app_id(view)) || !matches(r.title, view_title(view)))
                continue;
            if (r.floating)
                view->floating = true;
            if (r.center)
                view->want_center = true;
            if (r.no_focus)
                no_focus = true;
        }
        return no_focus;
    }

    void center_view(Server& server, View* view) {
        output::Output* out = view_output(server, view);
        if (!out)
            return;
        // Prefer the usable area (minus bars); fall back to the full output box, exactly
        // as tiling::arrange does.
        int ax = out->usable_area.x, ay = out->usable_area.y;
        int aw = out->usable_area.width, ah = out->usable_area.height;
        if (aw <= 0 || ah <= 0) {
            wlr_box full;
            wlr_output_layout_get_box(server.output_layout, out->handle, &full);
            ax = full.x, ay = full.y, aw = full.width, ah = full.height;
        }
        view->box.x = ax + (aw - view->box.width) / 2;
        view->box.y = ay + (ah - view->box.height) / 2;
        place_view_nodes(view);
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
        wlr_surface* surface = view_surface(view);
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

    void view_reconcile_float_size(View* view) {
        // A floating window sizes itself (we un-tile it, so GTK/Gecko restore their own
        // natural size + CSD margins and never honor a configure). Track the committed
        // size so the border/shadow/clip tighten onto the real content instead of leaving
        // a band of the desktop behind the float showing through. Tiled/fullscreen boxes
        // stay compositor-authoritative; skip while this view is under an interactive grab
        // so a lagging commit can't fight the cursor mid-resize. xdg reports a window
        // geometry (CSD margin excluded); X11 has none, so use the raw surface size.
        const bool xdg = view->kind == View::Kind::Xdg;
        const wlr_box geo =
            xdg ? view->toplevel->base->geometry
                : wlr_box{0, 0, view->xwl->surface->current.width, view->xwl->surface->current.height};
        if (view->floating && !view->fullscreen && geo.width > 0 && geo.height > 0 &&
            cursor::grabbed_view() != view) {
            const int bw = view->server->config.border_width;
            view->box.width = geo.width + 2 * bw;
            view->box.height = geo.height + 2 * bw;
            // A window-rule center can only run once the float has its real size (now).
            if (view->want_center) {
                center_view(*view->server, view);
                view->want_center = false;
            }
        }
        // Re-sync the scene nodes so the inset stays correct. No-op until the nodes exist.
        place_view_nodes(view);
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
        // Popups position themselves against the window-geometry origin, which is exactly where surface_tree sits
        if (view->popup_tree)
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
            // xdg reports a window geometry whose origin is the CSD content corner (shadow
            // margin excluded); anchor the clip there. X11 has no geometry — its buffer is the
            // window, so anchor at 0,0.
            const wlr_box geo = view->kind == View::Kind::Xdg ? view->toplevel->base->geometry : wlr_box{0, 0, 0, 0};
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
        n = std::clamp(n, 0, WS_COUNT - 1);
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

    View::View(Server& server, wlr_xwayland_surface* xwl) : server(&server), kind(Kind::Xwl), xwl(xwl) {
        // map/unmap/commit are wired on the wlr_surface at `associate` (it doesn't exist yet).
        associate.notify = view_handle_associate;
        wl_signal_add(&xwl->events.associate, &associate);
        dissociate.notify = view_handle_dissociate;
        wl_signal_add(&xwl->events.dissociate, &dissociate);
        destroy.notify = view_xwl_handle_destroy;
        wl_signal_add(&xwl->events.destroy, &destroy);
        set_title.notify = view_handle_set_title;
        wl_signal_add(&xwl->events.set_title, &set_title);
        set_app_id.notify = view_handle_set_app_id; // X11 WM_CLASS
        wl_signal_add(&xwl->events.set_class, &set_app_id);
        request_fullscreen.notify = view_handle_request_fullscreen;
        wl_signal_add(&xwl->events.request_fullscreen, &request_fullscreen);
        request_configure.notify = view_handle_request_configure;
        wl_signal_add(&xwl->events.request_configure, &request_configure);
        request_activate.notify = view_handle_request_activate;
        wl_signal_add(&xwl->events.request_activate, &request_activate);
    }

} // namespace fenriz
