#include "keyboard.hpp"

#include <algorithm>
#include <vector>

#include "config.hpp"
#include "cursor.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz {

    void execute_bind(Server& server, const Bind& b); // defined below; run a matched bind's action

    namespace {

        // Per-keyboard state. Standard-layout so wl_container_of recovers it cleanly.
        struct Keyboard {
            Server* server;
            wlr_keyboard* kb;
            wl_listener key;
            wl_listener modifiers;
            wl_listener destroy;
        };

        // A lid/tablet switch device. The lid is why this exists: reading it here means the
        // clamshell response happens synchronously inside the compositor, instead of a script
        // polling /proc/acpi and racing the monitor events.
        struct Switch {
            Server* server;
            wl_listener toggle;
            wl_listener destroy;
        };

        void switch_handle_toggle(wl_listener* listener, void* data) {
            Switch* sw = wl_container_of(listener, sw, toggle);
            auto* event = static_cast<wlr_switch_toggle_event*>(data);
            if (event->switch_type != WLR_SWITCH_TYPE_LID)
                return;
            sw->server->lid_closed = event->switch_state == WLR_SWITCH_STATE_ON;
            wlr_log(WLR_INFO, "fenriz: lid %s", sw->server->lid_closed ? "closed" : "opened");
            // Only ever disables/restores the internal panel when docked; suspend is logind's
            // job (its HandleLidSwitch default), so there's deliberately no suspend here.
            output::refresh(*sw->server);
        }

        void switch_handle_destroy(wl_listener* listener, void* data) {
            Switch* sw = wl_container_of(listener, sw, destroy);
            (void)data;
            wl_list_remove(&sw->toggle.link);
            wl_list_remove(&sw->destroy.link);
            delete sw;
        }

        void new_switch(Server& server, wlr_input_device* device) {
            wlr_switch* handle = wlr_switch_from_input_device(device);
            Switch* sw = new Switch{};
            sw->server = &server;
            sw->toggle.notify = switch_handle_toggle;
            wl_signal_add(&handle->events.toggle, &sw->toggle);
            sw->destroy.notify = switch_handle_destroy;
            wl_signal_add(&device->events.destroy, &sw->destroy);
        }

        void cycle_focus(Server& server, int dir) {
            // Cycle among the windows currently on screen — which, with more than one output,
            // spans every shown workspace rather than just one.
            std::vector<View*> vis;
            for (View* v : server.views)
                if (view_visible(server, v))
                    vis.push_back(v);
            if (vis.size() < 2)
                return;
            auto it = std::find(vis.begin(), vis.end(), server.focused_view);
            if (it == vis.end()) {
                focus_view(server, vis.front());
                return;
            }
            size_t i = it - vis.begin();
            i = (dir > 0) ? (i + 1) % vis.size() : (i + vis.size() - 1) % vis.size();
            focus_view(server, vis[i]);
        }

        void keyboard_handle_modifiers(wl_listener* listener, void* data) {
            Keyboard* keyboard = wl_container_of(listener, keyboard, modifiers);
            (void)data;
            wlr_seat_set_keyboard(keyboard->server->seat, keyboard->kb);
            wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->kb->modifiers);
        }

        // Stop any in-progress held-key repeat.
        void stop_repeat(Server& server) {
            if (server.repeat_timer)
                wl_event_source_timer_update(server.repeat_timer, 0);
            server.repeat_keycode = 0;
        }

        // Timer tick while a `binde` key is held: re-run the action, re-arm at the rate.
        int keybind_repeat_cb(void* data) {
            Server& server = *static_cast<Server*>(data);
            if (server.locked || server.repeat_keycode == 0)
                return 0;
            execute_bind(server, server.repeat_bind);
            wl_event_source_timer_update(server.repeat_timer, 1000 / server.config.repeat_rate);
            return 0;
        }

        // Begin repeating bind `b` (a `binde`) held under `keycode`. Copies the bind so it
        // survives a config hot-reload that replaces config.binds mid-hold.
        void start_repeat(Server& server, const Bind& b, uint32_t keycode) {
            if (!server.repeat_timer) {
                wl_event_loop* loop = wl_display_get_event_loop(server.display);
                server.repeat_timer = wl_event_loop_add_timer(loop, keybind_repeat_cb, &server);
            }
            server.repeat_bind = b;
            server.repeat_keycode = keycode;
            wl_event_source_timer_update(server.repeat_timer, server.config.repeat_delay);
        }

        // Does the focused surface hold an active keyboard-shortcuts inhibitor?
        bool shortcuts_inhibited(Server& server) {
            if (!server.shortcuts_inhibit_manager)
                return false;
            wlr_surface* focused = server.seat->keyboard_state.focused_surface;
            if (!focused)
                return false;
            wlr_keyboard_shortcuts_inhibitor_v1* inhibitor;
            wl_list_for_each(inhibitor, &server.shortcuts_inhibit_manager->inhibitors, link) {
                if (inhibitor->surface == focused && inhibitor->active)
                    return true;
            }
            return false;
        }

        void keyboard_handle_key(wl_listener* listener, void* data) {
            Keyboard* keyboard = wl_container_of(listener, keyboard, key);
            auto* event = static_cast<wlr_keyboard_key_event*>(data);
            Server& server = *keyboard->server;
            wlr_keyboard* kb = keyboard->kb;

            wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

            const uint32_t keycode = event->keycode + 8; // libinput -> xkb keycode
            const uint32_t mods = wlr_keyboard_get_modifiers(kb);

            // Releasing (or locking on) the held key ends its repeat.
            if (server.locked || (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && keycode == server.repeat_keycode))
                stop_repeat(server);

            bool handled = false;
            // While locked, compositor keybinds are disabled — every key goes to the lock
            // surface so the user can type their password (and can't switch workspace etc.).
            if (!server.locked && !shortcuts_inhibited(server) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                // Match binds against base-level (unshifted) keysyms so a bind like
                // "SUPER SHIFT, E" resolves to XKB_KEY_e, matching the config parser.
                xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
                const xkb_keysym_t* syms;
                int nsyms = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode, layout, 0, &syms);
                for (int i = 0; i < nsyms; i++) {
                    if (const Bind* b = handle_keybind(server, mods, syms[i])) {
                        handled = true;
                        // A repeating bind arms the timer; any other bind cancels a stale repeat.
                        if (b->repeat)
                            start_repeat(server, *b, keycode);
                        else
                            stop_repeat(server);
                        break;
                    }
                }
            }

            if (!handled) {
                wlr_seat_set_keyboard(server.seat, kb);
                wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
            }
        }

        void keyboard_handle_destroy(wl_listener* listener, void* data) {
            Keyboard* keyboard = wl_container_of(listener, keyboard, destroy);
            (void)data;
            wl_list_remove(&keyboard->key.link);
            wl_list_remove(&keyboard->modifiers.link);
            wl_list_remove(&keyboard->destroy.link);
            delete keyboard;
        }

        void new_keyboard(Server& server, wlr_input_device* device) {
            wlr_keyboard* kb = wlr_keyboard_from_input_device(device);
            // A virtual keyboard (wtype/ydotool/wayvnc) supplies its own keymap
            const bool virt = wlr_input_device_get_virtual_keyboard(device) != nullptr;

            if (!virt) {
                // Compile the default keymap from XKB_DEFAULT_* env / system defaults.
                xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
                // A bad XKB_DEFAULT_* in the env yields a null keymap; set_keymap would assert.
                if (keymap)
                    wlr_keyboard_set_keymap(kb, keymap);
                else
                    wlr_log(WLR_ERROR, "fenriz: keymap compile failed (check XKB_DEFAULT_*)");
                xkb_keymap_unref(keymap);
                xkb_context_unref(ctx);
            }
            wlr_keyboard_set_repeat_info(kb, 60, 200);

            Keyboard* keyboard = new Keyboard{};
            keyboard->server = &server;
            keyboard->kb = kb;
            keyboard->key.notify = keyboard_handle_key;
            wl_signal_add(&kb->events.key, &keyboard->key);
            keyboard->modifiers.notify = keyboard_handle_modifiers;
            wl_signal_add(&kb->events.modifiers, &keyboard->modifiers);
            keyboard->destroy.notify = keyboard_handle_destroy;
            wl_signal_add(&device->events.destroy, &keyboard->destroy);

            // Handing a keymap-less keyboard to the seat makes it broadcast an invalid keymap fd that clients can't
            // mmap
            if (!virt)
                wlr_seat_set_keyboard(server.seat, kb);
        }

        // An inhibitor may only take effect while its own surface has keyboard focus
        void sync_inhibitors(Server& server) {
            wlr_surface* focused = server.seat->keyboard_state.focused_surface;
            wlr_keyboard_shortcuts_inhibitor_v1* inhibitor;
            wl_list_for_each(inhibitor, &server.shortcuts_inhibit_manager->inhibitors, link) {
                const bool want = inhibitor->surface == focused;
                if (want && !inhibitor->active)
                    wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
                else if (!want && inhibitor->active)
                    wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inhibitor);
            }
        }

        // Keyboard focus moved. This is the seat's own signal rather than a hook in view.cpp's focus_surface
        void on_keyboard_focus_change(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            (void)data;
            sync_inhibitors(*sl->server);
        }

        void on_new_inhibitor(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            (void)data;
            // wlroots has already linked it into manager->inhibitors, and drops it there on destroy
            sync_inhibitors(*sl->server);
        }

        void on_new_virtual_keyboard(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* vkbd = static_cast<wlr_virtual_keyboard_v1*>(data);
            // Straight into the normal keyboard path: it's a wlr_keyboard like any other,
            // and new_keyboard leaves its client-supplied keymap alone.
            handle_new_input(*sl->server, &vkbd->keyboard.base);
        }

    } // namespace

    void init_keyboard(Server& server) {
        // virtual-keyboard: wtype/ydotool/wayvnc and on-screen keyboards synthesize keys.
        server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.display);
        server.l_new_virtual_keyboard.server = &server;
        server.l_new_virtual_keyboard.listener.notify = on_new_virtual_keyboard;
        wl_signal_add(&server.virtual_keyboard_manager->events.new_virtual_keyboard,
                      &server.l_new_virtual_keyboard.listener);

        // keyboard-shortcuts-inhibit: let a focused VM / remote-desktop client swallow binds.
        server.shortcuts_inhibit_manager = wlr_keyboard_shortcuts_inhibit_v1_create(server.display);
        server.l_new_inhibitor.server = &server;
        server.l_new_inhibitor.listener.notify = on_new_inhibitor;
        wl_signal_add(&server.shortcuts_inhibit_manager->events.new_inhibitor, &server.l_new_inhibitor.listener);

        server.l_keyboard_focus_change.server = &server;
        server.l_keyboard_focus_change.listener.notify = on_keyboard_focus_change;
        wl_signal_add(&server.seat->keyboard_state.events.focus_change, &server.l_keyboard_focus_change.listener);
    }

    void handle_new_input(Server& server, wlr_input_device* device) {
        switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            new_keyboard(server, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            cursor::attach_pointer(server, device);
            break;
        case WLR_INPUT_DEVICE_SWITCH:
            new_switch(server, device); // the laptop lid
            break;
        default:
            // Touch/tablet devices are not handled yet.
            break;
        }
    }

    void execute_bind(Server& server, const Bind& b) {
        switch (b.action) {
        case Action::Exec:
            spawn(b.arg);
            break;
        case Action::KillActive:
            if (server.focused_view)
                view_close(server.focused_view);
            break;
        case Action::Exit:
            server.stop();
            break;
        case Action::FocusNext:
            cycle_focus(server, +1);
            break;
        case Action::FocusPrev:
            cycle_focus(server, -1);
            break;
        case Action::FocusLeft:
            focus_direction(server, -1, 0);
            break;
        case Action::FocusRight:
            focus_direction(server, 1, 0);
            break;
        case Action::FocusUp:
            focus_direction(server, 0, -1);
            break;
        case Action::FocusDown:
            focus_direction(server, 0, 1);
            break;
        case Action::ToggleLayout:
            // TODO: alternate layouts once more than master-stack exists.
            break;
        case Action::Fullscreen:
            toggle_fullscreen(server);
            break;
        case Action::ToggleFloat:
            toggle_floating(server);
            break;
        case Action::Pin:
            toggle_pin(server);
            break;
        case Action::Workspace:
        case Action::MoveToWorkspace: {
            int n = 0;
            try {
                n = std::stoi(b.arg);
            } catch (...) {
                n = 0;
            }
            if (n >= 1 && n <= 10) {
                if (b.action == Action::Workspace)
                    set_workspace(server, n - 1);
                else
                    move_focused_to_workspace(server, n - 1);
            }
            break;
        }
        case Action::None:
            break;
        }
    }

    const Bind* handle_keybind(Server& server, uint32_t mods, xkb_keysym_t sym) {
        for (const Bind& b : server.config.binds) {
            if (b.mods != mods || b.sym != sym)
                continue;
            execute_bind(server, b);
            return &b;
        }
        return nullptr;
    }

} // namespace fenriz
