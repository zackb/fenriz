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
                const int bw = server.config.border_width;
                wlr_xdg_toplevel_set_size(
                    v->toplevel, std::max(1, v->box.width - 2 * bw), std::max(1, v->box.height - 2 * bw));
                return true;
            }
            case Grab::ResizeTile:
                tiling::resize_split(server, v, dx, dy);
                return true;
            }
            return false;
        }

        // Update pointer focus + cursor image for the surface under the cursor.
        void process_motion(Cursor* c, uint32_t time) {
            Server& server = *c->server;
            wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

            const double lx = c->cursor->x, ly = c->cursor->y;
            double sx, sy;
            // The scene graph resolves z-order (and the locked-only lock tree) for us.
            wlr_surface* surface = scene_surface_at(server, lx, ly, &sx, &sy);

            if (!surface) {
                // Over empty desktop: show the default cursor and drop pointer focus.
                wlr_cursor_set_xcursor(c->cursor, c->mgr, "default");
                wlr_seat_pointer_notify_clear_focus(server.seat);
                return;
            }
            wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
        }

        void cursor_motion(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, motion);
            auto* event = static_cast<wlr_pointer_motion_event*>(data);
            const double ox = c->cursor->x, oy = c->cursor->y;
            wlr_cursor_move(c->cursor, &event->pointer->base, event->delta_x, event->delta_y);
            if (process_grab(c, c->cursor->x - ox, c->cursor->y - oy)) {
                if (c->grabbed)
                    place_view_nodes(c->grabbed); // reflect the move now; no client damage drives the drag
                if (c->server->output)
                    wlr_output_schedule_frame(c->server->output);
                return;
            }
            process_motion(c, event->time_msec);
        }

        void cursor_motion_absolute(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, motion_absolute);
            auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
            const double ox = c->cursor->x, oy = c->cursor->y;
            wlr_cursor_warp_absolute(c->cursor, &event->pointer->base, event->x, event->y);
            if (process_grab(c, c->cursor->x - ox, c->cursor->y - oy)) {
                if (c->grabbed)
                    place_view_nodes(c->grabbed);
                if (c->server->output)
                    wlr_output_schedule_frame(c->server->output);
                return;
            }
            process_motion(c, event->time_msec);
        }

        void cursor_button(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, button);
            auto* event = static_cast<wlr_pointer_button_event*>(data);
            Server& server = *c->server;

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
            auto* event = static_cast<wlr_pointer_axis_event*>(data);
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
        }
    }

} // namespace fenriz::cursor
