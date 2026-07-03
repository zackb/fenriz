#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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

        uint32_t parse_color(const std::string& s, uint32_t fallback) {
            try {
                return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
            } catch (...) {
                return fallback;
            }
        }

        int parse_int(const std::string& s, int fallback) {
            try {
                return std::stoi(s);
            } catch (...) {
                return fallback;
            }
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
        if (s == "togglelayout")
            return Action::ToggleLayout;
        return Action::None;
    }

    Config Config::parse(const std::string& text) {
        Config cfg;
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(strip_comment(line));
            if (line.empty())
                continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "bind") {
                std::vector<std::string> parts = split(val, ',');
                if (parts.size() < 2)
                    continue;
                Bind b;
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
                if (!val.empty())
                    cfg.exec_once.push_back(val);
                continue;
            }

            if (key == "border_width")
                cfg.border_width = parse_int(val, cfg.border_width);
            else if (key == "border_active")
                cfg.border_active = parse_color(val, cfg.border_active);
            else if (key == "border_inactive")
                cfg.border_inactive = parse_color(val, cfg.border_inactive);
            else if (key == "gaps")
                cfg.gaps = parse_int(val, cfg.gaps);
            else if (key == "rounding")
                cfg.rounding = parse_int(val, cfg.rounding);
            else if (key == "opacity") {
                try {
                    cfg.opacity = std::stof(val);
                } catch (...) {
                }
            }
        }
        return cfg;
    }

    Config Config::load() {
        std::string path;
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            path = std::string(xdg) + "/fenriz/fenriz.conf";
        else if (const char* home = std::getenv("HOME"); home && *home)
            path = std::string(home) + "/.config/fenriz/fenriz.conf";

        std::ifstream f(path);
        if (!f)
            return Config{}; // built-in defaults
        std::stringstream buf;
        buf << f.rdbuf();
        return parse(buf.str());
    }

} // namespace fenriz
