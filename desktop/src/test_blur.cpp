#include <algorithm>
#include <cassert>
#include <cmath>

#include "blur.hpp"

using fenriz::desktop::blur::Band;
using fenriz::desktop::blur::MAX_BANDS;
using fenriz::desktop::blur::rounded_bands;

namespace {

    // Is (px, py) inside the w*h rounded rect with radius r? Only the corner quadrants can
    // fall outside, so the check is the distance to the nearest corner centre.
    bool inside(double px, double py, int w, int h, int r) {
        const double cx = px < r ? r : (px > w - r ? w - r : px);
        const double cy = py < r ? r : (py > h - r ? h - r : py);
        if (px >= r && px <= w - r)
            return py >= 0 && py <= h;
        if (py >= r && py <= h - r)
            return px >= 0 && px <= w;
        return std::hypot(px - cx, py - cy) <= r + 1e-9;
    }

    // Every band must lie wholly within the rounded rect: a band poking out is blur drawn
    // past the card's corner, which is the whole reason this is not one rectangle.
    void assert_inscribed(int w, int h, int r) {
        r = std::min(r, std::min(w, h) / 2); // the same clamp rounded_bands applies
        Band b[MAX_BANDS];
        const int n = rounded_bands(w, h, r, b);
        assert(n > 0 && n <= MAX_BANDS);
        for (int i = 0; i < n; i++) {
            assert(b[i].w > 0 && b[i].h > 0);
            assert(b[i].x >= 0 && b[i].y >= 0);
            assert(b[i].x + b[i].w <= w && b[i].y + b[i].h <= h);
            const double x0 = b[i].x, y0 = b[i].y, x1 = b[i].x + b[i].w, y1 = b[i].y + b[i].h;
            assert(inside(x0, y0, w, h, r));
            assert(inside(x1, y0, w, h, r));
            assert(inside(x0, y1, w, h, r));
            assert(inside(x1, y1, w, h, r));
        }
    }

    void test_square_is_one_band() {
        Band b[MAX_BANDS];
        assert(rounded_bands(100, 60, 0, b) == 1);
        assert(b[0].x == 0 && b[0].y == 0 && b[0].w == 100 && b[0].h == 60);
    }

    void test_degenerate() {
        Band b[MAX_BANDS];
        assert(rounded_bands(0, 60, 8, b) == 0);
        assert(rounded_bands(100, 0, 8, b) == 0);
        // A radius bigger than the box clamps rather than producing negative widths.
        assert_inscribed(40, 40, 500);
    }

    void test_card_and_capsule() {
        assert_inscribed(620, 420, 12); // launcher
        assert_inscribed(400, 96, 12);  // toast
        assert_inscribed(280, 52, 26);  // the OSD pill: radius is half the height
        assert_inscribed(280, 53, 26);  // odd height, where the middle band is 1px
        assert_inscribed(60, 60, 30);   // a circle
    }

    // The bands have to cover most of the card, or the blur is a thin strip down the middle.
    // A capsule is the hard case: at one band per corner it would lose both end caps.
    void assert_covers(int w, int h, int r) {
        Band b[MAX_BANDS];
        const int n = rounded_bands(w, h, r, b);
        long area = 0;
        for (int i = 0; i < n; i++)
            area += (long)b[i].w * b[i].h;
        const double full = (double)w * h - (4 - M_PI) * (double)r * r;
        assert(area > 0.85 * full);
    }

    void test_covers_the_card() {
        assert_covers(280, 52, 26); // OSD pill
        assert_covers(620, 420, 12);
        assert_covers(400, 96, 12);
    }

    // A stack of notifications is what sets the compositor's blur-node ceiling, so the bands
    // per card have to stay within it. Keep in step with RECTS_MAX in background_blur.hpp.
    void test_fits_the_node_budget() {
        Band b[MAX_BANDS];
        assert(rounded_bands(400, 96, 12, b) * 5 <= 16);
    }

} // namespace

int main() {
    test_square_is_one_band();
    test_degenerate();
    test_card_and_capsule();
    test_covers_the_card();
    test_fits_the_node_budget();
    return 0;
}
