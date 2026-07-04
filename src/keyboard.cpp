#include "keyboard.hpp"

#include <algorithm>
#include <vector>

#include "config.hpp"
#include "cursor.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz {

    namespace {

        // Per-keyboard state. Standard-layout so wl_container_of recovers it cleanly.
        struct Keyboard {
            Server* server;
            wlr_keyboard* kb;
            wl_listener key;
            wl_listener modifiers;
            wl_listener destroy;
        };

        void cycle_focus(Server& server, int dir) {
            // Only cycle among windows on the active workspace.
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

        void keyboard_handle_key(wl_listener* listener, void* data) {
            Keyboard* keyboard = wl_container_of(listener, keyboard, key);
            auto* event = static_cast<wlr_keyboard_key_event*>(data);
            Server& server = *keyboard->server;
            wlr_keyboard* kb = keyboard->kb;

            wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

            const uint32_t keycode = event->keycode + 8; // libinput -> xkb keycode
            const uint32_t mods = wlr_keyboard_get_modifiers(kb);

            bool handled = false;
            // While locked, compositor keybinds are disabled — every key goes to the lock
            // surface so the user can type their password (and can't switch workspace etc.).
            if (!server.locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                // Match binds against base-level (unshifted) keysyms so a bind like
                // "SUPER SHIFT, E" resolves to XKB_KEY_e, matching the config parser.
                xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
                const xkb_keysym_t* syms;
                int nsyms = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode, layout, 0, &syms);
                for (int i = 0; i < nsyms; i++) {
                    if (handle_keybind(server, mods, syms[i])) {
                        handled = true;
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

            // Compile the default keymap from XKB_DEFAULT_* env / system defaults.
            xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
            wlr_keyboard_set_keymap(kb, keymap);
            xkb_keymap_unref(keymap);
            xkb_context_unref(ctx);
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

            wlr_seat_set_keyboard(server.seat, kb);
        }

    } // namespace

    void handle_new_input(Server& server, wlr_input_device* device) {
        switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            new_keyboard(server, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            cursor::attach_pointer(server, device);
            break;
        default:
            // Touch/tablet/switch devices are not handled yet.
            break;
        }
    }

    bool handle_keybind(Server& server, uint32_t mods, xkb_keysym_t sym) {
        for (const Bind& b : server.config.binds) {
            if (b.mods != mods || b.sym != sym)
                continue;

            switch (b.action) {
            case Action::Exec:
                spawn(b.arg);
                break;
            case Action::KillActive:
                if (server.focused_view)
                    wlr_xdg_toplevel_send_close(server.focused_view->toplevel);
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
            return true;
        }
        return false;
    }

} // namespace fenriz
