#include "scrolling.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace fenriz;
using namespace fenriz::scrolling;

namespace {
    // Fake View handles: the strip only uses View* as an identity tag, never derefs it.
    View* tag(int i) { return reinterpret_cast<View*>(static_cast<intptr_t>(i)); }

    // 1000x1000 viewport at the origin, gap 10.
    constexpr Rect AREA{0, 0, 1000, 1000};
    constexpr int GAP = 10;

    Rect box(Strip& s, View* v) {
        std::vector<std::pair<View*, Rect>> out;
        place(s, AREA, GAP, out);
        for (auto& [view, r] : out)
            if (view == v)
                return r;
        assert(!"view not placed");
        return {};
    }
} // namespace

int main() {
    // Half-width columns: two tile the viewport exactly, the third opens off-screen right.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        assert(box(s, tag(1)).x == 0 && box(s, tag(1)).w == 495 && box(s, tag(1)).h == 1000);
        assert(box(s, tag(2)).x == 505 && box(s, tag(2)).x + box(s, tag(2)).w == 1000);

        insert(s, tag(3), tag(2), 0.5);
        assert(box(s, tag(3)).x == 1010);
        assert(ensure_visible(s, 2, AREA.w, GAP)); // minimal scroll: 3 lands flush right
        assert(box(s, tag(3)).x + box(s, tag(3)).w == 1000 && box(s, tag(2)).x == 0);
        assert(!ensure_visible(s, 1, AREA.w, GAP)); // already visible: no movement
        assert(ensure_visible(s, 0, AREA.w, GAP) && s.scroll == 0);
    }

    // New windows open right of the focused column, not at the end.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        insert(s, tag(3), tag(1), 0.5);
        assert(find(s, tag(3)).col == 1 && find(s, tag(2)).col == 2);
    }

    // Removal hands focus to the same column, else the left, else the right neighbour.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        insert(s, tag(3), tag(2), 0.5);
        assert(remove(s, tag(2)) == tag(1));
        assert(remove(s, tag(1)) == tag(3));
        assert(remove(s, tag(3)) == nullptr && s.cols.empty());
    }

    // Consume joins the neighbour column (rows split evenly); expel splits it out again.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        assert(consume_or_expel(s, tag(2), -1));
        assert(s.cols.size() == 1 && find(s, tag(2)).row == 1);
        assert(box(s, tag(1)).h == 495 && box(s, tag(2)).y == 505 && box(s, tag(2)).h == 495);
        assert(remove(s, tag(1)) == tag(2) && s.cols.size() == 1); // stacked row: same column

        insert(s, tag(1), tag(2), 0.5);
        assert(consume_or_expel(s, tag(1), -1) && s.cols.size() == 1);
        assert(consume_or_expel(s, tag(1), 1)); // shares a column -> expelled to the right
        assert(s.cols.size() == 2 && find(s, tag(1)).col == 1);
        assert(!consume_or_expel(s, tag(1), 1)); // alone at the right end: nothing to join
    }

    // Moving and navigating.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        insert(s, tag(3), tag(2), 0.5);
        consume_or_expel(s, tag(3), -1);             // [1] [2/3]
        assert(neighbor(s, tag(1), 1, 0) == tag(3)); // column remembers its focused row
        assert(neighbor(s, tag(3), 0, -1) == tag(2) && !neighbor(s, tag(3), 0, 1));
        assert(!neighbor(s, tag(1), -1, 0));
        assert(move_column(s, tag(1), 1) && find(s, tag(1)).col == 1);
        assert(!move_column(s, tag(1), 1));
        assert(move_in_column(s, tag(3), -1) && find(s, tag(3)).row == 0);
    }

    // Widths: presets cycle, maximize round-trips, drag clamps.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        cycle_width(s, tag(1));
        assert(s.cols[0].width > 0.66 && s.cols[0].width < 0.67);
        cycle_width(s, tag(1));
        assert(s.cols[0].width < 0.34);
        toggle_maximize(s, tag(1));
        assert(box(s, tag(1)).w == 1000);
        toggle_maximize(s, tag(1));
        assert(s.cols[0].width < 0.34);
        resize(s, tag(1), -5000, 0, AREA.w, AREA.h);
        assert(s.cols[0].width == WIDTH_MIN);
    }

    // Row resize moves the boundary under the cursor and never collapses a row.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        consume_or_expel(s, tag(2), -1);
        resize(s, tag(1), 0, 100, AREA.w, AREA.h);
        assert(box(s, tag(1)).h > 495 && box(s, tag(2)).h < 495);
        resize(s, tag(2), 0, 100, AREA.w, AREA.h); // last row: boundary above moves down
        assert(box(s, tag(1)).h > 550);
        resize(s, tag(1), 0, 99999, AREA.w, AREA.h);
        assert(box(s, tag(2)).h >= 90);
    }

    // A tall stack still resizes in both directions, and no row is squeezed away.
    {
        Strip s;
        for (int i = 1; i <= 12; i++) {
            insert(s, tag(i), i > 1 ? tag(i - 1) : nullptr, 0.5);
            if (i > 1)
                consume_or_expel(s, tag(i), -1);
        }
        assert(s.cols.size() == 1 && s.cols[0].rows.size() == 12);
        resize(s, tag(1), 0, 99999, AREA.w, AREA.h);
        resize(s, tag(1), 0, -99999, AREA.w, AREA.h);
        for (const Row& r : s.cols[0].rows)
            assert(r.weight >= 0.2 - 1e-9);
        assert(box(s, tag(1)).h < box(s, tag(2)).h);
    }

    // Removing columns pulls an overhanging scroll back onto the strip.
    {
        Strip s;
        insert(s, tag(1), nullptr, 0.5);
        insert(s, tag(2), tag(1), 0.5);
        insert(s, tag(3), tag(2), 0.5);
        ensure_visible(s, 2, AREA.w, GAP);
        remove(s, tag(3));
        assert(box(s, tag(1)).x == 0);
    }

    // A free scroll snaps to the nearest column edge.
    {
        Strip s;
        for (int i = 1; i <= 4; i++)
            insert(s, tag(i), i > 1 ? tag(i - 1) : nullptr, 0.5);
        s.scroll = 700; // nearer column 1 (505) than column 2 (1010)
        assert(nearest_column(s, AREA.w, GAP) == 1);
        s.scroll = 800;
        assert(nearest_column(s, AREA.w, GAP) == 2);
    }

    // Conversion round-trips window order.
    {
        Strip s = from_views({tag(1), tag(2), tag(3)}, 0.5);
        consume_or_expel(s, tag(2), -1);
        const auto v = views_of(s);
        assert(v.size() == 3 && v[0] == tag(1) && v[1] == tag(2) && v[2] == tag(3));
        *slot(s, tag(2)) = tag(9);
        assert(find(s, tag(9)).col == 0 && !find(s, tag(2)));
    }

    std::puts("scrolling: ok");
    return 0;
}
