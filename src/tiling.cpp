#include "tiling.hpp"

#include <algorithm>

#include "ipc.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::tiling {

    namespace {

        bool scrolling_ws(const Workspace& ws) { return ws.layout == Layout::Scrolling; }

        // The rect windows tile into on `out`: its usable area inset by a gap it can absorb.
        bool tile_area(Server& server, const output::Output* out, Rect& area, int& gap) {
            if (!out)
                return false;
            const output::Area a = output::usable(server, out);
            gap = std::clamp(server.config.gaps, 0, std::min(a.width, a.height) / 4);
            area = {a.x + gap, a.y + gap, a.width - 2 * gap, a.height - 2 * gap};
            return area.w > 0 && area.h > 0;
        }

        // The strip `v` tiles in, or null for a float or a dwindle workspace.
        scrolling::Strip* strip_of(Server& server, const View* v) {
            if (!v || v->floating)
                return nullptr;
            Workspace& ws = server.workspaces[v->workspace];
            return scrolling_ws(ws) ? &ws.strip : nullptr;
        }

        // The tile slot holding `v`, in whichever layout its workspace uses.
        View** slot(Server& server, View* v) {
            Workspace& ws = server.workspaces[v->workspace];
            if (scrolling_ws(ws))
                return scrolling::slot(ws.strip, v);
            Node* leaf = find_leaf(ws.root, v);
            return leaf ? &leaf->view : nullptr;
        }

        // Apply `edit` to the strip `v` tiles in; if it changed anything, keep `v` in view and
        // re-arrange. No-op for a float or a dwindle workspace.
        template <class Edit> void edit_strip(Server& server, View* v, Edit edit) {
            scrolling::Strip* s = strip_of(server, v);
            if (s && edit(*s)) {
                reveal(server, v);
                arrange(server);
            }
        }

        // The workspace `out` shows, if it scrolls.
        Workspace* shown_strip(Server& server, output::Output* out) {
            if (!out || out->active_ws < 0)
                return nullptr;
            Workspace& ws = server.workspaces[out->active_ws];
            return scrolling_ws(ws) ? &ws : nullptr;
        }

    } // namespace

    void insert(Server& server, View* v, View* focus) {
        if (v->flip_back)
            return; // the back of a flip pair has no tile of its own; it shadows its front
        Workspace& ws = server.workspaces[v->workspace];
        if (scrolling_ws(ws))
            scrolling::insert(ws.strip, v, focus, server.config.column_width);
        else
            tree_insert(ws.root, v, focus);
    }

    View* remove(Server& server, View* v) {
        Workspace& ws = server.workspaces[v->workspace];
        if (scrolling_ws(ws))
            return scrolling::remove(ws.strip, v);
        tree_remove(ws.root, v);
        return nullptr;
    }

    void replace(Server& server, View* old, View* nu) {
        if (View** s = slot(server, old))
            *s = nu;
    }

    bool has_tiles(const Workspace& ws) { return ws.root || !ws.strip.cols.empty(); }

    void swap(Server& server, View* a, View* b) {
        if (a == b)
            return;
        View** sa = slot(server, a);
        View** sb = slot(server, b);
        if (!sa || !sb)
            return;
        std::swap(*sa, *sb);

        if (a->workspace != b->workspace) {
            std::swap(a->workspace, b->workspace);
            view_update_output(server, a); // each landed on a new screen: re-announce scale
            view_update_output(server, b);
        }
        arrange(server);
    }

    void toggle_split(Server& server, View* v) {
        if (!v || v->floating)
            return;
        Node* leaf = find_leaf(server.workspaces[v->workspace].root, v);
        if (!leaf)
            return;
        flip_split(leaf);
        arrange(server);
    }

    void toggle_layout(Server& server, int n) {
        Workspace& ws = server.workspaces[n];
        if (scrolling_ws(ws)) {
            Rect area{0, 0, 1600, 900};
            int gap = 0;
            tile_area(server, ws.output, area, gap);
            for (View* v : scrolling::views_of(ws.strip)) {
                tree_insert(ws.root, v, nullptr);
                place(ws.root, area, gap);
            }
            ws.strip = {};
            ws.layout = Layout::Dwindle;
        } else {
            std::vector<Node*> leaves;
            collect_leaves(ws.root, leaves);
            std::vector<View*> views;
            for (Node* l : leaves)
                views.push_back(l->view);
            for (View* v : views)
                tree_remove(ws.root, v);
            ws.strip = scrolling::from_views(views, server.config.column_width);
            ws.layout = Layout::Scrolling;
            if (ws.last_focused)
                reveal(server, ws.last_focused);
        }
        arrange(server);
        ipc::publish(server);
    }

    void move(Server& server, View* v, int dx, int dy) {
        if (!v || v->floating || v->fullscreen)
            return;
        if (strip_of(server, v)) {
            edit_strip(server, v, [&](scrolling::Strip& s) {
                return dx ? scrolling::move_column(s, v, dx) : scrolling::move_in_column(s, v, dy);
            });
            return;
        }
        View* t = direction_target(server, v, dx, dy);
        if (t && !t->floating && !t->fullscreen)
            swap(server, v, t);
    }

    View* neighbor(Server& server, View* v, int dx, int dy) {
        scrolling::Strip* s = strip_of(server, v);
        return s ? scrolling::neighbor(*s, v, dx, dy) : nullptr;
    }

    bool reveal(Server& server, View* v) {
        scrolling::Strip* s = strip_of(server, v);
        if (!s)
            return false;
        const scrolling::Pos p = scrolling::find(*s, v);
        if (!p)
            return false;
        s->cols[p.col].focus_row = p.row;
        Rect area;
        int gap;
        if (!tile_area(server, view_output(server, v), area, gap))
            return false;
        return scrolling::ensure_visible(*s, p.col, area.w, gap);
    }

    void cycle_width(Server& server, View* v) {
        edit_strip(server, v, [&](scrolling::Strip& s) { return scrolling::cycle_width(s, v); });
    }

    void maximize_column(Server& server, View* v) {
        edit_strip(server, v, [&](scrolling::Strip& s) { return scrolling::toggle_maximize(s, v); });
    }

    void consume_or_expel(Server& server, View* v, int dir) {
        edit_strip(server, v, [&](scrolling::Strip& s) { return scrolling::consume_or_expel(s, v, dir); });
    }

    bool shows_strip(Server& server, output::Output* out) { return shown_strip(server, out) != nullptr; }

    bool scroll_by(Server& server, output::Output* out, double dx) {
        Workspace* ws = shown_strip(server, out);
        if (!ws)
            return false;

        ws->strip.scroll -= dx;
        server.layout_dirty = true;
        wlr_output_schedule_frame(out->handle);
        return true;
    }

    View* snap(Server& server, output::Output* out) {
        Workspace* ws = shown_strip(server, out);
        Rect area;
        int gap;
        if (!ws || !tile_area(server, out, area, gap))
            return nullptr;
        const int c = scrolling::nearest_column(ws->strip, area.w, gap);
        if (c < 0)
            return nullptr;
        ws->strip.scroll = scrolling::col_x(ws->strip, c, area.w, gap);
        arrange(server);
        return scrolling::focus_of(ws->strip.cols[c]);
    }

    void resize_split(Server& server, View* v, double dx, double dy) {
        if (scrolling::Strip* s = strip_of(server, v)) {
            Rect area;
            int gap;
            if (tile_area(server, view_output(server, v), area, gap))
                scrolling::resize(*s, v, dx, dy, area.w, area.h);
        } else if (Node* leaf = find_leaf(server.workspaces[v->workspace].root, v)) {
            resize_node(leaf, dx, dy); // the ratio math itself is pure
        } else {
            return;
        }

        // Don't arrange here. This runs once per cursor motion event
        server.layout_dirty = true;
    }

    void arrange(Server& server, bool animate) {
        auto apply = [&](View* view, const Rect& r, bool resize_only) {
            if (view->fullscreen || view->dragging)
                return;
            const View::Box old = view->box;
            view->box = {r.x, r.y, r.w, r.h};
            if (animate && server.config.animation_ms > 0 && old.width > 0)
                anim_slide(view, old.x - view->box.x, old.y - view->box.y);
            view_configure(view, resize_only); // size the client to the inner tile area (shell-agnostic)
        };

        // Lay out the workspace each output is showing, into that output's own usable area.
        for (output::Output* out : server.outputs) {
            if (out->active_ws < 0)
                continue;
            Workspace& ws = server.workspaces[out->active_ws];
            Rect area;
            int gap;
            if (!has_tiles(ws) || !tile_area(server, out, area, gap))
                continue; // empty workspace; the visibility sync below still runs

            if (scrolling_ws(ws)) {
                std::vector<std::pair<View*, Rect>> boxes;
                scrolling::place(ws.strip, area, gap, boxes);
                for (auto& [view, r] : boxes)
                    apply(view, r, true);
            } else {
                place(ws.root, area, gap);
                std::vector<Node*> leaves;
                collect_leaves(ws.root, leaves);
                for (Node* n : leaves)
                    apply(n->view, n->rect, false);
            }
        }

        // Push every view's box/anim/visibility into its scene nodes. Covers all workspaces
        // (not just the shown leaves above) so views on hidden workspaces get disabled and
        // a window just moved elsewhere stops rendering.
        for (View* view : server.views) {
            // The hidden half of a flip pair tracks its front's slot, so it is already the right
            // size and place the moment the two trade roles.
            if (view->flip_back && view->flip_peer) {
                view->box = view->flip_peer->box;
                view_configure(view);
            }
            // A float resize deferred its configure to here
            if (view->configure_pending) {
                view->configure_pending = false;
                view_configure(view);
            }
            if (view->fullscreen) {
                // Cover the output the window is on — not the whole layout, which would span
                // every monitor. A homeless workspace has no output: leave the box as-is.
                if (output::Output* out = view_output(server, view)) {
                    wlr_box full;
                    wlr_output_layout_get_box(server.output_layout, out->handle, &full);
                    view->box = {full.x, full.y, full.width, full.height};
                    view_configure(view); // fullscreen: bw=0, so the client fills the output
                }
            }
            place_view_nodes(view);
        }
    }

} // namespace fenriz::tiling
