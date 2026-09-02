#pragma once

#include <string>

namespace fenriz {

    class Server;

    // Keyboard cleaning mode: for `seconds`, every key, pointer, and touchpad event is dropped, so the hardware can be
    // wiped down. The only live bind is the one bound to Action::Cleaning, which toggles the mode
    namespace cleaning {

        void start(Server& server, int seconds);
        void stop(Server& server);

        // Human-readable chord for the bind that ends the mode ("SUPER+SHIFT+CTRL+C"),
        // empty when nothing is bound to it.
        std::string cancel_bind(const Server& server);

    } // namespace cleaning

} // namespace fenriz
