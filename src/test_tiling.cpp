#include "tiling.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

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
    // not the right — the regression this whole change fixes.
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

    std::printf("tiling layout: all assertions passed\n");
    return 0;
}
