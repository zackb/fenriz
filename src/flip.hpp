#pragma once

#include <cmath>

namespace fenriz {

    class Server;
    class View;
    namespace output {
        struct Output;
    }

    // A flip pair: two windows sharing one tile, only one of them visible. The hidden half
    // ("back") is in no tiling tree and never floating on its own. Uses front's geometry.
    namespace flip {

        // Horizontal squash factor for a turn at progress t (0..1)
        inline double squash(double t) { return std::fabs(std::cos(M_PI * t)); }

        // Whether advancing from `prev` to `now` crossed the halfway pinch.
        inline bool past_pinch(double prev, double now) { return prev < 0.5 && now >= 0.5; }

        // Remember the focused window as the front of a pair-to-be.
        void mark(Server& server);

        // Attach the focused window as the back of the marked front.
        void pair(Server& server);

        // Turn the focused window's pair over. Pressing again mid-turn reverses it.
        void turn(Server& server);

        // Split the focused window's pair back into two windows.
        void unpair(Server& server);

        // Drop `view` from its pair before it leaves the tree (unmap).
        void forget(Server& server, View* view);

        // Advance the turns running on `output` by `dt` seconds. Returns true while one is still going.
        bool tick(Server& server, output::Output* output, double dt);

    } // namespace flip

} // namespace fenriz
