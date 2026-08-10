#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fenriz::desktop {

    struct Config {
        std::string wallpaper;
        std::map<std::string, std::string> output_wallpaper; // connector name (eDP-1, DP-1) -> image path
        std::string wallpaper_dir;
        std::string selected_wallpaper;

        std::string terminal;
        std::vector<std::pair<std::string, std::string>> menu;
        bool launcher = true;

        int lock_blur = 24;
        int idle_lock = 0;
        int idle_dim = 0;        // seconds before dimming the backlight; 0 = never
        int dim_brightness = 10; // percent of each backlight's maximum while dimmed
        int idle_dpms = 0;       // seconds before switching the screens off; 0 = never

        // A runtime pick if there is one, else the image for `output`, else the global one.
        const std::string& wallpaper_for(const std::string& output) const;

        static Config parse(const std::string& text);
        static Config load();
        static std::string config_path();
    };

    std::string wallpaper_state_path();
    std::string load_selected_wallpaper();
    void save_selected_wallpaper(const std::string& path);

} // namespace fenriz::desktop
