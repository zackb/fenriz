#pragma once

#include <gtk/gtk.h>

#include "config.hpp"

namespace fenriz::desktop::theme {

    // One style sheet for every fenriz-desktop surface. Installed above the user's gtk.css
    void install(const Config& cfg);

} // namespace fenriz::desktop::theme
