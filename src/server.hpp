#pragma once

#include <list>
#include <string>
#include <wayland-server-core.h>

#include "config.hpp"
#include "output.hpp"

struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_output;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_compositor;
struct wlr_xwayland;
struct wlr_xdg_shell;
struct wlr_cursor;
struct wlr_layer_shell_v1;
struct wlr_idle_notifier_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_output_power_manager_v1;
struct wlr_xdg_activation_v1;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_output_manager_v1;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_tree;
struct wlr_xdg_popup;

namespace fenriz {

    class Server;
    class View;
    struct LayerSurface;
    namespace tiling {
        struct Node;
    }

    // A workspace: a dwindle BSP tree (tiling.hpp) plus which output it lives on.
    //
    // `output` is where the workspace currently *lives*; the output shows it only if it is
    // that output's Output::active_ws. `home` is the output name it's configured to prefer
    // (`workspace = N, NAME`), so it returns there whenever that output is present.
    //
    // `origin` is the screen it was involuntarily evacuated off — set only when a screen goes
    // away with this workspace on it, and honored (then cleared) when that screen returns. It
    // is what makes a lid cycle round-trip with no config at all; see assign_workspaces.
    //
    // Output events only ever reassign `output`/`origin`. `root` and the views' workspace
    // index are invariant across hotplug/lid — that's what makes a layout survive a lid close.
    struct Workspace {
        tiling::Node* root = nullptr;
        output::Output* output = nullptr; // null = unassigned (no screen); tree is kept
        View* last_focused = nullptr;     // restored on workspace return; cleared when it dies
        std::string home;                 // configured output name; empty = no preference
        std::string origin;               // output it was evacuated off; empty = none
    };

    // Launch a shell command detached (`/bin/sh -c cmd`); no-op on empty. Used for
    // keybind `exec` actions and `exec-once` startup commands. Double-forks so the command
    // reparents to init, fenriz keeps no long-lived children and never sets SIGCHLD to
    // SIG_IGN (which would leak into Xwayland and break its keymap compile).
    void spawn(const std::string& cmd);

    // Re-read fenriz.conf and apply it live
    void reload_config(Server& server);

    // Add an xdg popup (menu, tooltip, submenu) to the scene under `parent_tree`, and answer
    // its initial commit with a configure, without one the client never maps it and the popup
    // is never drawn. Callers: the xdg-shell handler (server.cpp) and layer-shell (layer.cpp).
    void popup_create(Server& server, wlr_xdg_popup* popup, wlr_scene_tree* parent_tree);

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
        std::list<View*> views;                  // bottom -> top (all workspaces)
        std::list<LayerSurface*> layer_surfaces; // all layers; z-order resolved at render
        std::list<output::Output*> outputs;      // live outputs, in the order they appeared
        View* focused_view = nullptr;

        // Lid state, from the libinput switch device (keyboard.cpp). Drives
        // output::apply_lid_policy; fenriz only acts on it when docked (suspend is logind's).
        bool lid_closed = false;

        // ext-session-lock-v1 engaged: normal content is blanked and input is routed
        // only to the lock surface. Owned by src/lock.cpp; other modules just read it.
        bool locked = false;

        // A client (wlsunset/gammastep) changed the gamma LUT. Setting gamma doesn't damage
        // the scene, so the frame handler must commit even when the scene needs no repaint.
        bool gamma_dirty = false;

        // The workspaces (see Workspace above). Tree nodes leak at shutdown.
        Workspace workspaces[WS_COUNT];

        int inotify_fd = -1; // watches the config dir for hot-reload; closed in ~Server

        // Held-key repeat for `binde` binds. One timer for the seat.
        wl_event_source* repeat_timer = nullptr;
        Bind repeat_bind;
        // xkb keycode currently held; 0 = idle.
        uint32_t repeat_keycode = 0;

