#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "cpp/cam/hct.h"
#include "palette.hpp"
#include "theme.hpp"

namespace fs = std::filesystem;
namespace mcu = material_color_utilities;
using fenriz::desktop::Config;

namespace {

    const fs::path ROOT = fs::temp_directory_path() / "fenriz-desktop-palette-test";

    // `split` of the rows are `top`, the rest `bottom`.
    std::string write_png(const char* name, guint32 top, guint32 bottom, double split) {
        const fs::path path = ROOT / name;
        GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 400, 300);
        gdk_pixbuf_fill(pixbuf, bottom);
        GdkPixbuf* band = gdk_pixbuf_new_subpixbuf(pixbuf, 0, 0, 400, static_cast<int>(300 * split));
        gdk_pixbuf_fill(band, top);
        g_object_unref(band);
        assert(gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr));
        g_object_unref(pixbuf);
        return path;
    }

    double hue_distance(double a, double b) {
        const double d = std::fabs(a - b);
        return std::min(d, 360.0 - d);
    }

    void test_seed_follows_the_image() {
        const auto seed = fenriz::desktop::palette::seed(write_png("blue.png", 0x336699ff, 0x336699ff, 1.0));
        assert(seed);
        assert(hue_distance(mcu::Hct(*seed).get_hue(), mcu::Hct(0xff336699).get_hue()) < 5.0);
    }

    // Scoring prefers a vivid minority over a grey majority, which is what keeps the palette from going muddy.
    void test_seed_skips_grey() {
        const auto seed = fenriz::desktop::palette::seed(write_png("mostly-grey.png", 0xe02030ff, 0x808080ff, 0.3));
        assert(seed);
        assert(hue_distance(mcu::Hct(*seed).get_hue(), mcu::Hct(0xffe02030).get_hue()) < 10.0);
    }

    void test_undecodable_image_has_no_seed() {
        assert(!fenriz::desktop::palette::seed((ROOT / "missing.png").string()));
    }

    int errors = 0;

    void on_parsing_error(GtkCssProvider*, GtkCssSection*, GError* error, gpointer) {
        g_printerr("palette: %s\n", error->message);
        errors++;
    }

    void test_palette_sheet_parses() {
        const std::string css = fenriz::desktop::palette::css(0xff336699);
        for (const char* name : {"fenriz_accent ",
                                 "fenriz_accent2 ",
                                 "fenriz_on_accent ",
                                 "fenriz_error ",
                                 "window_bg_color ",
                                 "theme_fg_color ",
                                 "accent_bg_color "})
            assert(css.find(std::string("@define-color ") + name) != std::string::npos);

        GtkCssProvider* provider = gtk_css_provider_new();
        g_signal_connect(provider, "parsing-error", G_CALLBACK(on_parsing_error), nullptr);
        gtk_css_provider_load_from_string(provider, fenriz::desktop::theme::sheet(Config{}, css).c_str());
        g_object_unref(provider);
        assert(errors == 0);
    }

} // namespace

int main() {
    fs::remove_all(ROOT);
    fs::create_directories(ROOT);
    setenv("XDG_CACHE_HOME", (ROOT / "cache").c_str(), 1);

    test_seed_follows_the_image();
    test_seed_skips_grey();
    test_undecodable_image_has_no_seed();
    test_palette_sheet_parses();
    fs::remove_all(ROOT);
    return 0;
}
