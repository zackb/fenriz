#pragma once

#include <list>
#include <wayland-server-core.h>

#include "config.hpp"

struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_xdg_shell;
struct wlr_cursor;

namespace fenriz {

    class Server;
    class View;

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
        std::list<View*> views; // bottom -> top
        View* focused_view = nullptr;

        wl_display* display = nullptr;
        wlr_backend* backend = nullptr;
        wlr_renderer* renderer = nullptr;
        wlr_allocator* allocator = nullptr;
        wlr_output_layout* output_layout = nullptr;
        wlr_seat* seat = nullptr;
        wlr_xdg_shell* xdg_shell = nullptr;
        wlr_cursor* cursor = nullptr;

        SignalListener l_new_output;
        SignalListener l_new_toplevel;
        SignalListener l_new_input;
    };

} // namespace fenriz