        wl_display* display = nullptr;
        wlr_backend* backend = nullptr;
        wlr_compositor* compositor = nullptr; // stored: wlr_xwayland_create needs it
        wlr_xwayland* xwayland = nullptr;     // null if XWayland failed to start
        wlr_renderer* renderer = nullptr;
        wlr_allocator* allocator = nullptr;
        wlr_output_layout* output_layout = nullptr;
        wlr_seat* seat = nullptr;
        wlr_xdg_shell* xdg_shell = nullptr;
        wlr_layer_shell_v1* layer_shell = nullptr;
        wlr_idle_notifier_v1* idle_notifier = nullptr;
        wlr_idle_inhibit_manager_v1* idle_inhibit_manager = nullptr;
        int active_inhibitors = 0; // live zwp_idle_inhibitor_v1 count; >0 => idle inhibited
        wlr_xdg_decoration_manager_v1* xdg_decoration_manager = nullptr;
        wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager = nullptr;
        wlr_gamma_control_manager_v1* gamma_control_manager = nullptr;
        wlr_output_power_manager_v1* output_power_manager = nullptr;
        wlr_xdg_activation_v1* xdg_activation = nullptr;
        wlr_virtual_keyboard_manager_v1* virtual_keyboard_manager = nullptr;
        wlr_keyboard_shortcuts_inhibit_manager_v1* shortcuts_inhibit_manager = nullptr;
        // ext-foreign-toplevel-list: the standardized taskbar protocol. List-only (no
        // activate/close), so it supplements foreign_toplevel_manager rather than replacing
        // it — both globals are live and every view carries a handle for each.
        wlr_ext_foreign_toplevel_list_v1* ext_foreign_toplevel_list = nullptr;
        // wlr-output-management: kanshi/wlr-randr. An apply is folded into config.outputs
        // (see store_head in output.cpp), so there's no second source of truth to reconcile.
        wlr_output_manager_v1* output_manager = nullptr;
        wlr_cursor* cursor = nullptr;

        // Screen zoom (modifier + scroll magnifier). zoom is the current, animated level;
        // zoom_target is what it eases toward. 1.0 = off (fast direct render path).
        float zoom = 1.0f;
        float zoom_target = 1.0f;

        // Scene graph: the render + damage-tracking layer. Idle outputs commit nothing,
        // so the compositor sleeps instead of repainting every vblank. The trees below
        // are direct children of scene->tree in bottom -> top z-order; views/layers/lock
        // parent their nodes into the matching one.
        wlr_scene* scene = nullptr;
        wlr_scene_output_layout* scene_layout = nullptr;
        wlr_scene_tree* scene_background = nullptr;
        wlr_scene_tree* scene_bottom = nullptr;
        wlr_scene_tree* scene_tiles = nullptr;    // normal windows
        wlr_scene_tree* scene_floating = nullptr; // floats, above tiles / below top layer
        wlr_scene_tree* scene_top = nullptr;
        wlr_scene_tree* scene_fullscreen = nullptr; // above top, below overlay
        wlr_scene_tree* scene_overlay = nullptr;
        wlr_scene_tree* scene_lock = nullptr; // ext-session-lock, above everything

        SignalListener l_new_output;
        SignalListener l_new_xwayland_surface;
        SignalListener l_new_toplevel;
        SignalListener l_new_popup;
        SignalListener l_new_input;
        SignalListener l_new_layer_surface;
        SignalListener l_new_decoration;
        SignalListener l_set_selection;
        SignalListener l_set_primary_selection;
        SignalListener l_start_drag;
        SignalListener l_set_gamma;
        SignalListener l_output_power;
        SignalListener l_new_idle_inhibitor;
        SignalListener l_activation_request;
        SignalListener l_new_virtual_keyboard;
        SignalListener l_new_inhibitor;
        SignalListener l_keyboard_focus_change;
        SignalListener l_output_apply;
        SignalListener l_output_test;
    };

} // namespace fenriz
