#pragma once

namespace fenriz {

    class Server;

    namespace output {
        // Register the backend's new_output listener.
        void register_handlers(Server& server);
    } // namespace output

} // namespace fenriz
