#pragma once

#include <algorithm>
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

        // Vertical strips a turning window is drawn as: enough that the stepped edge reads as a straight slope.
        constexpr int STRIPS = 32;

        // Camera distance from the nearest edge of a turning window, in half-widths. Smaller is a stronger
        // perspective.
        constexpr double DEPTH = 3.0;

        // Where a point of a window turning about its vertical axis lands on screen.
        struct Projected {
            double x;     // horizontal offset from the frame centre, in half-widths
            double scale; // height at that point, relative to the flat window
        };

        // Project frame-relative `x` (-1 left edge .. 1 right edge) at turn progress t (0..1). The front face turns
        // away until the pinch at 0.5, then the back face turns in with the opposite edge receding. A reversed turn
        // runs t backwards over the same face, so it keeps the receding edge. The nearest edge is drawn at full
        // size, so the turning window never draws outside its own frame.
        inline Projected project(double t, double x, bool reversed = false) {
            const double turn = M_PI * std::min(t, 1.0 - t);          // 0 flat .. pi/2 edge-on
            const double side = (t < 0.5) != reversed ? 1.0 : -1.0; // +1: right edge recedes
            const double depth = (side * x + 1.0) * std::sin(turn);
            const double s = DEPTH / (DEPTH + depth);
            return {x * std::cos(turn) * s, s};
        }

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
