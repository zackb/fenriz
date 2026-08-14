#include <cassert>
#include <string>
#include <vector>

#include "theme.hpp"

using fenriz::desktop::Config;

namespace {

    int errors = 0;

    // GTK does not fail on a bad sheet, it warns and drops the rule — so a typo or an
    // unsupported colour function silently un-styles a surface. This is the only thing that
    // notices.
    void on_parsing_error(GtkCssProvider*, GtkCssSection* section, GError* error, gpointer) {
        char* where = gtk_css_section_to_string(section);
        g_printerr("theme: %s: %s\n", where, error->message);
        g_free(where);
        errors++;
    }

    void parse(const Config& cfg) {
        GtkCssProvider* provider = gtk_css_provider_new();
        g_signal_connect(provider, "parsing-error", G_CALLBACK(on_parsing_error), nullptr);
        gtk_css_provider_load_from_string(provider, fenriz::desktop::theme::sheet(cfg).c_str());
        g_object_unref(provider);
    }

    void test_sheet_parses() {
        parse(Config{});
        assert(errors == 0);
    }

    // shell_opacity reaches the sheet as a bare number inside alpha(), so a locale that
    // formats it "0,800000" would produce a sheet that does not parse.
    void test_opacity_is_interpolated() {
        Config cfg;
        for (double o : {0.0, 0.5, 0.8, 1.0}) {
            cfg.shell_opacity = o;
            parse(cfg);
        }
        assert(errors == 0);
    }

    void test_accents_from_the_compositor() {
        Config cfg;
        cfg.parse_accents("border_active = 0x16b8f3CC\nborder_gradient = 0xff2090ff\n");
        parse(cfg); // an rgba() accent has to survive being fed to mix()
        assert(errors == 0);
    }

    // The value of `prop` in the rule starting at `from`, or "" if it has none.
    std::string value_of(const std::string& css, size_t from, const std::string& prop) {
        const size_t end = css.find('}', from);
        const size_t at = css.find(prop + ":", from);
        if (at == std::string::npos || at > end)
            return "";
        const size_t start = at + prop.size() + 1;
        return css.substr(start, css.find(';', start) - start);
    }

    int layers(const std::string& value) {
        int depth = 0, n = value.empty() ? 0 : 1;
        for (char c : value) {
            if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            else if (c == ',' && depth == 0)
                n++;
        }
        return n;
    }

    // A background-size/position/clip list shorter than the image list is not an error: CSS
    // repeats it, silently mis-clipping a layer. That is exactly how the gradient ring leaked
    // across the card the first time, and nothing else would notice.
    void test_ring_layer_lists_line_up() {
        const std::string css = fenriz::desktop::theme::sheet(Config{});
        for (size_t at = css.find("background-image:"); at != std::string::npos;
             at = css.find("background-image:", at + 1)) {
            const size_t rule = css.rfind('{', at);
            const std::string image = value_of(css, rule, "background-image");
            if (layers(image) < 2)
                continue; // the single-layer rules have no lists to keep in step
            // An absent list is fine — it means "the default, for every layer". Only a list
            // that is present and short is the bug.
            for (const char* prop : {"background-size", "background-position", "background-clip"}) {
                const std::string v = value_of(css, rule, prop);
                if (!v.empty())
                    assert(layers(v) == layers(image));
            }
        }
    }

} // namespace

int main() {
    test_sheet_parses();
    test_opacity_is_interpolated();
    test_accents_from_the_compositor();
    test_ring_layer_lists_line_up();
    return 0;
}
