#include "server.hpp"

#include <csignal>
#include <cstdlib>
#include <unistd.h>

#include "cursor.hpp"
#include "decoration.hpp"
#include "ipc.hpp"
#include "keyboard.hpp"
#include "layer.hpp"
#include "lock.hpp"
#include "output.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz {

    namespace {

        void on_new_toplevel(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            // View registers its own map/unmap/destroy listeners and deletes itself on
            // destroy, so the raw new is intentional (not a leak).
            new View(*sl->server, static_cast<wlr_xdg_toplevel*>(data));
        }

        // A client created an xdg popup (menu, tooltip, nested submenu). Parent it into the
        // parent surface's scene tree so it renders above the parent and tracks its position.
        // The parent's scene tree is stashed in xdg_surface->data (by view map and here for
        // nested popups). Layer-shell popups have a non-xdg parent and are handled in layer.cpp.
        void on_new_popup(wl_listener* listener, void* data) {
            (void)listener;
            auto* popup = static_cast<wlr_xdg_popup*>(data);
            wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
            if (!parent || !parent->data)
                return;
            auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
            popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
        }

        void on_new_input(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_new_input(*sl->server, static_cast<wlr_input_device*>(data));
        }

        // Clipboard: a client with keyboard focus asks to own the selection. Honor it so
        // copy/paste works between clients. Same shape for the primary (middle-click) one.
        void on_set_selection(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_set_selection_event*>(data);
            wlr_seat_set_selection(sl->server->seat, ev->source, ev->serial);
        }

        void on_set_primary_selection(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
            wlr_seat_set_primary_selection(sl->server->seat, ev->source, ev->serial);
        }

        void on_request_start_drag(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_start_drag_event*>(data);
            wlr_seat* seat = sl->server->seat;
            if (wlr_seat_validate_pointer_grab_serial(seat, ev->origin, ev->serial))
                wlr_seat_start_pointer_drag(seat, ev->drag, ev->serial);
            else if (ev->drag->source)
                wlr_data_source_destroy(ev->drag->source);
        }

        void on_set_gamma(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_gamma_control_manager_v1_set_gamma_event*>(data);
            // Applied on the next frame (output.cpp); mark dirty + wake the output so the
            // frame handler commits even though the scene itself needs no repaint.
            sl->server->gamma_dirty = true;
            wlr_output_schedule_frame(ev->output);
        }

        // wlr-output-power-management: a shell/idle daemon (wlopm, hypridle) toggles DPMS.
        // Shares set_dpms with the IPC `dpms` command.
        void on_output_power(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_output_power_v1_set_mode_event*>(data);
            output::set_dpms(*sl->server, ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
        }

    } // namespace

    void spawn(const std::string& cmd) {
        if (cmd.empty())
            return;
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(1);
        }
    }

    Server::Server() { config = Config::load(); }

    Server::~Server() {
        if (display) {
            wl_display_destroy_clients(display);
            wl_display_destroy(display);
        }
        // ponytail: backend/renderer/allocator leak at process exit — add explicit
        // teardown (wlr_backend_destroy etc.) if fenriz ever restarts in-process.
    }

    bool Server::start() {
        // Reap exec'd children (see keyboard spawn) without wait() bookkeeping.
        signal(SIGCHLD, SIG_IGN);

        display = wl_display_create();
        wl_event_loop* loop = wl_display_get_event_loop(display);

        backend = wlr_backend_autocreate(loop, nullptr);
        if (!backend) {
            wlr_log(WLR_ERROR, "failed to create backend");
            return false;
        }

        renderer = wlr_renderer_autocreate(backend);
        if (!renderer) {
            wlr_log(WLR_ERROR, "failed to create renderer");
            return false;
        }
        wlr_renderer_init_wl_display(renderer, display);

        allocator = wlr_allocator_autocreate(backend, renderer);
        if (!allocator) {
            wlr_log(WLR_ERROR, "failed to create allocator");
            return false;
        }

        wlr_compositor_create(display, 5, renderer);
        wlr_subcompositor_create(display);
        wlr_data_device_manager_create(display);

        // HiDPI: viewporter + fractional-scale let clients submit correctly-sized buffers
        // for a fractional output scale (see config.scale / layer::arrange rendering).
        wlr_viewporter_create(display);
        wlr_fractional_scale_manager_v1_create(display, 1);

        output_layout = wlr_output_layout_create(display);

        // Scene graph must exist before outputs connect (handle_new_output creates a
        // scene-output per wlr_output). Trees are created in bottom -> top z-order.
        scene = wlr_scene_create();
        scene_layout = wlr_scene_attach_output_layout(scene, output_layout);
        scene_background = wlr_scene_tree_create(&scene->tree);
        scene_bottom = wlr_scene_tree_create(&scene->tree);
        scene_tiles = wlr_scene_tree_create(&scene->tree);
        scene_floating = wlr_scene_tree_create(&scene->tree);
        scene_top = wlr_scene_tree_create(&scene->tree);
        scene_fullscreen = wlr_scene_tree_create(&scene->tree);
        scene_overlay = wlr_scene_tree_create(&scene->tree);
        scene_lock = wlr_scene_tree_create(&scene->tree);

        // linux-dmabuf lets GPU clients (QtQuick/quickshell, browsers) share their GPU
        // buffers zero-copy instead of falling back to SHM (a per-frame GPU->CPU->upload
        // treadmill that burns CPU on both sides). Wiring it into the scene also enables
        // direct scanout for fullscreen. presentation-time gives clients accurate frame
        // pacing so they throttle to vblank instead of rendering continuously.
        wlr_linux_dmabuf_v1* dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer);
        wlr_scene_set_linux_dmabuf_v1(scene, dmabuf);
        wlr_presentation_create(display, backend, 2);
        wlr_single_pixel_buffer_manager_v1_create(display);
        wlr_content_type_manager_v1_create(display, 1);

        output::register_handlers(*this);

        xdg_shell = wlr_xdg_shell_create(display, 3);
        l_new_toplevel.server = this;
        l_new_toplevel.listener.notify = on_new_toplevel;
        wl_signal_add(&xdg_shell->events.new_toplevel, &l_new_toplevel.listener);
        l_new_popup.server = this;
        l_new_popup.listener.notify = on_new_popup;
        wl_signal_add(&xdg_shell->events.new_popup, &l_new_popup.listener);

        seat = wlr_seat_create(display, "seat0");
        wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
        l_new_input.server = this;
        l_new_input.listener.notify = on_new_input;
        wl_signal_add(&backend->events.new_input, &l_new_input.listener);

        // Clipboard / selection: data_device_manager (above) needs these seat handlers to
        // actually move selections between clients, plus the primary (middle-click) manager
        // and data-control (wl-clipboard / clipboard managers).
        l_set_selection.server = this;
        l_set_selection.listener.notify = on_set_selection;
        wl_signal_add(&seat->events.request_set_selection, &l_set_selection.listener);
        l_set_primary_selection.server = this;
        l_set_primary_selection.listener.notify = on_set_primary_selection;
        wl_signal_add(&seat->events.request_set_primary_selection, &l_set_primary_selection.listener);
        l_start_drag.server = this;
        l_start_drag.listener.notify = on_request_start_drag;
        wl_signal_add(&seat->events.request_start_drag, &l_start_drag.listener);
        wlr_primary_selection_v1_device_manager_create(display);
        wlr_data_control_manager_v1_create(display);
        wlr_ext_data_control_manager_v1_create(display, 1);

        // Let external tools see the display and windows, grab screenshots, tune gamma.
        wlr_xdg_output_manager_v1_create(display, output_layout);
        wlr_screencopy_manager_v1_create(display);
        foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(display);
        gamma_control_manager = wlr_gamma_control_manager_v1_create(display);
        l_set_gamma.server = this;
        l_set_gamma.listener.notify = on_set_gamma;
        wl_signal_add(&gamma_control_manager->events.set_gamma, &l_set_gamma.listener);

        // DPMS control for shells/idle daemons (also reachable via the IPC `dpms` command).
        output_power_manager = wlr_output_power_manager_v1_create(display);
        l_output_power.server = this;
        l_output_power.listener.notify = on_output_power;
        wl_signal_add(&output_power_manager->events.set_mode, &l_output_power.listener);

        cursor::init(*this);

        layer::init(*this);
        lock::init(*this);
        decoration::init(*this);

        const char* socket = wl_display_add_socket_auto(display);
        if (!socket) {
            wlr_log(WLR_ERROR, "failed to create wayland socket");
            return false;
        }
        if (!wlr_backend_start(backend)) {
            wlr_log(WLR_ERROR, "failed to start backend");
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, true);
        wlr_log(WLR_INFO, "fenriz running on WAYLAND_DISPLAY=%s", socket);

        // Control socket (FENRIZ_SOCKET) — needs WAYLAND_DISPLAY set, and must be up before
        // exec_once so bars/tools spawned below inherit the env and can connect immediately.
        ipc::init(*this);

        // Export configured env vars (QT_QPA_PLATFORMTHEME) before spawning anything,
        // so exec-once clients inherit them. Set after WAYLAND_DISPLAY/FENRIZ_SOCKET so a
        // stray `env` line can't shadow those.
        for (const auto& [name, value] : config.env)
            setenv(name.c_str(), value.c_str(), 1);

        // Run startup commands now that the socket is live and WAYLAND_DISPLAY is set,
        // so the spawned clients connect to us.
        for (const std::string& cmd : config.exec_once)
            spawn(cmd);

        // DBG: exercise workspace switching without a physical keyboard. Cycles
        // ws0 -> ws1 (empty) -> ws0 on a timer so a headless run can verify filtering/focus.
        if (getenv("FENRIZ_DBG_WORKSPACES")) {
            static wl_event_source* t = wl_event_loop_add_timer(
                loop,
                [](void* data) -> int {
                    Server* s = static_cast<Server*>(data);
                    int next = s->active_workspace == 0 ? 1 : 0;
                    wlr_log(WLR_INFO, "fenriz DBG: timer switching to ws %d", next);
                    set_workspace(*s, next);
                    return 0;
                },
                this);
            wl_event_source_timer_update(t, 2500);
        }

        return true;
    }

    void Server::run() { wl_display_run(display); }

    void Server::stop() {
        if (display)
            wl_display_terminate(display);
    }

} // namespace fenriz
