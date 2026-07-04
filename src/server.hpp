#pragma once

#include <list>
#include <wayland-server-core.h>

#include "config.hpp"

struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_output;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_xdg_shell;
struct wlr_cursor;
struct wlr_layer_shell_v1;
struct wlr_idle_notifier_v1;
struct wlr_xdg_decoration_manager_v1;

namespace fenriz {

    class Server;
    class View;
    struct LayerSurface;

    // Launch a shell command detached (`/bin/sh -c cmd`); no-op on empty. Used for
    // keybind `exec` actions and `exec-once` startup commands. Children are reaped via
    // SIGCHLD=SIG_IGN (set in Server::start).
    void spawn(const std::string& cmd);

    // POD wrapper so wl_container_of recovers the owning Server without taking an
    // offsetof into a non-standard-layout class.
    struct SignalListener {
        wl_listener listener;
        Server* server;
    };

    class Server {
    public:
        Server();
        ~Server();

        bool start(); // create backend/renderer/allocator/shells/seat, start backend
        void run();   // enter the wl_display event loop (blocks)
        void stop();

        Config config;
        std::list<View*> views;                   // bottom -> top (all workspaces)
        std::list<LayerSurface*> layer_surfaces;   // all layers; z-order resolved at render
        View* focused_view = nullptr;
        int active_workspace = 0; // 0-indexed; 10 workspaces (0..9)

        wl_display* display = nullptr;
        wlr_backend* backend = nullptr;
        wlr_renderer* renderer = nullptr;
        wlr_allocator* allocator = nullptr;
        wlr_output* output = nullptr; // primary output (fenriz is single-output; see layer::arrange)
        wlr_output_layout* output_layout = nullptr;
        wlr_seat* seat = nullptr;
        wlr_xdg_shell* xdg_shell = nullptr;
        wlr_layer_shell_v1* layer_shell = nullptr;
        wlr_idle_notifier_v1* idle_notifier = nullptr;
        wlr_xdg_decoration_manager_v1* xdg_decoration_manager = nullptr;
        wlr_cursor* cursor = nullptr;

        // Tiling region left after layer-shell exclusive zones (bars) are subtracted.
        struct {
            int x, y, width, height;
        } usable_area{};

        SignalListener l_new_output;
        SignalListener l_new_toplevel;
        SignalListener l_new_input;
        SignalListener l_new_layer_surface;
        SignalListener l_new_decoration;
    };

} // namespace fenriz
