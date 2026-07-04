#pragma once

namespace fenriz {

    class Server;

    namespace ipc {

        // Create the native fenriz control socket ($XDG_RUNTIME_DIR/fenriz-<disp>.sock,
        // exported as FENRIZ_SOCKET) and register it on the compositor event loop.
        // Streams newline-delimited JSON state snapshots and accepts one-line commands.
        // Call before spawning exec-once clients so they inherit FENRIZ_SOCKET.
        void init(Server& server);

        // Rebuild the state snapshot and, if it changed, broadcast it to every connected
        // client. Cheap and idempotent — call from any state-change site (focus, workspace
        // switch, view map/unmap/destroy).
        void publish(Server& server);

    } // namespace ipc

} // namespace fenriz
