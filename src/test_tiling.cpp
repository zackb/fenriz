#include "tiling.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

using namespace fenriz;
using namespace fenriz::tiling;

namespace {
    // Fake View handles: the tree only uses View* as an identity tag, never derefs it.
    View* tag(int i) { return reinterpret_cast<View*>(static_cast<intptr_t>(i)); }

    // Insert `v` splitting `focus`, then recompute geometry over a 1000x1000 area / gap 10
    // (usable inset -> origin 10,10 size 980x980), as arrange() does after every map.
    void add(Node*& root, View* v, View* focus) {
        tree_insert(root, v, focus);
        place(root, {10, 10, 980, 980}, 10);
    }

    Rect box(Node* root, View* v) { return find_leaf(root, v)->rect; }
} // namespace

int main() {
    // Always focusing the newest window reproduces the classic dwindle spiral.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 980 && box(root, tag(1)).h == 980);

        add(root, tag(2), tag(1)); // split master -> left | right
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 485 && box(root, tag(1)).h == 980);
        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).w == 485 && box(root, tag(2)).h == 980);

        add(root, tag(3), tag(2));                                        // split right column top / bottom
        assert(box(root, tag(1)).w == 485 && box(root, tag(1)).h == 980); // master unchanged
        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).y == 10 && box(root, tag(2)).h == 485);
        assert(box(root, tag(3)).y == 505 && box(root, tag(3)).y + box(root, tag(3)).h == 990);

        add(root, tag(4), tag(3)); // bottom-right splits left | right
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 485 && box(root, tag(1)).h == 980);
        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).y == 10 && box(root, tag(2)).h == 485);
        assert(box(root, tag(3)).x == 505 && box(root, tag(3)).y == 505 && box(root, tag(3)).w == 237);
        assert(box(root, tag(4)).x == 752 && box(root, tag(4)).y == 505 && box(root, tag(4)).w == 238);
    }

    // Focus-aware: focusing the LEFT master and opening a window splits the LEFT column,
    // not the right.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1)); // 1 = left, 2 = right
        add(root, tag(3), tag(1)); // focus master (left) -> split it, not the right column

        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).w == 485); // right untouched
        // Left column (x < 505) now holds both 1 and 3, stacked (was wide -> vertical split...
        // left tile is 485x980, taller than wide -> stacked top/bottom).
        assert(box(root, tag(1)).x == 10 && box(root, tag(3)).x == 10);
        assert(box(root, tag(1)).y == 10 && box(root, tag(3)).y == 505);
    }

    // Closing a window: its sibling reclaims the whole freed tile.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1));
        tree_remove(root, tag(2));
        place(root, {10, 10, 980, 980}, 10);
        assert(box(root, tag(1)).w == 980 && box(root, tag(1)).h == 980); // back to full area
    }

    // Split ratio: bumping the root ratio shifts the boundary. Default 0.5 gives a 485px
    // left column; 0.75 gives (980-10)*0.75 = 727, pushing the divider right.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1)); // 1 | 2, vertical split at the root
        root->ratio = 0.75;
        place(root, {10, 10, 980, 980}, 10);
        assert(box(root, tag(1)).w == 727);
        assert(box(root, tag(2)).x == 10 + 727 + 10 && box(root, tag(2)).w == 980 - 10 - 727);
    }

    // Swap: exchanging two leaves' views trades their geometry.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1)); // 1 = left, 2 = right
        Rect left = box(root, tag(1)), right = box(root, tag(2));
        std::swap(find_leaf(root, tag(1))->view, find_leaf(root, tag(2))->view);
        assert(box(root, tag(1)).x == right.x); // 1 now sits where 2 was
        assert(box(root, tag(2)).x == left.x);
    }

    // Evacuation: a workspace moved to another output is re-placed into that output's area and
    // nothing else. The tree is never rebuilt, so topology and split ratios come through
    // untouched and the windows land in proportionally the same tiles — this is the property
    // that makes closing the lid safe. Geometry differs (different screen); structure doesn't.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1));
        add(root, tag(3), tag(2));
        root->ratio = 0.6; // a ratio the user dragged; must survive the move
        place(root, {10, 10, 980, 980}, 10);

        // Relative geometry on the laptop panel (1000x1000).
        const Rect a1 = box(root, tag(1)), a2 = box(root, tag(2)), a3 = box(root, tag(3));
        const double f1 = (double)a1.w / 980, f2 = (double)a2.w / 980;

        // Same tree, external monitor's area (2000x1200 at layout x=1000).
        place(root, {1010, 10, 1980, 1180}, 10);
        const Rect b1 = box(root, tag(1)), b2 = box(root, tag(2)), b3 = box(root, tag(3));

        // Same tiles, same proportions, dragged ratio intact.
        assert(std::abs((double)b1.w / 1980 - f1) < 0.01);
        assert(std::abs((double)b2.w / 1980 - f2) < 0.01);
        assert(root->ratio == 0.6);
        // Landed inside the new output's area (place() takes the already-inset area, so the
        // leftmost tile sits at area.x), and stayed in the same arrangement.
        assert(b1.x == 1010 && b1.x + b1.w <= 2990);
        assert(b2.x > b1.x && b3.x == b2.x && b3.y > b2.y);
        assert((a2.x > a1.x) == (b2.x > b1.x)); // left/right relationship preserved
        assert((a3.y > a2.y) == (b3.y > b2.y)); // above/below relationship preserved

        // And moving back reproduces the original geometry exactly — the lid-open case.
        place(root, {10, 10, 980, 980}, 10);
        assert(box(root, tag(1)).x == a1.x && box(root, tag(1)).w == a1.w);
        assert(box(root, tag(2)).x == a2.x && box(root, tag(2)).w == a2.w);
        assert(box(root, tag(3)).y == a3.y && box(root, tag(3)).h == a3.h);
    }

    // A gap wider than the area it has to fit in must not produce negative geometry.
    // place() feeds View::box, which reaches wlr_scene_rect_set_size and the surface clip;
    // a negative width there is meaningless to the renderer rather than merely ugly.
    // arrange() clamps the gap before calling us, so this is the second line of defense.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1));
        add(root, tag(3), tag(2));

        for (const Rect area : {Rect{0, 0, 20, 20}, Rect{0, 0, 1, 400}, Rect{0, 0, 0, 0}}) {
            place(root, area, 30);
            for (View* v : {tag(1), tag(2), tag(3)}) {
                const Rect r = box(root, v);
                assert(r.w >= 0 && r.h >= 0);
            }
        }
    }

    // Drag-to-resize. The load-bearing claim is that the divider follows the cursor
    // regardless of which tile you grabbed: dragging right from the LEFT tile and dragging
    // right from the RIGHT tile must both move the split right. Get the sign wrong for one
    // of them and half of all windows resize backwards — which reads as wobble, not a bug.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr); // left
        add(root, tag(2), tag(1));  // right (root is now a vertical split)
        assert(!root->leaf() && root->vertical);

        const double base = root->ratio;

        // Grabbing the left tile and dragging right grows the left tile.
        resize_node(find_leaf(root, tag(1)), +100, 0);
        assert(root->ratio > base);
        const double after_left = root->ratio;

        // Same drag from the right tile moves the divider the same way.
        root->ratio = base;
        resize_node(find_leaf(root, tag(2)), +100, 0);
        assert(root->ratio > base);
        assert(std::abs(root->ratio - after_left) < 1e-9);

        // Dragging left moves it back the other way.
        root->ratio = base;
        resize_node(find_leaf(root, tag(1)), -100, 0);
        assert(root->ratio < base);

        // The ratio is clamped: a tile can never be dragged out of existence.
        root->ratio = base;
        for (int i = 0; i < 50; i++)
            resize_node(find_leaf(root, tag(1)), +1000, 0);
        assert(root->ratio <= 0.9 && root->ratio >= 0.1);
        for (int i = 0; i < 50; i++)
            resize_node(find_leaf(root, tag(1)), -1000, 0);
        assert(root->ratio >= 0.1);

        // dy on a purely side-by-side tree has no stacked ancestor to move: a no-op, not a
        // stray write to the vertical split.
        root->ratio = base;
        resize_node(find_leaf(root, tag(1)), 0, +100);
        assert(root->ratio == base);

        // A lone window has no split at all — must not crash or invent one.
        Node* solo = nullptr;
        add(solo, tag(9), nullptr);
        resize_node(find_leaf(solo, tag(9)), +50, +50);
        assert(solo->leaf());
    }

    // The stacked axis, with a tree that has one of each: tag(3) splits tag(2) horizontally,
    // so tag(3)'s dy walks up to that stacked split while dx still finds the root.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1));
        add(root, tag(3), tag(2));

        Node* stacked = enclosing_split(find_leaf(root, tag(3)), false);
        assert(stacked && !stacked->vertical);
        const double v0 = root->ratio, h0 = stacked->ratio;

        resize_node(find_leaf(root, tag(3)), 0, +60); // +dy moves the stacked split down
        assert(stacked->ratio > h0);
        assert(root->ratio == v0); // and leaves the side-by-side split alone
    }

    std::printf("tiling layout: all assertions passed\n");
    return 0;
}
