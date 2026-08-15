#pragma once

#include <algorithm>
#include <vector>

namespace fenriz {

    class Server;
    class View;

    namespace tiling {

        struct Rect {
            int x, y, w, h;
        };

        // Focus-aware dwindle BSP tree. Each leaf holds a View; each internal node splits
        // its area between two children by `ratio` (default 50/50, moved by drag-to-resize;
        // vertical = side-by-side, else stacked). A new
        // window bisects the *focused* window's tile, so focus on the left column grows the
        // left column — unlike a stateless spiral, which always subdivides the last tile.
        struct Node {
            View* view = nullptr; // leaf payload (may be null in geometry-only tests)
            Node* parent = nullptr;
            Node* child[2] = {nullptr, nullptr};
            bool vertical = true; // internal nodes: true => child[0] | child[1]
            double ratio = 0.5;   // internal nodes: fraction of the split going to child[0]
            Rect rect{};          // last computed geometry (leaves consumed by arrange)

            bool leaf() const { return child[0] == nullptr; }
        };

        inline Node* find_leaf(Node* n, View* v) {
            if (!n)
                return nullptr;
            if (n->leaf())
                return n->view == v ? n : nullptr;
            Node* r = find_leaf(n->child[0], v);
            return r ? r : find_leaf(n->child[1], v);
        }

        // Deepest leaf down the child[1] (spiral) side; the append point when no focus hint.
        inline Node* last_leaf(Node* n) {
            while (n && !n->leaf())
                n = n->child[1];
            return n;
        }

        inline void collect_leaves(Node* n, std::vector<Node*>& out) {
            if (!n)
                return;
            if (n->leaf())
                out.push_back(n);
            else {
                collect_leaves(n->child[0], out);
                collect_leaves(n->child[1], out);
            }
        }

        // Split `focus`'s tile in two: it keeps one half, `v` takes the other. Orientation
        // follows the focused tile's current aspect (wider -> side-by-side). First window
        // becomes the root. Falls back to the spiral tail if focus isn't in this tree.
        inline void tree_insert(Node*& root, View* v, View* focus) {
            Node* leaf = new Node{};
            leaf->view = v;
            if (!root) {
                root = leaf;
                return;
            }
            Node* f = find_leaf(root, focus);
            if (!f)
                f = last_leaf(root);
            Node* parent = f->parent;
            Node* split = new Node{};
            split->vertical = f->rect.w >= f->rect.h;
            split->parent = parent;
            split->child[0] = f;
            split->child[1] = leaf;
            f->parent = split;
            leaf->parent = split;
            if (!parent)
                root = split;
            else
                parent->child[parent->child[0] == f ? 0 : 1] = split;
        }

        // Remove `v`'s leaf; its sibling takes over the parent's slot (reclaiming the space).
        inline void tree_remove(Node*& root, View* v) {
            Node* leaf = find_leaf(root, v);
            if (!leaf)
                return;
            Node* parent = leaf->parent;
            if (!parent) {
                root = nullptr;
                delete leaf;
                return;
            }
            Node* sibling = parent->child[0] == leaf ? parent->child[1] : parent->child[0];
            Node* grand = parent->parent;
            sibling->parent = grand;
            if (!grand)
                root = sibling;
            else
                grand->child[grand->child[0] == parent ? 0 : 1] = sibling;
            delete leaf;
            delete parent;
        }

        // Recursively assign geometry: split `area` along each internal node's axis (minus a
        // `gap`) and recurse; leaves get their whole area. Pure (no wlroots) and testable.
        inline void place(Node* n, Rect area, int gap) {
            if (!n)
                return;

            n->rect = area;

            if (n->leaf())
                return;

            if (n->vertical) {
                const int inner = std::max(0, area.w - gap);
                const int lw = std::clamp((int)(inner * n->ratio), 0, inner);
                place(n->child[0], {area.x, area.y, lw, area.h}, gap);
                place(n->child[1], {area.x + lw + gap, area.y, inner - lw, area.h}, gap);
            } else {
                const int inner = std::max(0, area.h - gap);
                const int th = std::clamp((int)(inner * n->ratio), 0, inner);
                place(n->child[0], {area.x, area.y, area.w, th}, gap);
                place(n->child[1], {area.x, area.y + th + gap, area.w, inner - th}, gap);
            }
        }

        // Shrink `tile` onto the content the client actually committed (border on each side) and center the shortfall
        inline Rect fit_content(Rect tile, int content_w, int content_h, int bw) {
            Rect r = tile;
            if (content_w > 0)
                r.w = std::clamp(content_w + 2 * bw, 0, tile.w);
            if (content_h > 0)
                r.h = std::clamp(content_h + 2 * bw, 0, tile.h);
            r.x += (tile.w - r.w) / 2;
            r.y += (tile.h - r.h) / 2;
            return r;
        }

        // The content area inside a tile's border: `tile` inset by `bw` on every side.
        inline Rect inner_box(Rect tile, int bw) {
            return {tile.x + bw, tile.y + bw, std::max(0, tile.w - 2 * bw), std::max(0, tile.h - 2 * bw)};
        }

        // A size the client will actually accept
        inline int clamp_size(int want, int min, int max) {
            if (max > 0)
                want = std::min(want, max);
            if (min > 0)
                want = std::max(want, min);
            return std::max(1, want);
        }

        // Nearest ancestor of `leaf` whose split orientation matches `vertical`.
        inline Node* enclosing_split(Node* leaf, bool vertical) {
            for (Node* n = leaf; n && n->parent; n = n->parent)
                if (n->parent->vertical == vertical)
                    return n->parent;
            return nullptr;
        }

        // Flip the split holding `leaf`
        inline void flip_split(Node* leaf) {
            if (leaf && leaf->parent)
                leaf->parent->vertical = !leaf->parent->vertical;
        }

        // Drag the splits around `leaf` by a cursor delta in px.
        inline void resize_node(Node* leaf, double dx, double dy) {
            if (Node* h = enclosing_split(leaf, true); h && h->rect.w > 0)
                h->ratio = std::clamp(h->ratio + dx / h->rect.w, 0.1, 0.9);
            if (Node* v = enclosing_split(leaf, false); v && v->rect.h > 0)
                v->ratio = std::clamp(v->ratio + dy / v->rect.h, 0.1, 0.9);
        }

        // Insert/remove a view in the tree for its own workspace (server.ws_roots).
        void insert(Server& server, View* v, View* focus);
        void remove(Server& server, View* v);

        // Trade two tiled views' positions in place (swaps the leaves' view pointers),
        // then re-arrange. No-op if either view isn't a leaf on the active workspace.
        void swap(Server& server, View* a, View* b);

        // Flip the orientation of the split holding `v` then re-arrange.
        void toggle_split(Server& server, View* v);

        // resize_node() for the leaf holding `v`, then flag the layout dirty so the frame
        // handler re-arranges once per frame rather than once per motion event.
        void resize_split(Server& server, View* v, double dx, double dy);

        // Apply the dwindle layout to server.views on the active workspace, setting each
        // View::box and requesting the toplevel resize. `animate` feeds the position delta
        // into the slide-into-place offset; pass false for changes that must land instantly
        // (an interactive resize tracks the cursor 1:1; a workspace switch appears in place).
        void arrange(Server& server, bool animate = true);

    } // namespace tiling

} // namespace fenriz
