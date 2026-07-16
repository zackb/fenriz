#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "layer.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::output {

    namespace {

        // Desktop background / gap color: fills anything not covered by a window or a
        // layer-shell wallpaper. wlr_scene otherwise clears uncovered regions to black.
        constexpr float BG[4] = {0.1f, 0.1f, 0.12f, 1.0f};

        // Per-output state. Standard-layout, so wl_container_of recovers it cleanly.
        struct Output {
            Server* server;
            wlr_output* handle;
            wlr_scene_rect* bg;    // full-output backdrop in the background tree
            wl_listener frame;
            wl_listener request_state;
            wl_listener destroy;
            timespec last_frame{}; // for frame-rate-independent animation decay
        };

        // Advance the slide-into-place animation: decay each visible view's render offset
        // toward 0 by an exponential factor scaled to the elapsed frame time (so the speed
        // is independent of refresh rate), pushing the result into its scene node. A held
        // (dragging) view keeps its offset. Returns true while anything is still moving, so
        // the caller keeps requesting frames — moving a scene node also self-damages, but we
        // schedule explicitly so a motionless held drag still ticks.
        bool animate(Output* output, const timespec& now) {
            Server& server = *output->server;
            double dt = (now.tv_sec - output->last_frame.tv_sec) +
                        (now.tv_nsec - output->last_frame.tv_nsec) / 1e9;
            output->last_frame = now;
            if (dt <= 0 || dt > 1.0)
                dt = 1.0 / 60; // first frame or a long stall: assume one 60Hz tick
            const double tau = std::max(1, server.config.animation_ms) / 1000.0 * 0.35;
            const double factor = std::exp(-dt / tau);
            bool animating = false;
            for (View* view : server.views) {
                if (!view_visible(server, view))
                    continue;
                if (view->dragging) {
                    animating = true;
                    place_view_nodes(view); // keep the held window under the cursor each frame
                    continue;
                }
                if (view->anim_ox != 0 || view->anim_oy != 0) {
                    view->anim_ox *= factor;
                    view->anim_oy *= factor;
                    if (std::abs(view->anim_ox) < 1 && std::abs(view->anim_oy) < 1)
                        view->anim_ox = view->anim_oy = 0;
                    else
                        animating = true;
                    place_view_nodes(view);
                }
            }
            return animating;
        }

        void output_handle_frame(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, frame);
            (void)data;
            Server& server = *output->server;
            wlr_scene_output* so = wlr_scene_get_scene_output(server.scene, output->handle);
            if (!so)
                return;

            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            // The fix: only commit when the scene actually needs a repaint (or a gamma LUT
            // change is pending). An idle, unchanged output commits nothing, so the frame
            // loop goes quiet instead of re-arming itself every vblank.
            if (wlr_scene_output_needs_frame(so) || server.gamma_dirty) {
                // (Re)apply SceneFX per-window effects right before rendering. scenefx re-syncs
                // each surface buffer during its own commit handling (after our commit handler),
                // resetting opacity to 1.0 — so effects set at commit time never reach the
                // render. Applying here, per visible view, is the reliable point.
                for (View* view : server.views)
                    if (view_visible(server, view))
                        apply_view_effects(view);

                wlr_output_state state;
                wlr_output_state_init(&state);
                wlr_scene_output_build_state(so, &state, nullptr);

                // Apply any client-set gamma LUT (wlsunset/gammastep) on this commit.
                if (server.gamma_dirty) {
                    server.gamma_dirty = false;
                    if (server.gamma_control_manager) {
                        if (auto* g = wlr_gamma_control_manager_v1_get_control(
                                server.gamma_control_manager, output->handle))
                            wlr_gamma_control_v1_apply(g, &state);
                    }
                }

                wlr_output_commit_state(output->handle, &state);
                wlr_output_state_finish(&state);
                wlr_scene_output_send_frame_done(so, &now);
            }

            if (animate(output, now))
                wlr_output_schedule_frame(output->handle);
        }

        void output_handle_request_state(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, request_state);
            auto* event = static_cast<wlr_output_event_request_state*>(data);
            wlr_output_commit_state(output->handle, event->state);
            // Keep the backdrop covering the (possibly resized) output.
            wlr_box box;
            wlr_output_layout_get_box(output->server->output_layout, output->handle, &box);
            if (output->bg) {
                wlr_scene_rect_set_size(output->bg, box.width, box.height);
                wlr_scene_node_set_position(&output->bg->node, box.x, box.y);
            }
            layer::arrange(*output->server);
        }

        void output_handle_destroy(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, destroy);
            (void)data;
            wl_list_remove(&output->frame.link);
            wl_list_remove(&output->request_state.link);
            wl_list_remove(&output->destroy.link);
            delete output;
        }

        void handle_new_output(Server& server, wlr_output* out) {
            wlr_output_init_render(out, server.allocator, server.renderer);

            // Enable the output at its preferred mode.
            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_enabled(&state, true);
            if (wlr_output_mode* mode = wlr_output_preferred_mode(out))
                wlr_output_state_set_mode(&state, mode);
            if (server.config.scale > 0)
                wlr_output_state_set_scale(&state, server.config.scale);
            wlr_output_commit_state(out, &state);
            wlr_output_state_finish(&state);

            Output* output = new Output{};
            output->server = &server;
            output->handle = out;

            output->frame.notify = output_handle_frame;
            wl_signal_add(&out->events.frame, &output->frame);
            output->request_state.notify = output_handle_request_state;
            wl_signal_add(&out->events.request_state, &output->request_state);
            output->destroy.notify = output_handle_destroy;
            wl_signal_add(&out->events.destroy, &output->destroy);

            // Wire the output into the scene graph: a scene-output renders + commits it.
            wlr_output_layout_output* lo = wlr_output_layout_add_auto(server.output_layout, out);
            wlr_scene_output* scene_output = wlr_scene_output_create(server.scene, out);
            wlr_scene_output_layout_add_output(server.scene_layout, lo, scene_output);

            // Full-output backdrop at the bottom of the scene (below wallpaper/windows).
            wlr_box box;
            wlr_output_layout_get_box(server.output_layout, out, &box);
            output->bg = wlr_scene_rect_create(server.scene_background, box.width, box.height, BG);
            wlr_scene_node_set_position(&output->bg->node, box.x, box.y);

            // Adopt the first output as primary and give layer surfaces a home + geometry.
            if (!server.output)
                server.output = out;
            layer::arrange(server);
        }

        void on_new_output(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_new_output(*sl->server, static_cast<wlr_output*>(data));
        }

    } // namespace

    void register_handlers(Server& server) {
        server.l_new_output.server = &server;
        server.l_new_output.listener.notify = on_new_output;
        wl_signal_add(&server.backend->events.new_output, &server.l_new_output.listener);
    }

    void set_dpms(Server& server, bool on) {
        if (!server.output)
            return;
        // Same enable pattern as handle_new_output: re-apply mode + scale when powering on.
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, on);
        if (on) {
            if (wlr_output_mode* mode = wlr_output_preferred_mode(server.output))
                wlr_output_state_set_mode(&state, mode);
            if (server.config.scale > 0)
                wlr_output_state_set_scale(&state, server.config.scale);
        }
        wlr_output_commit_state(server.output, &state);
        wlr_output_state_finish(&state);
        if (on)
            wlr_output_schedule_frame(server.output);
        wlr_log(WLR_INFO, "fenriz: display %s", on ? "on" : "off");
    }

} // namespace fenriz::output
