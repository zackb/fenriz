#include "tiling.hpp"

#include <algorithm>

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    void insert(Server& server, View* v, View* focus) { tree_insert(server.workspaces[v->workspace].root, v, focus); }

    void remove(Server& server, View* v) { tree_remove(server.workspaces[v->workspace].root, v); }

    void swap(Server& server, View* a, View* b) {
        if (a == b)
            return;
        Node* la = find_leaf(server.workspaces[a->workspace].root, a);
        Node* lb = find_leaf(server.workspaces[b->workspace].root, b);
        if (!la || !lb)
            return;
        std::swap(la->view, lb->view);
        // The leaves may be in different workspaces' trees — dragging a window onto a tile on
        // another monitor is a swap across outputs. The views trade workspaces along with
        // their slots, or their workspace index would no longer match the tree they sit in.
        if (a->workspace != b->workspace) {
            std::swap(a->workspace, b->workspace);
            view_update_output(server, a); // each landed on a new screen: re-announce scale
            view_update_output(server, b);
        }
        arrange(server);
    }

    // Nearest ancestor of `leaf` whose split orientation matches `vertical`.
    static Node* enclosing_split(Node* leaf, bool vertical) {
        for (Node* n = leaf; n && n->parent; n = n->parent)
            if (n->parent->vertical == vertical)
                return n->parent;
        return nullptr;
    }

    void resize_split(Server& server, View* v, double dx, double dy) {
        Node* leaf = find_leaf(server.workspaces[v->workspace].root, v);
        if (!leaf)
            return;
        // The divider follows the cursor: +dx moves the vertical split right, +dy moves the
        // stacked split down, no matter which side `leaf` is on. (ratio is the fraction to
        // child[0], i.e. the left/top tile, so raising it grows that tile toward the cursor.)
        if (Node* h = enclosing_split(leaf, true); h && h->rect.w > 0)
            h->ratio = std::clamp(h->ratio + dx / h->rect.w, 0.1, 0.9);
        if (Node* w = enclosing_split(leaf, false); w && w->rect.h > 0)
            w->ratio = std::clamp(w->ratio + dy / w->rect.h, 0.1, 0.9);

        arrange(server, false);
    }

    void arrange(Server& server, bool animate) {
        // Lay out the workspace each output is showing, into that output's own usable area.
        // Workspaces that aren't shown anywhere keep their tree untouched — that's what lets a
        // layout survive a lid close and come back identical.
        for (output::Output* out : server.outputs) {
            if (out->active_ws < 0)
                continue;
            Node* root = server.workspaces[out->active_ws].root;
            if (!root)
                continue; // empty workspace; the visibility sync below still runs

            // Prefer the usable area left by this output's exclusive zones (bars); fall back to
            // its full box before any layer surface has reserved space.
            wlr_box full;
            wlr_output_layout_get_box(server.output_layout, out->handle, &full);
            const auto& u = out->usable_area;
            int ax = u.x, ay = u.y, aw = u.width, ah = u.height;
            if (aw <= 0 || ah <= 0) {
                ax = full.x, ay = full.y, aw = full.width, ah = full.height;
            }

            const int gap = server.config.gaps;
            place(root, {ax + gap, ay + gap, aw - 2 * gap, ah - 2 * gap}, gap);

            const int bw = server.config.border_width;
            std::vector<Node*> leaves;
            collect_leaves(root, leaves);
            for (Node* n : leaves) {
                View* view = n->view;
                if (view->fullscreen) {
                    // Fullscreen covers the output the window is on — not the whole layout,
                    // which would span every monitor. It keeps its tree slot so it returns to
                    // the same tile when restored.
                    view->box = {full.x, full.y, full.width, full.height};
                    wlr_xdg_toplevel_set_size(view->toplevel, full.width, full.height);
                    continue;
                }
                // view->box is the full tile (outer border edge); the client is sized to the
                // inner area so the border frames it and content stays off the rounded edge.
                // Feed the position delta into the render offset so the window slides from its
                // old slot to the new one; the offset decays to 0 in output.cpp. Skip windows
                // that weren't placed yet (new maps: box.width == 0) so they don't fly in.
                const View::Box old = view->box;
                view->box = {n->rect.x, n->rect.y, n->rect.w, n->rect.h};
                if (animate && server.config.animation_ms > 0 && old.width > 0) {
                    view->anim_ox += old.x - view->box.x;
                    view->anim_oy += old.y - view->box.y;
                }
                int cw = std::max(1, n->rect.w - 2 * bw);
                int ch = std::max(1, n->rect.h - 2 * bw);
                wlr_xdg_toplevel_set_size(view->toplevel, cw, ch);
            }
        }

        // Push every view's box/anim/visibility into its scene nodes. Covers all workspaces
        // (not just the shown leaves above) so views on hidden workspaces get disabled and
        // a window just moved elsewhere stops rendering — this must run even when every
        // shown workspace is empty.
        for (View* view : server.views)
            place_view_nodes(view);
    }

} // namespace fenriz::tiling
