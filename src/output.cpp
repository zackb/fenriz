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

        // Apply a pending client gamma LUT (wlsunset/gammastep) to an output state, if any.
        void apply_gamma(Server& server, wlr_output* handle, wlr_output_state* state) {
            if (!server.gamma_dirty)
                return;
            server.gamma_dirty = false;
            if (server.gamma_control_manager)
                if (auto* g = wlr_gamma_control_manager_v1_get_control(server.gamma_control_manager, handle))
                    wlr_gamma_control_v1_apply(g, state);
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

            // Zoom viewport, centered on the cursor and clamped to the output.
            wlr_box lb; // output box in layout coords (== effective resolution)
            wlr_output_layout_get_box(server.output_layout, handle, &lb);
            const double z = server.zoom;
            double cx = std::clamp(server.cursor->x - lb.x, 0.0, (double)lb.width);
            double cy = std::clamp(server.cursor->y - lb.y, 0.0, (double)lb.height);
            double vw = lb.width / z, vh = lb.height / z;
            double vx = std::clamp(cx - vw / 2, 0.0, lb.width - vw);
            double vy = std::clamp(cy - vh / 2, 0.0, lb.height - vh);
            // src_box is in buffer pixels; buffer may be larger than layout box under output scale.
            const double sx = (double)handle->width / lb.width, sy = (double)handle->height / lb.height;

            wlr_output_state out_state;
            wlr_output_state_init(&out_state);
            if (tex) {
                if (wlr_render_pass* pass = wlr_output_begin_render_pass(handle, &out_state, nullptr)) {
                    // Plain textured blit — no SceneFX effects needed on the zoom, and its
                    // pass.h is un-includable here (pulls a private egl.h). The fx_renderer
                    // still services this base wlr_render_pass call.
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
            apply_gamma(server, handle, &out_state);
            wlr_output_commit_state(handle, &out_state);
            wlr_output_state_finish(&out_state);
            wlr_output_state_finish(&scene_state);
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

            // Only commit when the scene needs a repaint, a gamma LUT change is pending, or a
            // zoom is active/animating here. An idle, unchanged output commits nothing.
            if (wlr_scene_output_needs_frame(so) || server.gamma_dirty || zoomed || zoom_animating) {
                // (Re)apply SceneFX per-window effects right before rendering. scenefx re-syncs
                // each surface buffer during its own commit handling (after our commit handler),
                // resetting opacity to 1.0 — so effects set at commit time never reach the
                // render. Applying here, per visible view, is the reliable point.
                for (View* view : server.views)
                    if (view_visible(server, view))
                        apply_view_effects(view);

                if (zoomed) {
                    render_zoomed(output, so, &now);
                } else {
                    wlr_output_state state;
                    wlr_output_state_init(&state);
                    wlr_scene_output_build_state(so, &state, nullptr);
                    apply_gamma(server, output->handle, &state);
                    wlr_output_commit_state(output->handle, &state);
                    wlr_output_state_finish(&state);
                    wlr_scene_output_send_frame_done(so, &now);
                }
            }

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
            delete output;

            // Covers undocking with the lid shut: the external left, so the panel comes back.
            refresh(server);
        }

        void handle_new_output(Server& server, wlr_output* out) {
            wlr_output_init_render(out, server.allocator, server.renderer);

            Output* output = new Output{};
            output->server = &server;
            output->handle = out;

            output->frame.notify = output_handle_frame;
            wl_signal_add(&out->events.frame, &output->frame);
            output->request_state.notify = output_handle_request_state;
            wl_signal_add(&out->events.request_state, &output->request_state);
            output->destroy.notify = output_handle_destroy;
            wl_signal_add(&out->events.destroy, &output->destroy);

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

        // Commit mode + scale from config (or the preferred mode / global scale fallback).
        void commit_mode(Server& server, Output* o) {
            const OutputCfg* cfg = config_for(server, name_of(o));

            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_enabled(&state, true);

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
                    if (best) {
                        wlr_output_state_set_mode(&state, best);
                        mode_set = true;
                    } else {
                        wlr_output_state_set_custom_mode(&state, w, h, hz > 0 ? (int)(hz * 1000) : 0);
                        mode_set = true;
                    }
                }
                if (!mode_set)
                    wlr_log(WLR_ERROR,
                            "fenriz: output %s: bad mode '%s', using preferred",
                            name_of(o).c_str(),
                            cfg->mode.c_str());
            }
            if (!mode_set)
                if (wlr_output_mode* mode = wlr_output_preferred_mode(o->handle))
                    wlr_output_state_set_mode(&state, mode);

            // Per-output scale, falling back to the global config.scale (pre-multi-output
            // configs keep working).
            const float scale = cfg && cfg->scale > 0 ? cfg->scale : server.config.scale;
            if (scale > 0)
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
        server.l_new_output.server = &server;
        server.l_new_output.listener.notify = on_new_output;
        wl_signal_add(&server.backend->events.new_output, &server.l_new_output.listener);

        // wlr-output-management: kanshi / wlr-randr / nwg-displays drive mode, scale and position at runtime
        server.output_manager = wlr_output_manager_v1_create(server.display);
        server.l_output_apply.server = &server;
        server.l_output_apply.listener.notify = on_output_apply;
        wl_signal_add(&server.output_manager->events.apply, &server.l_output_apply.listener);
        server.l_output_test.server = &server;
        server.l_output_test.listener.notify = on_output_test;
        wl_signal_add(&server.output_manager->events.test, &server.l_output_test.listener);
    }

    std::string name_of(const Output* o) { return o && o->handle && o->handle->name ? o->handle->name : ""; }

    float scale_of(Server& server, const Output* o) {
        const OutputCfg* cfg = config_for(server, name_of(o));
        if (cfg && cfg->scale > 0)
            return cfg->scale;
        return server.config.scale > 0 ? server.config.scale : 1.0f;
    }

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

        // Drop the shown workspace of any output that no longer holds it (or is off).
        for (Output* o : server.outputs)
            if (!o->enabled || (o->active_ws >= 0 && server.workspaces[o->active_ws].output != o))
                o->active_ws = -1;

        // Your work follows you. If the focused window's workspace just got evacuated, show it
        // on the screen it landed on — this is the main clamshell case: you're working on the
        // laptop, docked, and shut the lid. Leaving the external on whatever empty workspace it
        // happened to display would strand your session one keypress away for no reason.
        if (View* f = server.focused_view; f && f->mapped) {
            Workspace& ws = server.workspaces[f->workspace];
            if (ws.output && ws.output->active_ws != f->workspace)
                ws.output->active_ws = f->workspace;
        }

        // Two workspaces must never be shown on the same output.
        for (Output* o : server.outputs)
            for (Output* p : server.outputs)
                if (o != p && o->active_ws >= 0 && o->active_ws == p->active_ws)
                    p->active_ws = -1;

        // Every enabled output shows exactly one workspace. One that has none picks the best
        // workspace living here — preferring a configured home, then one with windows — and
        // failing that CLAIMS a free one. The claim is what gives a freshly plugged-in monitor
        // something to display: without it a second screen renders nothing and no keybind can
        // fix it, because every workspace already belongs to the first screen.
        for (Output* o : server.outputs) {
            if (!o->enabled || o->active_ws >= 0)
                continue;
            const std::string oname = name_of(o);

            int best = -1, best_rank = 99;
            for (int i = 0; i < WS_COUNT; i++) {
                const Workspace& ws = server.workspaces[i];
                if (ws.output != o)
                    continue;
                const bool homed = ws.home == oname;
                const bool windows = ws.root != nullptr;
                const int rank = homed ? (windows ? 0 : 1) : (windows ? 2 : 3);
                if (rank < best_rank) {
                    best_rank = rank;
                    best = i;
                }
            }
            if (best < 0) {
                // Nothing lives here yet: claim the lowest-numbered unassigned workspace,
                // preferring one configured for this output.
                for (int i = 0; i < WS_COUNT && best < 0; i++)
                    if (!server.workspaces[i].output && server.workspaces[i].home == oname)
                        best = i;
                for (int i = 0; i < WS_COUNT && best < 0; i++)
                    if (!server.workspaces[i].output && server.workspaces[i].home.empty())
                        best = i;
                if (best >= 0)
                    server.workspaces[best].output = o;
            }
            o->active_ws = best; // -1 only if all 10 are spoken for elsewhere
        }

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
