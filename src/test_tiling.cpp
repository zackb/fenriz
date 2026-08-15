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
                for (const int bw : {0, 2, 40}) {
                    const Rect in = inner_box(r, bw);
                    assert(in.w >= 0 && in.h >= 0);
                    assert(in.w <= r.w && in.h <= r.h);
                }
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

    // togglesplit: the insert-time orientation is a guess from the tile's aspect, and this is
    // the user's override. Flipping a pair swaps which axis it divides, and flipping twice is
    // the identity — a key you press to fix a layout must also undo itself.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1)); // wide area -> side-by-side
        assert(box(root, tag(1)).x == 10 && box(root, tag(2)).x == 505);

        flip_split(find_leaf(root, tag(2)));
        place(root, {10, 10, 980, 980}, 10);
        // Now stacked: both span the full width, one above the other.
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 980 && box(root, tag(1)).y == 10);
        assert(box(root, tag(2)).x == 10 && box(root, tag(2)).w == 980 && box(root, tag(2)).y == 505);

        flip_split(find_leaf(root, tag(1))); // either leaf of the pair reaches the same split
        place(root, {10, 10, 980, 980}, 10);
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 485);
        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).w == 485);
    }

    // The flip hits the focused leaf's *immediate* parent, not the root: flipping deep in the
    // tree must not reshuffle the whole workspace.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        add(root, tag(2), tag(1));
        add(root, tag(3), tag(2)); // right column splits top / bottom

        flip_split(find_leaf(root, tag(3)));
        place(root, {10, 10, 980, 980}, 10);

        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 485 && box(root, tag(1)).h == 980);
        // 2 and 3 now divide the right column side-by-side instead of stacked.
        assert(box(root, tag(2)).x == 505 && box(root, tag(2)).h == 980);
        assert(box(root, tag(3)).x == 752 && box(root, tag(3)).h == 980);
    }

    // A lone window is the root leaf: no split to flip, and no null deref on the way to
    // finding that out.
    {
        Node* root = nullptr;
        add(root, tag(1), nullptr);
        flip_split(find_leaf(root, tag(1)));
        flip_split(nullptr); // and a view that isn't in this tree at all
        place(root, {10, 10, 980, 980}, 10);
        assert(box(root, tag(1)).x == 10 && box(root, tag(1)).w == 980 && box(root, tag(1)).h == 980);
    }

    // fit_content: a client that won't fill its tile gets drawn centered in it, with the
    // frame hugging the content. This is what keeps the border and the glow off empty space.
    {
        const Rect tile{0, 0, 1000, 800};

        // 400x300 of content + a 2px border on each side = a 404x304 frame, centered.
        const Rect f = fit_content(tile, 400, 300, 2);
        assert(f.w == 404 && f.h == 304);
        // Equal margin on both sides is the whole point — an off-by-one here reads as the
        // window being nudged up-left.
        assert(f.x - tile.x == (tile.x + tile.w) - (f.x + f.w));
        assert(f.y - tile.y == (tile.y + tile.h) - (f.y + f.h));

        // A client that fills its tile is untouched: no drift on the common path.
        const Rect full = fit_content(tile, 1000 - 2 * 2, 800 - 2 * 2, 2);
        assert(full.x == tile.x && full.y == tile.y && full.w == tile.w && full.h == tile.h);

        // A client bigger than its tile (min size beats the layout) is capped, not grown —
        // the frame must never spill onto a neighbor.
        const Rect big = fit_content(tile, 5000, 5000, 2);
        assert(big.w == tile.w && big.h == tile.h && big.x == tile.x && big.y == tile.y);

        // Nothing committed yet: keep the tile rather than collapsing to a 2*bw stub.
        const Rect none = fit_content(tile, 0, 0, 2);
        assert(none.w == tile.w && none.h == tile.h);

        // Borderless config, and a tile offset in layout coords (the multi-output case).
        const Rect off = fit_content({1920, 100, 600, 400}, 200, 100, 0);
        assert(off.w == 200 && off.h == 100);
        assert(off.x == 1920 + 200 && off.y == 100 + 150);
    }

    // clamp_size: never ask a client for a size it has told us it will refuse.
    {
        assert(clamp_size(1000, 0, 400) == 400); // max clamps down
        assert(clamp_size(100, 300, 0) == 300);  // min pushes up
        assert(clamp_size(500, 0, 0) == 500);    // no hints declared: unchanged
        // A client declaring min > max is contradicting itself; the floor is the safer of
        // the two — undersizing is what makes clients refuse and leave the tile ragged.
        assert(clamp_size(500, 800, 400) == 800);
        assert(clamp_size(0, 0, 0) == 1);  // never configure a zero-size window
        assert(clamp_size(-5, 0, 0) == 1); // place() can hand us a collapsed tile
    }

    std::printf("tiling layout: all assertions passed\n");
    return 0;
}
