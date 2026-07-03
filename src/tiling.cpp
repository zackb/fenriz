#include "tiling.hpp"

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    void arrange(Server& server) {
        if (server.views.empty())
            return;

        wlr_box area;
        wlr_output_layout_get_box(server.output_layout, nullptr, &area);

        std::vector<Rect> rects =
            layout(area.x, area.y, area.width, area.height, server.config.gaps, (int)server.views.size());

        int i = 0;
        for (View* view : server.views) {
            const Rect& r = rects[i++];
            view->box = { r.x, r.y, r.w, r.h };
            wlr_xdg_toplevel_set_size(view->toplevel, r.w, r.h);
        }
    }

} // namespace fenriz::tiling
