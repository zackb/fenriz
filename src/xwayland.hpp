#pragma once

namespace fenriz {

    class Server;

    namespace xwayland {

        // Start the (lazy) XWayland server, export DISPLAY, and wire the new-surface handler
        // that adopts managed X11 toplevels as Views. On failure it logs and leaves
        // server.xwayland null (the rest of the compositor runs fine without X support).
        void setup(Server& server);

    } // namespace xwayland

} // namespace fenriz
