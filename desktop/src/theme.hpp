#pragma once

#include <gtk/gtk.h>

#include <string>

#include "config.hpp"

namespace fenriz::desktop::theme {

    // The whole sheet, config values already folded in. `palette` is palette::css() output, which overrides the
    // config accents and the GTK theme's colours.
    std::string sheet(const Config& cfg, const std::string& palette = "");

    // One style sheet for every fenriz-desktop surface. Installed above the user's gtk.css. With theme = wallpaper it
    // follows colors_path() as it is rewritten.
    void install(const Config& cfg);

} // namespace fenriz::desktop::theme
