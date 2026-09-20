#include "flip.hpp"

#include "tiling.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace fenriz;
using namespace fenriz::flip;

namespace {
    // The tree only uses View* as an identity tag, never derefs it.
    View* tag(int i) { return reinterpret_cast<View*>(static_cast<intptr_t>(i)); }

    bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
} // namespace

int main() {
    // The squash is full width at either end and pinched to nothing in the middle.
    assert(near(squash(0.0), 1.0));
    assert(near(squash(0.5), 0.0));
    assert(near(squash(1.0), 1.0));

    // Symmetric about the pinch: that's what lets turn() reverse by mirroring progress
    // without the frame jumping width.
    for (int i = 0; i <= 50; i++) {
        const double t = i / 100.0;
        assert(near(squash(t), squash(1.0 - t)));
    }

    // Monotonic on each side of the pinch, so the turn never stutters.
    for (int i = 0; i < 50; i++)
        assert(squash(i / 100.0) > squash((i + 1) / 100.0));

    // The pinch fires exactly once per turn, on the step that crosses the midpoint.
    {
        int crossings = 0;
        double t = 0.0;
        while (t < 1.0) {
            const double prev = t;
            t = std::fmin(1.0, t + 0.07);
            if (past_pinch(prev, t))
                crossings++;
        }
        assert(crossings == 1);
    }
    // A step landing exactly on the midpoint counts, and doesn't count twice.
    assert(past_pinch(0.4, 0.5));
    assert(!past_pinch(0.5, 0.6));
    // A single step straight past the midpoint (a stalled frame) still counts once.
    assert(past_pinch(0.1, 1.0));

    // Trading roles rewrites the leaf in place: the tile keeps its slot, its geometry and
    // its parent's split ratio, so a flip never disturbs the layout.
    {
        tiling::Node* root = nullptr;
        tiling::tree_insert(root, tag(1), nullptr);
        tiling::tree_insert(root, tag(2), tag(1));
        tiling::tree_insert(root, tag(3), tag(2));
        tiling::place(root, {10, 10, 980, 980}, 10);

        tiling::Node* leaf = tiling::find_leaf(root, tag(2));
        assert(leaf);
        const tiling::Rect before = leaf->rect;
        leaf->parent->ratio = 0.3;

        leaf->view = tag(4); // the back half steps into the front's leaf

        assert(tiling::find_leaf(root, tag(2)) == nullptr);
        tiling::Node* now = tiling::find_leaf(root, tag(4));
        assert(now == leaf);
        assert(now->rect.x == before.x && now->rect.y == before.y);
        assert(now->rect.w == before.w && now->rect.h == before.h);
        assert(near(now->parent->ratio, 0.3));
        assert(tiling::find_leaf(root, tag(1)) && tiling::find_leaf(root, tag(3)));
    }

    printf("flip tests passed\n");
    return 0;
}
