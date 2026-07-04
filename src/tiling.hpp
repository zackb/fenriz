#pragma once

#include <vector>

namespace fenriz {

    class Server;

    namespace tiling {

        struct Rect {
            int x, y, w, h;
        };

        // Recursive dwindle bisection: place one tile in half the rect (split along the
        // longer axis, minus a gap) and recurse the rest into the other half. Produces
        // Hyprland's "spiral" instead of a fixed master column.
        // ponytail: stateless spiral by spawn order, not a persistent focus-aware BSP
        // tree — real Hyprland splits at the focused leaf and remembers manual resize
        // ratios; add a tree only if focus-aware insertion/resizing is wanted later.
        inline void dwindle(std::vector<Rect>& out, int x, int y, int w, int h, int n, int gap) {
            if (n <= 0)
                return;
            if (n == 1) {
                out.push_back({x, y, w, h});
                return;
            }
            if (w >= h) { // wider -> split left | right
                const int lw = (w - gap) / 2, rw = w - gap - lw;
                out.push_back({x, y, lw, h});
                dwindle(out, x + lw + gap, y, rw, h, n - 1, gap);
            } else { // taller -> split top / bottom
                const int th = (h - gap) / 2, bh = h - gap - th;
                out.push_back({x, y, w, th});
                dwindle(out, x, y + th + gap, w, bh, n - 1, gap);
            }
        }

        // Pure dwindle geometry: place `n` tiles within the area [ax,ay,aw,ah], inset by
        // `gap` on the outside and separated by `gap`. Kept wlroots-free and header-only
        // so it is unit-testable in isolation.
        inline std::vector<Rect> layout(int ax, int ay, int aw, int ah, int gap, int n) {
            std::vector<Rect> out;
            dwindle(out, ax + gap, ay + gap, aw - 2 * gap, ah - 2 * gap, n, gap);
            return out;
        }

        // Apply the master-stack layout to server.views on the current output layout,
        // setting each View::box and requesting the toplevel resize.
        void arrange(Server& server);

    } // namespace tiling

} // namespace fenriz
