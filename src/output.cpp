#include "output.hpp"

#include <ctime>

#include "layer.hpp"
#include "renderer.hpp"
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
        };

        // Passed through wlr_xdg_surface_for_each_surface while rendering a view.
        struct RenderContext {
            wlr_render_pass* pass;
            int x, y;           // view origin in logical output-local coordinates
            float scale;        // output scale: logical -> physical buffer pixels
            const float* alpha; // per-window opacity, or nullptr for opaque
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
            opts.dst_box = {(int)((ctx->x + sx) * s), (int)((ctx->y + sy) * s),
                            (int)(surface->current.width * s), (int)(surface->current.height * s)};
            opts.alpha = ctx->alpha;
            wlr_render_pass_add_texture(ctx->pass, &opts);
        }

        // Draw a `bw`-thick border frame just inside the edges of `box` (logical coords,
        // scaled to physical pixels by `scale`).
        void draw_border(wlr_render_pass* pass, const View::Box& logical, uint32_t rgba, int bw, float scale) {
            if (bw <= 0)
                return;
            wlr_render_color color = color_from_u32(rgba);
            const wlr_box box = scale_box({logical.x, logical.y, logical.width, logical.height}, scale);
            bw = (int)(bw * scale);
            const wlr_box rects[4] = {
                {box.x, box.y, box.width, bw},                                 // top
                {box.x, box.y + box.height - bw, box.width, bw},               // bottom
                {box.x, box.y + bw, bw, box.height - 2 * bw},                  // left
                {box.x + box.width - bw, box.y + bw, bw, box.height - 2 * bw}, // right
            };
            for (const wlr_box& r : rects) {
                wlr_render_rect_options opts = {};
                opts.box = r;
                opts.color = color;
                wlr_render_pass_add_rect(pass, &opts);
            }
        }

        // Render every mapped layer surface on a given layer (surface + its popups).
        // Layers are drawn opaque (alpha=nullptr); the surface's own alpha still applies.
        void render_layer(wlr_render_pass* pass, Server& server, int lyr, float scale) {
            for (LayerSurface* ls : server.layer_surfaces) {
                if (!ls->mapped || ls->handle->current.layer != (uint32_t)lyr)
                    continue;
                RenderContext ctx = {pass, ls->geo.x, ls->geo.y, scale, nullptr};
                wlr_layer_surface_v1_for_each_surface(ls->handle, render_surface, &ctx);
            }
        }

        void send_frame_done(wlr_surface* surface, int sx, int sy, void* data) {
            (void)sx;
            (void)sy;
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(data));
        }

        void output_handle_frame(wl_listener* listener, void* data) {
            Output* output = wl_container_of(listener, output, frame);
            (void)data;
            Server& server = *output->server;
            const Config& cfg = server.config;
            const float scale = output->handle->scale;

            wlr_output_state state;
            wlr_output_state_init(&state);

            wlr_render_pass* pass = wlr_output_begin_render_pass(output->handle, &state, nullptr);
            if (pass) {
                wlr_render_rect_options bg = {};
                bg.box = {0, 0, output->handle->width, output->handle->height};
                bg.color = {BG[0], BG[1], BG[2], BG[3]};
                wlr_render_pass_add_rect(pass, &bg);

                // Layer-shell backdrop (wallpapers, bottom bars) sits below windows.
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, scale);
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, scale);

                // Content (with opacity) + borders, bottom -> top. Rounded corners are
                // applied afterward by the GLES2 pass below.
                for (View* view : server.views) {
                    if (!view->mapped)
                        continue;
                    RenderContext ctx = {pass, view->box.x, view->box.y, scale, &cfg.opacity};
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, render_surface, &ctx);
                    uint32_t border = (view == server.focused_view) ? cfg.border_active : cfg.border_inactive;
                    draw_border(pass, view->box, border, cfg.border_width, scale);
                }

                // Top bars/panels and overlays (e.g. quickshell) sit above windows.
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_TOP, scale);
                render_layer(pass, server, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, scale);

                wlr_render_pass_submit(pass);
            }

            // Round window corners by overdrawing the corners with BG (GLES2-only).
            renderer::round_corners(server, state.buffer, output->handle->width, output->handle->height, BG, scale);

            wlr_output_commit_state(output->handle, &state);
            wlr_output_state_finish(&state);

            // Let clients know they may draw the next frame.
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            for (View* view : output->server->views) {
                if (view->mapped)
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, send_frame_done, &now);
            }
            for (LayerSurface* ls : output->server->layer_surfaces) {
                if (ls->mapped)
                    wlr_layer_surface_v1_for_each_surface(ls->handle, send_frame_done, &now);
            }
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

} // namespace fenriz::output
