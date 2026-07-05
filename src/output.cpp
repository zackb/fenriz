#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <pixman.h>

#include "layer.hpp"
#include "lock.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::output {

    namespace {

        // Desktop background / gap color. Shared with the corner-rounding pass so the
        // rounded-off corners blend seamlessly into the gaps between tiles.
        constexpr float BG[4] = {0.1f, 0.1f, 0.12f, 1.0f};

        // Per-output state. Standard-layout, so wl_container_of recovers it cleanly.
        struct Output {
            Server* server;
            wlr_output* handle;
            wl_listener frame;
            wl_listener request_state;
            wl_listener destroy;
            timespec last_frame{}; // for frame-rate-independent animation decay
        };

        // Passed through wlr_xdg_surface_for_each_surface while rendering a view.
        struct RenderContext {
            wlr_render_pass* pass;
            int x, y;                      // surface origin in logical output-local coords
            float scale;                   // output scale: logical -> physical buffer pixels
            const float* alpha;            // per-window opacity, or nullptr for opaque
            const pixman_region32_t* clip; // physical-pixel clip (tile), or nullptr
        };

        // Scale a logical box into physical buffer pixels.
        wlr_box scale_box(const wlr_box& b, float scale) {
            return {(int)(b.x * scale), (int)(b.y * scale), (int)(b.width * scale), (int)(b.height * scale)};
        }

        wlr_render_color color_from_u32(uint32_t c) {
            return {
                ((c >> 24) & 0xff) / 255.0f,
                ((c >> 16) & 0xff) / 255.0f,
                ((c >> 8) & 0xff) / 255.0f,
                (c & 0xff) / 255.0f,
            };
        }

        void render_surface(wlr_surface* surface, int sx, int sy, void* data) {
            auto* ctx = static_cast<RenderContext*>(data);
            wlr_texture* texture = wlr_surface_get_texture(surface);
            if (!texture)
                return;
            // Destination is the surface's *logical* size scaled to physical pixels; the
            // client's buffer is already rendered at the output scale, so this maps 1:1.
            const float s = ctx->scale;
            wlr_render_texture_options opts = {};
            opts.texture = texture;
            opts.dst_box = {(int)((ctx->x + sx) * s),
                            (int)((ctx->y + sy) * s),
                            (int)(surface->current.width * s),
                            (int)(surface->current.height * s)};
            opts.alpha = ctx->alpha;
            opts.clip = ctx->clip; // keep CSD shadow from bleeding past the tile
            wlr_render_pass_add_texture(ctx->pass, &opts);
        }

        // Build a filled rounded-rectangle pixman region (physical pixels). Corners are
        // approximated per-scanline from the corner circle; pixman coalesces the strips.
        // radius <= 0 yields a plain rectangle. Used to clip window content (corners reveal
        // the real backdrop) and to shape the border ring.
        void build_rounded_region(pixman_region32_t* out, const wlr_box& box, int radius) {
            pixman_region32_init(out);
            if (box.width <= 0 || box.height <= 0)
                return;
            radius = std::min({radius, box.width / 2, box.height / 2});
            if (radius <= 0) {
                pixman_region32_init_rect(out, box.x, box.y, box.width, box.height);
                return;
            }
            pixman_region32_union_rect(out, out, box.x, box.y + radius, box.width, box.height - 2 * radius);
            for (int i = 0; i < radius; i++) {
                int dy = radius - 1 - i;
                int inset = radius - (int)std::floor(std::sqrt((double)(radius * radius - dy * dy)));
                int w = box.width - 2 * inset;
                pixman_region32_union_rect(out, out, box.x + inset, box.y + i, w, 1);
                pixman_region32_union_rect(out, out, box.x + inset, box.y + box.height - 1 - i, w, 1);
            }
        }

        // Render every mapped layer surface on a given layer (surface + its popups).
        // Layers are drawn opaque (alpha=nullptr); the surface's own alpha still applies.
        void render_layer(wlr_render_pass* pass, Server& server, int lyr, float scale) {
            for (LayerSurface* ls : server.layer_surfaces) {
                if (!ls->mapped || ls->handle->current.layer != (uint32_t)lyr)
                    continue;
                RenderContext ctx = {pass, ls->geo.x, ls->geo.y, scale, nullptr, nullptr};
                wlr_layer_surface_v1_for_each_surface(ls->handle, render_surface, &ctx);
            }
        }

        void send_frame_done(wlr_surface* surface, int sx, int sy, void* data) {
            (void)sx;
            (void)sy;
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(data));
        }

        // Draw one non-fullscreen window: content clipped to a rounded rect, then the
        // border ring. Positioned at view->box plus its animation offset (anim_ox/oy),
        // so a window sliding into place or held under a drag renders off its tile.
        void render_window(wlr_render_pass* pass, Server& server, const Config& cfg, View* view, float scale) {
            const int ox = (int)std::lround(view->anim_ox);
            const int oy = (int)std::lround(view->anim_oy);
            // Honor the client's window geometry: align its geometry origin to the tile
            // (CSD apps put a shadow margin at negative offset).
            const wlr_box& geo = view->toplevel->base->geometry;
            const wlr_box tile =
                scale_box({view->box.x + ox, view->box.y + oy, view->box.width, view->box.height}, scale);
            const int radius = (int)(cfg.rounding * scale);
            const int bw = (int)(cfg.border_width * scale);
            const bool has_border = bw > 0 && tile.width > 2 * bw && tile.height > 2 * bw;

            // Content is inset by the border so it sits *inside* the frame (the client is
            // sized to this inner area by tiling::arrange). The rounded inner clip also
            // clips CSD shadow bleed.
            const wlr_box inner_box =
                has_border ? wlr_box{tile.x + bw, tile.y + bw, tile.width - 2 * bw, tile.height - 2 * bw} : tile;
            pixman_region32_t inner;
            build_rounded_region(&inner, inner_box, has_border ? std::max(0, radius - bw) : radius);

            const int inset = has_border ? cfg.border_width : 0; // logical
            RenderContext ctx = {pass,
                                 view->box.x + ox + inset - geo.x,
                                 view->box.y + oy + inset - geo.y,
                                 scale,
                                 &cfg.opacity,
                                 &inner};
            wlr_xdg_surface_for_each_surface(view->toplevel->base, render_surface, &ctx);

            if (has_border) {
                // Border ring: the rounded outer edge minus the inner content region.
                pixman_region32_t outer, ring;
                build_rounded_region(&outer, tile, radius);
                pixman_region32_init(&ring);
                pixman_region32_subtract(&ring, &outer, &inner);
                uint32_t border = (view == server.focused_view) ? cfg.border_active : cfg.border_inactive;
                wlr_render_rect_options opts = {};
                opts.box = tile;
                opts.color = color_from_u32(border);
                opts.clip = &ring;
                wlr_render_pass_add_rect(pass, &opts);
                pixman_region32_fini(&outer);
                pixman_region32_fini(&ring);
            }
            pixman_region32_fini(&inner);
        }

        void output_handle_frame(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, frame);
            (void)data;
            Server& server = *output->server;
            const Config& cfg = server.config;
            const float scale = output->handle->scale;

            wlr_output_state state;
            wlr_output_state_init(&state);

            // Apply any client-set gamma LUT (wlsunset/gammastep) on this commit. Routing it
            // through the single frame path avoids a separate gamma commit racing the render.
            // ponytail: reapplied each frame (a cheap LUT copy) rather than tracking dirty.
            if (server.gamma_control_manager) {
                wlr_gamma_control_v1* gamma =
                    wlr_gamma_control_manager_v1_get_control(server.gamma_control_manager, output->handle);
                if (gamma)
                    wlr_gamma_control_v1_apply(gamma, &state);
            }

            wlr_render_pass* pass = wlr_output_begin_render_pass(output->handle, &state, nullptr);
            if (pass) {
                wlr_render_rect_options bg = {};
                bg.box = {0, 0, output->handle->width, output->handle->height};
                bg.color = {BG[0], BG[1], BG[2], BG[3]};
                wlr_render_pass_add_rect(pass, &bg);

                if (server.locked) {
                    // Locked: draw only the lock surface over the blank background — never
                    // client windows. A missing surface (not yet mapped, or the lock client
                    // died) leaves just the background, so the desktop stays hidden.
                    if (wlr_surface* lsurf = lock::surface_for(server, output->handle)) {
                        RenderContext ctx = {pass, 0, 0, scale, nullptr, nullptr};
                        wlr_surface_for_each_surface(lsurf, render_surface, &ctx);
                    }
                    wlr_render_pass_submit(pass);
                    wlr_output_commit_state(output->handle, &state);
                    wlr_output_state_finish(&state);
                    timespec lnow;
                    clock_gettime(CLOCK_MONOTONIC, &lnow);
                    if (wlr_surface* lsurf = lock::surface_for(server, output->handle))
                        wlr_surface_for_each_surface(lsurf, send_frame_done, &lnow);
                    return;
                }

                // Layer-shell backdrop (wallpapers, bottom bars) sits below windows.
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, scale);
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, scale);

                // Content + rounded border, bottom -> top. Rounding is done by clipping the
                // window (and shaping the border) to a rounded-rect region, so the corners
                // reveal whatever is actually behind them. A window held under a drag is
                // skipped here and drawn last, so it floats above the other tiles.
                View* dragging = nullptr;
                for (View* view : server.views) {
                    if (!view_visible(server, view) || view->fullscreen)
                        continue; // fullscreen windows are drawn above the top layer, below
                    if (view->dragging) {
                        dragging = view;
                        continue;
                    }
                    render_window(pass, server, cfg, view, scale);
                }
                if (dragging)
                    render_window(pass, server, cfg, dragging, scale);

                // Top bars/panels sit above windows.
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_TOP, scale);

                // A fullscreen window covers everything including the top bar, drawn with
                // no border and no rounded clip. Still below the overlay layer so lockers
                // and notifications stay on top.
                for (View* view : server.views) {
                    if (!view_visible(server, view) || !view->fullscreen)
                        continue;
                    const wlr_box& geo = view->toplevel->base->geometry;
                    RenderContext ctx = {pass, view->box.x - geo.x, view->box.y - geo.y, scale, &cfg.opacity, nullptr};
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, render_surface, &ctx);
                }

                // Overlays (e.g. quickshell) sit above everything.
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, scale);

                wlr_render_pass_submit(pass);
            }

            wlr_output_commit_state(output->handle, &state);
            wlr_output_state_finish(&state);

            // Let clients know they may draw the next frame.
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            for (View* view : output->server->views) {
                if (view_visible(server, view))
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, send_frame_done, &now);
            }
            for (LayerSurface* ls : output->server->layer_surfaces) {
                if (ls->mapped)
                    wlr_layer_surface_v1_for_each_surface(ls->handle, send_frame_done, &now);
            }

            // Advance the slide-into-place animation: decay each view's render offset
            // toward 0 by an exponential factor scaled to the elapsed frame time (so the
            // speed is independent of refresh rate). A held (dragging) view is left alone.
            // While anything is still moving, keep requesting frames — there is no
            // continuous render clock, so without this the animation (and a live drag)
            // would stall until a client next damages the screen.
            double dt = (now.tv_sec - output->last_frame.tv_sec) +
                        (now.tv_nsec - output->last_frame.tv_nsec) / 1e9;
            output->last_frame = now;
            if (dt <= 0 || dt > 1.0)
                dt = 1.0 / 60; // first frame or a long stall: assume one 60Hz tick
            const double tau = std::max(1, cfg.animation_ms) / 1000.0 * 0.35;
            const double factor = std::exp(-dt / tau);
            bool animating = false;
            for (View* view : server.views) {
                if (!view_visible(server, view))
                    continue;
                if (view->dragging) {
                    animating = true;
                    continue;
                }
                if (view->anim_ox != 0 || view->anim_oy != 0) {
                    view->anim_ox *= factor;
                    view->anim_oy *= factor;
                    if (std::abs(view->anim_ox) < 1 && std::abs(view->anim_oy) < 1)
                        view->anim_ox = view->anim_oy = 0;
                    else
                        animating = true;
                }
            }
            if (animating)
                wlr_output_schedule_frame(output->handle);
        }

        void output_handle_request_state(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, request_state);
            auto* event = static_cast<wlr_output_event_request_state*>(data);
            wlr_output_commit_state(output->handle, event->state);
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

            wlr_output_layout_add_auto(server.output_layout, out);

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
