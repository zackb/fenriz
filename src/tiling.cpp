#include "tiling.hpp"

#include <algorithm>

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    void insert(Server& server, View* v, View* focus) { tree_insert(server.ws_roots[v->workspace], v, focus); }

    void remove(Server& server, View* v) { tree_remove(server.ws_roots[v->workspace], v); }

    void arrange(Server& server) {
        Node* root = server.ws_roots[server.active_workspace];
        if (!root)
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

        const int gap = server.config.gaps;
        place(root, {ax + gap, ay + gap, aw - 2 * gap, ah - 2 * gap}, gap);

        wlr_box out;
        wlr_output_layout_get_box(server.output_layout, nullptr, &out);
        const int bw = server.config.border_width;

        std::vector<Node*> leaves;
        collect_leaves(root, leaves);
        for (Node* n : leaves) {
            View* view = n->view;
            if (view->fullscreen) {
                // Fullscreen covers the whole output (no border inset); it keeps its tree
                // slot so it returns to the same tile when restored.
                view->box = {out.x, out.y, out.width, out.height};
                wlr_xdg_toplevel_set_size(view->toplevel, out.width, out.height);
                continue;
            }
            // view->box is the full tile (outer border edge); the client is sized to the
            // inner area so the border frames it and content stays off the rounded edge.
            view->box = {n->rect.x, n->rect.y, n->rect.w, n->rect.h};
            int cw = std::max(1, n->rect.w - 2 * bw);
            int ch = std::max(1, n->rect.h - 2 * bw);
            wlr_xdg_toplevel_set_size(view->toplevel, cw, ch);
        }
    }

} // namespace fenriz::tiling
