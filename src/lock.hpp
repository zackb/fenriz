#pragma once

struct wlr_surface;
struct wlr_output;

namespace fenriz {

    class Server;

    namespace lock {

        // Create the ext-session-lock-v1 global and wire the new-lock handler. This is
        // what makes a session locker (quickshell's WlSessionLock, swaylock, …) able to
        // lock: without it the client can't lock and the screen never covers.
        void init(Server& server);

        // Force-unlock the session escape hatch (reachable via the IPC `unlock` command)
        // for when the lock client crashes or hangs
        void force_unlock(Server& server);

    } // namespace lock

} // namespace fenriz
