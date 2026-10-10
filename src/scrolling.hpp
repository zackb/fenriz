#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "tiling.hpp" // Rect

namespace fenriz {

    class View;

    // Scrolling layout: an unbounded horizontal strip of columns, each stacking
    // one or more windows top to bottom. The output is a viewport onto the strip.
    namespace scrolling {

        using tiling::Rect;

        struct Row {
            View* view;
            double weight = 1.0; // share of the column height, relative to the other rows
        };

        struct Column {
            std::vector<Row> rows;
            double width = 0.5;
            double restore_width = 0;
            int focus_row = 0;

            static Column of(View* v, double width) { return {{{v}}, width}; }
            double total_weight() const {
                double t = 0;
                for (const Row& r : rows)
                    t += r.weight;
                return t;
            }
        };

        struct Strip {
            std::vector<Column> cols;
            double scroll = 0; // strip x shown at the viewport's left edge, px
        };

        struct Pos {
            int col = -1, row = -1;
            explicit operator bool() const { return col >= 0; }
        };

        constexpr double WIDTH_MIN = 0.1;
        constexpr double WIDTH_PRESETS[] = {1.0 / 3, 0.5, 2.0 / 3};

        inline Pos find(const Strip& s, const View* v) {
            for (int c = 0; c < (int)s.cols.size(); c++)
                for (int r = 0; r < (int)s.cols[c].rows.size(); r++)
                    if (s.cols[c].rows[r].view == v)
                        return {c, r};
            return {};
        }

        // Column width in px for a viewport `area_w` wide. Fractions count one gap each so
        // that n columns of 1/n tile the viewport exactly.
        inline int col_px(double frac, int area_w, int gap) {
            return std::max(1, (int)std::lround(frac * (area_w + gap)) - gap);
        }

        // Strip x of column `c`'s left edge.
        inline int col_x(const Strip& s, int c, int area_w, int gap) {
            int x = 0;
            for (int i = 0; i < c; i++)
                x += col_px(s.cols[i].width, area_w, gap) + gap;
            return x;
        }

        inline int strip_px(const Strip& s, int area_w, int gap) {
            return s.cols.empty() ? 0 : col_x(s, (int)s.cols.size(), area_w, gap) - gap;
        }

        // Keep the viewport on the strip: no blank space before the first column, and none
        // past the last once the strip is wider than the viewport.
        inline void clamp_scroll(Strip& s, int area_w, int gap) {
            s.scroll = std::clamp(s.scroll, 0.0, (double)std::max(0, strip_px(s, area_w, gap) - area_w));
        }

        // Scroll as little as possible to bring column `c` fully into view.
        // A column wider than the viewport aligns its left edge. True if the scroll moved.
        inline bool ensure_visible(Strip& s, int c, int area_w, int gap) {
            if (c < 0 || c >= (int)s.cols.size())
                return false;
            const double before = s.scroll;
            const int l = col_x(s, c, area_w, gap), r = l + col_px(s.cols[c].width, area_w, gap);
            if (r - l >= area_w || l < s.scroll)
                s.scroll = l;
            else if (r > s.scroll + area_w)
                s.scroll = r - area_w;
            clamp_scroll(s, area_w, gap);
            return s.scroll != before;
        }

        // The column whose left edge is nearest the viewport's: where a free scroll snaps to.
        inline int nearest_column(const Strip& s, int area_w, int gap) {
            int best = -1;
            double best_d = 0;
            for (int c = 0; c < (int)s.cols.size(); c++) {
                const double d = std::abs(col_x(s, c, area_w, gap) - s.scroll);
                if (best < 0 || d < best_d) {
                    best = c;
                    best_d = d;
                }
            }
            return best;
        }

        // A new column holding `v`, right of `focus`'s column (or at the end).
        inline void insert(Strip& s, View* v, const View* focus, double width) {
            const Pos f = find(s, focus);
            const int at = f ? f.col + 1 : (int)s.cols.size();
            s.cols.insert(s.cols.begin() + at, Column::of(v, width));
        }

        // Take the window at `p` out of its column, dropping the column when it empties.
        inline void take(Strip& s, Pos p) {
            Column& col = s.cols[p.col];
            col.rows.erase(col.rows.begin() + p.row);
            if (col.rows.empty())
                s.cols.erase(s.cols.begin() + p.col);
            else
                col.focus_row = std::min(col.focus_row, (int)col.rows.size() - 1);
        }

        inline View* focus_of(const Column& c) {
            return c.rows[std::clamp(c.focus_row, 0, (int)c.rows.size() - 1)].view;
        }

        // Remove `v`. Returns who focus should pass to: the window now in its slot in the
        // same column, else the column to the left, else the one to the right.
        inline View* remove(Strip& s, const View* v) {
            const Pos p = find(s, v);
            if (!p)
                return nullptr;
            const bool last = s.cols[p.col].rows.size() == 1;
            take(s, p);
            if (s.cols.empty())
                return nullptr;
            if (!last) {
                Column& col = s.cols[p.col];
                col.focus_row = std::min(p.row, (int)col.rows.size() - 1);
                return col.rows[col.focus_row].view;
            }
            return focus_of(s.cols[std::max(0, p.col - 1)]);
        }

        // The neighbour of `v` in direction (dx,dy): the adjacent column's remembered row, or
        // the row above/below. Null at the strip's ends.
        inline View* neighbor(const Strip& s, const View* v, int dx, int dy) {
            const Pos p = find(s, v);
            if (!p)
                return nullptr;
            if (dx) {
                const int c = p.col + dx;
                return c >= 0 && c < (int)s.cols.size() ? focus_of(s.cols[c]) : nullptr;
            }
            const auto& rows = s.cols[p.col].rows;
            const int r = p.row + dy;
            return r >= 0 && r < (int)rows.size() ? rows[r].view : nullptr;
        }

