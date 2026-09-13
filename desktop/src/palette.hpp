#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fenriz::desktop::palette {

    // The Material You seed colour (ARGB) of `image`, ranked the way matugen's source index 0 is.
    std::optional<uint32_t> seed(const std::string& image);

    // A dark tonal-spot scheme for `seed` as @define-color rules: the fenriz_* accents plus the GTK and libadwaita
    // colour names the theme sheet builds on.
    std::string css(uint32_t seed);

    // Derives the palette of `image` off the main thread and writes it to colors_path() if it changed.
    void refresh(const std::string& image);

} // namespace fenriz::desktop::palette
