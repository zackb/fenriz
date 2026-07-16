#pragma once

#include <cstdint>
#include <xkbcommon/xkbcommon.h>

struct wlr_input_device;

namespace fenriz {

    class Server;

    // Set up a newly-attached input device (keyboards handled here; pointers -> cursor).
    void handle_new_input(Server& server, wlr_input_device* device);

    // Look up (mods, sym) in the config bind table and run the matching action.
    // Returns the matched bind (key consumed) or nullptr to forward to the client.
    // The pointer is into server.config.binds — valid only until the next config reload.
    struct Bind;
    const Bind* handle_keybind(Server& server, uint32_t mods, xkb_keysym_t sym);

} // namespace fenriz
