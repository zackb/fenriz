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
    // Flat at either end: every point where it started, at full height.
    for (double x : {-1.0, -0.3, 0.0, 1.0}) {
        assert(near(project(0.0, x).x, x) && near(project(0.0, x).scale, 1.0));
        assert(near(project(1.0, x).x, x) && near(project(1.0, x).scale, 1.0));
    }
    // Edge-on at the pinch.
    assert(std::fabs(project(0.5, 1.0).x) < 1e-9 && std::fabs(project(0.5, -1.0).x) < 1e-9);

    for (int i = 1; i < 50; i++) {
        const double t = i / 100.0;
        // The front turns its right edge away; after the pinch the back turns in with its left edge away.
        assert(project(t, 1.0).scale < 1.0 && near(project(t, -1.0).scale, 1.0));
        assert(project(1.0 - t, -1.0).scale < 1.0 && near(project(1.0 - t, 1.0).scale, 1.0));
        // A reversed turn at 1-t is the same picture as the forward turn at t, so reversing never jumps.
        for (double x : {-1.0, -0.5, 0.5, 1.0}) {
            assert(near(project(1.0 - t, x, true).x, project(t, x).x));
            assert(near(project(1.0 - t, x, true).scale, project(t, x).scale));
        }
        // Never outside the flat frame, and left stays left.
        for (int k = -10; k < 10; k++) {
            const Projected a = project(t, k / 10.0), b = project(t, (k + 1) / 10.0);
            assert(a.x < b.x && std::fabs(a.x) <= 1.0 && a.scale <= 1.0);
        }
        // Narrows monotonically toward the pinch.
        const double w = project(t, 1.0).x - project(t, -1.0).x;
        const double w2 = project(t + 0.01, 1.0).x - project(t + 0.01, -1.0).x;
        assert(w2 < w);
    }

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
