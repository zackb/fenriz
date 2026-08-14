#pragma once

struct wlr_drag;
struct wlr_surface;

namespace fenriz {

    class Server;
    class View;

    // xdg-toplevel-drag-v1: a client attaches a toplevel to an in-flight drag-and-drop so the
    // compositor moves the window with the cursor
    namespace toplevel_drag {

        void init(Server& server);

        // A drag just started. `origin` is the surface it was started from.
        void begin(Server& server, wlr_drag* drag, wlr_surface* origin);

        // The window being dragged, or null when no drag has one attached.
        View* attached(Server& server);

        // Move the attached window to the cursor.
        void track(Server& server);

        // A window that maps while attached joins the drag floating, instead.
        void adopt(View* view);

    } // namespace toplevel_drag

} // namespace fenriz
