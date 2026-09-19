#include "shot.hpp"

#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>

namespace fenriz::bar {

    namespace {

        int int_member(JsonObject* obj, const char* name) {
            return static_cast<int>(json_object_get_int_member_with_default(obj, name, 0));
        }

        Rect rect_members(JsonObject* obj) {
            return {int_member(obj, "x"), int_member(obj, "y"), int_member(obj, "width"), int_member(obj, "height")};
        }

    } // namespace

    Rect Rect::intersect(const Rect& o) const {
        const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const int x1 = std::min(x + width, o.x + o.width), y1 = std::min(y + height, o.y + o.height);
        if (x1 <= x0 || y1 <= y0)
            return {};
        return {x0, y0, x1 - x0, y1 - y0};
    }

    Rect Rect::spanning(int x0, int y0, int x1, int y1) {
        return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
    }

    std::optional<ShotArgs> parse_shot_args(const std::vector<std::string>& args, std::string& error) {
        if (args.empty()) {
            error = "usage: shot screen|window|region [--focused] [--copy] [--save[=PATH]] [--edit]";
            return std::nullopt;
        }
        ShotArgs out;
        if (args[0] == "screen")
            out.mode = ShotMode::Screen;
        else if (args[0] == "window")
            out.mode = ShotMode::Window;
        else if (args[0] == "region")
            out.mode = ShotMode::Region;
        else {
            error = "unknown shot mode: " + args[0];
            return std::nullopt;
        }
        for (size_t i = 1; i < args.size(); i++) {
            const std::string& a = args[i];
            if (a == "--copy")
                out.copy = true;
            else if (a == "--edit")
                out.edit = true;
            else if (a == "--focused" && out.mode == ShotMode::Window)
                out.focused = true;
            else if (a == "--save")
                out.save = true;
            else if (a.starts_with("--save=") && a.size() > 7) {
                out.save = true;
                out.path = a.substr(7);
            } else {
                error = "unknown shot option: " + a;
                return std::nullopt;
            }
        }
        if (!out.copy && !out.save) {
            error = "shot needs --copy, --save or both";
            return std::nullopt;
        }
        return out;
    }

    const ShotLayout::Output* ShotLayout::focused_output() const {
        for (const Output& o : outputs)
            if (o.focused)
                return &o;
        return outputs.empty() ? nullptr : &outputs.front();
    }

    const ShotLayout::Window* ShotLayout::focused_window() const {
        for (const Window& w : windows)
            if (w.focused)
                return &w;
        return nullptr;
    }

    const ShotLayout::Output* ShotLayout::output_of(const Rect& r) const {
        const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        for (const Output& o : outputs)
            if (o.box.contains(cx, cy))
                return &o;
        return nullptr;
    }

    const ShotLayout::Window* ShotLayout::window_at(const Output& output, int x, int y) const {
        for (auto it = windows.rbegin(); it != windows.rend(); ++it)
            if (it->workspace == output.active && it->box.contains(x, y))
                return &*it;
        return nullptr;
    }

    std::optional<ShotLayout> parse_layout(const std::string& line) {
        JsonParser* parser = json_parser_new();
        std::optional<ShotLayout> result;
        JsonNode* root = nullptr;
        if (json_parser_load_from_data(parser, line.data(), static_cast<gssize>(line.size()), nullptr))
            root = json_parser_get_root(parser);
        JsonObject* obj = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
        if (obj && json_object_has_member(obj, "outputs")) {
            ShotLayout s;
            if (JsonArray* outputs = json_object_get_array_member(obj, "outputs"))
                for (guint i = 0; i < json_array_get_length(outputs); i++)
                    if (JsonObject* o = json_array_get_object_element(outputs, i))
                        s.outputs.push_back(
                            {json_object_get_string_member_with_default(o, "name", ""),
                             rect_members(o),
                             int_member(o, "active"),
                             json_object_get_boolean_member_with_default(o, "focused", false) != FALSE});
            if (json_object_has_member(obj, "windows"))
                if (JsonArray* windows = json_object_get_array_member(obj, "windows"))
                    for (guint i = 0; i < json_array_get_length(windows); i++)
                        if (JsonObject* w = json_array_get_object_element(windows, i))
                            s.windows.push_back(
                                {rect_members(w),
                                 int_member(w, "workspace"),
                                 json_object_get_boolean_member_with_default(w, "focused", false) != FALSE});
            result = std::move(s);
        }
        g_object_unref(parser);
        return result;
    }

    Rect to_pixels(const Rect& logical, const Rect& output, int buffer_width, int buffer_height) {
        const Rect r = logical.intersect(output);
        if (r.empty() || output.empty())
            return {};

        const double sx = static_cast<double>(buffer_width) / output.width;
        const double sy = static_cast<double>(buffer_height) / output.height;
        const int x0 = static_cast<int>(std::lround((r.x - output.x) * sx));
        const int y0 = static_cast<int>(std::lround((r.y - output.y) * sy));
        const int x1 = static_cast<int>(std::lround((r.x + r.width - output.x) * sx));
        const int y1 = static_cast<int>(std::lround((r.y + r.height - output.y) * sy));
        return Rect{x0, y0, x1 - x0, y1 - y0}.intersect({0, 0, buffer_width, buffer_height});
    }

    std::string shot_filename(std::time_t when) {
        std::tm tm = {};
        localtime_r(&when, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        return std::string("fenriz-shot-") + stamp + ".png";
    }

} // namespace fenriz::bar
