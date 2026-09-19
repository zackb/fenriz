#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace fenriz::bar {

    struct Rect {
        int x = 0, y = 0, width = 0, height = 0;

        bool empty() const { return width <= 0 || height <= 0; }
        bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
        Rect intersect(const Rect& o) const;
        static Rect spanning(int x0, int y0, int x1, int y1);

        bool operator==(const Rect&) const = default;
    };

    enum class ShotMode { Screen, Window, Region };

    struct ShotArgs {
        ShotMode mode = ShotMode::Screen;
        bool copy = false;
        bool save = false;
        std::string path; // empty = shot_filename() in XDG_PICTURES_DIR/Screenshots
        bool edit = false;
    };

    // `fenriz-bar shot` arguments: screen|window|region [--copy] [--save[=PATH]] [--edit].
    std::optional<ShotArgs> parse_shot_args(const std::vector<std::string>& args, std::string& error);

    // Output and window geometry from one IPC state line, in layout coordinates.
    struct ShotLayout {
        struct Output {
            std::string name;
            Rect box;
            int active = 0; // workspace shown
            bool focused = false;
        };
        struct Window {
            Rect box;
            int workspace = 0;
            bool focused = false;
        };

        std::vector<Output> outputs;
        std::vector<Window> windows; // bottom to top

        const Output* focused_output() const;
        const Window* focused_window() const;
        // The output holding the centre of `r`, or nullptr.
        const Output* output_of(const Rect& r) const;
        // Topmost window shown on `output` under the layout point, or nullptr.
        const Window* window_at(const Output& output, int x, int y) const;
    };

    std::optional<ShotLayout> parse_layout(const std::string& line);

    // `logical` (layout coordinates) clipped to `output` and scaled into that output's capture buffer.
    Rect to_pixels(const Rect& logical, const Rect& output, int buffer_width, int buffer_height);

    std::string shot_filename(std::time_t when);

} // namespace fenriz::bar
