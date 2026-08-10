#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "wallpaper.hpp"

namespace fs = std::filesystem;
namespace wp = fenriz::desktop::wallpaper;

namespace {

    const fs::path ROOT = fs::temp_directory_path() / "fenriz-desktop-wallpaper-test";

    void write_png(const fs::path& path, int width, int height) {
        fs::create_directories(path.parent_path());
        GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
        gdk_pixbuf_fill(pixbuf, 0x336699ff);
        assert(gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr));
        g_object_unref(pixbuf);
    }

    void test_is_image() {
        assert(wp::is_image("/pic/a.png"));
        assert(wp::is_image("/pic/a.jpg"));
        assert(wp::is_image("/pic/a.webp"));
        // Extensions are matched case-insensitively; a camera dump is all .JPG.
        assert(wp::is_image("/pic/DSC0001.JPG"));
        assert(wp::is_image("/pic/a.JPEG"));
        assert(!wp::is_image("/pic/notes.txt"));
        assert(!wp::is_image("/pic/no-extension"));
    }

    void test_scan_is_recursive_and_skips_non_images() {
        fs::remove_all(ROOT);
        write_png(ROOT / "top.png", 8, 8);
        write_png(ROOT / "nested" / "deep" / "inner.png", 8, 8);
        std::ofstream(ROOT / "readme.txt") << "not an image\n";

        std::vector<std::string> found = wp::scan(ROOT.string());
        assert(found.size() == 2);
        for (const std::string& path : found)
            assert(wp::is_image(path));

        // A missing directory is a warning, not a crash.
        assert(wp::scan((ROOT / "does-not-exist").string()).empty());
    }

    // The cache key is what makes an edited wallpaper regenerate rather than serving
    // a stale thumbnail forever.
    void test_cache_path_tracks_the_file() {
        const fs::path image = ROOT / "keyed.png";
        write_png(image, 8, 8);

        const std::string first = wp::cache_path(image.string());
        assert(first == wp::cache_path(image.string()));
        assert(first.find(".png") != std::string::npos);
        // A hash that overflowed into a negative number would land here.
        assert(fs::path(first).filename().string()[0] != '-');

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        write_png(image, 16, 16); // different size and mtime
        assert(wp::cache_path(image.string()) != first);
    }

    void test_ensure_thumbnail_scales_and_caches() {
        const fs::path image = ROOT / "big.png";
        write_png(image, 1600, 900);

        const std::string thumb = wp::ensure_thumbnail(image.string());
        assert(!thumb.empty());
        assert(fs::exists(thumb));

        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(thumb.c_str(), nullptr);
        assert(pixbuf);
        const int width = gdk_pixbuf_get_width(pixbuf);
        const int height = gdk_pixbuf_get_height(pixbuf);
        g_object_unref(pixbuf);

        assert(width <= wp::THUMB_WIDTH && height <= wp::THUMB_HEIGHT);
        // Aspect preserved: a 16:9 source fills the 16:9 box exactly.
        assert(width == wp::THUMB_WIDTH && height == wp::THUMB_HEIGHT);

        // Second call is a cache hit, not a second decode.
        assert(wp::ensure_thumbnail(image.string()) == thumb);
    }

    void test_undecodable_file_is_reported() {
        const fs::path fake = ROOT / "lying.png";
        std::ofstream(fake) << "this is not a PNG\n";
        assert(wp::ensure_thumbnail(fake.string()).empty());
    }

} // namespace

int main() {
    setenv("XDG_CACHE_HOME", (ROOT / "cache").c_str(), 1);

    test_is_image();
    test_scan_is_recursive_and_skips_non_images();
    test_cache_path_tracks_the_file();
    test_ensure_thumbnail_scales_and_caches();
    test_undecodable_file_is_reported();

    fs::remove_all(ROOT);
    return 0;
}
