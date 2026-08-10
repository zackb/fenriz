#pragma once

#include <string>
#include <vector>

namespace fenriz::desktop::wallpaper {

    // Thumbnails are generated at the size the picker draws them
    constexpr int THUMB_WIDTH = 320;
    constexpr int THUMB_HEIGHT = 180;

    bool is_image(const std::string& path);

    // Every image under `dir`, recursively, newest first. Directory symlinks are not followed.
    std::vector<std::string> scan(const std::string& dir);

    // Where `image`'s thumbnail belongs. Keyed on path, size and mtime.
    std::string cache_path(const std::string& image);

    // Generates the thumbnail if it is missing. Returns its path, or "" if the image could not be decoded.
    std::string ensure_thumbnail(const std::string& image);

} // namespace fenriz::desktop::wallpaper