        // Swap `v`'s column with the adjacent one. False at the strip's ends.
        inline bool move_column(Strip& s, const View* v, int dir) {
            const Pos p = find(s, v);
            const int t = p.col + dir;
            if (!p || t < 0 || t >= (int)s.cols.size())
                return false;
            std::swap(s.cols[p.col], s.cols[t]);
            return true;
        }

        // Swap `v` with the window above/below it in its column.
        inline bool move_in_column(Strip& s, const View* v, int dir) {
            const Pos p = find(s, v);
            if (!p)
                return false;
            Column& col = s.cols[p.col];
            const int t = p.row + dir;
            if (t < 0 || t >= (int)col.rows.size())
                return false;
            std::swap(col.rows[p.row], col.rows[t]);
            col.focus_row = t;
            return true;
        }

        inline bool consume_or_expel(Strip& s, View* v, int dir) {
            const Pos p = find(s, v);
            if (!p)
                return false;
            Column& col = s.cols[p.col];
            if (col.rows.size() > 1) {
                const double w = col.width;
                take(s, p);
                const int at = dir < 0 ? p.col : p.col + 1;
                s.cols.insert(s.cols.begin() + at, Column::of(v, w));
                return true;
            }
            const int t = p.col + dir;
            if (t < 0 || t >= (int)s.cols.size())
                return false;
            Column& dst = s.cols[t];
            for (Row& r : dst.rows)
                r.weight = 1.0; // even split, as niri
            dst.rows.push_back({v});
            dst.focus_row = (int)dst.rows.size() - 1;
            s.cols.erase(s.cols.begin() + p.col);
            return true;
        }

        // Step `v`'s column through the width presets.
        inline bool cycle_width(Strip& s, const View* v) {
            const Pos p = find(s, v);
            if (!p)
                return false;
            Column& col = s.cols[p.col];
            double next = WIDTH_PRESETS[0];
            for (double w : WIDTH_PRESETS)
                if (w > col.width + 0.01) {
                    next = w;
                    break;
                }
            col.width = next;
            col.restore_width = 0;
            return true;
        }

        // Toggle `v`'s column between full viewport width and its previous width.
        inline bool toggle_maximize(Strip& s, const View* v) {
            const Pos p = find(s, v);
            if (!p)
                return false;
            Column& col = s.cols[p.col];
            if (col.restore_width > 0) {
                col.width = col.restore_width;
                col.restore_width = 0;
            } else {
                col.restore_width = col.width;
                col.width = 1.0;
            }
            return true;
        }

        // Drag `v`'s column right edge by dx and its row boundary by dy (px in an area_w x
        // area_h viewport). The boundary moved is the one below the row, or above the last.
        inline void resize(Strip& s, const View* v, double dx, double dy, int area_w, int area_h) {
            const Pos p = find(s, v);
            if (!p)
                return;
            Column& col = s.cols[p.col];
            if (area_w > 0) {
                col.width = std::clamp(col.width + dx / area_w, WIDTH_MIN, 1.0);
                col.restore_width = 0;
            }
            const int n = (int)col.rows.size();
            if (n < 2 || area_h <= 0)
                return;
            const double total = col.total_weight();
            const int o = p.row + 1 < n ? p.row + 1 : p.row - 1;
            double& mine = col.rows[p.row].weight;
            double& other = col.rows[o].weight;
            const double floor = 0.2 * total / n;
            const double want = dy / area_h * total * (o > p.row ? 1 : -1);
            const double d = std::clamp(want, std::min(0.0, floor - mine), std::max(0.0, other - floor));
            mine += d;
            other -= d;
        }

        // Geometry for every window: columns laid left to right from area.x - scroll, rows
        // split by weight. Clamps the scroll first, so callers never see a stale overhang.
        inline void place(Strip& s, Rect area, int gap, std::vector<std::pair<View*, Rect>>& out) {
            clamp_scroll(s, area.w, gap);
            int x = area.x - (int)std::lround(s.scroll);
            for (const Column& col : s.cols) {
                const int w = col_px(col.width, area.w, gap);
                const int n = (int)col.rows.size();
                const int inner = std::max(0, area.h - gap * (n - 1));
                const double total = col.total_weight();
                int y = area.y;
                for (int r = 0; r < n; r++) {
                    const int h = r == n - 1 ? area.y + area.h - y : (int)(inner * col.rows[r].weight / total);
                    out.push_back({col.rows[r].view, {x, y, w, std::max(1, h)}});
                    y += h + gap;
                }
                x += w + gap;
            }
        }

        // One column per window, in order: converting a dwindle workspace.
        inline Strip from_views(const std::vector<View*>& views, double width) {
            Strip s;
            for (View* v : views)
                s.cols.push_back(Column::of(v, width));
            return s;
        }

        // Every window, column by column: converting back to dwindle.
        inline std::vector<View*> views_of(const Strip& s) {
            std::vector<View*> out;
            for (const Column& c : s.cols)
                for (const Row& r : c.rows)
                    out.push_back(r.view);
            return out;
        }

        // The slot holding `v`, for replacing it in place (flip pairs, swaps). Null if absent.
        inline View** slot(Strip& s, const View* v) {
            const Pos p = find(s, v);
            return p ? &s.cols[p.col].rows[p.row].view : nullptr;
        }

    } // namespace scrolling

} // namespace fenriz
