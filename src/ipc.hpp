#pragma once

struct wlr_surface;

namespace fenriz {

    class Server;

    namespace ipc {

        // Create the two native fenriz sockets and register them on the compositor event loop:
        // the control socket ($XDG_RUNTIME_DIR/fenriz-<disp>.sock, exported as FENRIZ_SOCKET),
        // and the read-only event socket (.events, FENRIZ_EVENT_SOCKET).
        void init(Server& server);

        // Rebuild the state snapshot and, if it changed, broadcast it to every connected
        // client. Cheap and idempotent — call from any state-change site (focus, workspace
        // switch, view map/unmap/destroy).
        void publish(Server& server);

        // Ring: broadcast one line on the event feed.
        void bell(Server& server, wlr_surface* surface);

        // Close both sockets, drop every connected client, and remove the socket files.
        void shutdown();

    } // namespace ipc

} // namespace fenriz
