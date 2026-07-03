#include "tiling.hpp"

#include <cassert>
#include <cstdio>

using namespace fenriz::tiling;

int main() {
    // 1000x1000 area, gap 10 -> usable area is (10,10) size 980x980.

    // n=1: single tile fills the usable area.
    std::vector<Rect> a = layout(0, 0, 1000, 1000, 10, 1);
    assert(a.size() == 1);
    assert(a[0].x == 10 && a[0].y == 10 && a[0].w == 980 && a[0].h == 980);

    // n=2: master left (w=(980-10)/2=485), one stacked right (same width, full height).
    std::vector<Rect> b = layout(0, 0, 1000, 1000, 10, 2);
    assert(b.size() == 2);
    assert(b[0].x == 10 && b[0].w == 485 && b[0].h == 980);
    assert(b[1].x == 505 && b[1].w == 485 && b[1].h == 980); // 10 + 485 + 10

    // n=3: two tiles stacked in the right column, each_h=(980-10)/2=485, 10px between.
    std::vector<Rect> c = layout(0, 0, 1000, 1000, 10, 3);
    assert(c.size() == 3);
    assert(c[0].w == 485 && c[0].h == 980); // master unchanged
    assert(c[1].x == 505 && c[1].y == 10 && c[1].h == 485);
    assert(c[2].y == 505);                  // 10 + 485 + 10
    assert(c[1].y + c[1].h + 10 == c[2].y); // exactly one gap between tiles
    assert(c[2].y + c[2].h == 990);         // last tile reaches usable bottom edge

    std::printf("tiling layout: all assertions passed\n");
    return 0;
}
