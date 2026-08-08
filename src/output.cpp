#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "cursor.hpp"
#include "ipc.hpp"
#include "layer.hpp"
#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::output {

    namespace {

        // Desktop background / gap color: fills anything not covered by a window or a
        // layer-shell wallpaper. wlr_scene otherwise clears uncovered regions to black.
        constexpr float BG[4] = {0.1f, 0.1f, 0.12f, 1.0f};

        // Advance the slide-into-place animation: decay each visible view's render offset
        // toward 0 by an exponential factor scaled to the elapsed frame time (so the speed
        // is independent of refresh rate), pushing the result into its scene node. A held
        // (dragging) view keeps its offset. Returns true while anything is still moving, so
        // the caller keeps requesting frames — moving a scene node also self-damages, but we
        // schedule explicitly so a motionless held drag still ticks.
        //
        // Only animates views on THIS output, so a busy screen doesn't drive frames on a
        // quiet one (each output tracks its own dt).
        bool animate(Output* output, const timespec& now) {
            Server& server = *output->server;
            double dt = (now.tv_sec - output->last_frame.tv_sec) + (now.tv_nsec - output->last_frame.tv_nsec) / 1e9;
            output->last_frame = now;
            if (dt <= 0 || dt > 1.0)
                dt = 1.0 / 60; // first frame or a long stall: assume one 60Hz tick
            const double tau = std::max(1, server.config.animation_ms) / 1000.0 * 0.35;
            const double factor = std::exp(-dt / tau);
            bool animating = false;
            for (View* view : server.views) {
                if (!view_visible(server, view) || view_output(server, view) != output)
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

        // How many commits in a row may be retried before backing off
        constexpr int MAX_COMMIT_RETRIES = 8;

        bool retry_after_failed_commit(Output* output, bool committed) {
            output->commit_failures = committed ? 0 : output->commit_failures + 1;
            if (committed)
                return false;
            wlr_log(WLR_DEBUG,
                    "fenriz: output %s: commit rejected (%d in a row)",
                    name_of(output).c_str(),
                    output->commit_failures);
            return output->commit_failures <= MAX_COMMIT_RETRIES;
        }

        // Apply a pending client gamma LUT (wlsunset/gammastep) to an output state, if any.
        // Returns whether a LUT was consumed, so a failed commit can hand it back.
        bool apply_gamma(Server& server, Output* output, wlr_output_state* state) {
            if (!output->gamma_dirty)
                return false;
            output->gamma_dirty = false;
            if (server.gamma_control_manager)
                if (auto* g = wlr_gamma_control_manager_v1_get_control(server.gamma_control_manager, output->handle))
                    wlr_gamma_control_v1_apply(g, state);
            return true;
        }

        // Zoomed render: draw the whole scene into an offscreen buffer, then blit a
        // cursor-centered sub-region of it scaled up to fill the output
        void render_zoomed(Output* output, wlr_scene_output* so, timespec* now) {
            Server& server = *output->server;
            wlr_output* handle = output->handle;

            // A swapchain sized/formatted for this output's primary buffer; reused across
            // frames and reallocated automatically on a mode change.
            if (!wlr_output_configure_primary_swapchain(handle, nullptr, &output->zoom_swapchain))
                return;

            // Render the composited scene into a buffer from our offscreen swapchain.
            wlr_output_state scene_state;
            wlr_output_state_init(&scene_state);
            wlr_scene_output_state_options opts = {};
            opts.swapchain = output->zoom_swapchain;
            if (!wlr_scene_output_build_state(so, &scene_state, &opts) || !scene_state.buffer) {
                wlr_output_state_finish(&scene_state);
                return;
            }
            wlr_texture* tex = wlr_texture_from_buffer(server.renderer, scene_state.buffer);

            // Zoom viewport
            wlr_box lb; // output box in layout coords (== effective resolution)
            wlr_output_layout_get_box(server.output_layout, handle, &lb);
            const double z = server.zoom;
            const double cx = std::clamp(server.cursor->x - lb.x, 0.0, (double)lb.width);
            const double cy = std::clamp(server.cursor->y - lb.y, 0.0, (double)lb.height);
            const double vw = lb.width / z, vh = lb.height / z;
            const double vx = zoom_viewport_origin(cx, z);
            const double vy = zoom_viewport_origin(cy, z);
            const double sx = (double)handle->width / lb.width, sy = (double)handle->height / lb.height;

            wlr_output_state out_state;
            wlr_output_state_init(&out_state);
            if (tex) {
                if (wlr_render_pass* pass = wlr_output_begin_render_pass(handle, &out_state, nullptr)) {
                    wlr_render_texture_options o = {};
                    o.texture = tex;
                    o.src_box = {vx * sx, vy * sy, vw * sx, vh * sy};
                    o.dst_box = {0, 0, handle->width, handle->height};
                    o.filter_mode = WLR_SCALE_FILTER_BILINEAR;
                    wlr_render_pass_add_texture(pass, &o);
                    wlr_render_pass_submit(pass);
                }
                wlr_texture_destroy(tex);
            }
            const bool took_gamma = apply_gamma(server, output, &out_state);
            const bool committed = wlr_output_commit_state(handle, &out_state);
            wlr_output_state_finish(&out_state);
            wlr_output_state_finish(&scene_state);

            const bool retry = retry_after_failed_commit(output, committed);
            if (!committed) {
                output->gamma_dirty = output->gamma_dirty || took_gamma;
                if (retry)
                    wlr_output_schedule_frame(handle);
                return;
            }
            wlr_scene_output_send_frame_done(so, now);
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

            if (server.layout_dirty) {
                server.layout_dirty = false;
                tiling::arrange(server, false);
            }

            // Ease the global zoom level toward its target only on the output holding the cursor
            const bool has_cursor =
                wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y) == output->handle;
            bool zoom_animating = false;
            if (has_cursor && server.zoom != server.zoom_target) {
                double dt = (now.tv_sec - output->last_frame.tv_sec) + (now.tv_nsec - output->last_frame.tv_nsec) / 1e9;
                if (dt <= 0 || dt > 1.0)
                    dt = 1.0 / 60;
                const double tau = std::max(1, server.config.animation_ms) / 1000.0 * 0.35;
                server.zoom = server.zoom_target + (server.zoom - server.zoom_target) * std::exp(-dt / tau);
                if (std::abs(server.zoom - server.zoom_target) < 0.01f)
                    server.zoom = server.zoom_target;
                else
                    zoom_animating = true;
            }
            const bool zoomed = has_cursor && server.zoom > 1.0f;
            // The zoom path renders via a manual pass into the output's own buffers, which the
            // scene's damage tracking never sees. Returning to the direct (damage-tracked) path
            // would then leave stale zoom pixels in undamaged regions, so the first unzoomed
            // frame must force a whole-output repaint. ponytail: only cleans the cursor's output;
            // zooming then moving to another output mid-zoom can strand a stale frame there.
            const bool exiting_zoom = output->zoom_active && !zoomed;

            // Only commit when the scene needs a repaint, a gamma LUT change is pending, or a
            // zoom is active/animating/just-ended here. An idle, unchanged output commits nothing.
            if (wlr_scene_output_needs_frame(so) || output->gamma_dirty || zoomed || zoom_animating || exiting_zoom) {
                // (Re)apply SceneFX per-window effects right before rendering. scenefx re-syncs
                // each surface buffer during its own commit handling (after our commit handler),
                // resetting opacity to 1.0 — so effects set at commit time never reach the
                // render. Applying here, per visible view, is the reliable point.
                for (View* view : server.views)
                    if (view_visible(server, view) && view_output(server, view) == output)
                        apply_view_effects(view);

                if (zoomed) {
                    render_zoomed(output, so, &now);
                } else {
                    if (exiting_zoom)
                        wlr_damage_ring_add_whole(&so->damage_ring); // propagates to every buffer via rotate
                    wlr_output_state state;
                    wlr_output_state_init(&state);
                    wlr_scene_output_build_state(so, &state, nullptr);
                    const bool took_gamma = apply_gamma(server, output, &state);
                    const bool committed = wlr_output_commit_state(output->handle, &state);
                    wlr_output_state_finish(&state);
                    const bool retry = retry_after_failed_commit(output, committed);
                    if (committed) {
                        wlr_scene_output_send_frame_done(so, &now);
                    } else {
                        wlr_damage_ring_add_whole(&so->damage_ring);
                        output->gamma_dirty = output->gamma_dirty || took_gamma;
                        if (retry)
                            wlr_output_schedule_frame(output->handle);
                    }
                }
            }
            output->zoom_active = zoomed;

            if (animate(output, now) || zoom_animating)
                wlr_output_schedule_frame(output->handle);
        }

        // Keep the backdrop covering the whole output, wherever it sits in the layout.
        void sync_backdrop(Output* o) {
            if (!o->bg)
                return;
            wlr_box box;
            wlr_output_layout_get_box(o->server->output_layout, o->handle, &box);
            wlr_scene_rect_set_size(o->bg, box.width, box.height);
            wlr_scene_node_set_position(&o->bg->node, box.x, box.y);
        }

        // Close (client-side) every layer surface anchored to this output before it goes away,
        // rather than waiting for the client to notice the wl_output global vanish — otherwise a
        // bar outlives its screen and layer::arrange keeps walking a dangling handle->output.
        void close_layer_surfaces(Server& server, wlr_output* handle) {
            for (LayerSurface* ls : std::list<LayerSurface*>(server.layer_surfaces))
                if (ls->handle->output == handle)
                    wlr_layer_surface_v1_destroy(ls->handle);
        }

        void output_handle_request_state(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, request_state);
            auto* event = static_cast<wlr_output_event_request_state*>(data);
            wlr_output_commit_state(output->handle, event->state);
            sync_backdrop(output);
            layer::arrange(*output->server);
        }

        void output_handle_destroy(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, destroy);
            (void)data;
            Server& server = *output->server;

            wl_list_remove(&output->frame.link);
            wl_list_remove(&output->request_state.link);
            wl_list_remove(&output->destroy.link);
            server.outputs.remove(output);

            close_layer_surfaces(server, output->handle);
            if (output->zoom_swapchain)
                wlr_swapchain_destroy(output->zoom_swapchain);
            if (output->bg)
                wlr_scene_node_destroy(&output->bg->node); // backdrop lives in the session-long tree

            // Any workspace still pointing here must be re-homed before the memory goes away;
            // refresh re-runs the policy against the outputs that remain.
            for (Workspace& ws : server.workspaces)
                if (ws.output == output)
                    ws.output = nullptr;

            for (View* v : server.views)
                if (v->announced_output == output)
                    v->announced_output = nullptr;

            delete output;

            // Covers undocking with the lid shut: the external left, so the panel comes back.
            refresh(server);
        }

        void handle_new_output(Server& server, wlr_output* out) {
            wlr_output_init_render(out, server.allocator, server.renderer);

            Output* output = new Output{};
            output->server = &server;
            output->handle = out;

            add_listener(output->frame, out->events.frame, output_handle_frame);
            add_listener(output->request_state, out->events.request_state, output_handle_request_state);
            add_listener(output->destroy, out->events.destroy, output_handle_destroy);

            server.outputs.push_back(output);

            // Scene output must exist before the output is added to the layout below.
            wlr_scene_output* scene_output = wlr_scene_output_create(server.scene, out);
            wlr_scene_output_layout_add_output(
                server.scene_layout, wlr_output_layout_add_auto(server.output_layout, out), scene_output);

            // Full-output backdrop at the bottom of the scene (below wallpaper/windows).
            output->bg = wlr_scene_rect_create(server.scene_background, 0, 0, BG);

            // Applies mode/scale/position from config and (re)adds it to the layout at the
            // right spot; enable_output does the commit.
            set_enabled(server, output, true);

            // Docking with the lid already shut must not light the panel back up.
            refresh(server);
        }

        void on_new_output(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_new_output(*sl->server, static_cast<wlr_output*>(data));
        }

        // The config entry for an output, or null. Later entries win (last match).
        const OutputCfg* config_for(Server& server, const std::string& name) {
            const OutputCfg* hit = nullptr;
            for (const OutputCfg& c : server.config.outputs)
                if (c.name == name)
                    hit = &c;
            return hit;
        }

        // commit mode + scale from this output's config, falling back to the preferred mode or guess
        void commit_mode(Server& server, Output* o) {
            const OutputCfg* cfg = config_for(server, name_of(o));

            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_enabled(&state, true);

            int px_w = o->handle->width, px_h = o->handle->height;

            bool mode_set = false;
            if (cfg && !cfg->mode.empty() && cfg->mode != "preferred" && cfg->mode != "disable") {
                int w = 0, h = 0;
                float hz = 0;
                if (std::sscanf(cfg->mode.c_str(), "%dx%d@%f", &w, &h, &hz) >= 2 && w > 0 && h > 0) {
                    // Prefer an advertised mode that matches; fall back to a custom modeline.
                    wlr_output_mode* best = nullptr;
                    wlr_output_mode* m;
                    wl_list_for_each(m, &o->handle->modes, link) {
                        if (m->width != w || m->height != h)
                            continue;
                        if (hz > 0 && std::abs(m->refresh / 1000.0 - hz) > 1.0)
                            continue;
                        if (!best || m->refresh > best->refresh)
                            best = m;
                    }
                    if (best)
                        wlr_output_state_set_mode(&state, best);
                    else
                        wlr_output_state_set_custom_mode(&state, w, h, hz > 0 ? (int)(hz * 1000) : 0);
                    mode_set = true;
                    px_w = w;
                    px_h = h;
                }
                if (!mode_set)
                    wlr_log(WLR_ERROR,
                            "fenriz: output %s: bad mode '%s', using preferred",
                            name_of(o).c_str(),
                            cfg->mode.c_str());
            }
            if (!mode_set)
                if (wlr_output_mode* mode = wlr_output_preferred_mode(o->handle)) {
                    wlr_output_state_set_mode(&state, mode);
                    px_w = mode->width;
                    px_h = mode->height;
                }

            float scale = cfg ? cfg->scale : 0;
            if (scale == 0)
                scale = server.config.scale;
            if (scale <= 0) {
                scale = guess_scale(o->handle->phys_width, o->handle->phys_height, px_w, px_h);
                wlr_log(WLR_INFO,
                        "fenriz: output %s: %dx%d at %dx%dmm, guessed scale %.2f",
                        name_of(o).c_str(),
                        px_w,
                        px_h,
                        o->handle->phys_width,
                        o->handle->phys_height,
                        scale);
            }
            wlr_output_state_set_scale(&state, scale);

            // Say so loudly if the driver rejects it: a silently-dropped commit leaves the
            // screen on whatever the firmware set, which looks like "my scale config is
            // ignored" and is otherwise invisible.
            if (!wlr_output_commit_state(o->handle, &state))
                wlr_log(WLR_ERROR, "fenriz: output %s: commit failed (mode/scale not applied)", name_of(o).c_str());
            wlr_output_state_finish(&state);
        }

        // An output's explicit `position` from config, if it has a valid one.
        bool config_position(Server& server, Output* o, int* x, int* y) {
            const OutputCfg* cfg = config_for(server, name_of(o));
            return cfg && !cfg->position.empty() && cfg->position != "auto" &&
                   std::sscanf(cfg->position.c_str(), "%dx%d", x, y) == 2;
        }

        // Add to the layout, which is also what creates the wl_output global (see set_enabled).
        // Position is settled by relayout_positions; this just gets it in.
        void layout_add(Server& server, Output* o) {
            int x, y;
            if (config_position(server, o, &x, &y))
                wlr_output_layout_add(server.output_layout, o->handle, x, y);
            else
                wlr_output_layout_add_auto(server.output_layout, o->handle);
        }

        // The order outputs are placed left-to-right when their position is `auto`: the order
        // they appear in the config first, then the order they showed up.
        //
        // This ordering is what makes auto positions stable. wlr_output_layout_add_auto puts an
        // output at the current right edge, so with it alone a screen's position depends on
        // hotplug history: close the lid and reopen it and the panel lands to the RIGHT of the
        // external, silently swapping your monitors. Sorting by a fixed key instead means a lid
        // cycle (or a replug) always reproduces the same arrangement.
        int placement_rank(Server& server, Output* o) {
            const std::string name = name_of(o);
            int i = 0;
            for (const OutputCfg& c : server.config.outputs) {
                if (c.name == name)
                    return i;
                i++;
            }
            int j = (int)server.config.outputs.size();
            for (Output* it : server.outputs) { // appearance order for unconfigured outputs
                if (it == o)
                    return j;
                if (!config_for(server, name_of(it)))
                    j++;
            }
            return j;
        }

        // Give every enabled output a deterministic position: explicit `position` where set,
        // otherwise packed left-to-right in placement_rank order.
        void relayout_positions(Server& server) {
            std::vector<Output*> ordered;
            for (Output* o : server.outputs)
                if (o->enabled)
                    ordered.push_back(o);
            std::stable_sort(ordered.begin(), ordered.end(), [&](Output* a, Output* b) {
                return placement_rank(server, a) < placement_rank(server, b);
            });

            int next_x = 0;
            for (Output* o : ordered) {
                int w = 0, h = 0;
                wlr_output_effective_resolution(o->handle, &w, &h);
                int x, y;
                if (!config_position(server, o, &x, &y)) {
                    x = next_x;
                    y = 0;
                }
                wlr_output_layout_add(server.output_layout, o->handle, x, y);
                next_x = std::max(next_x, x + w);
            }
        }

        std::string fmt(const char* f, ...) {
            char buf[64];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        // Ask the backend whether a client's whole configuration is possible, atomically.
        bool backend_accepts(Server& server, wlr_output_configuration_v1* config) {
            size_t len = 0;
            wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config, &len);
            if (!states)
                return false;
            const bool ok = wlr_backend_test(server.backend, states, len);
            for (size_t i = 0; i < len; i++)
                wlr_output_state_finish(&states[i].base);
            free(states);
            return ok;
        }

        // Fold one client-configured head into the runtime config.
        void store_head(Server& server, const wlr_output_head_v1_state& s) {
            OutputCfg cfg;
            cfg.name = s.output->name ? s.output->name : "";
            if (!s.enabled)
                cfg.mode = "disable";
            else if (s.mode)
                cfg.mode = fmt("%dx%d@%.3f", s.mode->width, s.mode->height, s.mode->refresh / 1000.0);
            else if (s.custom_mode.width)
                cfg.mode = fmt("%dx%d@%.3f", s.custom_mode.width, s.custom_mode.height, s.custom_mode.refresh / 1000.0);
            else
                cfg.mode = "preferred";
            cfg.position = fmt("%dx%d", s.x, s.y);
            cfg.scale = s.scale;

            // Replace rather than append: kanshi re-applies on every hotplug, and appending
            // would grow config.outputs without bound over a session.
            std::erase_if(server.config.outputs, [&](const OutputCfg& c) { return c.name == cfg.name; });
            server.config.outputs.push_back(cfg);
        }

        void handle_manager_config(Server& server, wlr_output_configuration_v1* config, bool test_only) {
            if (!backend_accepts(server, config)) {
                wlr_output_configuration_v1_send_failed(config);
            } else if (test_only) {
                wlr_output_configuration_v1_send_succeeded(config);
            } else {
                wlr_output_configuration_head_v1* head;
                wl_list_for_each(head, &config->heads, link) store_head(server, head->state);
                apply_config(server); // commits mode/scale/position, evacuates workspaces, republishes
                wlr_output_configuration_v1_send_succeeded(config);
            }
            wlr_output_configuration_v1_destroy(config); // ours to free either way
        }

        void on_output_apply(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_manager_config(*sl->server, static_cast<wlr_output_configuration_v1*>(data), false);
        }

        void on_output_test(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_manager_config(*sl->server, static_cast<wlr_output_configuration_v1*>(data), true);
        }

    } // namespace

    void publish_heads(Server& server) {
        if (!server.output_manager)
            return;
        wlr_output_configuration_v1* config = wlr_output_configuration_v1_create();
        for (Output* o : server.outputs) {
            wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(config, o->handle);
            // The head is pre-filled from wlr_output, but position isn't the output's to know
            wlr_box box;
            wlr_output_layout_get_box(server.output_layout, o->handle, &box);
            head->state.x = box.x;
            head->state.y = box.y;
        }
        wlr_output_manager_v1_set_configuration(server.output_manager, config); // takes ownership
    }

    void register_handlers(Server& server) {
        add_listener(server, server.l_new_output, server.backend->events.new_output, on_new_output);

        // wlr-output-management: kanshi / wlr-randr / nwg-displays drive mode, scale and position at runtime
        server.output_manager = wlr_output_manager_v1_create(server.display);
        add_listener(server, server.l_output_apply, server.output_manager->events.apply, on_output_apply);
        add_listener(server, server.l_output_test, server.output_manager->events.test, on_output_test);
    }

    std::string name_of(const Output* o) { return o && o->handle && o->handle->name ? o->handle->name : ""; }

    float scale_of(const Output* o) { return o && o->handle && o->handle->scale > 0 ? o->handle->scale : 1.0f; }

    Output* by_name(Server& server, const std::string& name) {
        for (Output* o : server.outputs)
            if (name_of(o) == name)
                return o;
        return nullptr;
    }

    Output* by_handle(Server& server, const wlr_output* handle) {
        for (Output* o : server.outputs)
            if (o->handle == handle)
                return o;
        return nullptr;
    }

    Output* focused(Server& server) {
        // The focused window's output, if it's on a live one.
        if (View* v = server.focused_view)
            if (Output* o = view_output(server, v))
                return o;
        // Else whatever is under the cursor.
        if (server.cursor) {
            if (wlr_output* h = wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y))
                for (Output* o : server.outputs)
                    if (o->handle == h)
                        return o;
        }
        // Else the first enabled output.
        for (Output* o : server.outputs)
            if (o->enabled)
                return o;
        return nullptr;
    }

    void apply_layout(Server& server) {
        // Settle output positions first: everything below is computed from layout boxes.
        relayout_positions(server);

        // Names of the live (enabled) outputs, in the order they appeared — live.front() is
        // the evacuation fallback, so this order decides where orphaned workspaces land.
        std::vector<std::string> live;
        for (Output* o : server.outputs)
            if (o->enabled)
                live.push_back(name_of(o));

        std::string home[WS_COUNT], current[WS_COUNT], origin[WS_COUNT];
        bool needed[WS_COUNT];
        for (int i = 0; i < WS_COUNT; i++) {
            const Workspace& ws = server.workspaces[i];
            home[i] = ws.home;
            current[i] = name_of(ws.output);
            origin[i] = ws.origin;
            // A workspace needs a screen if it has windows, or if an output is showing it
            // (an empty workspace you're looking at mustn't be yanked away).
            needed[i] = ws.root != nullptr || (ws.output && ws.output->active_ws == i);
        }

        assign_workspaces(home, needed, live, current, origin);

        for (int i = 0; i < WS_COUNT; i++) {
            server.workspaces[i].output = current[i].empty() ? nullptr : by_name(server, current[i]);
            server.workspaces[i].origin = origin[i];
        }

        // Which workspace each output SHOWS — the other half of the policy, and the half
        // that has regressed before. Same marshal-in / marshal-out shape as above.
        std::vector<OutSlot> slots;
        for (Output* o : server.outputs)
            slots.push_back({name_of(o), o->enabled, o->active_ws});

        WsSlot wss[WS_COUNT];
        for (int i = 0; i < WS_COUNT; i++) {
            const Workspace& ws = server.workspaces[i];
            wss[i] = {ws.home, name_of(ws.output), ws.root != nullptr};
        }

        View* f = server.focused_view;
        assign_active(slots, wss, f && f->mapped ? f->workspace : -1);

        {
            size_t n = 0;
            for (Output* o : server.outputs)
                o->active_ws = slots[n++].active_ws;
        }
        // assign_active's claim step can hand a free workspace to an output, so ws.output
        // is an output of the policy too, not just an input.
        for (int i = 0; i < WS_COUNT; i++)
            server.workspaces[i].output = wss[i].output.empty() ? nullptr : by_name(server, wss[i].output);

        for (Output* o : server.outputs)
            sync_backdrop(o);

        layer::arrange(server); // recomputes each output's usable_area, then tiles

        // A view may have moved to an output with a different scale; re-announce so it renders
        // at native resolution instead of a scaled-up blur.
        for (View* v : server.views)
            view_update_output(server, v);

        // Re-seat the keyboard. Two cases: the focused window went hidden (its workspace isn't
        // shown anywhere), or focus was dropped earlier because nothing was visible and a
        // screen has now come back with windows on it. Without the second case, opening the lid
        // hands you back your windows with nothing focused, and you'd have to click.
        if (!server.focused_view || !view_visible(server, server.focused_view))
            focus_topmost_visible(server);

        cursor::clamp_to_layout(server);
        // The one place head state is broadcast: every output event (hotplug, destroy, lid,
        // IPC, reload) funnels through here via refresh().
        publish_heads(server);
        ipc::publish(server);
    }

    void set_enabled(Server& server, Output* o, bool on) {
        if (!o)
            return;

        // A `output = NAME, disable` config entry pins it off.
        if (on) {
            const OutputCfg* cfg = config_for(server, name_of(o));
            if (cfg && cfg->mode == "disable")
                on = false;
        }

        const bool was = o->enabled;
        o->enabled = on;
        if (on) {
            // ALWAYS (re)commit mode + scale, even when the output already reports itself
            // enabled. A DRM connector arrives already lit, carrying the firmware's mode and
            // scale 1 — so short-circuiting on "it's already enabled" silently skips our scale
            // and a HiDPI panel runs at 1x. (Nested outputs arrive disabled, so this path is
            // only exercised on real hardware.)
            commit_mode(server, o);
            // Adding to the layout re-creates the wl_output global: clients see the screen
            // appear and build their per-screen surfaces again, no reload needed.
            layout_add(server, o);
            wlr_output_schedule_frame(o->handle);
        } else if (was || o->handle->enabled) {
            close_layer_surfaces(server, o->handle);

            // Removing from the layout destroys the wl_output global (wlr_output_layout.h:23):
            // the screen genuinely disappears for clients.
            wlr_output_layout_remove(server.output_layout, o->handle);

            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_enabled(&state, false);
            wlr_output_commit_state(o->handle, &state);
            wlr_output_state_finish(&state);
        }
        if (was != on) // re-applying config on an already-enabled output isn't news
            wlr_log(WLR_INFO, "fenriz: output %s %s", name_of(o).c_str(), on ? "enabled" : "disabled");
    }

    Area usable(Server& server, const Output* o) {
        if (!o)
            return {0, 0, 0, 0};

        if (o->usable_area.width > 0 && o->usable_area.height > 0)
            return o->usable_area;

        wlr_box full = {0, 0, 0, 0};
        if (server.output_layout)
            wlr_output_layout_get_box(server.output_layout, o->handle, &full);

        return {full.x, full.y, full.width, full.height};
    }

    bool lid_controls(Server& server, const Output* o) {
        // `lid_output` pins it explicitly; otherwise fall back to the connector-name rule.
        if (!server.config.lid_output.empty())
            return name_of(o) == server.config.lid_output;
        return is_internal(name_of(o));
    }

    void apply_lid_policy(Server& server) {
        // Docked = at least one live screen the lid doesn't control. fenriz only handles this
        // case: with no external screen it does nothing and logind suspends (its
        // HandleLidSwitch default), which is why there's no suspend call here.
        bool docked = false;
        for (Output* o : server.outputs)
            if (o->enabled && !lid_controls(server, o))
                docked = true;

        const bool panel_off = server.lid_closed && docked;
        for (Output* o : server.outputs) {
            if (!lid_controls(server, o))
                continue;
            if (o->enabled == !panel_off)
                continue;
            set_enabled(server, o, !panel_off);
        }
    }

    void refresh(Server& server) {
        apply_lid_policy(server); // decide which screens are on
        apply_layout(server);     // then settle workspaces/layout/focus around them
    }

    void apply_config(Server& server) {
        for (int i = 0; i < WS_COUNT; i++)
            server.workspaces[i].home = server.config.ws_home[i];
        // set_enabled re-applies mode/scale/position whether or not the enable state changed,
        // so an edited `output = ...` line lands live.
        for (Output* o : server.outputs)
            set_enabled(server, o, true); // a `disable` entry is honored inside
        refresh(server);
    }

    void set_dpms(Server& server, Output* o, bool on) {
        // Null = every output (the IPC `dpms` command); the protocol names one.
        for (Output* it : server.outputs) {
            if (o && it != o)
                continue;
            if (!it->enabled)
                continue; // a disabled panel has no DPMS state worth setting
            if (on) {
                // Powering back on is a full re-enable: it must re-apply mode AND scale, or the
                // screen wakes at 1x after every idle blank. commit_mode is the one place that
                // knows how to bring an output up — never hand-roll the state here.
                commit_mode(server, it);
                wlr_output_schedule_frame(it->handle);
            } else {
                wlr_output_state state;
                wlr_output_state_init(&state);
                wlr_output_state_set_enabled(&state, false);
                // Check the commit: a rejected one would otherwise be logged as a successful blank below
                if (!wlr_output_commit_state(it->handle, &state))
                    wlr_log(WLR_ERROR, "fenriz: output %s: DPMS off commit failed", name_of(it).c_str());
                wlr_output_state_finish(&state);
            }
            wlr_log(WLR_INFO, "fenriz: display %s %s", name_of(it).c_str(), on ? "on" : "off");
        }
    }

} // namespace fenriz::output
