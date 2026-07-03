#include "output.hpp"

#include <ctime>

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
            int x, y;           // view origin in output-local coordinates
            const float* alpha; // per-window opacity, or nullptr for opaque
        };

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
            wlr_render_texture_options opts = {};
            opts.texture = texture;
            opts.dst_box = {ctx->x + sx, ctx->y + sy, (int)texture->width, (int)texture->height};
            opts.alpha = ctx->alpha;
            wlr_render_pass_add_texture(ctx->pass, &opts);
        }

        // Draw a `bw`-thick border frame just inside the edges of `box`.
        void draw_border(wlr_render_pass* pass, const View::Box& box, uint32_t rgba, int bw) {
            if (bw <= 0)
                return;
            wlr_render_color color = color_from_u32(rgba);
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

            wlr_output_state state;
            wlr_output_state_init(&state);

            wlr_render_pass* pass = wlr_output_begin_render_pass(output->handle, &state, nullptr);
            if (pass) {
                wlr_render_rect_options bg = {};
                bg.box = {0, 0, output->handle->width, output->handle->height};
                bg.color = {BG[0], BG[1], BG[2], BG[3]};
                wlr_render_pass_add_rect(pass, &bg);

                // Content (with opacity) + borders, bottom -> top. Rounded corners are
                // applied afterward by the GLES2 pass below.
                for (View* view : server.views) {
                    if (!view->mapped)
                        continue;
                    RenderContext ctx = {pass, view->box.x, view->box.y, &cfg.opacity};
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, render_surface, &ctx);
                    uint32_t border = (view == server.focused_view) ? cfg.border_active : cfg.border_inactive;
                    draw_border(pass, view->box, border, cfg.border_width);
                }

                wlr_render_pass_submit(pass);
            }

            // Round window corners by overdrawing the corners with BG (GLES2-only).
            renderer::round_corners(server, state.buffer, output->handle->width, output->handle->height, BG);

            wlr_output_commit_state(output->handle, &state);
            wlr_output_state_finish(&state);

            // Let clients know they may draw the next frame.
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            for (View* view : output->server->views) {
                if (view->mapped)
                    wlr_xdg_surface_for_each_surface(view->toplevel->base, send_frame_done, &now);
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
