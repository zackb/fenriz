#pragma once

namespace fenriz {

    class Server;

    namespace output {
        // Register the backend's new_output listener.
        void register_handlers(Server& server);

        // Power the primary output off/on (DPMS). Backs both the wlr-output-power-management
        // protocol and the IPC `dpms` command. No-op if there's no output yet.
        void set_dpms(Server& server, bool on);
    } // namespace output

} // namespace fenriz
