#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

        void output_handle_frame(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, frame);
            (void)data;
            Server& server = *output->server;
            wlr_scene_output* so = wlr_scene_get_scene_output(server.scene, output->handle);
            if (!so)
                return;

            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            // Only commit when the scene actually needs a repaint (or a gamma LUT change is
            // pending). An idle, unchanged output commits nothing, so the frame loop goes quiet
            // instead of re-arming itself every vblank.
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
                        if (auto* g =
                                wlr_gamma_control_manager_v1_get_control(server.gamma_control_manager, output->handle))
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

        // Keep the backdrop covering the whole output, wherever it sits in the layout.
        void sync_backdrop(Output* o) {
            if (!o->bg)
                return;
            wlr_box box;
            wlr_output_layout_get_box(o->server->output_layout, o->handle, &box);
            wlr_scene_rect_set_size(o->bg, box.width, box.height);
            wlr_scene_node_set_position(&o->bg->node, box.x, box.y);
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

    } // namespace

    void register_handlers(Server& server) {
        server.l_new_output.server = &server;
        server.l_new_output.listener.notify = on_new_output;
        wl_signal_add(&server.backend->events.new_output, &server.l_new_output.listener);
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
            // and a HiDPI panel runs at 1x. (It looks fine nested, where outputs arrive
            // disabled, which is exactly how this got missed.)
            commit_mode(server, o);
            // Adding to the layout re-creates the wl_output global: clients see the screen
            // appear and build their per-screen surfaces again, no reload needed.
            layout_add(server, o);
            wlr_output_schedule_frame(o->handle);
        } else if (was || o->handle->enabled) {
            // Close this output's layer surfaces ourselves rather than waiting for clients to
            // notice the global vanish — otherwise a bar can outlive its screen for a frame.
            for (LayerSurface* ls : std::list<LayerSurface*>(server.layer_surfaces))
                if (ls->handle->output == o->handle)
                    wlr_layer_surface_v1_destroy(ls->handle);

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
                wlr_output_commit_state(it->handle, &state);
                wlr_output_state_finish(&state);
            }
            wlr_log(WLR_INFO, "fenriz: display %s %s", name_of(it).c_str(), on ? "on" : "off");
        }
    }

} // namespace fenriz::output
