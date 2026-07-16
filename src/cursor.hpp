#pragma once

struct wlr_input_device;

namespace fenriz {

    class Server;
    class View;
    namespace output {
        struct Output;
    }

    namespace cursor {
        // Create the cursor + xcursor manager and wire pointer/seat events.
        void init(Server& server);
        // Route a newly-attached pointer device's motion through the cursor.
        void attach_pointer(Server& server, wlr_input_device* device);
        // Cancel any interactive grab on `view` (called when it unmaps, before it's freed,
        // so a subsequent motion event can't dereference the dangling grab pointer).
        void forget_view(View* view);
        // Put the cursor in the middle of an output. Used when focus crosses screens, so the
        // pointer doesn't stay behind on the one the user just left.
        void warp_to_output(Server& server, output::Output* o);
        // Pull the cursor back inside the layout if the output it was on just went away —
        // otherwise it's stranded at coordinates that no longer exist and no click lands.
        void clamp_to_layout(Server& server);
    } // namespace cursor

} // namespace fenriz
