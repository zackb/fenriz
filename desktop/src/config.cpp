#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace fenriz::desktop {

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

        // Split into at most `max` fields
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

        int parse_int(const std::string& s, int fallback, int lo, int hi) {
            try {
                return std::clamp(std::stoi(s), lo, hi);
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

        std::string expand(const std::string& path) {
            if (path.empty())
                return path;
            if (path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home && *home)
                    return std::string(home) + path.substr(1);
            }
            return path;
        }

    } // namespace

    const std::string& Config::wallpaper_for(const std::string& output) const {
        auto it = output_wallpaper.find(output);
        if (it != output_wallpaper.end())
            return it->second;
        return wallpaper;
    }

    Config Config::parse(const std::string& text) {
        Config cfg;
        std::stringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(strip_comment(line));
            if (line.empty())
                continue;

            std::vector<std::string> kv = split_n(line, '=', 2);
            if (kv.size() != 2)
                continue;
            const std::string& key = kv[0];
            const std::string& value = kv[1];

            if (key == "wallpaper") {
                cfg.wallpaper = expand(value);
            } else if (key == "output_wallpaper") {
                std::vector<std::string> f = split_n(value, ',', 2);
                if (f.size() == 2 && !f[0].empty() && !f[1].empty())
                    cfg.output_wallpaper[f[0]] = expand(f[1]);
            } else if (key == "terminal") {
                cfg.terminal = value;
            } else if (key == "lock_blur") {
                cfg.lock_blur = parse_int(value, cfg.lock_blur, 0, 200);
            } else if (key == "idle_lock") {
                cfg.idle_lock = parse_int(value, cfg.idle_lock, 0, 86400);
            } else if (key == "launcher") {
                cfg.launcher = parse_bool(value, cfg.launcher);
            } else if (key == "menu") {
                std::vector<std::string> f = split_n(value, ',', 2);
                if (f.size() == 2 && !f[0].empty() && !f[1].empty())
                    cfg.menu.emplace_back(f[0], f[1]);
            }
        }
        return cfg;
    }

    std::string Config::config_path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::string(xdg) + "/fenriz-desktop/fenriz-desktop.conf";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::string(home) + "/.config/fenriz-desktop/fenriz-desktop.conf";
        return "";
    }

    Config Config::load() {
        std::ifstream f(config_path());
        if (!f)
            return Config{}; // built-in defaults
        std::stringstream buf;
        buf << f.rdbuf();
        return parse(buf.str());
    }

} // namespace fenriz::desktop
