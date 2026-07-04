#pragma once

struct wlr_input_device;

namespace fenriz {

    class Server;
    class View;

    namespace cursor {
        // Create the cursor + xcursor manager and wire pointer/seat events.
        void init(Server& server);
        // Route a newly-attached pointer device's motion through the cursor.
        void attach_pointer(Server& server, wlr_input_device* device);
        // Cancel any interactive grab on `view` (called when it unmaps, before it's freed,
        // so a subsequent motion event can't dereference the dangling grab pointer).
        void forget_view(View* view);
    } // namespace cursor

} // namespace fenriz
