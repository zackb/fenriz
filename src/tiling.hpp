#pragma once

#include <vector>

namespace fenriz {

    class Server;

    namespace tiling {

        struct Rect {
            int x, y, w, h;
        };

        // Pure master-stack geometry: place `n` tiles within the area [ax,ay,aw,ah],
        // inset by `gap` on the outside and separated by `gap`. The first tile is the
        // master (left column); the rest stack vertically in the right column. Kept
        // wlroots-free and header-only so it is unit-testable in isolation.
        inline std::vector<Rect> layout(int ax, int ay, int aw, int ah, int gap, int n) {
            std::vector<Rect> out;
            if (n <= 0)
                return out;

            const int ux = ax + gap, uy = ay + gap;
            const int uw = aw - 2 * gap, uh = ah - 2 * gap;

            if (n == 1) {
                out.push_back({ux, uy, uw, uh});
                return out;
            }

            const int master_w = (uw - gap) / 2;
            const int stack_w = uw - gap - master_w;
            out.push_back({ux, uy, master_w, uh});

            const int stack_x = ux + master_w + gap;
            const int stack_n = n - 1;
            const int each_h = (uh - (stack_n - 1) * gap) / stack_n;
            for (int i = 0; i < stack_n; ++i) {
                const int y = uy + i * (each_h + gap);
                // Last tile absorbs the rounding remainder so the column reaches the edge.
                const int h = (i == stack_n - 1) ? (uy + uh - y) : each_h;
                out.push_back({stack_x, y, stack_w, h});
            }
            return out;
        }

        // Apply the master-stack layout to server.views on the current output layout,
        // setting each View::box and requesting the toplevel resize.
        void arrange(Server& server);

    } // namespace tiling

} // namespace fenriz
