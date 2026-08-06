#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace fenriz {

    namespace {

        std::string trim(const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)
                return "";
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        std::string strip_comment(const std::string& s) {
            size_t h = s.find('#');
            return h == std::string::npos ? s : s.substr(0, h);
        }

        std::vector<std::string> split(const std::string& s, char delim) {
            std::vector<std::string> out;
            std::stringstream ss(s);
            std::string item;
            while (std::getline(ss, item, delim))
                out.push_back(trim(item));
            return out;
        }

        // split(), but into at most `max` fields: the last one keeps any remaining delimiters verbatim.
        std::vector<std::string> split_n(const std::string& s, char delim, size_t max) {
            std::vector<std::string> out;
            size_t start = 0;
            while (out.size() + 1 < max) {
                size_t d = s.find(delim, start);
                if (d == std::string::npos)
                    break;
                out.push_back(trim(s.substr(start, d - start)));
                start = d + 1;
            }
            out.push_back(trim(s.substr(start)));
            return out;
        }

        uint32_t parse_color(const std::string& s, uint32_t fallback) {
            try {
                return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
            } catch (...) {
                return fallback;
            }
        }

        // Every setting below that reaches the renderer is geometry, and geometry has no
        // meaning outside its range. Clamp at the parser so no downstream code has to wonder.
        int parse_int(const std::string& s, int fallback, int lo = INT_MIN, int hi = INT_MAX) {
            try {
                return std::clamp(std::stoi(s), lo, hi);
            } catch (...) {
                return fallback;
            }
        }

        float parse_float(const std::string& s, float fallback, float lo, float hi) {
            try {
                return std::clamp(std::stof(s), lo, hi);
            } catch (...) {
                return fallback;
            }
        }

        bool parse_bool(const std::string& s, bool fallback) {
            if (s == "true" || s == "1" || s == "on" || s == "yes")
                return true;
            if (s == "false" || s == "0" || s == "off" || s == "no")
                return false;
            return fallback;
        }

        // Mirror WLR_MODIFIER_* bit values (wlr/types/wlr_keyboard.h) so the config
        // parser stays free of a wlroots include and its test needs no wlroots.
        // ponytail: 4 constants beats dragging wlr headers into the pure-logic unit.
        uint32_t mod_from_token(const std::string& t) {
            std::string u = t;
            std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return std::toupper(c); });
            if (u == "SUPER" || u == "LOGO" || u == "MOD4")
                return 64;
            if (u == "SHIFT")
                return 1;
            if (u == "CTRL" || u == "CONTROL")
                return 4;
            if (u == "ALT" || u == "MOD1")
                return 8;
            return 0;
        }

    } // namespace

    RuleResult match_rules(const std::vector<WindowRule>& rules, const char* app_id, const char* title) {
        auto matches = [](const std::string& pat, const char* value) {
            if (pat.empty())
                return true;
            try {
                return std::regex_search(value ? value : "", std::regex(pat));
            } catch (...) {
                return false;
            }
        };
        RuleResult out;
        for (const WindowRule& r : rules) {
            if (!matches(r.app_id, app_id) || !matches(r.title, title))
                continue;
            out.floating |= r.floating;
            out.center |= r.center;
            out.no_focus |= r.no_focus;
        }
        return out;
    }

    bool auto_float(int min_w, int max_w, int min_h, int max_h, bool has_parent) {
        const bool fixed = max_w > 0 && max_w == min_w && max_h > 0 && max_h == min_h;
        return has_parent || fixed;
    }

    Action action_from_string(const std::string& s) {
        if (s == "exec")
            return Action::Exec;
        if (s == "killactive")
            return Action::KillActive;
        if (s == "exit")
            return Action::Exit;
        if (s == "focusnext")
            return Action::FocusNext;
        if (s == "focusprev")
            return Action::FocusPrev;
        if (s == "focusleft")
            return Action::FocusLeft;
        if (s == "focusright")
            return Action::FocusRight;
        if (s == "focusup")
            return Action::FocusUp;
        if (s == "focusdown")
            return Action::FocusDown;
        if (s == "togglelayout")
            return Action::ToggleLayout;
        if (s == "fullscreen")
            return Action::Fullscreen;
        if (s == "togglefloating")
            return Action::ToggleFloat;
        if (s == "workspace")
            return Action::Workspace;
        if (s == "movetoworkspace")
            return Action::MoveToWorkspace;
        if (s == "pin")
            return Action::Pin;
        return Action::None;
    }

    Config Config::parse(const std::string& text) {
        Config cfg;
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') // whole-line comment (may contain an `=`)
                continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = trim(line.substr(0, eq));
            const std::string raw = trim(line.substr(eq + 1));
            const std::string val = trim(strip_comment(raw));

            if (key == "bind" || key == "binde") {
                // Only the first three commas separate fields; the rest belongs to the arg.
                std::vector<std::string> parts = split_n(raw, ',', 4);
                if (parts.size() < 2)
                    continue;

                for (size_t i = 0; i < parts.size() && i < 3; i++)
                    parts[i] = trim(strip_comment(parts[i]));

                Bind b;
                b.repeat = (key == "binde"); // `binde` re-fires while held (volume/brightness)
                for (const std::string& tok : split(parts[0], ' '))
                    b.mods |= mod_from_token(tok);
                b.sym = xkb_keysym_from_name(parts[1].c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
                b.action = parts.size() > 2 ? action_from_string(parts[2]) : Action::None;
                if (parts.size() > 3)
                    b.arg = parts[3];
                if (b.sym != XKB_KEY_NoSymbol)
                    cfg.binds.push_back(b);
                continue;
            }

            if (key == "exec-once") {
                if (!raw.empty())
                    cfg.exec_once.push_back(raw); // a shell command, verbatim
                continue;
            }

            if (key == "output") {
                // NAME, mode, position, scale — trailing fields optional.
                std::vector<std::string> parts = split(val, ',');
                if (parts.empty() || parts[0].empty())
                    continue;
                OutputCfg o;
                o.name = parts[0];
                if (parts.size() > 1 && !parts[1].empty())
                    o.mode = parts[1];
                if (parts.size() > 2 && !parts[2].empty())
                    o.position = parts[2];
                if (parts.size() > 3 && !parts[3].empty())
                    o.scale = parse_float(parts[3], o.scale, 0.25f, 10.0f);
                cfg.outputs.push_back(o);
                continue;
            }

            if (key == "windowrule") {
                // name=value fields, comma-separated, any order: class/app_id, title
                // (regexes), float/center/no_focus (bools), name (label, ignored).
                // ponytail: split on ',' — a regex with a comma in a quantifier ({2,4})
                // would break; the common cases don't, upgrade to a smarter tokenizer if needed.
                WindowRule r;
                for (const std::string& tok : split(val, ',')) {
                    size_t e = tok.find('=');
                    if (e == std::string::npos)
                        continue;
                    std::string k = trim(tok.substr(0, e));
                    std::string v = trim(tok.substr(e + 1));
                    if (k == "class" || k == "app_id")
                        r.app_id = v;
                    else if (k == "title")
                        r.title = v;
                    else if (k == "float")
                        r.floating = parse_bool(v, false);
                    else if (k == "center")
                        r.center = parse_bool(v, false);
                    else if (k == "no_focus")
                        r.no_focus = parse_bool(v, false);
                    // `name` and unknown fields: ignored (label only).
                }
                if (!r.app_id.empty() || !r.title.empty())
                    cfg.window_rules.push_back(r);
                continue;
            }

            if (key == "workspace") {
                // N, OUTPUT — the workspace's home output, so it returns there on hotplug.
                std::vector<std::string> parts = split(val, ',');
                if (parts.size() < 2)
                    continue;
                const int n = parse_int(parts[0], 0);
                if (n >= 1 && n <= 10)
                    cfg.ws_home[n - 1] = parts[1];
                continue;
            }

            if (key == "env") {
                // NAME,VALUE split on the first comma only (values may contain commas,
                size_t comma = val.find(',');
                if (comma == std::string::npos)
                    continue;
                std::string name = trim(val.substr(0, comma));
                std::string value = trim(val.substr(comma + 1));
                if (!name.empty())
                    cfg.env.emplace_back(name, value);
                continue;
            }

            if (key == "border_width")
                cfg.border_width = parse_int(val, cfg.border_width, 0, 100);
            else if (key == "border_active")
                cfg.border_active = parse_color(val, cfg.border_active);
            else if (key == "border_inactive")
                cfg.border_inactive = parse_color(val, cfg.border_inactive);
            else if (key == "border_gradient")
                cfg.border_gradient = parse_color(val, cfg.border_gradient);
            else if (key == "shadow")
                cfg.shadow = parse_bool(val, cfg.shadow);
            else if (key == "shadow_color")
                cfg.shadow_color = parse_color(val, cfg.shadow_color);
            else if (key == "shadow_blur")
                cfg.shadow_blur = parse_int(val, cfg.shadow_blur, 0, 200);
            else if (key == "gaps")
                // The ceiling is generous on purpose: tiling::arrange clamps the gap again
                cfg.gaps = parse_int(val, cfg.gaps, 0, 500);
            else if (key == "rounding")
                cfg.rounding = parse_int(val, cfg.rounding, 0, 200);
            else if (key == "animation")
                cfg.animation_ms = parse_int(val, cfg.animation_ms, 0, 5000);
            else if (key == "opacity")
                cfg.opacity = parse_float(val, cfg.opacity, 0.0f, 1.0f);
            else if (key == "scale")
                cfg.scale = parse_float(val, cfg.scale, 0.25f, 10.0f);
            else if (key == "natural_scroll")
                cfg.natural_scroll = parse_bool(val, cfg.natural_scroll);
            else if (key == "tap_to_click")
                cfg.tap_to_click = parse_bool(val, cfg.tap_to_click);
            else if (key == "clickfinger")
                cfg.clickfinger = parse_bool(val, cfg.clickfinger);
            else if (key == "focus_follows_pointer")
                cfg.focus_follows_pointer = parse_bool(val, cfg.focus_follows_pointer);
            else if (key == "sensitivity")
                cfg.sensitivity = parse_float(val, cfg.sensitivity, -1.0f, 1.0f);
            else if (key == "lid_output")
                cfg.lid_output = val;
            else if (key == "repeat_delay")
                cfg.repeat_delay = parse_int(val, cfg.repeat_delay, 0, 10000);
            else if (key == "repeat_rate")
                cfg.repeat_rate = parse_int(val, cfg.repeat_rate, 1, 1000); // lower bound avoids /0 in timer
            else if (key == "zoom_mod")
                cfg.zoom_mod = mod_from_token(val); // "ctrl"/"alt"/"super"/"shift"; unknown = 0 = off
            else if (key == "zoom_max")
                cfg.zoom_max = parse_float(val, cfg.zoom_max, 1.0f, 10.0f);
            else if (key == "zoom_step")
                cfg.zoom_step = parse_float(val, cfg.zoom_step, 0.01f, 1.0f);
        }
        return cfg;
    }

    std::string Config::config_path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::string(xdg) + "/fenriz/fenriz.conf";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::string(home) + "/.config/fenriz/fenriz.conf";
        return "";
    }

    Config Config::load() {
        std::string path = config_path();
        std::ifstream f(path);
        if (!f)
            return Config{}; // built-in defaults
        std::stringstream buf;
        buf << f.rdbuf();
        return parse(buf.str());
    }

} // namespace fenriz
