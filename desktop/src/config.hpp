#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fenriz::desktop {

    struct Config {
        std::string wallpaper;
        std::map<std::string, std::string> output_wallpaper; // connector name (eDP-1, DP-1) -> image path

        std::string terminal;
        std::vector<std::pair<std::string, std::string>> menu;
        bool launcher = true;

        int lock_blur = 24;
        int idle_lock = 0;

        // The image for `output`, falling back to the global one.
        const std::string& wallpaper_for(const std::string& output) const;

        static Config parse(const std::string& text);
        static Config load();
        static std::string config_path();
    };

} // namespace fenriz::desktop
