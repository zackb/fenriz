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

        // The mapped lock surface to draw fullscreen on `output` while locked, or nullptr
        // when there's no surface yet or the lock client died — the caller then leaves the
        // screen blank (a locked compositor must never reveal normal content).
        wlr_surface* surface_for(Server& server, wlr_output* output);

        // (Sub)surface of the lock UI under layout point (lx,ly), for routing the pointer
        // while locked; *sx,*sy return surface-local coords on hit. nullptr if none.
        wlr_surface* surface_at(Server& server, double lx, double ly, double* sx, double* sy);

    } // namespace lock

} // namespace fenriz
