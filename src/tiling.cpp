#include "tiling.hpp"

#include <algorithm>

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    void arrange(Server& server) {
        // Only windows on the active workspace are tiled/shown. A fullscreen view is
        // pulled out of the tiled set and sized to the whole output instead.
        std::vector<View*> visible;
        View* fs = nullptr;
        for (View* view : server.views)
            if (view_visible(server, view)) {
                if (view->fullscreen)
                    fs = view; // ponytail: last one wins if two are somehow fullscreen
                else
                    visible.push_back(view);
            }

        if (fs) {
            wlr_box out;
            wlr_output_layout_get_box(server.output_layout, nullptr, &out);
            fs->box = {out.x, out.y, out.width, out.height};
            wlr_xdg_toplevel_set_size(fs->toplevel, out.width, out.height); // no border inset
        }
        if (visible.empty())
            return;

        // Prefer the usable area left by layer-shell exclusive zones (bars); fall back to
        // the full output layout before any layer surface has reserved space.
        auto& u = server.usable_area;
        int ax = u.x, ay = u.y, aw = u.width, ah = u.height;
        if (aw <= 0 || ah <= 0) {
            wlr_box area;
            wlr_output_layout_get_box(server.output_layout, nullptr, &area);
            ax = area.x, ay = area.y, aw = area.width, ah = area.height;
        }

        std::vector<Rect> rects = layout(ax, ay, aw, ah, server.config.gaps, (int)visible.size());

        // view->box is the full tile (outer border edge); the client is sized to the inner
        // area so the border frames it and content doesn't run under the rounded edge.
        const int bw = server.config.border_width;
        int i = 0;
        for (View* view : visible) {
            const Rect& r = rects[i++];
            view->box = {r.x, r.y, r.w, r.h};
            int cw = std::max(1, r.w - 2 * bw);
            int ch = std::max(1, r.h - 2 * bw);
            wlr_xdg_toplevel_set_size(view->toplevel, cw, ch);
        }
    }

} // namespace fenriz::tiling
