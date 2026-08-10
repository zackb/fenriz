#include "wallpaper.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace fenriz::desktop::wallpaper {

    namespace {

        constexpr std::array<const char*, 6> EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".avif"};

        std::string cache_dir() {
            const char* xdg = g_getenv("XDG_CACHE_HOME");
            std::string dir = (xdg && *xdg) ? std::string(xdg) : std::string(g_get_home_dir()) + "/.cache";
            return dir + "/fenriz/thumbnails";
        }

    } // namespace

    bool is_image(const std::string& path) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        return std::find(EXTENSIONS.begin(), EXTENSIONS.end(), ext) != EXTENSIONS.end();
    }

    std::vector<std::string> scan(const std::string& dir) {
        std::vector<std::pair<fs::file_time_type, std::string>> found;
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, ec), end;
        if (ec) {
            g_warning("wallpaper: cannot scan %s: %s", dir.c_str(), ec.message().c_str());
            return {};
        }
        for (; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (!it->is_regular_file(ec) || ec || !is_image(it->path().string()))
                continue;
            found.emplace_back(fs::last_write_time(it->path(), ec), it->path().string());
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });

        std::vector<std::string> out;
        out.reserve(found.size());
        for (auto& [when, path] : found)
            out.push_back(std::move(path));
        return out;
    }

    std::string cache_path(const std::string& image) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(image, ec);
        // The epoch of file_time_type is implementation-defined, but it only has to be
        // stable and to change with the file.
        const auto mtime = fs::last_write_time(image, ec).time_since_epoch().count();

        const std::string key =
            image + '\0' + std::to_string(size) + '\0' + std::to_string(static_cast<int64_t>(mtime));
        char* sum =
            g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(key.data()), key.size());
        std::string name(sum, 32);
        g_free(sum);

        return cache_dir() + "/" + name + "-" + std::to_string(THUMB_WIDTH) + "x" + std::to_string(THUMB_HEIGHT) +
               ".png";
    }

    std::string ensure_thumbnail(const std::string& image) {
        const std::string thumb = cache_path(image);
        if (g_file_test(thumb.c_str(), G_FILE_TEST_EXISTS))
            return thumb;

        GError* err = nullptr;
        // Decodes straight to the target size
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(image.c_str(), THUMB_WIDTH, THUMB_HEIGHT, TRUE, &err);
        if (!pixbuf) {
            g_warning("wallpaper: %s: %s", image.c_str(), err->message);
            g_error_free(err);
            return "";
        }

        char* dir = g_path_get_dirname(thumb.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        if (!gdk_pixbuf_save(pixbuf, thumb.c_str(), "png", &err, nullptr)) {
            g_warning("wallpaper: cannot cache thumbnail for %s: %s", image.c_str(), err->message);
            g_error_free(err);
            g_object_unref(pixbuf);
            return "";
        }
        g_object_unref(pixbuf);
        return thumb;
    }

} // namespace fenriz::desktop::wallpaper
