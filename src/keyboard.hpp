#pragma once

#include <cstdint>
#include <initializer_list>
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

    // Calls f on the level-0 keysyms of kc in the active layout, then in layout 0, binds
    // still fire under a non-Latin layout. Stops at the first sym for which f returns true.
    template <typename F> bool for_each_bind_sym(xkb_state* state, xkb_keycode_t kc, F&& f) {
        xkb_keymap* km = xkb_state_get_keymap(state);
        const xkb_layout_index_t active = xkb_state_key_get_layout(state, kc);
        for (const xkb_layout_index_t layout : {active, xkb_layout_index_t{0}}) {
            const xkb_keysym_t* syms;
            const int n = xkb_keymap_key_get_syms_by_level(km, kc, layout, 0, &syms);
            for (int i = 0; i < n; i++)
                if (f(syms[i]))
                    return true;
            if (active == 0)
                break;
        }
        return false;
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
