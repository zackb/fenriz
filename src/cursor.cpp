#include "cursor.hpp"

#include "layer.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::cursor {

    namespace {

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
        };

        // Update pointer focus + cursor image for the surface under the cursor.
        void process_motion(Cursor* c, uint32_t time) {
            Server& server = *c->server;
            wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

            const double lx = c->cursor->x, ly = c->cursor->y;
            double sx, sy;
            // Z-order: overlay/top layers, then windows, then bottom/background layers.
            wlr_surface* surface = layer::surface_at(server, lx, ly, &sx, &sy, true);
            if (!surface)
                view_at(server, lx, ly, &surface, &sx, &sy);
            if (!surface)
                surface = layer::surface_at(server, lx, ly, &sx, &sy, false);

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
            wlr_cursor_move(c->cursor, &event->pointer->base, event->delta_x, event->delta_y);
            process_motion(c, event->time_msec);
        }

        void cursor_motion_absolute(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, motion_absolute);
            auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
            wlr_cursor_warp_absolute(c->cursor, &event->pointer->base, event->x, event->y);
            process_motion(c, event->time_msec);
        }

        void cursor_button(wl_listener* listener, void* data) {
            Cursor* c = wl_container_of(listener, c, button);
            auto* event = static_cast<wlr_pointer_button_event*>(data);
            Server& server = *c->server;

            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                // Click-to-focus: focus the window under the cursor.
                double sx, sy;
                wlr_surface* surface = nullptr;
                View* view = view_at(server, c->cursor->x, c->cursor->y, &surface, &sx, &sy);
                if (view)
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

    } // namespace

    void init(Server& server) {
        Cursor* c = new Cursor{};
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
    }

    void attach_pointer(Server& server, wlr_input_device* device) {
        wlr_cursor_attach_input_device(server.cursor, device);
    }

} // namespace fenriz::cursor
