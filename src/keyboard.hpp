#pragma once

#include <cstdint>
#include <xkbcommon/xkbcommon.h>

struct wlr_input_device;

namespace fenriz {

    class Server;

    // Create the keyboard-side protocol globals (virtual-keyboard, shortcuts-inhibit).
    void init_keyboard(Server& server);

    // Set up a newly-attached input device (keyboards handled here; pointers -> cursor).
    void handle_new_input(Server& server, wlr_input_device* device);

    inline unsigned vt_for_keysym(xkb_keysym_t sym) {
        if (sym < XKB_KEY_XF86Switch_VT_1 || sym > XKB_KEY_XF86Switch_VT_12)
            return 0;
        return sym - XKB_KEY_XF86Switch_VT_1 + 1;
    }

    // Look up (mods, sym) in the config bind table. The pointer is into server.config.binds
    struct Bind;
    const Bind* find_bind(Server& server, uint32_t mods, xkb_keysym_t sym);

    // find_bind plus running the action. Returns the matched bind (key consumed) or
    // nullptr to forward to the client.
    const Bind* handle_keybind(Server& server, uint32_t mods, xkb_keysym_t sym);

    // Run a bind's action.
    void execute_bind(Server& server, const Bind& b);

    // Does the focused surface hold an active keyboard-shortcuts inhibitor?
    bool shortcuts_inhibited(Server& server);

} // namespace fenriz
