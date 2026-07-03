#pragma once

struct wlr_input_device;

namespace fenriz {

    class Server;

    namespace cursor {
        // Create the cursor + xcursor manager and wire pointer/seat events.
        void init(Server& server);
        // Route a newly-attached pointer device's motion through the cursor.
        void attach_pointer(Server& server, wlr_input_device* device);
    } // namespace cursor

} // namespace fenriz
