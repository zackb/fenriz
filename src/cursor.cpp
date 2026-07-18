#include "cursor.hpp"

#include <algorithm>
#include <linux/input-event-codes.h>

#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::cursor {

    namespace {

        // Interactive drag mode. Move/resize of a floating window is free; a tiled window is
        // swapped with the tile it's dropped on, or has its split ratio dragged.
        enum class Grab { None, MoveFloat, ResizeFloat, Swap, ResizeTile };

        // Singleton cursor state, allocated once in init() and kept for the session.
        struct Cursor {
            Server* server;
            wlr_cursor* cursor;
            wlr_xcursor_manager* mgr;
            wl_listener motion;
            wl_listener motion_absolute;
            wl_listener button;
            wl_listener axis;
            wl_listener frame;
            wl_listener request_set_cursor;
            wl_listener request_set_shape;

            // Touchpad swipe/pinch/hold, forwarded verbatim to the focused client.
            wlr_pointer_gestures_v1* gestures;
            wl_listener swipe_begin, swipe_update, swipe_end;
            wl_listener pinch_begin, pinch_update, pinch_end;
            wl_listener hold_begin, hold_end;

            wlr_virtual_pointer_manager_v1* virtual_pointers;
            wl_listener new_virtual_pointer;

            // pointer-constraints: a client (a game) pins or fences the pointer to its surface.
            wlr_pointer_constraints_v1* constraints;
            wlr_relative_pointer_manager_v1* relative_pointers;
            wl_listener new_constraint;
            wlr_pointer_constraint_v1* active = nullptr;
            wl_listener constraint_destroy; // linked only while `active` is set

            Grab grab = Grab::None;
            View* grabbed = nullptr;
            uint32_t resize_edges = 0; // WLR_EDGE_* corner chosen at resize-grab start
        };

        // The singleton, so forget_view() can reach the grab state (init() sets it).
        Cursor* g_cursor = nullptr;

        // Surface under (lx,ly) via the scene graph, honoring z-order. While locked, only
        // the lock tree is considered so input can't reach the desktop. *sx,*sy return
        // surface-local coords on hit.
        wlr_surface* scene_surface_at(Server& server, double lx, double ly, double* sx, double* sy) {
            wlr_scene_node* root = server.locked ? &server.scene_lock->node : &server.scene->tree.node;
            wlr_scene_node* node = wlr_scene_node_at(root, lx, ly, sx, sy);
            if (!node || node->type != WLR_SCENE_NODE_BUFFER)
                return nullptr;
            wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
            return ss ? ss->surface : nullptr;
        }

        // Recover the View that owns a scene node by walking up to the container tree we
        // tagged with the View* (view_handle_map). Null for layer surfaces / empty desktop.
        View* view_from_node(wlr_scene_node* node) {
            for (; node; node = node->parent ? &node->parent->node : nullptr)
                if (node->data)
                    return static_cast<View*>(node->data);
            return nullptr;
        }

        // Topmost View whose surface is under the point (for click-to-focus). Unlike
        // view_box_at this only hits actual surface pixels, not borders/gaps.
        View* view_at_point(Server& server, double lx, double ly) {
            double sx, sy;
            wlr_scene_node* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, &sx, &sy);
            return node ? view_from_node(node) : nullptr;
        }

        // Topmost visible view whose tile box contains the point. Unlike view_at_point this
        // hits borders/gaps too, so a drag started on a window's frame still grabs it.
        View* view_box_at(Server& server, double lx, double ly) {
            for (auto it = server.views.rbegin(); it != server.views.rend(); ++it) {
                View* v = *it;
                if (!view_visible(server, v))
                    continue;
                const auto& b = v->box;
                if (lx >= b.x && lx < b.x + b.width && ly >= b.y && ly < b.y + b.height)
                    return v;
            }
            return nullptr;
        }

        // Apply an interactive drag by a cursor delta (px). Returns true if a grab consumed
        // the motion (so passthrough pointer handling is skipped).
        bool process_grab(Cursor* c, double dx, double dy) {
            Server& server = *c->server;
            View* v = c->grabbed;
            switch (c->grab) {
            case Grab::None:
                return false;
            case Grab::Swap:
                // Float the window up under the cursor: track the pointer via the render
                // offset (box stays in the tree so the swap-on-drop target is unchanged).
                v->anim_ox += dx;
                v->anim_oy += dy;
                return true;
            case Grab::MoveFloat:
                v->box.x += (int)dx;
                v->box.y += (int)dy;
                return true;
            case Grab::ResizeFloat: {
                // Grab the corner nearest where the drag started (resize_edges)
                const uint32_t e = c->resize_edges;
                const int right = v->box.x + v->box.width;
                const int bottom = v->box.y + v->box.height;
                if (e & WLR_EDGE_LEFT) {
                    v->box.width = std::max(1, v->box.width - (int)dx);
                    v->box.x = right - v->box.width;
                } else {
                    v->box.width = std::max(1, v->box.width + (int)dx);
                }
                if (e & WLR_EDGE_TOP) {
                    v->box.height = std::max(1, v->box.height - (int)dy);
                    v->box.y = bottom - v->box.height;
                } else {
                    v->box.height = std::max(1, v->box.height + (int)dy);
                }
                view_configure(v); // size the client to the new inner box (shell-agnostic)
                return true;
            }
            case Grab::ResizeTile:
                tiling::resize_split(server, v, dx, dy);
                return true;
            }
            return false;
        }

        // Drive frames on the output the pointer is over: a drag is driven by cursor motion,
        // not client damage, so nothing else wakes that output up. Only the output under the
        // cursor needs it — scheduling all of them would spin idle screens (see the frame
        // handler's needs_frame guard).
        void schedule_frame_at_cursor(Cursor* c) {
            if (wlr_output* o = wlr_output_layout_output_at(c->server->output_layout, c->cursor->x, c->cursor->y))
                wlr_output_schedule_frame(o);
        }

        void on_constraint_destroy(wl_listener* listener, void* data);

        // Put the cursor where the client asked it to reappear (the locked_pointer
        void warp_to_hint(Cursor* c) {
            wlr_pointer_constraint_v1* con = c->active;
            if (con->type != WLR_POINTER_CONSTRAINT_V1_LOCKED || !con->current.cursor_hint.enabled)
                return;
            for (View* v : c->server->views) {
                if (view_surface(v) != con->surface || !v->surface_tree)
                    continue;
                int lx, ly;
                if (!wlr_scene_node_coords(&v->surface_tree->node, &lx, &ly))
                    return; // not currently drawn (other workspace) — nowhere to warp to
                wlr_cursor_warp(c->cursor, nullptr, lx + con->current.cursor_hint.x, ly + con->current.cursor_hint.y);
                wlr_seat_pointer_warp(c->server->seat, con->current.cursor_hint.x, con->current.cursor_hint.y);
                return;
            }
        }

        // Take or drop the pointer constraint. Null drops whatever is held.
        void set_constraint(Cursor* c, wlr_pointer_constraint_v1* con) {
            if (c->active == con)
                return;
            if (c->active) {
                warp_to_hint(c);
                // Unsubscribe before send_deactivated, which may destroy a oneshot constraint outright
                wl_list_remove(&c->constraint_destroy.link);
                wl_list_init(&c->constraint_destroy.link);
                wlr_pointer_constraint_v1* old = c->active;
                c->active = nullptr;
                wlr_pointer_constraint_v1_send_deactivated(old);
            }
            c->active = con;
            if (con) {
                wl_signal_add(&con->events.destroy, &c->constraint_destroy);
                wlr_pointer_constraint_v1_send_activated(con);
            }
        }

        void on_constraint_destroy(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, constraint_destroy);
            (void)data;
            // Drop it without the deactivate/warp dance — it's already going away.
            wl_list_remove(&c->constraint_destroy.link);
            wl_list_init(&c->constraint_destroy.link);
            c->active = nullptr;
        }

        void on_new_constraint(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, new_constraint);
            auto* con = static_cast<wlr_pointer_constraint_v1*>(data);
            // A game typically maps, takes focus, and only then asks for the lock — by which
            // point no motion is coming to notice it in process_motion. Activate on the spot
            // if its surface already holds the pointer.
            if (con->surface == c->server->seat->pointer_state.focused_surface)
                set_constraint(c, con);
        }

        // Clip a motion delta to the active constraint. True means swallow the motion
        // entirely; dx/dy are adjusted in place otherwise.
        bool constrain(Cursor* c, double* dx, double* dy) {
            // A session lock or an interactive grab outranks a client's pointer lock
            if (c->active && (c->server->locked || c->grab != Grab::None))
                set_constraint(c, nullptr);
            if (!c->active)
                return false;

            double sx, sy;
            if (scene_surface_at(*c->server, c->cursor->x, c->cursor->y, &sx, &sy) != c->active->surface)
                return false;
            if (c->active->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                return true; // pinned: the cursor does not move at all

            // clip to the region
            double cx, cy;
            if (!wlr_region_confine(&c->active->region, sx, sy, sx + *dx, sy + *dy, &cx, &cy))
                return true;
            *dx = cx - sx;
            *dy = cy - sy;
            return false;
        }

        // Update pointer focus + cursor image for the surface under the cursor.
        void process_motion(Cursor* c, uint32_t time) {
            Server& server = *c->server;
            wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

            // While zoomed, the viewport re-centers on the cursor each frame
            if (server.zoom > 1.0f)
                schedule_frame_at_cursor(c);

            const double lx = c->cursor->x, ly = c->cursor->y;
            double sx, sy;
            // The scene graph resolves z-order (and the locked-only lock tree) for us.
            wlr_surface* surface = scene_surface_at(server, lx, ly, &sx, &sy);

            // Pointer focus is changing right here, and a constraint only ever applies to
            // the surface that holds it
            set_constraint(c,
                           surface
                               ? wlr_pointer_constraints_v1_constraint_for_surface(c->constraints, surface, server.seat)
                               : nullptr);

            if (!surface) {
                // Over empty desktop: show the default cursor and drop pointer focus.
                wlr_cursor_set_xcursor(c->cursor, c->mgr, "default");
                wlr_seat_pointer_notify_clear_focus(server.seat);
                return;
            }
            wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);

            // Focus follows pointer: the view under the moving cursor gains focus.
            // focus_view no-ops when the view is already focused, so per-event calls are cheap.
            if (server.config.focus_follows_pointer)
                if (View* v = view_at_point(server, lx, ly))
                    focus_view(server, v);
        }

        void cursor_motion(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, motion);
            auto* event = static_cast<wlr_pointer_motion_event*>(data);

            // relative motion is what a pointer-locked client
            wlr_relative_pointer_manager_v1_send_relative_motion(c->relative_pointers,
                                                                 c->server->seat,
                                                                 (uint64_t)event->time_msec * 1000,
                                                                 event->delta_x,
                                                                 event->delta_y,
                                                                 event->unaccel_dx,
                                                                 event->unaccel_dy);

            double dx = event->delta_x, dy = event->delta_y;
            if (constrain(c, &dx, &dy)) {
                // The cursor stays put; process_motion would only re-send enter/motion.
                wlr_idle_notifier_v1_notify_activity(c->server->idle_notifier, c->server->seat);
                return;
            }

            const double ox = c->cursor->x, oy = c->cursor->y;
            wlr_cursor_move(c->cursor, &event->pointer->base, dx, dy);
            if (process_grab(c, c->cursor->x - ox, c->cursor->y - oy)) {
                if (c->grabbed)
                    place_view_nodes(c->grabbed); // reflect the move now; no client damage drives the drag
                schedule_frame_at_cursor(c);
                return;
            }
            process_motion(c, event->time_msec);
        }

        void cursor_motion_absolute(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, motion_absolute);
            auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);

            double lx, ly;
            wlr_cursor_absolute_to_layout_coords(c->cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
            const double ox = c->cursor->x, oy = c->cursor->y;
            double dx = lx - ox, dy = ly - oy;

            // An absolute event carries no unaccelerated delta, and needs none
            wlr_relative_pointer_manager_v1_send_relative_motion(
                c->relative_pointers, c->server->seat, (uint64_t)event->time_msec * 1000, dx, dy, dx, dy);

            if (constrain(c, &dx, &dy)) {
                wlr_idle_notifier_v1_notify_activity(c->server->idle_notifier, c->server->seat);
                return;
            }

            // warp_closest(ox+dx, oy+dy) is what wlr_cursor_warp_absolute does internally,
            // so this is identical when unconstrained
            wlr_cursor_warp_closest(c->cursor, &event->pointer->base, ox + dx, oy + dy);
            if (process_grab(c, c->cursor->x - ox, c->cursor->y - oy)) {
                if (c->grabbed)
                    place_view_nodes(c->grabbed);
                schedule_frame_at_cursor(c);
                return;
            }
            process_motion(c, event->time_msec);
        }

        void cursor_button(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, button);
            auto* event = static_cast<wlr_pointer_button_event*>(data);
            Server& server = *c->server;

            // Pointer focus is otherwise only refreshed on motion, so a click after the scene
            // changed under a stationary cursor (popup closed, window mapped, workspace switched)
            // would forward to a stale/blank pointer focus and be dropped. Rebase first.
            if (c->grab == Grab::None)
                process_motion(c, event->time_msec);

            if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
                // End an interactive drag. A tiled move resolves into a swap with the tile the
                // window is dropped on.
                if (c->grab != Grab::None) {
                    if (c->grab == Grab::Swap) {
                        // Stop holding the offset so it decays: the window slides from where
                        // it was dropped into its destination tile (or back home if no swap).
                        c->grabbed->dragging = false;
                        View* target = view_box_at(server, c->cursor->x, c->cursor->y);
                        if (target && target != c->grabbed && !target->floating)
                            tiling::swap(server, c->grabbed, target); // arrange folds old-new into the offset
                    }
                    c->grab = Grab::None;
                    c->grabbed = nullptr;
                    process_motion(c, event->time_msec); // restore the passthrough cursor image
                    return;                              // swallow the release that ended the drag
                }
            } else {
                // SUPER + left button starts an interactive move; + SHIFT resizes. Floating
                // windows move/resize freely; tiled windows swap / drag their split ratio.
                // ponytail: SUPER is hardcoded to match the reference config; make it a
                // configurable `bindm` (button field on Bind) if per-user mouse binds are wanted.
                wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat);
                const uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
                if (event->button == BTN_LEFT && (mods & WLR_MODIFIER_LOGO)) {
                    View* v = view_box_at(server, c->cursor->x, c->cursor->y);
                    if (v && !v->fullscreen) {
                        focus_view(server, v);
                        const bool resize = mods & WLR_MODIFIER_SHIFT;
                        c->grabbed = v;
                        c->grab = v->floating ? (resize ? Grab::ResizeFloat : Grab::MoveFloat)
                                              : (resize ? Grab::ResizeTile : Grab::Swap);
                        v->dragging = (c->grab == Grab::Swap); // float above tiles, hold offset
                        // Resize the corner nearest the grab point
                        const char* rc = "grabbing";
                        if (resize) {
                            const bool left = c->cursor->x < v->box.x + v->box.width / 2.0;
                            const bool top = c->cursor->y < v->box.y + v->box.height / 2.0;
                            c->resize_edges =
                                (left ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) | (top ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
                            rc = top ? (left ? "nw-resize" : "ne-resize") : (left ? "sw-resize" : "se-resize");
                        }
                        wlr_cursor_set_xcursor(c->cursor, c->mgr, rc);
                        return; // consume the press; don't forward to the client
                    }
                }
                // Click-to-focus: focus the window under the cursor.
                if (View* view = view_at_point(server, c->cursor->x, c->cursor->y))
                    focus_view(server, view);
            }
            wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
        }

        void cursor_axis(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, axis);
            Server& server = *c->server;
            auto* event = static_cast<wlr_pointer_axis_event*>(data);

            process_motion(c, event->time_msec); // rebase pointer focus; see cursor_button

            // Modifier + scroll = screen zoom (default CTRL, configurable via zoom_mod).
            // The compositor consumes the event: the client never sees it.
            const uint32_t zmod = server.config.zoom_mod;
            wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat);
            const uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
            if (zmod != 0 && (mods & zmod) && event->delta != 0) {
                const float step = server.config.zoom_step;
                // Scroll up (negative delta) zooms in, down zooms out
                float f = event->delta < 0 ? (1.0f + step) : (1.0f / (1.0f + step));
                server.zoom_target = std::clamp(server.zoom_target * f, 1.0f, server.config.zoom_max);
                schedule_frame_at_cursor(c);
                return;
            }

            wlr_seat_pointer_notify_axis(c->server->seat,
                                         event->time_msec,
                                         event->orientation,
                                         event->delta,
                                         event->delta_discrete,
                                         event->source,
                                         event->relative_direction);
        }

        void cursor_frame(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, frame);
            (void)data;
            wlr_seat_pointer_notify_frame(c->server->seat);
        }

        // Touchpad gestures
        void gesture_swipe_begin(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, swipe_begin);
            auto* e = static_cast<wlr_pointer_swipe_begin_event*>(data);
            wlr_pointer_gestures_v1_send_swipe_begin(c->gestures, c->server->seat, e->time_msec, e->fingers);
        }

        void gesture_swipe_update(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, swipe_update);
            auto* e = static_cast<wlr_pointer_swipe_update_event*>(data);
            wlr_pointer_gestures_v1_send_swipe_update(c->gestures, c->server->seat, e->time_msec, e->dx, e->dy);
        }

        void gesture_swipe_end(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, swipe_end);
            auto* e = static_cast<wlr_pointer_swipe_end_event*>(data);
            wlr_pointer_gestures_v1_send_swipe_end(c->gestures, c->server->seat, e->time_msec, e->cancelled);
        }

        void gesture_pinch_begin(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, pinch_begin);
            auto* e = static_cast<wlr_pointer_pinch_begin_event*>(data);
            wlr_pointer_gestures_v1_send_pinch_begin(c->gestures, c->server->seat, e->time_msec, e->fingers);
        }

        void gesture_pinch_update(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, pinch_update);
            auto* e = static_cast<wlr_pointer_pinch_update_event*>(data);
            wlr_pointer_gestures_v1_send_pinch_update(
                c->gestures, c->server->seat, e->time_msec, e->dx, e->dy, e->scale, e->rotation);
        }

        void gesture_pinch_end(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, pinch_end);
            auto* e = static_cast<wlr_pointer_pinch_end_event*>(data);
            wlr_pointer_gestures_v1_send_pinch_end(c->gestures, c->server->seat, e->time_msec, e->cancelled);
        }

        void gesture_hold_begin(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, hold_begin);
            auto* e = static_cast<wlr_pointer_hold_begin_event*>(data);
            wlr_pointer_gestures_v1_send_hold_begin(c->gestures, c->server->seat, e->time_msec, e->fingers);
        }

        void gesture_hold_end(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, hold_end);
            auto* e = static_cast<wlr_pointer_hold_end_event*>(data);
            wlr_pointer_gestures_v1_send_hold_end(c->gestures, c->server->seat, e->time_msec, e->cancelled);
        }

        void on_new_virtual_pointer(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, new_virtual_pointer);
            auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
            attach_pointer(*c->server, &event->new_pointer->pointer.base);
        }

        void request_set_cursor(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, request_set_cursor);
            auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
            // Only honor requests from the client that currently has pointer focus.
            if (c->server->seat->pointer_state.focused_client == event->seat_client)
                wlr_cursor_set_surface(c->cursor, event->surface, event->hotspot_x, event->hotspot_y);
        }

        // cursor-shape-v1: client requests a named shape instead of shipping its own
        // buffer, so we draw it from our xcursor theme at the output scale.
        void request_set_shape(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, request_set_shape);
            auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
            if (c->server->seat->pointer_state.focused_client == event->seat_client)
                wlr_cursor_set_xcursor(c->cursor, c->mgr, wlr_cursor_shape_v1_name(event->shape));
        }

    } // namespace

    void forget_view(View* view) {
        view->dragging = false; // never leave a stuck drag flag on an unmapping view
        if (g_cursor && g_cursor->grabbed == view) {
            g_cursor->grab = Grab::None;
            g_cursor->grabbed = nullptr;
        }
    }

    View* grabbed_view() { return g_cursor ? g_cursor->grabbed : nullptr; }

    void warp_to_output(Server& server, output::Output* o) {
        if (!server.cursor || !o)
            return;
        wlr_box box;
        wlr_output_layout_get_box(server.output_layout, o->handle, &box);
        if (wlr_box_empty(&box))
            return;
        // Already on this output: leave the pointer where the user put it.
        if (wlr_box_contains_point(&box, server.cursor->x, server.cursor->y))
            return;
        wlr_cursor_warp(server.cursor, nullptr, box.x + box.width / 2.0, box.y + box.height / 2.0);
        if (g_cursor)
            process_motion(g_cursor, 0); // refresh pointer focus at the new spot
    }

    void clamp_to_layout(Server& server) {
        if (!server.cursor)
            return;
        // Still over a live output? Nothing to do.
        if (wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y))
            return;
        // Its output is gone (lid closed / cable pulled): move it to the nearest live point,
        // so the pointer doesn't sit at dead coordinates where clicks hit nothing.
        double cx = server.cursor->x, cy = server.cursor->y;
        wlr_output_layout_closest_point(server.output_layout, nullptr, cx, cy, &cx, &cy);
        wlr_cursor_warp_closest(server.cursor, nullptr, cx, cy);
        if (g_cursor)
            process_motion(g_cursor, 0);
    }

    void init(Server& server) {
        Cursor* c = new Cursor{};
        g_cursor = c;
        c->server = &server;
        c->cursor = wlr_cursor_create();
        server.cursor = c->cursor;
        wlr_cursor_attach_output_layout(c->cursor, server.output_layout);
        c->mgr = wlr_xcursor_manager_create(nullptr, 24);
        // Load the cursor theme at the output scale so the pointer isn't a tiny 1x sprite.
        if (server.config.scale > 0)
            wlr_xcursor_manager_load(c->mgr, server.config.scale);

        c->motion.notify = cursor_motion;
        wl_signal_add(&c->cursor->events.motion, &c->motion);
        c->motion_absolute.notify = cursor_motion_absolute;
        wl_signal_add(&c->cursor->events.motion_absolute, &c->motion_absolute);
        c->button.notify = cursor_button;
        wl_signal_add(&c->cursor->events.button, &c->button);
        c->axis.notify = cursor_axis;
        wl_signal_add(&c->cursor->events.axis, &c->axis);
        c->frame.notify = cursor_frame;
        wl_signal_add(&c->cursor->events.frame, &c->frame);

        c->request_set_cursor.notify = request_set_cursor;
        wl_signal_add(&server.seat->events.request_set_cursor, &c->request_set_cursor);

        wlr_cursor_shape_manager_v1* shape_mgr = wlr_cursor_shape_manager_v1_create(server.display, 1);
        c->request_set_shape.notify = request_set_shape;
        wl_signal_add(&shape_mgr->events.request_set_shape, &c->request_set_shape);

        // pointer-gestures: touchpad swipe/pinch/hold straight through to the client.
        c->gestures = wlr_pointer_gestures_v1_create(server.display);
        c->swipe_begin.notify = gesture_swipe_begin;
        wl_signal_add(&c->cursor->events.swipe_begin, &c->swipe_begin);
        c->swipe_update.notify = gesture_swipe_update;
        wl_signal_add(&c->cursor->events.swipe_update, &c->swipe_update);
        c->swipe_end.notify = gesture_swipe_end;
        wl_signal_add(&c->cursor->events.swipe_end, &c->swipe_end);
        c->pinch_begin.notify = gesture_pinch_begin;
        wl_signal_add(&c->cursor->events.pinch_begin, &c->pinch_begin);
        c->pinch_update.notify = gesture_pinch_update;
        wl_signal_add(&c->cursor->events.pinch_update, &c->pinch_update);
        c->pinch_end.notify = gesture_pinch_end;
        wl_signal_add(&c->cursor->events.pinch_end, &c->pinch_end);
        c->hold_begin.notify = gesture_hold_begin;
        wl_signal_add(&c->cursor->events.hold_begin, &c->hold_begin);
        c->hold_end.notify = gesture_hold_end;
        wl_signal_add(&c->cursor->events.hold_end, &c->hold_end);

        // virtual-pointer: wlr-randr's sibling for input
        c->virtual_pointers = wlr_virtual_pointer_manager_v1_create(server.display);
        c->new_virtual_pointer.notify = on_new_virtual_pointer;
        wl_signal_add(&c->virtual_pointers->events.new_virtual_pointer, &c->new_virtual_pointer);

        // pointer-constraints + relative-pointer: mouse lock for games.
        c->relative_pointers = wlr_relative_pointer_manager_v1_create(server.display);
        c->constraints = wlr_pointer_constraints_v1_create(server.display);
        c->new_constraint.notify = on_new_constraint;
        wl_signal_add(&c->constraints->events.new_constraint, &c->new_constraint);
        c->constraint_destroy.notify = on_constraint_destroy;
        // Cursor is value-initialized, so this link is {null,null}, not a valid empty list. Init it
        // so set_constraint's unconditional wl_list_remove is safe before the first constraint.
        wl_list_init(&c->constraint_destroy.link);
    }

    void attach_pointer(Server& server, wlr_input_device* device) {
        wlr_cursor_attach_input_device(server.cursor, device);
        // Apply the scroll direction to real (libinput) pointers that support it — trackpads
        // and some mice. Nested/headless backends aren't libinput and are skipped.
        if (wlr_input_device_is_libinput(device)) {
            libinput_device* dev = wlr_libinput_get_device_handle(device);
            if (dev && libinput_device_config_scroll_has_natural_scroll(dev))
                libinput_device_config_scroll_set_natural_scroll_enabled(dev, server.config.natural_scroll);
            // Pointer/trackpad speed. Skipped on devices with no accel profile
            if (dev && libinput_device_config_accel_is_available(dev))
                libinput_device_config_accel_set_speed(dev, server.config.sensitivity);
            // Tap-to-click, and two-finger press as right-click
            if (dev && libinput_device_config_tap_get_finger_count(dev) > 0)
                libinput_device_config_tap_set_enabled(
                    dev, server.config.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
            if (dev && server.config.clickfinger &&
                (libinput_device_config_click_get_methods(dev) & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER))
                libinput_device_config_click_set_method(dev, LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
        }
    }

} // namespace fenriz::cursor
