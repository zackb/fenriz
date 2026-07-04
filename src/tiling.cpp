#include "tiling.hpp"

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    void arrange(Server& server) {
        if (server.views.empty())
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

        std::vector<Rect> rects = layout(ax, ay, aw, ah, server.config.gaps, (int)server.views.size());

        int i = 0;
        for (View* view : server.views) {
            const Rect& r = rects[i++];
            view->box = {r.x, r.y, r.w, r.h};
            wlr_xdg_toplevel_set_size(view->toplevel, r.w, r.h);
        }
    }

} // namespace fenriz::tiling
