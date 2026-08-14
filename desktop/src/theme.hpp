#pragma once

#include <gtk/gtk.h>

#include <string>

#include "config.hpp"

namespace fenriz::desktop::theme {

    // Corner radius of the shell's cards.
    constexpr int CARD_RADIUS = 12;
    constexpr int PILL_RADIUS = 27;

    // The whole sheet, config values already folded in.
    std::string sheet(const Config& cfg);

    // One style sheet for every fenriz-desktop surface. Installed above the user's gtk.css
    void install(const Config& cfg);

} // namespace fenriz::desktop::theme
